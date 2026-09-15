/*
 * mod-sanctuary-board
 *
 * A notice board in town. Players pin up bills - goods for sale, work wanted, a warning,
 * an invitation - and anyone can walk up and read them.
 *
 * Built on gossip rather than an addon on purpose. Gossip's coded input box gives a real
 * text field with no client-side code at all, which means a notice board works for someone
 * who has just arrived and installed nothing.
 *
 * Anonymity is preserved by construction: a bill is signed by whatever the author chose to
 * write on it, never by their character name. The author's guid is recorded so they can
 * take their own notice down and so a game master can trace abuse, and it is never shown.
 */

#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GameObjectScript.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    bool g_enabled = true;
    uint32 g_maxPostsPerCharacter = 3;
    uint32 g_expiryDays = 14;
    uint32 g_maxBodyLength = 255;
    uint32 g_listLimit = 12;
    uint32 g_minLevel = 1;
    uint32 g_moderatorSecurity = SEC_GAMEMASTER;

    /*
     * How close you must stay to keep reading.
     *
     * 0 means the board's own reach - GameObject::IsAtInteractDistance, which is the exact
     * test the core applies when you click it, bounding box and all. That is deliberately the
     * default: any other number invents a second, disagreeing range, and the half yard where
     * the cursor lights up but the server refuses is precisely the complaint the strongbox
     * had. A positive value overrides it with a plain centre-to-centre distance.
     */
    float g_range = 0.0f;

    enum BoardCategory : uint8
    {
        CATEGORY_GOODS = 0,
        CATEGORY_NOTICE = 1,
        CATEGORY_WANTED = 2,
        CATEGORY_EVENT = 3,
        CATEGORY_MAX
    };

    char const* CategoryName(uint8 category)
    {
        switch (category)
        {
            case CATEGORY_GOODS:  return "Goods & Services";
            case CATEGORY_NOTICE: return "Notice";
            case CATEGORY_WANTED: return "Wanted";
            case CATEGORY_EVENT:  return "Event";
            default:              return "Notice";
        }
    }

    /*
     * The gossip window is parchment, not one of the launcher's dark panels.
     *
     * These colours began life as the launcher's palette - near-white ink, pale gold - which
     * is close to invisible on tan paper, and is why the board read as washed out. Everything
     * below is dark enough to have real contrast against the parchment behind it.
     */
    constexpr char const* Ink = "|cff2f2114";      ///< body text, near-black brown
    constexpr char const* Muted = "|cff6b5a45";    ///< timestamps and asides
    constexpr char const* Accent = "|cff7a4e10";   ///< the actions worth spotting

    /// Colour per category, so a full board can be skimmed.
    char const* CategoryColour(uint8 category)
    {
        switch (category)
        {
            case CATEGORY_GOODS:  return "|cff1e5a2a";   // forest green
            case CATEGORY_NOTICE: return Ink;
            case CATEGORY_WANTED: return "|cff8f1d15";   // deep red
            case CATEGORY_EVENT:  return Accent;         // bronze
            default:              return Ink;
        }
    }

    std::string g_prefix = "SBOARD";

    /*
     * Who is reading the board through the addon rather than through gossip.
     *
     * The gossip menu stays exactly as it was, and that is the point: it works for somebody
     * who has just arrived and installed nothing, which is why the board was built on gossip
     * in the first place. The addon is an alternative for people who have it, not a
     * replacement - so the server has to know which it is talking to, and the only honest
     * way to know is to be told at login.
     */
    std::unordered_set<ObjectGuid::LowType> g_hasAddon;

    /*
     * The board each addon window was opened on, by spawn id.
     *
     * Without this the window was bound to nothing: it opened at a board and then went on
     * answering from anywhere in the world, so a notice could be pinned up in Orgrimmar from
     * a boat in Booty Bay. The id is remembered when the board is clicked and every later
     * message is checked against it.
     */
    std::unordered_map<ObjectGuid::LowType, ObjectGuid::LowType> g_atBoard;

    struct BoardPost
    {
        uint32 Id = 0;
        ObjectGuid::LowType Author = 0;
        uint8 Category = CATEGORY_NOTICE;
        std::string Body;
        uint32 PostedAt = 0;
    };

    /// Gossip actions. Offsets keep post ids and categories out of each other's way.
    constexpr uint32 ACTION_LIST = 1000;
    constexpr uint32 ACTION_COMPOSE = 1001;
    constexpr uint32 ACTION_MINE = 1002;
    constexpr uint32 ACTION_BACK = 1003;
    constexpr uint32 ACTION_CATEGORY_BASE = 2000;   ///< + category
    constexpr uint32 ACTION_READ_BASE = 10000;      ///< + row index
    constexpr uint32 ACTION_REMOVE_BASE = 20000;    ///< + row index, author taking their own down
    constexpr uint32 ACTION_GM_REMOVE_BASE = 30000; ///< + row index, moderator taking anyone's down

    /*
     * The paragraph above the options, one npc_text row per screen.
     *
     * Passing DEFAULT_GOSSIP_MESSAGE here is what made every board open with
     * "Greetings, <name>": that constant is 0xffffff, which is a real npc_text row whose
     * text is exactly that. A board is furniture and should not greet anybody.
     *
     * Rows live in data/sql/db-world/2026_08_31_03_sanctuary_board_text.sql.
     */
    constexpr uint32 TEXT_BOARD = 990010;
    constexpr uint32 TEXT_POST = 990011;
    constexpr uint32 TEXT_COMPOSE = 990012;
    constexpr uint32 TEXT_MINE = 990013;

    /// What each player is currently looking at, so a row index means something.
    std::unordered_map<ObjectGuid, std::vector<BoardPost>> g_viewing;

    uint32 Now()
    {
        return uint32(GameTime::GetGameTime().count());
    }

    /// Trims, collapses newlines and caps the length. Returns false if nothing is left.
    bool SanitiseBody(std::string& body)
    {
        // A notice is one block of text on a board. Newlines would let a single bill
        // scroll the whole gossip window and push everyone else's off.
        std::replace(body.begin(), body.end(), '\n', ' ');
        std::replace(body.begin(), body.end(), '\r', ' ');

        // The client already escapes these, but a stray pipe would let a bill recolour
        // or hyperlink the rest of the menu.
        std::string cleaned;
        cleaned.reserve(body.size());

        for (char c : body)
        {
            if (c == '|')
                continue;

            if (static_cast<unsigned char>(c) < 0x20)
                continue;

            cleaned += c;
        }

        std::size_t const first = cleaned.find_first_not_of(' ');
        std::size_t const last = cleaned.find_last_not_of(' ');

        if (first == std::string::npos)
            return false;

        cleaned = cleaned.substr(first, last - first + 1);

        if (cleaned.size() > g_maxBodyLength)
            cleaned = cleaned.substr(0, g_maxBodyLength);

        body = cleaned;
        return !body.empty();
    }

    std::vector<BoardPost> LoadPosts(uint32 limit)
    {
        std::vector<BoardPost> posts;

        QueryResult result = CharacterDatabase.Query(
            "SELECT `id`, `author_guid`, `category`, `body`, `posted_at` "
            "FROM `character_board_posts` "
            "WHERE `expires_at` = 0 OR `expires_at` > {} "
            "ORDER BY `posted_at` DESC LIMIT {}",
            Now(), limit);

        if (!result)
            return posts;

        do
        {
            Field* fields = result->Fetch();

            BoardPost post;
            post.Id = fields[0].Get<uint32>();
            post.Author = fields[1].Get<uint32>();
            post.Category = fields[2].Get<uint8>();
            post.Body = fields[3].Get<std::string>();
            post.PostedAt = fields[4].Get<uint32>();

            posts.push_back(std::move(post));
        } while (result->NextRow());

        return posts;
    }

    std::vector<BoardPost> LoadPostsBy(ObjectGuid::LowType author)
    {
        std::vector<BoardPost> posts;

        QueryResult result = CharacterDatabase.Query(
            "SELECT `id`, `author_guid`, `category`, `body`, `posted_at` "
            "FROM `character_board_posts` WHERE `author_guid` = {} ORDER BY `posted_at` DESC",
            author);

        if (!result)
            return posts;

        do
        {
            Field* fields = result->Fetch();

            BoardPost post;
            post.Id = fields[0].Get<uint32>();
            post.Author = fields[1].Get<uint32>();
            post.Category = fields[2].Get<uint8>();
            post.Body = fields[3].Get<std::string>();
            post.PostedAt = fields[4].Get<uint32>();

            posts.push_back(std::move(post));
        } while (result->NextRow());

        return posts;
    }

    uint32 CountPostsBy(ObjectGuid::LowType author)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM `character_board_posts` WHERE `author_guid` = {}", author);

        return result ? (*result)[0].Get<uint32>() : 0;
    }

    /*
     * Whether this player may take down other people's notices and see who wrote them.
     *
     * Checked on every action rather than once when the menu is drawn: the gossip action a
     * client sends back is just a number, and nothing stops an ordinary player sending the
     * moderator one by hand.
     */
    bool IsModerator(Player* player)
    {
        return player && player->GetSession()
            && player->GetSession()->GetSecurity() >= AccountTypes(g_moderatorSecurity);
    }

    /// Who wrote a notice, for moderators only. Works whether or not they are online.
    std::string DescribeAuthor(ObjectGuid::LowType author)
    {
        ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(author);

        std::string name;
        if (!sCharacterCache->GetCharacterNameByGuid(guid, name))
            name = "<deleted character>";

        return Acore::StringFormat("{} (guid {}, account {})",
            name, author, sCharacterCache->GetCharacterAccountIdByGuid(guid));
    }

    /*
     * The board a player opened, if they are still standing at it.
     *
     * Looked up through the map's spawn-id store rather than by guid arithmetic, and the
     * distance is measured against what that returns - the same shape of check the strongbox
     * needed after its first version assigned the object and never read it.
     */
    GameObject* AtBoard(Player* player)
    {
        if (!player || !player->GetMap())
            return nullptr;

        auto const at = g_atBoard.find(player->GetGUID().GetCounter());

        if (at == g_atBoard.end())
            return nullptr;

        auto const bounds = player->GetMap()->GetGameObjectBySpawnIdStore().equal_range(at->second);

        if (bounds.first == bounds.second)
            return nullptr;                          // unloaded, deleted, or another continent

        GameObject* board = bounds.first->second;

        // Not "near": windows.h still defines that as an empty macro from the 16-bit days,
        // so the declaration below it becomes "bool const = ..." and the compiler is right
        // to be confused.
        bool const withinReach = g_range > 0.0f
            ? player->IsWithinDistInMap(board, g_range)
            : board->IsAtInteractDistance(player);

        return withinReach ? board : nullptr;
    }

    std::string Ago(uint32 postedAt)
    {
        uint32 const seconds = Now() > postedAt ? Now() - postedAt : 0;

        if (seconds < 3600)
            return Acore::StringFormat("{}m", std::max<uint32>(1, seconds / 60));

        if (seconds < 86400)
            return Acore::StringFormat("{}h", seconds / 3600);

        return Acore::StringFormat("{}d", seconds / 86400);
    }

    /// One line on the board: category, a readable slice of the bill, and its age.
    void Tell(Player* player, std::string const& what)
    {
        if (player && player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage("{}", what);
    }

    void SendAddonPacket(Player* player, std::string const& payload)
    {
        if (!player || !player->GetSession())
            return;

        // 3.3.5a carries addon traffic as "PREFIX\tBODY" inside a whisper to self.
        std::string message = g_prefix + "\t" + payload;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, message);
        player->GetSession()->SendPacket(&data);
    }

    /*
     * Pinning a notice up, wherever the request came from.
     *
     * Extracted so the gossip screen and the addon window cannot drift apart on the rules -
     * the length limit, the per-character cap, and above all the log line that records what
     * was written before anybody can take it down again. Two copies of that would eventually
     * become one copy.
     */
    bool CreatePost(Player* player, uint8 category, std::string body, std::string& problem)
    {
        if (category >= CATEGORY_MAX)
        {
            problem = "That is not a category.";
            return false;
        }

        if (!SanitiseBody(body))
        {
            problem = "That notice was empty.";
            return false;
        }

        if (CountPostsBy(player->GetGUID().GetCounter()) >= g_maxPostsPerCharacter)
        {
            problem = Acore::StringFormat("You already have {} notices up.", g_maxPostsPerCharacter);
            return false;
        }

        uint32 const expires = g_expiryDays > 0 ? Now() + (g_expiryDays * 86400) : 0;

        std::string escaped = body;
        CharacterDatabase.EscapeString(escaped);

        CharacterDatabase.DirectExecute(
            "INSERT INTO `character_board_posts` (`author_guid`, `category`, `body`, `posted_at`, `expires_at`) "
            "VALUES ({}, {}, '{}', {}, {})",
            player->GetGUID().GetCounter(), category, escaped, Now(), expires);

        /*
         * Every bill is logged as it is written, author and all.
         *
         * The in-game moderator view only reaches notices still on the board; somebody who
         * posts a slur and takes it down a minute later would leave nothing behind. This is
         * the record that survives that.
         */
        LOG_INFO("module.sanctuaryboard", "POST: {} [{}] \"{}\"",
            DescribeAuthor(player->GetGUID().GetCounter()), CategoryName(category), body);

        return true;
    }

    /*
     * The board as the addon sees it.
     *
     * A body is up to 255 characters and an addon message has far less room than that once
     * the prefix and the rest of the line are counted, so the text arrives in pieces and is
     * stitched back together on the far side. Everything else about a notice fits in one
     * line, which is why the metadata and the words travel separately.
     */
    void SendBoard(Player* player)
    {
        std::vector<BoardPost> const posts = LoadPosts(g_listLimit);
        ObjectGuid::LowType const me = player->GetGUID().GetCounter();

        SendAddonPacket(player, Acore::StringFormat("OPEN {} {} {}",
            uint32(posts.size()), g_maxPostsPerCharacter, CountPostsBy(me)));

        bool const moderator = IsModerator(player);

        for (BoardPost const& post : posts)
        {
            SendAddonPacket(player, Acore::StringFormat("POST {} {} {} {}",
                post.Id, uint32(post.Category), post.Author == me ? 1 : 0, post.PostedAt));

            /*
             * Authorship, on its own line and only to moderators.
             *
             * A separate verb rather than a field on POST so the ordinary grammar is
             * untouched: a client that never receives WHO parses exactly what it always did,
             * and there is no line whose meaning depends on who is reading it.
             *
             * This is the answer to a slur on the board. The write is already logged as it
             * happens, but the log is a file somebody has to go and read - a moderator
             * standing in front of the board should be able to see who wrote what without
             * leaving the game.
             */
            if (moderator)
                SendAddonPacket(player, Acore::StringFormat("WHO {} {}",
                    post.Id, DescribeAuthor(post.Author)));

            constexpr std::size_t Chunk = 120;

            for (std::size_t at = 0; at < post.Body.size(); at += Chunk)
                SendAddonPacket(player, Acore::StringFormat("BODY {} {}",
                    post.Id, post.Body.substr(at, Chunk)));
        }

        SendAddonPacket(player, "DONE");
    }

    std::string Summarise(BoardPost const& post)
    {
        constexpr std::size_t Preview = 48;

        std::string preview = post.Body.size() > Preview
            ? post.Body.substr(0, Preview) + "..."
            : post.Body;

        return Acore::StringFormat("{}[{}]|r {}{}  {}({})|r",
            CategoryColour(post.Category), CategoryName(post.Category),
            Ink, preview, Muted, Ago(post.PostedAt));
    }

    void ShowBoard(Player* player, GameObject* board)
    {
        ClearGossipMenuFor(player);

        std::vector<BoardPost> posts = LoadPosts(g_listLimit);

        if (posts.empty())
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, std::string(Muted) + "The board is empty.|r",
                GOSSIP_SENDER_MAIN, ACTION_BACK);
        }
        else
        {
            for (std::size_t i = 0; i < posts.size(); ++i)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, Summarise(posts[i]),
                    GOSSIP_SENDER_MAIN, uint32(ACTION_READ_BASE + i));
        }

        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, std::string(Accent) + "Pin up a notice|r",
            GOSSIP_SENDER_MAIN, ACTION_COMPOSE);

        if (CountPostsBy(player->GetGUID().GetCounter()) > 0)
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                std::string(Ink) + "Take down one of my notices|r",
                GOSSIP_SENDER_MAIN, ACTION_MINE);

        g_viewing[player->GetGUID()] = std::move(posts);

        SendGossipMenuFor(player, TEXT_BOARD, board->GetGUID());
    }

    void ShowPost(Player* player, GameObject* board, BoardPost const& post, std::size_t index)
    {
        ClearGossipMenuFor(player);

        // The gossip item text is the only place a full bill fits; the body is shown as a
        // non-selectable line above the way back.
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            Acore::StringFormat("{}[{}]|r", CategoryColour(post.Category), CategoryName(post.Category)),
            GOSSIP_SENDER_MAIN, ACTION_BACK);

        // The bill itself is the thing people came to read, so it gets the darkest ink.
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, std::string(Ink) + post.Body + "|r",
            GOSSIP_SENDER_MAIN, ACTION_BACK);

        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            Acore::StringFormat("{}Pinned up {} ago.|r", Muted, Ago(post.PostedAt)),
            GOSSIP_SENDER_MAIN, ACTION_BACK);

        /*
         * Moderators, and only moderators, see who wrote it.
         *
         * The board is anonymous on purpose - a bill is signed by whatever its author chose
         * to write - but anonymity that nobody can see behind is a licence to post slurs.
         * The author's guid has been recorded since the first version precisely so this is
         * possible without weakening the rule for everyone else.
         */
        if (IsModerator(player))
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                Acore::StringFormat("{}Written by {}|r", Muted, DescribeAuthor(post.Author)),
                GOSSIP_SENDER_MAIN, ACTION_BACK);

            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                "|cff8f1d15Take this notice down|r",
                GOSSIP_SENDER_MAIN, uint32(ACTION_GM_REMOVE_BASE + index));
        }

        AddGossipItemFor(player, GOSSIP_ICON_TALK, std::string(Accent) + "< Back to the board|r",
            GOSSIP_SENDER_MAIN, ACTION_LIST);

        SendGossipMenuFor(player, TEXT_POST, board->GetGUID());
    }

    void ShowCompose(Player* player, GameObject* board)
    {
        ClearGossipMenuFor(player);

        for (uint8 category = 0; category < CATEGORY_MAX; ++category)
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                Acore::StringFormat("{}{}|r", CategoryColour(category), CategoryName(category)),
                GOSSIP_SENDER_MAIN, ACTION_CATEGORY_BASE + category,
                // The prompt is the only instruction the player gets, so it has to carry
                // the one thing they will otherwise get wrong: sign it yourself.
                "Write your notice. Sign it however you wish to be known - your character's "
                "name is never shown.",
                0, true);
        }

        AddGossipItemFor(player, GOSSIP_ICON_TALK, std::string(Accent) + "< Back to the board|r",
            GOSSIP_SENDER_MAIN, ACTION_LIST);

        SendGossipMenuFor(player, TEXT_COMPOSE, board->GetGUID());
    }

    void ShowMine(Player* player, GameObject* board)
    {
        ClearGossipMenuFor(player);

        std::vector<BoardPost> mine = LoadPostsBy(player->GetGUID().GetCounter());

        // Labelled, not just listed. Summarise() renders these exactly as the read-only
        // rows on the board, so without a prefix a click that destroys a notice looks
        // identical to one that opens it - and there is no confirmation behind it.
        for (std::size_t i = 0; i < mine.size(); ++i)
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                Acore::StringFormat("|cff8f1d15Take down:|r {}", Summarise(mine[i])),
                GOSSIP_SENDER_MAIN, uint32(ACTION_REMOVE_BASE + i));

        AddGossipItemFor(player, GOSSIP_ICON_TALK, std::string(Accent) + "< Back to the board|r",
            GOSSIP_SENDER_MAIN, ACTION_LIST);

        g_viewing[player->GetGUID()] = std::move(mine);

        SendGossipMenuFor(player, TEXT_MINE, board->GetGUID());
    }
}

class sanctuary_board_gameobject : public GameObjectScript
{
public:
    sanctuary_board_gameobject() : GameObjectScript("sanctuary_board") { }

    bool OnGossipHello(Player* player, GameObject* board) override
    {
        /*
         * The window for anybody who has the addon, the gossip menu for everybody else.
         *
         * Gossip is not being replaced. It is what makes the board work for somebody who has
         * just arrived and installed nothing, which is the reason it was chosen over an addon
         * to begin with - it simply cannot hold a whole notice, and that is what the window
         * is for.
         */
        if (!g_enabled || !player)
            return false;

        /*
         * The level gate first, ahead of the choice of interface.
         *
         * It used to sit below the addon branch, which meant it only applied to people
         * reading through gossip - install the addon and the gate was gone. A rule that a
         * client can opt out of by installing something is not a rule.
         */
        if (player->GetLevel() < g_minLevel)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "You must be level {} to use the notice board.", g_minLevel);
            return true;
        }

        // Which board this is. Everything the window sends afterwards is measured against it.
        g_atBoard[player->GetGUID().GetCounter()] = board->GetSpawnId();

        if (g_hasAddon.count(player->GetGUID().GetCounter()))
        {
            SendBoard(player);
            return true;
        }

        ShowBoard(player, board);
        return true;
    }

    bool OnGossipSelect(Player* player, GameObject* board, uint32 /*sender*/, uint32 action) override
    {
        if (action == ACTION_LIST || action == ACTION_BACK)
        {
            ShowBoard(player, board);
            return true;
        }

        if (action == ACTION_COMPOSE)
        {
            if (CountPostsBy(player->GetGUID().GetCounter()) >= g_maxPostsPerCharacter)
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "You already have {} notices up. Take one down first.", g_maxPostsPerCharacter);
                ShowBoard(player, board);
                return true;
            }

            ShowCompose(player, board);
            return true;
        }

        if (action == ACTION_MINE)
        {
            ShowMine(player, board);
            return true;
        }

        // Checked before the lower ranges, because the tests are >= and 30000 would
        // otherwise be caught by the ACTION_REMOVE_BASE branch and scoped to the author.
        if (action >= ACTION_GM_REMOVE_BASE)
        {
            if (!IsModerator(player))
            {
                // The action id came from the client, so an ordinary player can send this
                // without any addon at all. Refuse it and say so in the log.
                LOG_WARN("module.sanctuaryboard", "{} tried a moderator removal without the rights for it.",
                    player->GetName());
                ShowBoard(player, board);
                return true;
            }

            std::size_t const index = action - ACTION_GM_REMOVE_BASE;
            auto itr = g_viewing.find(player->GetGUID());

            if (itr != g_viewing.end() && index < itr->second.size())
            {
                BoardPost const& post = itr->second[index];

                // Logged before the delete, with the body intact: once the row is gone this
                // is the only remaining record of what was actually written, which is the
                // thing any punishment has to rest on.
                LOG_INFO("module.sanctuaryboard",
                    "MODERATION: {} removed notice {} by {} [{}] \"{}\"",
                    player->GetName(), post.Id, DescribeAuthor(post.Author),
                    CategoryName(post.Category), post.Body);

                CharacterDatabase.DirectExecute(
                    "DELETE FROM `character_board_posts` WHERE `id` = {}", post.Id);

                ChatHandler(player->GetSession()).PSendSysMessage(
                    "Notice removed. Author: {}", DescribeAuthor(post.Author));
            }

            ShowBoard(player, board);
            return true;
        }

        if (action >= ACTION_REMOVE_BASE)
        {
            std::size_t const index = action - ACTION_REMOVE_BASE;
            auto itr = g_viewing.find(player->GetGUID());

            if (itr != g_viewing.end() && index < itr->second.size())
            {
                BoardPost const& post = itr->second[index];

                // Scoped to the author as well as the id: the row index came from the
                // client, and only the author may take a notice down.
                // DirectExecute, not Execute: Execute is asynchronous, and the board is
                // redrawn on the very next line by a synchronous Query. The queued delete
                // lost that race, so the notice was still listed and the player had to
                // click a second time before it vanished.
                CharacterDatabase.DirectExecute(
                    "DELETE FROM `character_board_posts` WHERE `id` = {} AND `author_guid` = {}",
                    post.Id, player->GetGUID().GetCounter());

                ChatHandler(player->GetSession()).PSendSysMessage("Your notice has been taken down.");
            }

            ShowBoard(player, board);
            return true;
        }

        if (action >= ACTION_READ_BASE)
        {
            std::size_t const index = action - ACTION_READ_BASE;
            auto itr = g_viewing.find(player->GetGUID());

            if (itr != g_viewing.end() && index < itr->second.size())
                ShowPost(player, board, itr->second[index], index);
            else
                ShowBoard(player, board);

            return true;
        }

        CloseGossipMenuFor(player);
        return true;
    }

    bool OnGossipSelectCode(Player* player, GameObject* board, uint32 /*sender*/, uint32 action,
        char const* code) override
    {
        if (action < ACTION_CATEGORY_BASE || action >= ACTION_CATEGORY_BASE + CATEGORY_MAX)
        {
            ShowBoard(player, board);
            return true;
        }

        uint8 const category = uint8(action - ACTION_CATEGORY_BASE);

        // The rules live in CreatePost, which the addon window uses as well. Re-checked
        // there rather than trusted from the menu: the compose screen could have been opened
        // before another notice was pinned up elsewhere.
        std::string problem;

        if (!CreatePost(player, category, code ? code : "", problem))
            ChatHandler(player->GetSession()).PSendSysMessage("{}", problem);
        else
            ChatHandler(player->GetSession()).PSendSysMessage("Your notice is on the board.");

        ShowBoard(player, board);
        return true;
    }
};

class sanctuary_board_playerscript : public PlayerScript
{
public:
    sanctuary_board_playerscript() : PlayerScript("sanctuary_board_playerscript",
        { PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

    void OnPlayerLogout(Player* player) override
    {
        g_viewing.erase(player->GetGUID());
        g_hasAddon.erase(player->GetGUID().GetCounter());
        g_atBoard.erase(player->GetGUID().GetCounter());
    }

    /// Addon traffic arrives as a whisper the player sends to themselves.
    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg, Player* /*receiver*/) override
    {
        if (lang != LANG_ADDON || !player || !g_enabled)
            return true;

        std::string const marker = g_prefix + "	";

        // Every registered PlayerScript sees this message and the first false swallows it,
        // so anything that is not ours has to be passed along untouched.
        if (msg.rfind(marker, 0) != 0)
            return true;

        std::string const body = msg.substr(marker.size());
        std::string verb = body.substr(0, body.find(' '));
        std::string rest = body.size() > verb.size() + 1 ? body.substr(verb.size() + 1) : "";

        ObjectGuid::LowType const me = player->GetGUID().GetCounter();

        if (verb == "HELLO")
        {
            // Said at login, so that walking up to a board already knows which interface to
            // draw rather than guessing and being wrong once. No board involved yet.
            g_hasAddon.insert(me);
            SendAddonPacket(player, "HI");
            return false;
        }

        /*
         * Everything below this line needs the board.
         *
         * Checked on every message rather than once when the window opened, because the
         * window opening is the only moment the client's own reach was ever consulted. Walk
         * away, die, or have the board despawn, and the next thing the window says is refused
         * and the window is told to close - the client cannot be trusted to close itself, and
         * a window that still looks open is a window that lies.
         */
        if (!AtBoard(player))
        {
            g_atBoard.erase(me);
            SendAddonPacket(player, "SHUT");
            return false;
        }

        if (verb == "HERE")
        {
            // The window's heartbeat. Answers nothing while you are still at the board: the
            // check above is the whole of it, and a board of twelve notices is far too much
            // traffic to resend once a second just to prove the reader has not moved.
        }
        else if (verb == "SYNC")
        {
            SendBoard(player);
        }
        else if (verb == "POST")
        {
            uint32 category = CATEGORY_MAX;
            std::string text;

            std::size_t const space = rest.find(' ');

            if (space != std::string::npos)
            {
                category = uint32(atoi(rest.substr(0, space).c_str()));
                text = rest.substr(space + 1);
            }

            std::string problem;

            if (!CreatePost(player, uint8(category), text, problem))
                Tell(player, problem);
            else
                Tell(player, "Your notice is on the board.");

            SendBoard(player);
        }
        else if (verb == "REMOVE")
        {
            uint32 const id = uint32(atoi(rest.c_str()));

            /*
             * The author may take their own down; a moderator may take anyone's.
             *
             * The id came from the client and a client can say anything, so the row is
             * read first and the decision made on what is actually there: a forged id
             * finds nothing and deletes nothing, and an ordinary player naming somebody
             * else's notice is refused. The gossip board has let moderators do this since
             * the first version; the window simply never offered it.
             */
            QueryResult row = CharacterDatabase.Query(
                "SELECT `author_guid`, `category`, `body` FROM `character_board_posts` WHERE `id` = {}", id);

            if (!row)
                return false;

            Field* fields = row->Fetch();
            ObjectGuid::LowType const author = fields[0].Get<uint32>();
            uint32 const category = fields[1].Get<uint32>();
            std::string const body = fields[2].Get<std::string>();

            if (author != me)
            {
                if (!IsModerator(player))
                {
                    LOG_WARN("module.sanctuaryboard", "{} asked to remove notice {}, which is not theirs.",
                        player->GetName(), id);
                    return false;
                }

                // Logged before the delete, with the body intact: once the row is gone this
                // is the only remaining record of what was actually written, which is the
                // thing any punishment has to rest on. Same line the gossip path writes.
                LOG_INFO("module.sanctuaryboard",
                    "MODERATION: {} removed notice {} by {} [{}] \"{}\"",
                    player->GetName(), id, DescribeAuthor(author), CategoryName(category), body);
            }

            CharacterDatabase.DirectExecute(
                "DELETE FROM `character_board_posts` WHERE `id` = {}", id);

            SendBoard(player);
        }

        return false;
    }
};

class sanctuary_board_worldscript : public WorldScript
{
public:
    sanctuary_board_worldscript() : WorldScript("sanctuary_board_worldscript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>("SanctuaryBoard.Enable", true);
        g_maxPostsPerCharacter = sConfigMgr->GetOption<uint32>("SanctuaryBoard.MaxPostsPerCharacter", 3);
        g_expiryDays = sConfigMgr->GetOption<uint32>("SanctuaryBoard.ExpiryDays", 14);
        g_maxBodyLength = std::clamp(sConfigMgr->GetOption<uint32>("SanctuaryBoard.MaxLength", 255u), 16u, 255u);
        g_listLimit = std::clamp(sConfigMgr->GetOption<uint32>("SanctuaryBoard.ListLimit", 12u), 1u, 20u);
        g_minLevel = sConfigMgr->GetOption<uint32>("SanctuaryBoard.MinLevel", 1);
        g_range = std::max(0.0f, sConfigMgr->GetOption<float>("SanctuaryBoard.Range", 0.0f));
        g_moderatorSecurity = sConfigMgr->GetOption<uint32>("SanctuaryBoard.ModeratorSecurity",
            uint32(SEC_GAMEMASTER));

        LOG_INFO("module.sanctuaryboard", "Sanctuary notice boards {}.", g_enabled ? "enabled" : "disabled");
    }

    /// Sweeps expired notices, so the board does not need pruning by hand.
    void OnUpdate(uint32 diff) override
    {
        _sinceSweep += diff;
        if (_sinceSweep < 600000)
            return;

        _sinceSweep = 0;

        CharacterDatabase.Execute(
            "DELETE FROM `character_board_posts` WHERE `expires_at` > 0 AND `expires_at` < {}", Now());
    }

private:
    uint32 _sinceSweep = 0;
};

void AddSC_sanctuary_board_scripts()
{
    new sanctuary_board_gameobject();
    new sanctuary_board_playerscript();
    new sanctuary_board_worldscript();
}
