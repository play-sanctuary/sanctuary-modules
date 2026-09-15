/*
 * mod-sanctuary-lawman
 *
 * The other half of outlawry. A game master appoints a character to the office - Guard on
 * the Alliance side, Grunt on the Horde - and while on duty they wear their city's tabard
 * and carry a Writ of Accusation.
 *
 * The writ is the whole point of the module. Outlawry only ever covered people who
 * declared themselves, so a criminal could switch the mode off and become untouchable
 * whatever they had just done. Using the writ on someone *makes them an outlaw* for a
 * fixed sentence, and because the sentence has an expiry the outlaw module refuses to let
 * them stand down from it early.
 *
 * **A lawman gets no combat flags at all**, and that is deliberate, not an omission.
 * Anyone can already strike an outlaw: the outlaw carries faction 14, so every client
 * renders them hostile and `target->IsPvP()` passes them server-side. Withholding the
 * faction override and UNIT_FLAG2_IGNORE_REPUTATION is the entire reason a lawman does not
 * have the guards of their own city turn on them, which is what separates this from
 * mod-sanctuary-outlaw.
 */

#include "SanctuaryLawman.h"

#include "SanctuaryIdentity.h"
#include "SanctuaryOutlaw.h"
// Somebody unconscious holds still to be searched for the same reason somebody in irons
// does, so the two states are asked about together.
#include "SanctuaryDowned.h"

#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemScript.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <sstream>
#include <unordered_map>

namespace
{
    bool g_enabled = true;
    uint32 g_allianceTabard = 45574;   ///> Stormwind Tabard
    uint32 g_hordeTabard = 45581;      ///> Orgrimmar Tabard
    uint32 g_writEntry = 990000;
    uint32 g_pardonEntry = 990003;
    uint32 g_shacklesEntry = 990001;
    uint32 g_keyEntry = 990002;
    uint32 g_searchEntry = 990005;
    float g_searchRange = 5.0f;
    uint32 g_accusationMinutes = 30;
    uint32 g_cooldownSeconds = 60;
    float g_accusationRange = 30.0f;
    std::string g_allianceTitle = "Guard";
    std::string g_hordeTitle = "Grunt";
    std::string g_prefix = "SLAW";

    struct Lawman
    {
        time_t granted = 0;
        bool onDuty = false;
        time_t lastAccusation = 0;
        uint32 sinceCheckMs = 0;   ///> throttles the once-a-second tabard re-assert
    };

    /// Online lawmen only; the table is the durable copy.
    std::unordered_map<ObjectGuid::LowType, Lawman> g_lawmen;

    time_t Now() { return GameTime::GetGameTime().count(); }

    Lawman* Find(Player const* player)
    {
        if (!g_enabled || !player || g_lawmen.empty())
            return nullptr;

        auto it = g_lawmen.find(player->GetGUID().GetCounter());
        return it == g_lawmen.end() ? nullptr : &it->second;
    }

    /*
     * The racial side, not the current one.
     *
     * GetTeamId(true) goes through the character's race rather than m_team, so a faction
     * override - which is exactly what an outlaw is walking around with - cannot flip a
     * lawman's title from Guard to Grunt and back.
     */
    bool IsHorde(Player const* player)
    {
        return player->GetTeamId(true) == TEAM_HORDE;
    }

    uint32 TabardFor(Player const* player)
    {
        return IsHorde(player) ? g_hordeTabard : g_allianceTabard;
    }

    /*
     * Player::SetVisibleItemSlot takes an Item*, so it cannot express a tabard the player
     * does not own. The field is written directly instead. It is PUBLIC, so everyone
     * nearby sees it - and it carries no text, which is why it is the right marker: a
     * name-based one would be a second way to leak identities.
     */
    void ShowTabard(Player* player, uint32 entry)
    {
        player->SetUInt32Value(PLAYER_VISIBLE_ITEM_1_ENTRYID + (EQUIPMENT_SLOT_TABARD * 2), entry);
    }

    /// Puts back whatever they are actually wearing, which may be nothing.
    void RestoreTabard(Player* player)
    {
        player->SetVisibleItemSlot(EQUIPMENT_SLOT_TABARD,
            player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TABARD));
    }

    void SendAddonPacket(Player* player, std::string const& payload)
    {
        if (!player || !player->GetSession())
            return;

        // 3.3.5a carries addon traffic as "PREFIX	BODY" inside a whisper to self.
        std::string message = g_prefix + "	" + payload;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, message);
        player->GetSession()->SendPacket(&data);
    }

    void Store(ObjectGuid::LowType guid, Lawman const& entry)
    {
        CharacterDatabase.Execute(
            "REPLACE INTO `sanctuary_lawman` (`guid`, `granted`, `on_duty`) VALUES ({}, {}, {})",
            guid, uint32(entry.granted), entry.onDuty ? 1 : 0);
    }

    void Forget(ObjectGuid::LowType guid)
    {
        CharacterDatabase.Execute("DELETE FROM `sanctuary_lawman` WHERE `guid` = {}", guid);
    }

}

namespace SanctuaryLawman
{
    bool IsEnabled() { return g_enabled; }

    bool IsLawman(Player const* player) { return Find(player) != nullptr; }

    bool IsOnDuty(Player const* player)
    {
        Lawman const* entry = Find(player);
        return entry && entry->onDuty;
    }

    std::string TitleOf(Player const* player)
    {
        if (!IsLawman(player))
            return {};

        return IsHorde(player) ? g_hordeTitle : g_allianceTitle;
    }

    Result Appoint(Player* player)
    {
        if (!g_enabled)
            return Result::Disabled;

        if (!player)
            return Result::NoPlayer;

        ObjectGuid::LowType guid = player->GetGUID().GetCounter();

        if (g_lawmen.find(guid) != g_lawmen.end())
            return Result::AlreadyLawman;

        Lawman& entry = g_lawmen[guid];
        entry.granted = Now();
        entry.onDuty = false;

        Store(guid, entry);
        SendState(player);

        return Result::Ok;
    }

    Result Dismiss(Player* player)
    {
        if (!g_enabled)
            return Result::Disabled;

        if (!player)
            return Result::NoPlayer;

        ObjectGuid::LowType guid = player->GetGUID().GetCounter();

        if (g_lawmen.find(guid) == g_lawmen.end())
            return Result::NotLawman;

        // Erased before the tabard is restored, so the re-assert hook does not put it back.
        g_lawmen.erase(guid);
        Forget(guid);

        // The kit is not confiscated. It is theirs the way anything else that can be
        // handed over is theirs, and a dismissed lawman walking off with the irons is a
        // better story than them evaporating.
        RestoreTabard(player);
        SendState(player);

        return Result::Ok;
    }

    Result StartDuty(Player* player)
    {
        if (!g_enabled)
            return Result::Disabled;

        if (!player)
            return Result::NoPlayer;

        Lawman* entry = Find(player);

        if (!entry)
            return Result::NotLawman;

        if (entry->onDuty)
            return Result::AlreadyOnDuty;

        entry->onDuty = true;

        ShowTabard(player, TabardFor(player));
        Store(player->GetGUID().GetCounter(), *entry);
        SendState(player);

        return Result::Ok;
    }

    Result EndDuty(Player* player)
    {
        if (!g_enabled)
            return Result::Disabled;

        if (!player)
            return Result::NoPlayer;

        Lawman* entry = Find(player);

        if (!entry)
            return Result::NotLawman;

        if (!entry->onDuty)
            return Result::AlreadyOffDuty;

        // Cleared first: RestoreTabard goes through SetVisibleItemSlot, which fires the
        // hook that would otherwise put the office tabard straight back on.
        entry->onDuty = false;

        RestoreTabard(player);
        Store(player->GetGUID().GetCounter(), *entry);
        SendState(player);

        return Result::Ok;
    }

    std::string Describe(Player const* player)
    {
        if (!g_enabled)
            return "The watch is not keeping order on this realm.";

        if (!player)
            return "No character.";

        Lawman const* entry = Find(player);

        if (!entry)
            return "You hold no office.";

        std::string title = TitleOf(player);

        if (!entry->onDuty)
            return "You are a " + title + ", off duty. Nobody can tell by looking.";

        return "You are a " + title + " on duty. Your writ is good against anyone who "
               "deserves it.";
    }

    void SendState(Player* player)
    {
        if (!player)
            return;

        Lawman const* entry = Find(player);

        std::ostringstream payload;
        payload << "STATE"
                << " lawman=" << (entry ? 1 : 0)
                << " duty=" << (entry && entry->onDuty ? 1 : 0)
                << " title=" << (entry ? TitleOf(player) : std::string("-"));

        SendAddonPacket(player, payload.str());
    }
}

/*
 * The Writ of Accusation.
 *
 * Bound to item_template.ScriptName, so there is nothing to register. Returning true
 * suppresses the item's own spell entirely - that spell exists only to make the client
 * offer a targeting cursor, and its name and visual are never seen.
 *
 * Because OnItemUse fires before CastItemUseSpell, returning true also skips
 * Spell::CheckCast and every validation inside it. Range and line of sight are therefore
 * checked here; without that a modified client could accuse someone across the zone.
 */
class sanctuary_lawman_writ : public ItemScript
{
public:
    sanctuary_lawman_writ() : ItemScript("sanctuary_lawman_writ") { }

    bool OnUse(Player* player, Item* item, SpellCastTargets const& targets) override
    {
        if (!player || !player->GetSession())
            return true;

        auto refuse = [&](std::string const& why)
        {
            // Without an error the client leaves the item greyed out and the player reads
            // it as a cooldown rather than a refusal.
            player->SendEquipError(EQUIP_ERR_CANT_DO_RIGHT_NOW, item, nullptr);
            ChatHandler(player->GetSession()).PSendSysMessage("{}", why);
            return true;
        };

        if (!g_enabled)
            return refuse("The watch is not keeping order on this realm.");

        Lawman* entry = Find(player);

        if (!entry)
            return refuse("This writ is not yours to serve.");

        if (!entry->onDuty)
            return refuse("Not while you are off duty.");

        if (g_cooldownSeconds && Now() < entry->lastAccusation + time_t(g_cooldownSeconds))
        {
            time_t left = entry->lastAccusation + time_t(g_cooldownSeconds) - Now();
            return refuse("You have only just served one. Wait " + std::to_string(left) + " second(s).");
        }

        // Selection first, falling back to whatever the packet carried: the base spell has
        // an empty target mask in the client's files, so what arrives is not dependable.
        // Reading the packet alone is why this refused every accusation ever served -
        // a self-cast item sends no unit target, however carefully you have aimed it.
        Unit* unit = targets.GetUnitTarget();
        Player* accused = player->GetSelectedPlayer();

        if (!accused && unit)
            accused = unit->ToPlayer();

        if (!accused)
            return refuse("Select the person you mean to accuse.");

        if (accused == player)
            return refuse("You cannot accuse yourself.");

        if (accused->IsGameMaster())
            return refuse("That one is beyond your authority.");

        if (SanctuaryOutlaw::IsOutlaw(accused))
            return refuse("They are already an outlaw.");

        if (!player->IsWithinDistInMap(accused, g_accusationRange))
            return refuse("Too far away to be heard.");

        if (!player->IsWithinLOSInMap(accused))
            return refuse("You cannot see them.");

        SanctuaryOutlaw::Result declared = SanctuaryOutlaw::Flag(accused, g_accusationMinutes);

        if (declared != SanctuaryOutlaw::Result::Ok)
            return refuse("The accusation did not stick.");

        entry->lastAccusation = Now();

        std::string const title = SanctuaryLawman::TitleOf(player);

        // Labels, never names, and resolved separately for each side: handing either of
        // them the other's real name would make the writ an identity oracle.
        ChatHandler(player->GetSession()).PSendSysMessage(
            "You accuse |cffff4040{}|r. They are an outlaw for {} minute(s).",
            SanctuaryIdentity::LabelFor(player, accused), g_accusationMinutes);

        if (accused->GetSession())
            ChatHandler(accused->GetSession()).PSendSysMessage(
                "|cffff4040{} {} has accused you.|r You are an outlaw for {} minute(s), and cannot "
                "stand down until it is served.",
                title, SanctuaryIdentity::LabelFor(accused, player), g_accusationMinutes);

        LOG_INFO("module.sanctuarylawman", "{} ({}) accused {} for {} minutes.",
            player->GetName(), title, accused->GetName(), g_accusationMinutes);

        return true;
    }
};

/*
 * The Writ of Pardon.
 *
 * The same shape as the accusation, in reverse, and it is the ONLY way to grant a pardon:
 * `.lawman pardon` was removed in favour of it. Letting somebody go should cost a piece of
 * paper that somebody had to be given, and happen where it can be seen.
 */
class sanctuary_lawman_pardon : public ItemScript
{
public:
    sanctuary_lawman_pardon() : ItemScript("sanctuary_lawman_pardon") { }

    bool OnUse(Player* player, Item* item, SpellCastTargets const& targets) override
    {
        if (!player || !player->GetSession())
            return true;

        auto refuse = [&](std::string const& why)
        {
            player->SendEquipError(EQUIP_ERR_CANT_DO_RIGHT_NOW, item, nullptr);
            ChatHandler(player->GetSession()).PSendSysMessage("{}", why);
            return true;
        };

        if (!g_enabled)
            return refuse("The watch is not keeping order on this realm.");

        Lawman* entry = Find(player);

        if (!entry)
            return refuse("This writ is not yours to serve.");

        if (!entry->onDuty)
            return refuse("Not while you are off duty.");

        // Selection first, falling back to whatever the packet carried: the base spell has
        // an empty target mask in the client's files, so what arrives is not dependable.
        Unit* unit = targets.GetUnitTarget();
        Player* subject = player->GetSelectedPlayer();

        if (!subject && unit)
            subject = unit->ToPlayer();

        if (!subject)
            return refuse("Select the person you mean to pardon.");

        if (!SanctuaryOutlaw::IsOutlaw(subject))
            return refuse("They are not an outlaw.");

        if (!player->IsWithinDistInMap(subject, g_accusationRange))
            return refuse("Too far away to be heard.");

        // A reason, so the pardoned player is not told a game master did this.
        if (SanctuaryOutlaw::Revoke(subject, "; the watch has let you go") != SanctuaryOutlaw::Result::Ok)
            return refuse("The pardon did not take.");

        ChatHandler(player->GetSession()).PSendSysMessage(
            "You pardon {}.", SanctuaryIdentity::LabelFor(player, subject));

        LOG_INFO("module.sanctuarylawman", "{} ({}) pardoned {}.",
            player->GetName(), SanctuaryLawman::TitleOf(player), subject->GetName());

        return true;
    }
};

/*
 * The Search Gloves - going through a prisoner's pack.
 *
 * Authority by possession, like the writs and the shackles: the gloves permit the search,
 * not the office. They can be lost, handed on, or taken off a body, and whoever ends up
 * holding them can search anyone in irons. Being shackled is the only thing checked about
 * the person being searched, because that is what makes them hold still for it.
 *
 * They only LOOK, and that is a deliberate limit rather than an unfinished one. A window
 * onto another player's live inventory that also lets you take from it is an item
 * duplicator unless every take is mirrored back as a destroy on the prisoner - who may be
 * trading, drinking or dropping that very item while the window is open. Reading the pack
 * out loud is the whole feature. What changes hands afterwards is a trade, and a matter
 * between the two of them.
 */
namespace
{
    /// One line per stack, as an item link, so the searcher can read the tooltip too.
    void ReportStack(ChatHandler& to, Item const* held, uint32& found)
    {
        ItemTemplate const* proto = held ? held->GetTemplate() : nullptr;

        if (!proto)
            return;

        ++found;

        to.PSendSysMessage("   {}x |c{:08x}|Hitem:{}:0:0:0:0:0:0:0:0:0|h[{}]|h|r",
            held->GetCount(), ItemQualityColors[proto->Quality], proto->ItemId, proto->Name1);
    }

    /*
     * The backpack, then every bag hanging off it, then the purse.
     *
     * Worn gear is deliberately left out: the stock inspect window already shows it to
     * anybody who asks, so listing it here would only be a second, worse copy of something
     * the client does properly.
     */
    void ReadOutPack(Player* searcher, Player* prisoner)
    {
        ChatHandler to(searcher->GetSession());
        uint32 found = 0;

        to.PSendSysMessage("--- you search |cffffffff{}|r ---",
            SanctuaryIdentity::LabelFor(searcher, prisoner));

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            ReportStack(to, prisoner->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), found);

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = prisoner->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    ReportStack(to, bag->GetItemByPos(uint8(slot)), found);

        if (!found)
            to.PSendSysMessage("   Their pack is empty.");

        uint32 const money = prisoner->GetMoney();

        to.PSendSysMessage("   Purse: {}g {}s {}c", money / 10000, (money % 10000) / 100, money % 100);

        // They can feel it. A search nobody notices is a search nobody can object to.
        if (prisoner->GetSession())
            ChatHandler(prisoner->GetSession()).PSendSysMessage(
                "|cffd9a441{} goes through your pack.|r",
                SanctuaryIdentity::LabelFor(prisoner, searcher));

        LOG_INFO("module.sanctuarylawman", "{} searched {} ({} stack(s), {} copper).",
            searcher->GetName(), prisoner->GetName(), found, money);
    }
}

namespace SanctuaryLawman
{
    /*
     * One set of rules, whichever way the search was asked for.
     *
     * There are two ways in now - using the gloves from the pack, and the button the addon
     * puts up when you target somebody who cannot walk away - and they must agree exactly,
     * or the button becomes a way around a rule the item enforces. So both come through
     * here and neither has a check of its own.
     *
     * Carrying the gloves is one of those rules rather than a consequence of how it was
     * asked for: using an item proves possession, clicking a button proves nothing.
     */
    char const* SearchRefusal(Player* searcher, Player* prisoner)
    {
        if (!g_enabled)
            return "The watch is not keeping order on this realm.";

        if (!searcher || !searcher->GetSession())
            return "You cannot do that.";

        if (!prisoner)
            return "Select the person you mean to search.";

        if (prisoner == searcher)
            return "You know what you are carrying.";

        /*
         * Carrying the gloves is NOT required.
         *
         * They were the authority to begin with, in the same way the writs and the irons
         * are, but the state of the person being searched turns out to carry that weight
         * on its own: somebody in irons or on the floor has already been put there by
         * somebody, and going through their pack is the obvious next thing to reach for.
         * Making it wait on an item in the bag meant the offer appeared for almost nobody.
         *
         * The gloves still work as a way to do it - their OnUse comes through here - they
         * are simply no longer the price of admission.
         */

        // Being unconscious holds somebody still exactly as irons do, and it is the more
        // common of the two - most people who get searched were put on the floor first.
        if (!SanctuaryLawman::IsShackled(prisoner) && !SanctuaryDowned::IsDowned(prisoner))
            return "Only somebody in irons or on the floor will hold still to be searched.";

        if (!searcher->IsWithinDistInMap(prisoner, g_searchRange))
            return "Too far away to lay a hand on them.";

        if (!searcher->IsWithinLOSInMap(prisoner))
            return "You cannot see them.";

        return nullptr;
    }

    char const* Search(Player* searcher, Player* prisoner)
    {
        if (char const* why = SearchRefusal(searcher, prisoner))
            return why;

        ReadOutPack(searcher, prisoner);
        return nullptr;
    }
}

class sanctuary_lawman_search : public ItemScript
{
public:
    sanctuary_lawman_search() : ItemScript("sanctuary_lawman_search") { }

    bool OnUse(Player* player, Item* item, SpellCastTargets const& targets) override
    {
        if (!player || !player->GetSession())
            return true;

        // Selection first, falling back to whatever the packet carried - see the Writ of
        // Accusation, which refused every accusation ever served by reading only the packet.
        Unit* unit = targets.GetUnitTarget();
        Player* prisoner = player->GetSelectedPlayer();

        if (!prisoner && unit)
            prisoner = unit->ToPlayer();

        if (char const* why = SanctuaryLawman::Search(player, prisoner))
        {
            // Without an error the client leaves the item greyed out and the player reads
            // it as a cooldown rather than a refusal.
            player->SendEquipError(EQUIP_ERR_CANT_DO_RIGHT_NOW, item, nullptr);
            ChatHandler(player->GetSession()).PSendSysMessage("{}", why);
        }

        return true;
    }
};

class sanctuary_lawman_playerscript : public PlayerScript
{
public:
    sanctuary_lawman_playerscript() : PlayerScript("sanctuary_lawman_playerscript",
        {
            PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_LOGOUT,
            PLAYERHOOK_ON_UPDATE,
            PLAYERHOOK_ON_AFTER_SET_VISIBLE_ITEM_SLOT,
            // Addon traffic is a whisper to self, so it is the private-chat overload.
            PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT
        }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!g_enabled || !player)
            return;

        ObjectGuid::LowType guid = player->GetGUID().GetCounter();

        QueryResult result = CharacterDatabase.Query(
            "SELECT `granted`, `on_duty` FROM `sanctuary_lawman` WHERE `guid` = {}", guid);

        if (!result)
            return;

        Lawman entry;
        entry.granted = time_t((*result)[0].Get<uint32>());
        entry.onDuty = (*result)[1].Get<uint8>() != 0;

        g_lawmen[guid] = entry;

        // LoadFromDB clears every visible item slot before this runs, so an on-duty
        // tabard has to be put back by hand.
        if (entry.onDuty)
            ShowTabard(player, TabardFor(player));

        SanctuaryLawman::SendState(player);

        ChatHandler(player->GetSession()).PSendSysMessage("{}", SanctuaryLawman::Describe(player));
    }

    void OnPlayerLogout(Player* player) override
    {
        if (player)
            g_lawmen.erase(player->GetGUID().GetCounter());
    }

    /*
     * Equipping or removing anything in slot 18 rewrites the field, and so does every
     * login. This catches three of the four writers; login is handled above.
     */
    void OnPlayerAfterSetVisibleItemSlot(Player* player, uint8 slot, Item* /*item*/) override
    {
        if (slot != EQUIPMENT_SLOT_TABARD || !SanctuaryLawman::IsOnDuty(player))
            return;

        ShowTabard(player, TabardFor(player));
    }

    /// Backstop for anything that writes the field without going through the hook.
    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!g_enabled || g_lawmen.empty() || !player)
            return;

        Lawman* entry = Find(player);

        if (!entry || !entry->onDuty)
            return;

        entry->sinceCheckMs += diff;
        if (entry->sinceCheckMs < 1000)
            return;

        entry->sinceCheckMs = 0;

        uint32 const wanted = TabardFor(player);

        if (player->GetUInt32Value(PLAYER_VISIBLE_ITEM_1_ENTRYID + (EQUIPMENT_SLOT_TABARD * 2)) != wanted)
            ShowTabard(player, wanted);
    }

    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg, Player* /*receiver*/) override
    {
        if (lang != LANG_ADDON || !player || !g_enabled)
            return true;

        std::string const marker = g_prefix + "\t";

        // Every registered PlayerScript sees this message and the first false swallows it,
        // so anything that is not ours has to be passed along untouched.
        if (msg.rfind(marker, 0) != 0)
            return true;

        if (msg.compare(marker.size(), 4, "SYNC") == 0)
            SanctuaryLawman::SendState(player);

        /*
         * "May I search whoever I have selected?"
         *
         * The addon used to answer this itself by looking for the shackle and bleed-out
         * auras on the target, which duplicated a server rule on the client and got it
         * wrong for unconscious players - the button simply never appeared for the case it
         * was most wanted in. The server is the only thing that actually knows, so it is
         * asked, and the reply is the same set of checks the search itself runs.
         */
        else if (msg.compare(marker.size(), 9, "CANSEARCH") == 0)
            SendAddonPacket(player, SanctuaryLawman::SearchRefusal(player, player->GetSelectedPlayer())
                ? "SEARCH ok=0" : "SEARCH ok=1");

        return false;
    }
};

class sanctuary_lawman_worldscript : public WorldScript
{
public:
    sanctuary_lawman_worldscript() : WorldScript("sanctuary_lawman_worldscript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    /*
     * Says out loud whether the items this module hands out actually exist.
     *
     * Without it, a missing or unloaded row looks exactly like a bag problem or a client
     * cache from in game: the item simply never appears and nothing anywhere says why.
     * One line at startup turns that into a fact you can read.
     */
    void OnStartup() override
    {
        if (!g_enabled)
            return;

        struct { uint32 entry; char const* what; } const kit[] =
        {
            { g_writEntry,     "Writ of Accusation" },
            { g_pardonEntry,   "Writ of Pardon" },
            { g_shacklesEntry, "Iron Shackles" },
            { g_keyEntry,      "Iron Shackle Key" },
            { g_searchEntry,   "Search Gloves" }
        };

        for (auto const& piece : kit)
        {
            if (!piece.entry)
                continue;

            if (ItemTemplate const* item = sObjectMgr->GetItemTemplate(piece.entry))
                LOG_INFO("module.sanctuarylawman", "{} is item {} \"{}\".",
                    piece.what, piece.entry, item->Name1);
            else
                LOG_ERROR("module.sanctuarylawman",
                    "{} (item {}) is NOT loaded - it cannot be handed out. Has this module's "
                    "world SQL been applied?", piece.what, piece.entry);
        }
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>("SanctuaryLawman.Enable", true);
        g_allianceTabard = sConfigMgr->GetOption<uint32>("SanctuaryLawman.Tabard.Alliance", 45574);
        g_hordeTabard = sConfigMgr->GetOption<uint32>("SanctuaryLawman.Tabard.Horde", 45581);
        g_writEntry = sConfigMgr->GetOption<uint32>("SanctuaryLawman.Writ.Entry", 990000);
        g_pardonEntry = sConfigMgr->GetOption<uint32>("SanctuaryLawman.Writ.PardonEntry", 990003);
        g_shacklesEntry = sConfigMgr->GetOption<uint32>("SanctuaryLawman.Shackles.Entry", 990001);
        g_keyEntry = sConfigMgr->GetOption<uint32>("SanctuaryLawman.Shackles.KeyEntry", 990002);
        g_searchEntry = sConfigMgr->GetOption<uint32>("SanctuaryLawman.Search.Entry", 990005);
        g_searchRange = std::clamp(sConfigMgr->GetOption<float>("SanctuaryLawman.Search.Range", 5.0f), 1.0f, 30.0f);
        g_accusationMinutes = std::max(1u, sConfigMgr->GetOption<uint32>("SanctuaryLawman.Accusation.Minutes", 30));
        g_cooldownSeconds = sConfigMgr->GetOption<uint32>("SanctuaryLawman.Accusation.CooldownSeconds", 60);
        g_accusationRange = std::clamp(sConfigMgr->GetOption<float>("SanctuaryLawman.Accusation.Range", 30.0f), 1.0f, 100.0f);
        g_allianceTitle = sConfigMgr->GetOption<std::string>("SanctuaryLawman.Title.Alliance", "Guard");
        g_hordeTitle = sConfigMgr->GetOption<std::string>("SanctuaryLawman.Title.Horde", "Grunt");
        g_prefix = sConfigMgr->GetOption<std::string>("SanctuaryLawman.Addon.Prefix", "SLAW");

        LOG_INFO("module.sanctuarylawman",
                 "Sanctuary lawmen {}: {} and {}, writ {} declares an outlaw for {} minutes.",
                 g_enabled ? "enabled" : "disabled",
                 g_allianceTitle, g_hordeTitle, g_writEntry, g_accusationMinutes);
    }
};

void AddSC_sanctuary_lawman_scripts()
{
    new sanctuary_lawman_writ();
    new sanctuary_lawman_pardon();
    new sanctuary_lawman_search();
    new sanctuary_lawman_playerscript();
    new sanctuary_lawman_worldscript();
}
