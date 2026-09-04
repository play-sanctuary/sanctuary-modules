/*
 * mod-sanctuary-identity
 *
 * Players are strangers until they introduce themselves to one another. A stranger reads as an
 * alias - "Hooded Orc" - everywhere their name would otherwise appear.
 *
 * The disguise is applied on the server, in the name the client is given, rather than drawn over
 * the top of the real one by an addon. That is not a preference: the name above a character's head
 * is drawn by the client itself, unconditionally, from its own name cache. No CVar governs it and
 * no frame exists to hook, so the only way it can read anything but the real name is for the real
 * name never to have been sent.
 *
 * Introductions are one way. A row says "knower may see known's name"; a mutual acquaintance is two
 * rows. Nobody can add a row that reveals somebody else, only one that reveals themselves.
 */

#include "SanctuaryIdentity.h"

#include "Cell.h"
#include "CellImpl.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "CommandScript.h"
#include "RBAC.h"
#include "Config.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "QueryPackets.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <array>
#include <cstring>
#include <stdexcept>
#include <exception>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace Acore::ChatCommands;

namespace
{
    // Not an "s_" prefix: winsock2.h defines s_host and friends as macros on in_addr, and a
    // file-scope name colliding with one expands into nonsense before the compiler sees it.
    bool g_enabled = true;
    bool g_disguiseNames = false;
    bool g_gameMastersSeeNames = true;
    std::string g_prefix = "SVID";
    uint32 g_maxQueriesPerSecond = 20;

    /// knower -> everyone whose real name they have been given. Loaded on login.
    std::unordered_map<ObjectGuid::LowType, std::unordered_set<ObjectGuid::LowType>> g_known;

    struct QueryBudget
    {
        time_t Window = 0;
        uint32 Used = 0;
    };

    std::unordered_map<ObjectGuid::LowType, QueryBudget> g_budgets;

    /// Last seen GM-mode state per character, so the toggle can be noticed.
    std::unordered_map<ObjectGuid::LowType, bool> g_gmMode;

    void Send(Player* to, std::string const& body)
    {
        if (!to || !to->GetSession())
            return;

        std::string payload = g_prefix + "\t" + body;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, to, to, payload);
        to->GetSession()->SendPacket(&data);
    }

    /// The race as this viewer's client would spell it, so the disguise reads naturally.
    std::string RaceLabel(Player* subject, Player* viewer)
    {
        ChrRacesEntry const* race = sChrRacesStore.LookupEntry(subject->getRace());
        if (!race)
            return "Stranger";

        LocaleConstant locale = viewer && viewer->GetSession()
            ? viewer->GetSession()->GetSessionDbcLocale()
            : DEFAULT_LOCALE;

        char const* name = race->name[locale];
        if (!name || !*name)
            name = race->name[DEFAULT_LOCALE];

        return name && *name ? name : "Stranger";
    }

    // --- aliases ----------------------------------------------------------

    /*
     * What a stranger is called.
     *
     * Deliberately descriptive rather than anonymous: the character model already gives the race
     * away, so hiding it buys nothing, while "the Hooded Orc" is something two players can
     * actually use to refer to a third. None of these imply gender, class or anything else the
     * viewer cannot already see.
     */
    constexpr std::array<char const*, 32> Adjectives =
    {
        "Hooded",   "Cloaked",   "Scarred",  "Weathered", "Silent",   "Stern",    "Wary",     "Grim",
        "Quiet",    "Tall",      "Lean",     "Ragged",    "Dusty",    "Pale",     "Hardy",    "Sullen",
        "Restless", "Watchful",  "Solemn",   "Shrouded",  "Masked",   "Veiled",   "Weary",    "Guarded",
        "Stoic",    "Roaming",   "Wandering","Drifting",  "Shadowed", "Muffled",  "Faceless", "Nameless"
    };

    /// guid -> allocated alias, filled on login so the disguise never has to hit the database.
    std::unordered_map<ObjectGuid::LowType, std::string> g_aliases;

    /*
     * Two kinds of alias, sharing one uniqueness namespace.
     *
     * The stranger alias is who you are to somebody who has never met you. The disguise alias
     * is who you are to everybody, including people who have. They share the UNIQUE index on
     * `alias` deliberately: allocated from separate pools, a disguise could be handed the same
     * "Hooded Orc" as some other character's stranger alias and two people would read alike.
     */
    enum AliasKind : uint8
    {
        ALIAS_STRANGER = 0,
        ALIAS_DISGUISE = 1
    };

    /// guid -> disguise alias, allocated the first time one is ever put on.
    std::unordered_map<ObjectGuid::LowType, std::string> g_disguiseAliases;

    /// Characters currently wearing a disguise.
    std::unordered_set<ObjectGuid::LowType> g_disguised;

    bool g_disguiseEnabled = true;

    /// 100001-100004 are taken by voice, outlaw, lawman and carry.
    constexpr uint32 RBAC_PERM_COMMAND_DISGUISE = 100005;

    bool IsWearingDisguise(Player* subject)
    {
        return g_disguiseEnabled && subject && g_disguised.count(subject->GetGUID().GetCounter()) > 0;
    }

    /*
     * The race name in the default locale.
     *
     * The alias is stored in the database and must be byte-identical for every viewer, so it
     * cannot use the per-viewer locale spelling that RaceLabel does.
     */
    std::string CanonicalRace(Player* subject)
    {
        ChrRacesEntry const* race = sChrRacesStore.LookupEntry(subject->getRace());
        if (!race)
            return "Stranger";

        char const* name = race->name[DEFAULT_LOCALE];
        return name && *name ? name : "Stranger";
    }

    /*
     * Allocates this character's permanent alias, or returns the one they already have.
     *
     * Candidates are tried in an order derived from the guid, so a given character tends to keep
     * the same alias even if the table is rebuilt, and two characters rarely want the same one.
     * The UNIQUE index is the real arbiter: INSERT IGNORE simply fails when the name is taken and
     * we move to the next candidate.
     */
    std::string AllocateAlias(Player* player, AliasKind kind = ALIAS_STRANGER)
    {
        ObjectGuid::LowType const guid = player->GetGUID().GetCounter();

        if (QueryResult existing = CharacterDatabase.Query(
                "SELECT `alias` FROM `character_aliases` WHERE `guid` = {} AND `kind` = {}",
                guid, uint32(kind)))
        {
            return (*existing)[0].Get<std::string>();
        }

        std::string const race = CanonicalRace(player);

        // Knuth's multiplicative hash: spreads consecutive guids across the list rather than
        // handing every character created in a row the same first choice. The kind is folded
        // in so a character's disguise never opens on the same adjective as their stranger
        // alias - being "Hooded Orc" both ways would give the disguise away to anyone who had
        // met them before they were introduced.
        uint32 const seed = (guid * 2654435761u) + (uint32(kind) * 1013904223u);

        for (std::size_t attempt = 0; attempt < Adjectives.size(); ++attempt)
        {
            std::string candidate = std::string(Adjectives[(seed + attempt) % Adjectives.size()]) + " " + race;

            std::string escaped = candidate;
            CharacterDatabase.EscapeString(escaped);

            // DirectExecute, not Execute: the SELECT below has to see the result of this insert,
            // and an asynchronous write would not have landed yet.
            CharacterDatabase.DirectExecute(
                "INSERT IGNORE INTO `character_aliases` (`guid`, `kind`, `alias`) VALUES ({}, {}, '{}')",
                guid, uint32(kind), escaped);

            if (QueryResult check = CharacterDatabase.Query(
                    "SELECT `alias` FROM `character_aliases` WHERE `guid` = {} AND `kind` = {}",
                    guid, uint32(kind)))
            {
                return (*check)[0].Get<std::string>();
            }
        }

        // Every adjective for this race is taken, which needs 32 of one race online at once.
        // Fall back to a numbered form rather than leaving the character undisguised.
        for (uint32 suffix = 2; suffix < 10000; ++suffix)
        {
            std::string candidate = std::string(Adjectives[seed % Adjectives.size()]) + " " + race + " " + std::to_string(suffix);

            std::string escaped = candidate;
            CharacterDatabase.EscapeString(escaped);

            CharacterDatabase.DirectExecute(
                "INSERT IGNORE INTO `character_aliases` (`guid`, `kind`, `alias`) VALUES ({}, {}, '{}')",
                guid, uint32(kind), escaped);

            if (QueryResult check = CharacterDatabase.Query(
                    "SELECT `alias` FROM `character_aliases` WHERE `guid` = {} AND `kind` = {}",
                    guid, uint32(kind)))
            {
                return (*check)[0].Get<std::string>();
            }
        }

        LOG_ERROR("module.sanctuaryidentity", "Could not allocate an alias for {}.", player->GetName());
        return race;
    }

    /// The alias for a player who is online. Falls back to their race if allocation ever failed.
    std::string AliasOf(Player* subject, Player* viewer)
    {
        ObjectGuid::LowType const guid = subject->GetGUID().GetCounter();

        // A disguise replaces the stranger alias rather than sitting alongside it, so somebody
        // who met this character before any introduction does not recognise the name they knew
        // them by and see straight through it.
        if (IsWearingDisguise(subject))
        {
            auto worn = g_disguiseAliases.find(guid);
            if (worn != g_disguiseAliases.end() && !worn->second.empty())
                return worn->second;
        }

        auto itr = g_aliases.find(guid);
        if (itr != g_aliases.end() && !itr->second.empty())
            return itr->second;

        return RaceLabel(subject, viewer);
    }

    /*
     * Whether `knower` may be shown `known`'s real name.
     *
     * The game master check lives here rather than in the public KnowsName because this is
     * the one function every decision funnels through - the addon's reply, the chat
     * rewrite, the name-query rewrite, and KnowsName itself. Putting it anywhere else
     * would cover some surfaces and quietly miss others.
     *
     * It is GM *mode*, not account rank: a game master with .gm off meets strangers like
     * anybody else, which is the whole point of the realm and is what they will be playing
     * with most of the time. Turning it on is a deliberate act, and .gm off is what you use
     * to watch without being seen.
     *
     * Only the viewer's mode matters. A game master being *looked at* is disguised like
     * anyone else - reading this the other way round would broadcast their name to the room.
     */
    bool HasBeenIntroduced(Player* knower, Player* known)
    {
        if (g_gameMastersSeeNames && knower->IsGameMaster())
            return true;

        /*
         * A disguise outranks an introduction, and this is the only place it needs saying.
         *
         * Every decision about whether one player may see another's name funnels through here
         * - the name-query rewrite, SMSG_WHO, the group packets, the addon's reply, the voice
         * speaking indicator, the profile module - so answering "no" here hides a disguised
         * character on all of them at once, with nothing to remember per surface.
         *
         * The introduction rows are left untouched, so taking the disguise off restores
         * exactly the people who knew them before rather than making them start again.
         *
         * Below the game master check on purpose: somebody has to be able to tell who is
         * behind a hood, and .gm on is that.
         */
        if (IsWearingDisguise(known))
            return false;

        auto it = g_known.find(knower->GetGUID().GetCounter());
        if (it == g_known.end())
            return false;

        return it->second.count(known->GetGUID().GetCounter()) > 0;
    }

    /*
     * There was a "Q:<name>" verb here that answered with the character's real name.
     *
     * It has been removed rather than fixed, because with the disguise applied server-side it
     * was an enumeration oracle: anyone could probe names, and for any online player they had
     * not been introduced to the reply handed back both the real name and the alias, linking
     * the two. That is precisely what the disguise exists to prevent, and it needed no modified
     * client - the addon channel is ordinary chat.
     *
     * Nothing needs it any more. The addon asks by guid instead, and gets two booleans back.
     */

    /*
     * Drops the client's cached name for a character.
     *
     * The 3.3.5a client caches one name per guid and keeps it for the session, so simply answering
     * a second name query does not correct it - which is why disguising names used to mean an
     * introduction did not take effect until the viewer relogged. SMSG_INVALIDATE_PLAYER is the
     * client's own instruction to forget an entry; the core defines the opcode and never sends it.
     * With the entry gone, the next thing that needs the name asks for it again and gets the truth.
     *
     * This reaches every surface that resolves a name as it draws - tooltip, nameplate, target
     * frame - and they all correct themselves the moment the re-query below is answered.
     *
     * It does not reach the name floating above the character, and cannot. There are two caches
     * and this opcode only clears one: the overhead name is resolved once, when the client
     * creates the unit, and then kept on that object. Nothing the server sends re-reads it short
     * of building the unit again, which means a destroy and a create.
     *
     * That is deliberately not done. Forcing it works - it is exactly what walking out of range
     * and back does - but it costs the viewer a visible pop and, since you are usually looking
     * straight at somebody as they introduce themselves, their target as well. Jarring at the
     * one moment the feature is supposed to feel like meeting someone.
     *
     * So the overhead name is left to settle on its own. The cache already holds the real name
     * by then, so the next ordinary create - walking back into range, a zone change, a relog -
     * picks it up, and every one of those is a moment a create is expected anyway.
     */
    void InvalidateCachedName(Player* viewer, ObjectGuid subject)
    {
        if (!viewer || !viewer->GetSession())
            return;

        WorldPacket data(SMSG_INVALIDATE_PLAYER, 8);
        data << subject;
        viewer->GetSession()->SendPacket(&data);
    }

    /*
     * Re-asks for every name this viewer can see.
     *
     * Flipping GM mode changes the answer to every name query, but the client has already
     * cached the old answers and does not re-ask on its own - so without this a game master
     * would turn GM mode on and still be looking at a room full of aliases until they
     * relogged. Same going the other way: turning it off has to put the aliases back.
     *
     * The overhead name floating above each character is the one surface this cannot fix,
     * for the reason given on InvalidateCachedName: it is resolved once when the client
     * creates the unit. It corrects itself on the next ordinary create - walking out of
     * range and back, a zone change, a relog.
     */
    void RefreshNamesAround(Player* viewer)
    {
        if (!viewer || !viewer->IsInWorld() || !viewer->GetSession())
            return;

        float const range = viewer->GetMap()->GetVisibilityRange();

        std::list<Player*> nearby;
        Acore::AnyPlayerInObjectRangeCheck check(viewer, range, true);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(viewer, nearby, check);
        Cell::VisitObjects(viewer, searcher, range);

        for (Player* subject : nearby)
        {
            if (!subject || subject == viewer)
                continue;

            // Forget, then re-ask. A name query answered while the old entry is still
            // cached is the case the client ignores.
            InvalidateCachedName(viewer, subject->GetGUID());
            viewer->GetSession()->SendNameQueryOpcode(subject->GetGUID());
        }
    }

    /*
     * The transpose of RefreshNamesAround: push one character's changed name to everybody
     * near them, rather than refreshing what one viewer sees of everyone else.
     *
     * Putting a disguise on changes what a single character is called to a whole room, so it
     * is every other client's cache of *them* that has gone stale.
     *
     * Returns how many people were nearby, because the caller has something honest to tell
     * them: this fixes tooltips, nameplates and target frames at once, but not the name
     * floating overhead, which each client resolved when it created the unit and keeps until
     * it builds it again.
     */
    std::size_t RefreshMeForEveryoneNearby(Player* subject)
    {
        if (!subject || !subject->IsInWorld())
            return 0;

        float const range = subject->GetMap()->GetVisibilityRange();

        std::list<Player*> nearby;
        Acore::AnyPlayerInObjectRangeCheck check(subject, range, true);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(subject, nearby, check);
        Cell::VisitObjects(subject, searcher, range);

        std::size_t reached = 0;

        for (Player* viewer : nearby)
        {
            if (!viewer || viewer == subject || !viewer->GetSession())
                continue;

            InvalidateCachedName(viewer, subject->GetGUID());
            viewer->GetSession()->SendNameQueryOpcode(subject->GetGUID());
            ++reached;
        }

        return reached;
    }

    /*
     * Puts a disguise on, or takes it off.
     *
     * The alias is allocated on first use rather than at login: most characters never wear one,
     * and a second name held for everybody would crowd the shared uniqueness namespace for no
     * reason. Once allocated it is kept, so the same figure comes back every time rather than a
     * new stranger appearing each evening.
     */
    void SetDisguise(Player* player, bool wearing)
    {
        if (!player || !player->GetSession())
            return;

        ObjectGuid::LowType const guid = player->GetGUID().GetCounter();
        ChatHandler handler(player->GetSession());

        if (wearing == IsWearingDisguise(player))
        {
            handler.PSendSysMessage(wearing
                ? "You are already wearing a disguise."
                : "You are not wearing a disguise.");
            return;
        }

        if (wearing)
        {
            auto itr = g_disguiseAliases.find(guid);
            if (itr == g_disguiseAliases.end() || itr->second.empty())
                g_disguiseAliases[guid] = AllocateAlias(player, ALIAS_DISGUISE);

            g_disguised.insert(guid);
            CharacterDatabase.Execute("INSERT IGNORE INTO `character_disguised` (`guid`) VALUES ({})", guid);
        }
        else
        {
            g_disguised.erase(guid);
            CharacterDatabase.Execute("DELETE FROM `character_disguised` WHERE `guid` = {}", guid);
        }

        // Told as well as askable: the button should turn over on the click, not on its
        // next poll.
        Send(player, std::string("D:") + (wearing ? "1" : "0"));

        std::size_t const reached = RefreshMeForEveryoneNearby(player);

        if (wearing)
            handler.PSendSysMessage("You draw up your hood. You are |cffd8b46a{}|r to everyone now, including those who know you.",
                g_disguiseAliases[guid]);
        else
            handler.PSendSysMessage("You lower your hood. Those you have been introduced to know you again.");

        /*
         * Said plainly rather than left to be discovered.
         *
         * The name above a character's head is resolved once by each client when it creates the
         * unit, and nothing the server sends re-reads it. So anybody already stood nearby goes on
         * seeing the old name overhead until they walk out of range or change zone. Every other
         * surface changed a moment ago.
         *
         * Worth a warning rather than silence, because the failure runs the dangerous way: put a
         * hood up in a crowded inn and you would otherwise believe you were hidden from the very
         * people who can still read your name.
         *
         * The number is deliberately withheld. It counts players within visibility range -
         * including ones behind you, and ones you could not otherwise see - so reporting it on
         * demand would make this a radar: toggle twice and you have swept the room. That somebody
         * may still know you is the part that changes what a player does; how many of them is
         * only intelligence.
         */
        if (reached > 0)
            handler.PSendSysMessage("|cffff8800Anyone already near you may have already seen your face before you put on the mask.|r");
    }

    /// The asker gives their own name away. This is the only way a row is ever created.
    void IntroduceTo(Player* introducer, Player* target)
    {
        if (!target || target == introducer)
            return;

        ObjectGuid::LowType knower = target->GetGUID().GetCounter();
        ObjectGuid::LowType known = introducer->GetGUID().GetCounter();

        if (!g_known[knower].insert(known).second)
            return; // Already acquainted; nothing to write or announce.

        CharacterDatabase.Execute(
            "INSERT IGNORE INTO character_introductions (knower_guid, known_guid) VALUES ({}, {})",
            knower, known);

        Send(target, "R:" + introducer->GetName() + ":" + AliasOf(introducer, target) + ":1");

        // Forget, then re-ask. The order matters: a name query answered while the old entry is
        // still cached is the case the client ignores.
        InvalidateCachedName(target, introducer->GetGUID());

        if (target->GetSession())
            target->GetSession()->SendNameQueryOpcode(introducer->GetGUID());
    }

    /*
     * Answers "where do I stand with this character", by guid, in two booleans.
     *
     * Deliberately carries no name in either direction. The addon channel is readable by the
     * client, so answering this with a stranger's real name would reopen exactly the leak the
     * server-side disguise closes - the same mistake the voice module's speaking indicator used
     * to make.
     */
    void AnswerAboutGuid(Player* asker, std::string const& raw)
    {
        uint64 value = 0;

        try
        {
            value = std::stoull(raw, nullptr, 16);
        }
        catch (std::exception const&)
        {
            return;
        }

        ObjectGuid const guid(value);
        if (!guid.IsPlayer())
            return;

        Player* subject = ObjectAccessor::FindConnectedPlayer(guid);
        if (!subject)
            return;

        bool const iKnowThem = !g_enabled || subject == asker || HasBeenIntroduced(asker, subject);
        bool const theyKnowMe = !g_enabled || subject == asker || HasBeenIntroduced(subject, asker);

        Send(asker, "S:" + raw + ":" + (iKnowThem ? "1" : "0") + ":" + (theyKnowMe ? "1" : "0"));
    }

    /*
     * Introductions arrive as a GUID rather than a name.
     *
     * Once the name query is answered with the race, the client genuinely does not know who it is
     * looking at: asking it for the name of a stranger gets "Orc" back, and asking the server to
     * introduce us to "Orc" finds nobody. The GUID is the one handle both ends still agree on.
     *
     * This grants nothing. The row it writes reveals the sender and only the sender, so being able
     * to name any GUID on the realm is the same power as being able to type any name was.
     */
    void IntroduceToGuid(Player* introducer, std::string const& raw)
    {
        uint64 value = 0;

        try
        {
            value = std::stoull(raw, nullptr, 16);
        }
        catch (std::exception const&)
        {
            return; // Not a GUID. Nothing sane to do with it.
        }

        ObjectGuid guid(value);
        if (!guid.IsPlayer())
            return;

        IntroduceTo(introducer, ObjectAccessor::FindConnectedPlayer(guid));
    }

    /// Kept for a name typed by hand, which only works for somebody already known to the player.
    void Introduce(Player* introducer, std::string const& targetName)
    {
        IntroduceTo(introducer, ObjectAccessor::FindPlayerByName(targetName, true));
    }

    /*
     * Answering the name query with the alias is what makes the disguise real rather than cosmetic.
     *
     * The client draws the name over a character's head itself, and that drawing is unconditional:
     * the thirteen UnitName* options cover pets, totems, guild tags and titles, but none of them
     * governs it. No addon can reach that text and no CVar turns it off, so the only way it can
     * read "Hooded Orc" is for "Hooded Orc" to be the name the client was given in the first place.
     *
     * Doing it here rather than in an addon also covers every other surface fed by the name cache
     * at once, which is most of them, and puts the decision somewhere a modified client cannot
     * argue with it.
     *
     * The two costs this used to carry are both dealt with elsewhere in this file: introductions
     * land immediately via SMSG_INVALIDATE_PLAYER, and unique aliases keep two strangers of one
     * race distinguishable so the voice speaking indicator can still tell which plate is talking.
     */

    /// Set while sending our replacement, so the send hook does not swallow its own packet.
    thread_local bool g_replacing = false;

    void SendDisguisedName(WorldSession* session, Player* subject, std::string const& label)
    {
        WorldPackets::Query::NameQueryResponse response;
        response.Guid = subject->GetGUID().WriteAsPacked();
        response.NameUnknown = false;
        response.Name = label;
        response.Race = subject->getRace();
        response.Sex = subject->getGender();
        response.Class = subject->getClass();
        response.Declined = false;

        g_replacing = true;
        session->SendPacket(response.Write());
        g_replacing = false;
    }

    bool WithinBudget(Player* player)
    {
        QueryBudget& budget = g_budgets[player->GetGUID().GetCounter()];
        time_t now = time(nullptr);

        if (budget.Window != now)
        {
            budget.Window = now;
            budget.Used = 0;
        }

        return ++budget.Used <= g_maxQueriesPerSecond;
    }

    // --- the other packets that carry a name --------------------------------

    /*
     * The name cache is most of the disguise but not all of it. These packets write a player's
     * name directly rather than letting the client look it up, so each one is a way around it -
     * /who most of all, since it enumerates the whole realm in a single request.
     *
     * Each is parsed, rewritten and re-sent. On any parse failure the packet is dropped rather
     * than passed through: these layouts come from the core and should never fail to parse, so a
     * failure is a bug worth seeing, and quietly leaking every name is the worse outcome of the
     * two.
     */

    /// What this viewer should be shown in place of a name written into a packet.
    std::string LabelForName(Player* viewer, std::string const& name)
    {
        if (name.empty())
            return name;

        Player* subject = ObjectAccessor::FindPlayerByName(name, true);
        if (!subject || subject == viewer || HasBeenIntroduced(viewer, subject))
            return name;

        return AliasOf(subject, viewer);
    }

    void SendReplacement(WorldSession* session, WorldPacket& packet)
    {
        g_replacing = true;
        session->SendPacket(&packet);
        g_replacing = false;
    }

    /*
     * Proves this parser agrees with the core's writer.
     *
     * Each rewrite builds a second copy using the original names. That copy must come out
     * byte-identical to the packet we were handed; if it does not, the layout is not what this
     * code thinks it is, and the *real* replacement is therefore garbage that would desync the
     * client rather than merely show a wrong name.
     *
     * Cheap enough to run on every packet - these are rare and small - and it turns the one
     * failure mode that is genuinely hard to debug into a log line naming the opcode.
     */
    void AssertStructure(WorldPacket const& rebuilt, WorldPacket const& original)
    {
        if (rebuilt.size() != original.size() ||
            (original.size() > 0 && std::memcmp(rebuilt.contents(), original.contents(), original.size()) != 0))
        {
            throw std::runtime_error("rebuilding with the original names did not reproduce the packet");
        }
    }

    /// SMSG_WHO: two counts, then one flat record per displayed player.
    bool RewriteWho(WorldSession* session, Player* viewer, WorldPacket const& packet)
    {
        WorldPacket copy(packet);
        copy.rpos(0);

        uint32 displayCount = 0;
        uint32 matchCount = 0;
        copy >> displayCount >> matchCount;

        WorldPacket out(SMSG_WHO, packet.size());
        WorldPacket verify(SMSG_WHO, packet.size());
        out << displayCount << matchCount;
        verify << displayCount << matchCount;

        for (uint32 i = 0; i < displayCount; ++i)
        {
            std::string name, guild;
            uint32 level, playerClass, race, zone;
            uint8 gender;

            copy >> name >> guild >> level >> playerClass >> race >> gender >> zone;

            // The guild is left alone. It narrows a stranger down but it is not their name,
            // and blanking it would break guild recruitment for no anonymity gained.
            out << LabelForName(viewer, name) << guild << level << playerClass << race << gender << zone;
            verify << name << guild << level << playerClass << race << gender << zone;
        }

        AssertStructure(verify, packet);

        SendReplacement(session, out);
        return true;
    }

    /// SMSG_GROUP_INVITE: a flag, the inviter's name, and three fields nothing reads.
    bool RewriteGroupInvite(WorldSession* session, Player* viewer, WorldPacket const& packet)
    {
        WorldPacket copy(packet);
        copy.rpos(0);

        uint8 flag = 0;
        std::string name;
        uint32 unk1 = 0;
        uint8 count = 0;
        uint32 unk2 = 0;

        copy >> flag >> name >> unk1 >> count >> unk2;

        WorldPacket out(SMSG_GROUP_INVITE, packet.size());
        out << flag << LabelForName(viewer, name) << unk1 << count << unk2;

        WorldPacket verify(SMSG_GROUP_INVITE, packet.size());
        verify << flag << name << unk1 << count << unk2;
        AssertStructure(verify, packet);

        SendReplacement(session, out);
        return true;
    }

    /*
     * SMSG_GROUP_LIST: the party and raid frames.
     *
     * The most delicate of the three. The member block is flat, but it sits between a header
     * whose length depends on whether this is an LFG group and a trailer that is only present
     * when the group has other members, so both conditions have to be reproduced exactly. A
     * mistake here does not show a wrong name, it desyncs the client.
     */
    bool RewriteGroupList(WorldSession* session, Player* viewer, WorldPacket const& packet)
    {
        WorldPacket copy(packet);
        copy.rpos(0);

        uint8 groupType, subGroup, flags, roles;
        copy >> groupType >> subGroup >> flags >> roles;

        WorldPacket out(SMSG_GROUP_LIST, packet.size());
        WorldPacket verify(SMSG_GROUP_LIST, packet.size());
        out << groupType << subGroup << flags << roles;
        verify << groupType << subGroup << flags << roles;

        // GROUPTYPE_LFG is 0x08, and adds two fields to the header.
        if (groupType & 0x08)
        {
            uint8 lfgState = 0;
            uint32 lfgDungeon = 0;
            copy >> lfgState >> lfgDungeon;
            out << lfgState << lfgDungeon;
            verify << lfgState << lfgDungeon;
        }

        uint64 groupGuid = 0;
        uint32 counter = 0;
        uint32 memberCount = 0;
        copy >> groupGuid >> counter >> memberCount;
        out << groupGuid << counter << memberCount;
        verify << groupGuid << counter << memberCount;

        for (uint32 i = 0; i < memberCount; ++i)
        {
            std::string name;
            uint64 guid = 0;
            uint8 onlineState, subgroup, memberFlags, memberRoles;

            copy >> name >> guid >> onlineState >> subgroup >> memberFlags >> memberRoles;

            out << LabelForName(viewer, name) << guid << onlineState << subgroup << memberFlags << memberRoles;
            verify << name << guid << onlineState << subgroup << memberFlags << memberRoles;
        }

        uint64 leaderGuid = 0;
        copy >> leaderGuid;
        out << leaderGuid;
        verify << leaderGuid;

        if (memberCount)
        {
            uint8 lootMethod = 0;
            uint64 looterGuid = 0;
            uint8 lootThreshold, dungeonDifficulty, raidDifficulty, dynamicDifficulty;

            copy >> lootMethod >> looterGuid >> lootThreshold
                 >> dungeonDifficulty >> raidDifficulty >> dynamicDifficulty;

            out << lootMethod << looterGuid << lootThreshold
                << dungeonDifficulty << raidDifficulty << dynamicDifficulty;
            verify << lootMethod << looterGuid << lootThreshold
                << dungeonDifficulty << raidDifficulty << dynamicDifficulty;
        }

        AssertStructure(verify, packet);

        SendReplacement(session, out);
        return true;
    }
}

// --- the bit other modules call ---------------------------------------------

namespace SanctuaryIdentity
{
    bool IsDisguising()
    {
        return g_enabled && g_disguiseNames;
    }

    bool KnowsName(Player* viewer, Player* subject)
    {
        if (!viewer || !subject)
            return false;

        if (viewer == subject || !g_enabled)
            return true;

        return HasBeenIntroduced(viewer, subject);
    }

    std::string LabelFor(Player* viewer, Player* subject)
    {
        if (!subject)
            return {};

        return KnowsName(viewer, subject) ? subject->GetName() : AliasOf(subject, viewer);
    }
}

class sanctuary_identity_playerscript : public PlayerScript
{
public:
    sanctuary_identity_playerscript() : PlayerScript("sanctuary_identity_playerscript") { }

    void OnPlayerLogin(Player* player) override
    {
        // Seeded here so the first update tick does not read a default-constructed false as
        // a toggle and sweep for a game master who logged in already in GM mode.
        g_gmMode[player->GetGUID().GetCounter()] = player->IsGameMaster();

        ObjectGuid::LowType guid = player->GetGUID().GetCounter();

        // Allocated once and cached here, so the disguise - which runs on every name query -
        // never touches the database.
        g_aliases[guid] = AllocateAlias(player);

        /*
         * A disguise outlives the session that put it on: log out hooded and you come back
         * hooded, rather than walking into the room you were hiding in under your own name.
         *
         * The alias is only read back, never allocated here - a character who has never worn
         * one has no row, and minting it at every login would fill the shared namespace with
         * names nobody uses.
         */
        g_disguised.erase(guid);
        g_disguiseAliases.erase(guid);

        if (CharacterDatabase.Query("SELECT 1 FROM `character_disguised` WHERE `guid` = {}", guid))
        {
            if (QueryResult worn = CharacterDatabase.Query(
                    "SELECT `alias` FROM `character_aliases` WHERE `guid` = {} AND `kind` = {}",
                    guid, uint32(ALIAS_DISGUISE)))
            {
                g_disguiseAliases[guid] = (*worn)[0].Get<std::string>();
                g_disguised.insert(guid);
            }
        }

        std::unordered_set<ObjectGuid::LowType>& known = g_known[guid];
        known.clear();

        QueryResult result = CharacterDatabase.Query(
            "SELECT known_guid FROM character_introductions WHERE knower_guid = {}", guid);

        if (result)
        {
            do
            {
                known.insert((*result)[0].Get<uint32>());
            } while (result->NextRow());
        }
    }

    /*
     * Notices GM mode being switched.
     *
     * The core has no script hook on Player::SetGameMaster, so this is polled. It compares
     * one bit against a stored bool and does nothing the rest of the time; the sweep only
     * runs on the tick the flag actually changes.
     */
    void OnPlayerUpdate(Player* player, uint32 /*diff*/) override
    {
        if (!g_enabled || !g_gameMastersSeeNames || !player)
            return;

        bool const on = player->IsGameMaster();
        bool& last = g_gmMode[player->GetGUID().GetCounter()];

        if (on == last)
            return;

        last = on;
        RefreshNamesAround(player);

        ChatHandler(player->GetSession()).PSendSysMessage(on
            ? "|cffd8b46aIdentity:|r game master mode - you now see real names."
            : "|cffd8b46aIdentity:|r game master mode off - strangers read as aliases again.");
    }

    void OnPlayerLogout(Player* player) override
    {
        ObjectGuid::LowType guid = player->GetGUID().GetCounter();
        g_known.erase(guid);
        g_budgets.erase(guid);
        g_aliases.erase(guid);
        g_gmMode.erase(guid);
        g_disguised.erase(guid);
        g_disguiseAliases.erase(guid);
    }

    //[[ Addon traffic arrives as a whisper the player sends to themselves. ]]
    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg, Player* /*receiver*/) override
    {
        if (lang != LANG_ADDON || !player)
            return true;

        std::string const marker = g_prefix + "\t";
        if (msg.rfind(marker, 0) != 0)
            return true;

        std::string body = msg.substr(marker.size());
        if (body.size() < 3 || body[1] != ':')
            return false; // Ours, but malformed. Swallow it rather than let it show as a whisper.

        char kind = body[0];
        std::string argument = body.substr(2);

        if (kind == 'A')
        {
            // By guid, and answered with booleans only. Once names are disguised the client has
            // no name to ask with, and handing one back would undo the disguise.
            if (WithinBudget(player))
                AnswerAboutGuid(player, argument);
        }
        else if (kind == 'G')
        {
            IntroduceToGuid(player, argument);
        }
        else if (kind == 'I')
        {
            Introduce(player, argument);
        }
        else if (kind == 'D')
        {
            /*
             * The addon asks; it is never only told.
             *
             * A push at login lands while the client is still on the loading screen, and
             * after a /reload the addon has forgotten everything and no push is coming. So
             * the minimap button asks on entering the world and keeps asking until answered,
             * and this is what answers it. The argument is ignored - asking is the whole
             * message.
             */
            Send(player, std::string("D:") + (IsWearingDisguise(player) ? "1" : "0"));
        }

        // Never let our own traffic surface in the chat window.
        return false;
    }
};

/// Rewrites the name the client is told, for players it has not been introduced to.
class sanctuary_identity_serverscript : public ServerScript
{
public:
    sanctuary_identity_serverscript() : ServerScript("sanctuary_identity_serverscript") { }

    bool CanPacketSend(WorldSession* session, WorldPacket const& packet) override
    {
        if (!g_enabled || !g_disguiseNames || g_replacing || !session)
            return true;

        uint16 const opcode = packet.GetOpcode();

        if (opcode != SMSG_NAME_QUERY_RESPONSE &&
            opcode != SMSG_WHO &&
            opcode != SMSG_GROUP_INVITE &&
            opcode != SMSG_GROUP_LIST)
            return true;

        Player* viewer = session->GetPlayer();
        if (!viewer)
            return true;

        try
        {
            switch (opcode)
            {
                case SMSG_NAME_QUERY_RESPONSE:
                    return !RewriteNameQuery(session, viewer, packet);
                case SMSG_WHO:
                    return !RewriteWho(session, viewer, packet);
                case SMSG_GROUP_INVITE:
                    return !RewriteGroupInvite(session, viewer, packet);
                case SMSG_GROUP_LIST:
                    return !RewriteGroupList(session, viewer, packet);
                default:
                    return true;
            }
        }
        catch (std::exception const& e)
        {
            // These layouts come from the core and should never fail to parse, so this is a bug
            // worth seeing. Drop rather than forward: passing the original through would leak
            // every name in it, which is the one thing this module exists to prevent.
            g_replacing = false;

            LOG_ERROR("module.sanctuaryidentity",
                "Could not rewrite opcode {}; dropping it rather than leaking names. ({})",
                opcode, e.what());

            return false;
        }
    }

private:
    /// Returns true when the packet was replaced.
    static bool RewriteNameQuery(WorldSession* session, Player* viewer, WorldPacket const& packet)
    {
        // The hook hands out a const packet, so read the guid back off a copy of it.
        WorldPacket copy(packet);
        copy.rpos(0);

        ObjectGuid guid;
        copy >> guid.ReadAsPacked();

        if (!guid.IsPlayer())
            return false;

        // Only players standing in the world are disguised. Everyone you can see is one of those,
        // and leaving offline lookups alone keeps mail, ignore lists and the like working normally.
        Player* subject = ObjectAccessor::FindConnectedPlayer(guid);
        if (!subject || subject == viewer || HasBeenIntroduced(viewer, subject))
            return false;

        SendDisguisedName(session, subject, AliasOf(subject, viewer));
        return true;
    }
};

/// `.disguise` - become a stranger again, even to people who know you.
class sanctuary_identity_commandscript : public CommandScript
{
public:
    sanctuary_identity_commandscript() : CommandScript("sanctuary_identity_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable disguiseCommandTable =
        {
            { "on",     HandleDisguiseOnCommand,     RBAC_PERM_COMMAND_DISGUISE, Console::No },
            { "off",    HandleDisguiseOffCommand,    RBAC_PERM_COMMAND_DISGUISE, Console::No },
            { "status", HandleDisguiseStatusCommand, RBAC_PERM_COMMAND_DISGUISE, Console::No },
            // Bare `.disguise` toggles, because that is the verb people will reach for in the
            // middle of a scene. `status` is there for when you want to check without changing.
            { "",       HandleDisguiseToggleCommand, RBAC_PERM_COMMAND_DISGUISE, Console::No }
        };

        static ChatCommandTable commandTable = { { "disguise", disguiseCommandTable } };
        return commandTable;
    }

    static bool HandleDisguiseOnCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        SetDisguise(player, true);
        return true;
    }

    static bool HandleDisguiseOffCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        SetDisguise(player, false);
        return true;
    }

    static bool HandleDisguiseToggleCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        SetDisguise(player, !IsWearingDisguise(player));
        return true;
    }

    static bool HandleDisguiseStatusCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        if (!g_disguiseEnabled)
        {
            handler->PSendSysMessage("Disguises are switched off on this realm.");
            return true;
        }

        if (!IsWearingDisguise(player))
        {
            handler->PSendSysMessage("You are not wearing a disguise. People you have been introduced to know you.");
            return true;
        }

        handler->PSendSysMessage("You are disguised as |cffd8b46a{}|r.",
            g_disguiseAliases[player->GetGUID().GetCounter()]);

        return true;
    }
};

class sanctuary_identity_worldscript : public WorldScript
{
public:
    sanctuary_identity_worldscript() : WorldScript("sanctuary_identity_worldscript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>("SanctuaryIdentity.Enable", true);
        g_disguiseNames = sConfigMgr->GetOption<bool>("SanctuaryIdentity.DisguiseNames", true);
        g_prefix = sConfigMgr->GetOption<std::string>("SanctuaryIdentity.Addon.Prefix", "SVID");
        g_maxQueriesPerSecond = sConfigMgr->GetOption<uint32>("SanctuaryIdentity.MaxQueriesPerSecond", 20);
        g_gameMastersSeeNames = sConfigMgr->GetOption<bool>("SanctuaryIdentity.GameMastersSeeNames", true);
        g_disguiseEnabled = sConfigMgr->GetOption<bool>("SanctuaryIdentity.Disguise.Enable", true);

        LOG_INFO("module.sanctuaryidentity", "Sanctuary identity {}: strangers read as {}.",
            g_enabled ? "enabled" : "disabled",
            g_disguiseNames ? "an alias, server-side" : "themselves (DisguiseNames is off)");
    }
};

void AddSC_sanctuary_identity_scripts()
{
    new sanctuary_identity_playerscript();
    new sanctuary_identity_serverscript();
    new sanctuary_identity_commandscript();
    new sanctuary_identity_worldscript();
}
