/*
 * mod-sanctuary-faction - what the addon window is told
 *
 * The window shows three things: which faction you are in and at what rank, who else is in
 * it, and what your rank has taught you. All three are facts the client cannot work out on
 * its own, so all three come from here.
 *
 * ACTIONS DO NOT COME THROUGH THIS FILE. The addon's buttons send `.faction promote` and
 * the rest over AzerothCore's addon command channel, which lands in
 * SanctuaryFactionCommands.cpp with every rank check intact. That is deliberate and it is
 * the reason this file only ever answers questions: an addon is a program the player owns
 * and can edit, so a button that could act directly would be a button with no rules on it.
 *
 * The one rule this file DOES enforce is the disguise. A roster is the most natural place
 * on the realm to hand out a name somebody was never introduced to, so every name below
 * goes through mod-sanctuary-identity - and because what each viewer may be told differs,
 * the roster is built per recipient rather than broadcast once to the faction.
 */

#include "SanctuaryFactionInternal.h"

#include "Chat.h"
#include "CharacterCache.h"
#include "Config.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "RBAC.h"
#include "SanctuaryIdentity.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#include <sstream>
#include <string>
#include <unordered_map>

namespace
{
    /*
     * Fixed, not configurable.
     *
     * mod-sanctuary-stash makes its prefix a setting and this one deliberately does not: a
     * prefix is half of a protocol whose other half is a string literal in shipped Lua, so
     * a realm that changed it here would get a window that silently never answers. There is
     * nothing to tune - it is a name both ends have to agree on.
     */
    std::string const g_addonPrefix = "SFACTION";

    /// Who has asked for a padded roster, and how many rows of it. Testing only.
    std::unordered_map<ObjectGuid::LowType, uint32> g_dummies;

    void SendAddonPacket(Player* player, std::string const& payload)
    {
        if (!player || !player->GetSession())
            return;

        // 3.3.5a carries addon traffic as "PREFIX\tBODY" inside a whisper to self.
        std::string message = g_addonPrefix + "\t" + payload;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, message);
        player->GetSession()->SendPacket(&data);
    }

    /*
     * Every message carries its free text LAST and nothing after it.
     *
     * Faction and rank names are typed by a game master and can hold spaces; a player's
     * label can too, once the disguise is generating aliases. Putting the one free field at
     * the end means the addon can read the fixed fields with a pattern and take the rest of
     * the line verbatim, instead of guessing where a name stopped. mod-sanctuary-downed
     * learned this the expensive way with the carried-player name.
     */
    /*
     * Whether this player may run the game master half of `.faction`.
     *
     * Asked with the exact permission the command table gates on, rather than IsGameMaster()
     * or a security level: those three can disagree, and the one that decides whether the
     * button works is this one. A window offering a button the server then refuses is worse
     * than no button.
     */
    bool MayCreate(Player* player)
    {
        return player && player->GetSession()
            && player->GetSession()->HasPermission(rbac::RBAC_PERM_COMMAND_MODIFY_FACTION);
    }

    void SendState(Player* player)
    {
        // Sent before STATE, because STATE is what the window treats as the start of a
        // refresh and it clears what came before it.
        SendAddonPacket(player, std::string("MAYCREATE ") + (MayCreate(player) ? "1" : "0"));

        uint32 const faction = SanctuaryFaction::FactionOf(player);

        if (!faction)
        {
            SendAddonPacket(player, "STATE 0 0 0");
            SendAddonPacket(player, "DONE");
            return;
        }

        SanctuaryFactionStore::Member const* self =
            SanctuaryFactionStore::MemberOf(player->GetGUID().GetCounter());

        SanctuaryFactionStore::Faction const* row = SanctuaryFactionStore::Find(faction);

        if (!self || !row)
        {
            SendAddonPacket(player, "STATE 0 0 0");
            SendAddonPacket(player, "DONE");
            return;
        }

        uint32 permissions = 0;

        auto mine = row->ranks.find(self->rank);

        if (mine != row->ranks.end())
            permissions = mine->second.permissions;

        std::ostringstream state;
        state << "STATE " << faction << " " << uint32(self->rank) << " " << permissions;
        SendAddonPacket(player, state.str());

        SendAddonPacket(player, "FNAME " + row->name);

        // The whole ladder, not just the viewer's rung: the window labels everybody's rank
        // and the promote button has to know what the next one up is called.
        for (auto const& [rankId, rank] : row->ranks)
            SendAddonPacket(player, "RANK " + std::to_string(uint32(rankId)) + " " + rank.name);

        /*
         * What this rank has taught them, by id.
         *
         * Ids rather than names, because the client has the names already - GetSpellInfo
         * reads them out of its own Spell.dbc - and sending a name the server holds for a
         * spell the client draws differently is how the two get to disagree.
         *
         * Cumulative, the same way the grant itself is: every rung at or below theirs.
         */
        for (auto const& [rankId, rank] : row->ranks)
        {
            if (rankId > self->rank)
                continue;

            for (uint32 spell : rank.spells)
                SendAddonPacket(player, "SPELL " + std::to_string(spell));
        }

        SendAddonPacket(player, "DONE");
    }

    void SendRoster(Player* player)
    {
        uint32 const faction = SanctuaryFaction::FactionOf(player);

        if (!faction)
        {
            SendAddonPacket(player, "RSTART");
            SendAddonPacket(player, "RDONE");
            return;
        }

        // Marks the start of a run, so the window empties its list when the answer begins
        // rather than when the question was asked. A request the server never answers then
        // leaves the previous roster on screen instead of blanking it.
        SendAddonPacket(player, "RSTART");

        uint32 absent = 0;

        for (ObjectGuid::LowType guid : SanctuaryFactionStore::Roster(faction))
        {
            SanctuaryFactionStore::Member const* member = SanctuaryFactionStore::MemberOf(guid);

            if (!member)
                continue;

            Player* who = ObjectAccessor::FindConnectedPlayer(ObjectGuid(HighGuid::Player, guid));

            /*
             * Somebody who is not logged in is COUNTED and not NAMED.
             *
             * SanctuaryIdentity::LabelFor needs two live players - it decides between the
             * real name and the alias by looking up an introduction between them - so there
             * is no honest answer for an absent member yet. The dishonest answer is easy and
             * tempting: read the character name out of the cache and send that. It would
             * turn this window into a name lookup for every stranger on the realm, which is
             * the one thing the disguise exists to prevent.
             *
             * The fix, when it is wanted, is a guid-taking overload in the identity module -
             * its introduction and alias tables are both keyed by guid already, so the data
             * is there. Until then the window says how many are away and no more.
             */
            if (!who)
            {
                ++absent;
                continue;
            }

            /*
             * The guid travels with the label, and the buttons act on the guid.
             *
             * A label is not a name: for somebody this viewer has not been introduced to it
             * is an alias, which no character-name lookup can resolve. An addon that sent
             * `.faction kick <label>` back would therefore work on friends and fail on
             * strangers - in the same faction, which is exactly where it would be least
             * expected.
             *
             * A guid is not a name either, so sending it gives the disguise away to nobody.
             */
            std::ostringstream line;
            line << "MEMBER " << uint32(member->rank) << " " << guid << " "
                 << SanctuaryIdentity::LabelFor(player, who);

            SendAddonPacket(player, line.str());
        }

        /*
         * Invented members, after the real ones.
         *
         * Spread across whatever rungs the faction actually has, so the rank column and the
         * "may I act on this row" rules get exercised rather than every row looking the
         * same. They carry guid 0, which resolves to nobody - so pressing Promote on one is
         * refused by the server exactly as it would be for a member who had just logged
         * out, which is itself worth seeing.
         */
        auto padding = g_dummies.find(player->GetGUID().GetCounter());

        if (padding != g_dummies.end() && padding->second)
        {
            SanctuaryFactionStore::Faction const* row = SanctuaryFactionStore::Find(faction);

            uint8 const top = (row && !row->ranks.empty()) ? row->ranks.rbegin()->first : 0;

            for (uint32 i = 1; i <= padding->second; ++i)
            {
                std::ostringstream line;

                // Never the top rung: the rules say you may not act on your equal, and a
                // roster where the first row is refused teaches the wrong lesson first.
                line << "MEMBER " << uint32(top ? (i - 1) % top : 0) << " 0 "
                     << "Dummy " << i;

                SendAddonPacket(player, line.str());
            }
        }

        SendAddonPacket(player, "ABSENT " + std::to_string(absent));
        SendAddonPacket(player, "RDONE");
    }
}

namespace SanctuaryFactionStore
{
    void SetDummyRoster(ObjectGuid::LowType guid, uint32 count)
    {
        if (count)
            g_dummies[guid] = count;
        else
            g_dummies.erase(guid);
    }

    void PushState(Player* player)
    {
        if (player && SanctuaryFaction::IsEnabled())
            SendState(player);
    }
}

class sanctuary_faction_addonscript : public PlayerScript
{
public:
    sanctuary_faction_addonscript() : PlayerScript("sanctuary_faction_addonscript",
        { PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg,
                            Player* /*receiver*/) override
    {
        if (lang != LANG_ADDON || !player || !SanctuaryFaction::IsEnabled())
            return true;

        std::string const marker = g_addonPrefix + "\t";

        // Every registered PlayerScript sees this message and the first false swallows it,
        // so anything that is not ours has to be passed along untouched.
        if (msg.rfind(marker, 0) != 0)
            return true;

        std::istringstream body(msg.substr(marker.size()));
        std::string verb;
        body >> verb;

        if (verb == "HELLO")
        {
            SendState(player);
            return false;
        }

        if (verb == "ROSTER")
        {
            SendRoster(player);
            return false;
        }

        // Ours by prefix but not a verb we know - an older addon, or a newer one. Swallowed
        // rather than passed on, because it is still addressed to this module.
        return false;
    }
};

void AddSC_sanctuary_faction_addonscript()
{
    new sanctuary_faction_addonscript();
}
