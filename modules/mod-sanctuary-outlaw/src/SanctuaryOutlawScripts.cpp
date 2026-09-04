/*
 * mod-sanctuary-outlaw
 *
 * A flag a player puts on themselves to say "I am open to violence" - for a robbery, a
 * duel to the death, or an execution carried out by somebody else's hand.
 *
 * The obvious implementation does not work, and it fails silently, so it is worth
 * spelling out what is going on here.
 *
 * **FFA PvP is mutual.** Unit::_IsValidAttackTarget computes both reactions first and
 * returns false for two friendly players (Unit.cpp:10817) long before it reaches the FFA
 * test at 10869. Setting UNIT_BYTE2_FLAG_FFA_PVP on one player therefore does exactly
 * nothing until the other player sets it too. That is fine for a consensual duel and
 * useless for a robbery.
 *
 * **The client has its own copy of that logic.** _IsValidAttackTarget is, by its own
 * comment, "based on function Unit::CanAttack from 13850 client". A server that says
 * "attackable" while the client says "friendly" produces a target nobody can click, and
 * nothing is logged anywhere. The only lever the client also sees is the faction
 * template, which is why the faction override below exists and is the load-bearing part.
 *
 * So an outlaw gets four things, each answering a different check:
 *
 *   a faction override      - the only one the CLIENT sees, and the load-bearing one.
 *   FFA PvP byte            - the visual, and outlaw-vs-outlaw through the stock path.
 *   the ordinary PvP flag   - satisfies target->IsPvP() at Unit.cpp:10866, which is what
 *                             lets an ordinary player strike an outlaw.
 *   UNIT_BYTE2_FLAG_UNK1    - satisfies the either-side clause at Unit.cpp:10872, which is
 *                             what lets an outlaw strike a victim who is not flagged.
 *
 * **The faction does not have to be a hostile one.** Being open to violence and being
 * hunted by the town watch are different things, so there are two states. WANTED wears a
 * neutral template - attackable by anyone, hunted by nobody. HOSTILE wears faction 14 and
 * is earned by damaging a player who never made that declaration.
 *
 * Neutral is enough to be attacked because of a single bit: a Human's friendlyMask is 0x2,
 * ALLIANCE, not 0x1, PLAYER. A template carrying only the player bit is therefore friendly
 * to nobody and hostile to nobody, both ways, for all ten races - and Unit.cpp:10801 only
 * rejects a pair reading actively FRIENDLY. See the faction globals below.
 *
 * Each lever is separately switchable in the config, because which of them the 3.3.5a
 * client actually honours is a question only a live client can answer.
 *
 * See README.md for the consequences - an outlaw cannot be healed by ordinary players, a
 * hostile one is attacked on sight by every city NPC, and group members can never fight
 * each other whatever is set.
 */

#include "SanctuaryOutlaw.h"

#include "Chat.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "UnitScript.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <sstream>
#include <unordered_map>

namespace
{
    bool g_enabled = true;

    /*
     * Two factions, because an outlaw has two states.
     *
     * WANTED is the ordinary one, and it is neutral rather than hostile: enough to be
     * struck, not enough to be hunted. 2300 is ours, added by the module's world SQL and by
     * patch-enUS-4.MPQ - ourMask 0x1 keeps the outlaw in the player group, friendlyMask 0x0
     * means neither side claims them as kin, and hostileMask 0x8 leaves ordinary monsters
     * exactly as dangerous to them as to anybody else.
     *
     * The reason a neutral faction is attackable at all is one bit: a Human's friendlyMask
     * is 0x2 - ALLIANCE - not 0x1 - PLAYER. So a template carrying only the player bit is
     * friendly to nobody and hostile to nobody, both directions, for all ten races. That
     * clears the guard at Unit.cpp:10801, which only rejects a pair reading actively
     * friendly, and the PvP block below it then allows the strike on its own terms.
     *
     * Its faction is 0, and that is the load-bearing half. A stock neutral template was
     * tried first - Booty Bay's, whose masks are identical - and it failed completely:
     * GetReactionTo answers player-vs-player from REPUTATION whenever the target's faction
     * has an entry, returning FRIENDLY at Unit.cpp:7197 before any faction is compared. So
     * the server refused every attack while the client, weighing it differently, showed a
     * hostile cursor and began the swing. Faction 0 has no row in Faction.dbc at all, so
     * that lookup fails and the reaction falls through to where IfNormalReaction can reach
     * it. Any replacement must have no reputation entry; the check at startup enforces it.
     *
     * HOSTILE is what striking somebody earns: faction 14, Monster, whose hostileMask
     * covers the player group, so every guard in the city comes.
     */
    uint32 g_wantedFaction = 2300;
    uint32 g_hostileFaction = 14;
    bool g_escalateOnAttack = true;
    uint32 g_hostileMinutes = 0;      ///> 0 = hostile for as long as they are an outlaw
    bool g_setPvpFlag = true;
    bool g_allowStrikingUnflagged = true;
    bool g_forceReaction = true;
    bool g_guardsAttack = true;
    bool g_requireOutOfCombat = true;
    uint32 g_releaseSeconds = 60;
    uint32 g_maxMinutes = 1440;
    std::string g_prefix = "SOUTLAW";

    struct Outlaw
    {
        time_t since = 0;
        time_t expires = 0;      ///> 0 = until they ask to stop
        time_t releaseAt = 0;    ///> 0 = not releasing; otherwise when the flag drops
        uint32 sinceCheckMs = 0; ///> throttles the once-a-second faction re-assert
        bool hostile = false;    ///> they have drawn blood, so the watch wants them
        time_t hostileUntil = 0; ///> when that lapses; 0 = as long as they are an outlaw
    };

    /// Online outlaws only. The database row is the durable copy; this is what the hot
    /// paths look at, and IfNormalReaction leans on it being empty most of the time.
    std::unordered_map<ObjectGuid::LowType, Outlaw> g_outlaws;

    time_t Now() { return GameTime::GetGameTime().count(); }

    /*
     * The faction the player would have if nobody had interfered.
     *
     * Player::SetFactionForRace looks the same but returns early when the effective team
     * differs from the racial one (Player.cpp:6016), which is exactly the state a faction
     * override puts them in - so it cannot be used to undo one.
     */
    uint32 NativeFaction(Player const* player)
    {
        if (ChrRacesEntry const* race = sChrRacesStore.LookupEntry(player->getRace(true)))
            if (race->FactionID)
                return race->FactionID;

        return player->GetFaction();
    }

    void SetFactionWithPets(Player* player, uint32 faction)
    {
        if (player->GetFaction() != faction)
            player->SetFaction(faction);

        // A pet left on the owner's old faction fights for the wrong side, and the client
        // draws it a different colour from the person it belongs to.
        for (Unit* controlled : player->m_Controlled)
            if (controlled && controlled->GetFaction() != faction)
                controlled->SetFaction(faction);
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

    void Store(ObjectGuid::LowType guid, Outlaw const& entry)
    {
        CharacterDatabase.Execute(
            "REPLACE INTO `sanctuary_outlaw` "
            "(`guid`, `since`, `expires`, `release_at`, `hostile`, `hostile_until`) "
            "VALUES ({}, {}, {}, {}, {}, {})",
            guid, uint32(entry.since), uint32(entry.expires), uint32(entry.releaseAt),
            entry.hostile ? 1 : 0, uint32(entry.hostileUntil));
    }

    void Forget(ObjectGuid::LowType guid)
    {
        CharacterDatabase.Execute("DELETE FROM `sanctuary_outlaw` WHERE `guid` = {}", guid);
    }

    /// The faction a state calls for. 0 leaves the player's own alone.
    uint32 FactionFor(Outlaw const& entry)
    {
        return entry.hostile ? g_hostileFaction : g_wantedFaction;
    }

    /// Puts every configured lever on. Safe to call repeatedly; that is how it is held.
    void Apply(Player* player, Outlaw const& entry)
    {
        if (!player || player->IsGameMaster())
            return;

        if (!player->HasByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_FFA_PVP))
            player->SetByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_FFA_PVP);

        if (g_allowStrikingUnflagged && !player->HasByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_UNK1))
            player->SetByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_UNK1);

        // _override, so it does not start the five-minute unflag timer underneath us.
        if (g_setPvpFlag && !player->IsPvP())
            player->UpdatePvP(true, true);

        /*
         * What makes guards care - and now, what makes them wait.
         *
         * A creature's reaction to a *player* is answered from the player's reputation and
         * returns at Unit.cpp:7278; the faction template comparison at :7285 is never
         * reached. So an Exalted-with-Stormwind outlaw reads as friendly to a Stormwind
         * guard however monstrous their faction is, and no faction override alone will
         * change that. This flag is the switch the core offers for skipping that branch.
         *
         * Which makes it exactly the right lever for escalation. A wanted outlaw keeps
         * their reputation, so the watch goes on seeing a citizen in good standing and
         * leaves them be, whatever their faction says. Strike somebody and the flag goes
         * on, the faction is consulted at last, and every NPC whose hostileMask covers the
         * monster group turns out - the city NPCs of both sides, not only the guards.
         *
         * It has to come off again as well as on: hostility can lapse, and Apply is how
         * every state change is expressed.
         */
        if (g_guardsAttack && entry.hostile)
            player->SetUnitFlag2(UNIT_FLAG2_IGNORE_REPUTATION);
        else
            player->RemoveUnitFlag2(UNIT_FLAG2_IGNORE_REPUTATION);

        if (uint32 faction = FactionFor(entry))
            SetFactionWithPets(player, faction);
    }

    /*
     * The moment an outlaw stops being merely available and starts being wanted.
     *
     * Declaring yourself open to violence is not a crime and the watch has no opinion about
     * it. Hitting somebody who never made that declaration is, and this is where the town
     * finds out.
     */
    void Escalate(Player* player, Outlaw& entry)
    {
        entry.hostile = true;
        entry.hostileUntil = g_hostileMinutes ? Now() + time_t(g_hostileMinutes) * 60 : 0;

        Apply(player, entry);
        Store(player->GetGUID().GetCounter(), entry);
        SanctuaryOutlaw::SendState(player);

        if (player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff4040You have drawn blood.|r The watch is against you now.");
    }

    /*
     * Takes every lever off.
     *
     * The caller must have removed the player from g_outlaws first: UpdatePvPState tears
     * the FFA byte down through the core's own path, which fires OnPlayerFfaPvpStateUpdate,
     * which is where this module puts the flag back.
     */
    void Clear(Player* player)
    {
        if (!player)
            return;

        player->RemoveByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_UNK1);
        player->RemoveUnitFlag2(UNIT_FLAG2_IGNORE_REPUTATION);

        if (g_wantedFaction || g_hostileFaction)
            SetFactionWithPets(player, NativeFaction(player));

        // Hands both flags back to the core rather than guessing: it recomputes them from
        // the area the player is standing in and starts the ordinary unflag timer.
        player->UpdatePvPState();
    }

    void Drop(Player* player, char const* why)
    {
        if (!player)
            return;

        ObjectGuid::LowType guid = player->GetGUID().GetCounter();

        g_outlaws.erase(guid);
        Forget(guid);
        Clear(player);

        ChatHandler(player->GetSession()).PSendSysMessage("You are no longer an outlaw{}.", why);
        SanctuaryOutlaw::SendState(player);
    }

    std::string Remaining(Outlaw const& entry)
    {
        time_t deadline = entry.releaseAt ? entry.releaseAt : entry.expires;

        if (!deadline)
            return "";

        time_t left = deadline > Now() ? deadline - Now() : 0;

        std::ostringstream text;
        if (left >= 120)
            text << (left / 60) << " minutes";
        else
            text << left << " seconds";

        return text.str();
    }
}

namespace SanctuaryOutlaw
{
    bool IsEnabled() { return g_enabled; }

    bool IsOutlaw(Player const* player)
    {
        if (!g_enabled || !player || g_outlaws.empty())
            return false;

        return g_outlaws.find(player->GetGUID().GetCounter()) != g_outlaws.end();
    }

    bool IsHostile(Player const* player)
    {
        if (!g_enabled || !player || g_outlaws.empty())
            return false;

        auto it = g_outlaws.find(player->GetGUID().GetCounter());

        return it != g_outlaws.end() && it->second.hostile;
    }

    Result Flag(Player* player, uint32 minutes)
    {
        if (!g_enabled)
            return Result::Disabled;

        if (!player)
            return Result::NoPlayer;

        // A game master is deliberately unattackable, and SetGameMaster strips the flag
        // anyway - so accepting the request would only produce a flag that never applies.
        if (player->IsGameMaster())
            return Result::GameMaster;

        ObjectGuid::LowType guid = player->GetGUID().GetCounter();
        auto existing = g_outlaws.find(guid);

        bool wasOutlaw = existing != g_outlaws.end();
        bool wasReleasing = wasOutlaw && existing->second.releaseAt != 0;

        Outlaw& entry = g_outlaws[guid];

        if (!wasOutlaw)
            entry.since = Now();

        if (minutes)
            entry.expires = Now() + time_t(minutes) * 60;

        // Asking again cancels a pending release, which is the natural way to change your
        // mind before the timer runs out.
        entry.releaseAt = 0;

        Apply(player, entry);
        Store(guid, entry);
        SendState(player);

        if (wasOutlaw && !wasReleasing)
            return Result::AlreadyOutlaw;

        return Result::Ok;
    }

    Result Release(Player* player)
    {
        if (!g_enabled)
            return Result::Disabled;

        if (!player)
            return Result::NoPlayer;

        ObjectGuid::LowType guid = player->GetGUID().GetCounter();
        auto it = g_outlaws.find(guid);

        if (it == g_outlaws.end())
            return Result::NotOutlaw;

        if (it->second.releaseAt)
            return Result::AlreadyReleasing;

        /*
         * A sentence is not yours to end.
         *
         * Without this, `.outlaw set` is worthless: anyone handed a sentence simply stands
         * down from it, and the only people still outlawed are the ones who chose to be.
         */
        if (it->second.expires && it->second.expires > Now())
            return Result::Sentenced;

        /*
         * The whole point of the delay.
         *
         * Without this the flag is an escape button: strike, drop it, become untouchable
         * while the person you hit is still swinging. Refusing in combat closes the worst
         * of it and the timer closes the rest.
         */
        if (g_requireOutOfCombat && player->GetCombatManager().HasPvPCombat())
            return Result::InCombat;

        if (!g_releaseSeconds)
        {
            Drop(player, "");
            return Result::Ok;
        }

        it->second.releaseAt = Now() + g_releaseSeconds;

        Store(guid, it->second);
        SendState(player);

        return Result::Ok;
    }

    Result Revoke(Player* player, char const* why)
    {
        if (!g_enabled)
            return Result::Disabled;

        if (!player)
            return Result::NoPlayer;

        if (!IsOutlaw(player))
            return Result::NotOutlaw;

        Drop(player, why ? why : "; a game master has pardoned you");
        return Result::Ok;
    }

    std::string Describe(Player const* player)
    {
        if (!g_enabled)
            return "Outlawry is switched off on this realm.";

        if (!player)
            return "No character.";

        auto it = g_outlaws.find(player->GetGUID().GetCounter());

        if (it == g_outlaws.end())
            return "You are not an outlaw. Nobody of your own side can raise a hand to you.";

        std::string left = Remaining(it->second);

        if (it->second.releaseAt)
            return "You are still an outlaw for another " + left + ".";

        std::string const wanted = it->second.hostile
            ? " The watch is hunting you."
            : " The watch has no quarrel with you yet.";

        if (it->second.expires)
            return "You are an outlaw for another " + left + ", and cannot stand down early."
                   + wanted;

        return "You are an outlaw. Anyone may raise a hand to you, and you to them." + wanted;
    }

    uint32 MaxSentenceMinutes() { return g_maxMinutes; }

    void SendState(Player* player)
    {
        if (!player)
            return;

        auto it = g_outlaws.find(player->GetGUID().GetCounter());

        time_t deadline = 0;
        bool releasing = false;

        if (it != g_outlaws.end())
        {
            releasing = it->second.releaseAt != 0;
            deadline = releasing ? it->second.releaseAt : it->second.expires;
        }

        uint32 left = 0;
        if (deadline && deadline > Now())
            left = uint32(deadline - Now());

        std::ostringstream payload;
        payload << "STATE"
                << " on=" << (it != g_outlaws.end() ? 1 : 0)
                << " hostile=" << (it != g_outlaws.end() && it->second.hostile ? 1 : 0)
                << " releasing=" << (releasing ? 1 : 0)
                << " left=" << left;

        SendAddonPacket(player, payload.str());
    }
}

/*
 * Holds the flags on.
 *
 * The FFA byte is torn down constantly - by any zone change, any resurrect, teleport,
 * login, game master toggle, quest, taxi arrival or stat reset, and on any area change at
 * all since UpdateArea passes reset=false with no PvP timer running. Chasing those one at
 * a time is hopeless, but every single removal site is immediately followed by
 * OnPlayerFfaPvpStateUpdate(player, false), which makes that one hook a complete
 * interception point.
 *
 * The faction is the opposite: it survives area and zone churn, and is instead reset by
 * login, the game master toggle, RestoreFaction and instance scripts. Rather than chase
 * each of those, it is compared once a second and put back.
 */
class sanctuary_outlaw_playerscript : public PlayerScript
{
public:
    sanctuary_outlaw_playerscript() : PlayerScript("sanctuary_outlaw_playerscript",
        {
            PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_LOGOUT,
            PLAYERHOOK_ON_UPDATE,
            PLAYERHOOK_ON_FFA_PVP_STATE_UPDATE,
            // Addon traffic is a whisper to self, so it is the private-chat overload.
            PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT
        }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!g_enabled || !player)
            return;

        ObjectGuid::LowType guid = player->GetGUID().GetCounter();

        QueryResult result = CharacterDatabase.Query(
            "SELECT `since`, `expires`, `release_at`, `hostile`, `hostile_until` "
            "FROM `sanctuary_outlaw` WHERE `guid` = {}", guid);

        if (!result)
            return;

        Outlaw entry;
        entry.since = time_t((*result)[0].Get<uint32>());
        entry.expires = time_t((*result)[1].Get<uint32>());
        entry.releaseAt = time_t((*result)[2].Get<uint32>());

        // Hostility survives a logout on purpose. It is the one piece of this state that
        // somebody would otherwise launder by quitting to the character screen.
        entry.hostile = (*result)[3].Get<uint8>() != 0;
        entry.hostileUntil = time_t((*result)[4].Get<uint32>());

        if (entry.hostile && entry.hostileUntil && entry.hostileUntil <= Now())
        {
            entry.hostile = false;
            entry.hostileUntil = 0;
        }

        // A sentence that ran out while they were offline has been served.
        if (entry.expires && entry.expires <= Now())
        {
            Forget(guid);
            return;
        }

        /*
         * A pending release does not survive a logout.
         *
         * Otherwise logging out during the countdown and back in after it would be the
         * escape the countdown exists to prevent - and the player never saw it land, so
         * they would have no idea whether they were still flagged.
         */
        if (entry.releaseAt)
        {
            entry.releaseAt = 0;
            Store(guid, entry);
        }

        g_outlaws[guid] = entry;

        Apply(player, g_outlaws[guid]);
        SanctuaryOutlaw::SendState(player);

        ChatHandler(player->GetSession()).PSendSysMessage("{}", SanctuaryOutlaw::Describe(player));
    }

    void OnPlayerLogout(Player* player) override
    {
        if (player)
            g_outlaws.erase(player->GetGUID().GetCounter());
    }

    void OnPlayerFfaPvpStateUpdate(Player* player, bool state) override
    {
        // Only the teardown matters, and only for someone who should still be flagged.
        if (state || !g_enabled || g_outlaws.empty() || !player || player->IsGameMaster())
            return;

        if (!SanctuaryOutlaw::IsOutlaw(player))
            return;

        /*
         * SetByteFlag, never UpdateFFAPvPState.
         *
         * The core is part way through its own teardown when this fires; calling back into
         * it would re-enter that function. Setting the bit directly is enough, and doing it
         * here - before the loop at PlayerUpdates.cpp:1503 - also stops the core breaking
         * off attacks that were perfectly valid a moment ago.
         */
        player->SetByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_FFA_PVP);
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!g_enabled || g_outlaws.empty() || !player)
            return;

        auto it = g_outlaws.find(player->GetGUID().GetCounter());

        if (it == g_outlaws.end())
            return;

        Outlaw& entry = it->second;

        entry.sinceCheckMs += diff;
        if (entry.sinceCheckMs < 1000)
            return;

        entry.sinceCheckMs = 0;

        if (entry.releaseAt && Now() >= entry.releaseAt)
        {
            Drop(player, "");
            return;
        }

        if (entry.expires && Now() >= entry.expires)
        {
            Drop(player, "; your sentence is served");
            return;
        }

        // A game master turning the flag on should not have to fight this module for it.
        if (player->IsGameMaster())
            return;

        /*
         * Hostility lapsing is the only state change this module makes on its own. Anything
         * else - the sentence ending, a stand-down landing - drops the flag outright and has
         * already returned above.
         */
        if (entry.hostile && entry.hostileUntil && Now() >= entry.hostileUntil)
        {
            entry.hostile = false;
            entry.hostileUntil = 0;

            Apply(player, entry);
            Store(player->GetGUID().GetCounter(), entry);
            SanctuaryOutlaw::SendState(player);

            if (player->GetSession())
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "The guards have lost interested but some may still hunt you down.");
            return;
        }

        if (uint32 faction = FactionFor(entry))
            if (player->GetFaction() != faction)
                SetFactionWithPets(player, faction);
    }

    /// Addon traffic arrives as a whisper the player sends to themselves.
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
            SanctuaryOutlaw::SendState(player);

        return false;
    }
};

/*
 * Makes two players of one side stop reading as friends.
 *
 * This is the only hook that can bend Unit::_IsValidAttackTarget, which has none of its
 * own: GetReactionTo consults it at Unit.cpp:7207, and that line is reachable for two
 * players because every playable race's faction has reputationListID -1, so the
 * reputation branch above it never returns.
 *
 * It is also on the hot path of every reaction check in the world, creature-on-creature
 * included, so it does as little as possible before getting out of the way.
 */
class sanctuary_outlaw_unitscript : public UnitScript
{
public:
    sanctuary_outlaw_unitscript() : UnitScript("sanctuary_outlaw_unitscript") { }

    /*
     * Striking somebody is what turns the town against you.
     *
     * Damage rather than combat state, because combat is entered by being attacked as well
     * as by attacking, and only one of those is a crime. It is also the hardest of the
     * available signals to trip by accident.
     *
     * On the hot path of every blow landed anywhere in the world, so the cheap tests come
     * first and the map lookup last.
     */
    void OnDamage(Unit* attacker, Unit* victim, uint32& /*damage*/) override
    {
        if (!g_enabled || !g_escalateOnAttack || g_outlaws.empty() || !attacker || !victim)
            return;

        Player* striker = attacker->GetAffectingPlayer();
        Player* struck = victim->GetAffectingPlayer();

        if (!striker || !struck || striker == struck)
            return;

        auto it = g_outlaws.find(striker->GetGUID().GetCounter());

        if (it == g_outlaws.end() || it->second.hostile)
            return;

        // Outlaw on outlaw is the entire point of the flag, not a crime. Both parties said
        // they were open to this, and the watch has no business in it.
        if (SanctuaryOutlaw::IsOutlaw(struck))
            return;

        Escalate(striker, it->second);
    }

    bool IfNormalReaction(Unit const* unit, Unit const* target, ReputationRank& repRank) override
    {
        if (!g_enabled || !g_forceReaction || g_outlaws.empty() || !unit || !target)
            return true;

        Player const* self = unit->GetAffectingPlayer();
        Player const* other = target->GetAffectingPlayer();

        if (!self || !other || self == other)
            return true;

        if (!SanctuaryOutlaw::IsOutlaw(self) && !SanctuaryOutlaw::IsOutlaw(other))
            return true;

        /*
         * Neutral, not hostile.
         *
         * Neutral is all that is needed: the guard at Unit.cpp:10801 only rejects a pair
         * that is actively friendly, and everything after it is the PvP block that decides
         * this properly. Hostile would additionally invite anything that auto-attacks on
         * reaction to start swinging on sight.
         */
        repRank = REP_NEUTRAL;
        return false;
    }
};

class sanctuary_outlaw_worldscript : public WorldScript
{
public:
    sanctuary_outlaw_worldscript() : WorldScript("sanctuary_outlaw_worldscript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    /*
     * The one thing about a wanted faction that cannot be seen by reading it.
     *
     * Its masks can be perfectly neutral and it will still not work if the faction behind
     * it has a reputation entry: the player-vs-player branch of GetReactionTo answers from
     * the attacker's standing and returns FRIENDLY before the masks are ever compared, so
     * nobody can lay a hand on the outlaw. Nothing is logged when that happens, the client
     * disagrees with the server about it, and it looks for all the world like a client bug.
     *
     * It cost an evening once. It costs one lookup at startup now.
     */
    void OnStartup() override
    {
        if (!g_enabled || !g_wantedFaction)
            return;

        FactionTemplateEntry const* templ = sFactionTemplateStore.LookupEntry(g_wantedFaction);

        if (!templ)
        {
            LOG_ERROR("module.sanctuaryoutlaw",
                "SanctuaryOutlaw.WantedFaction {} is not a faction template the core knows. "
                "Wanted outlaws will keep whatever faction they logged in with.",
                g_wantedFaction);
            return;
        }

        FactionEntry const* faction = sFactionStore.LookupEntry(templ->faction);

        if (faction && faction->CanHaveReputation())
        {
            LOG_ERROR("module.sanctuaryoutlaw",
                "SanctuaryOutlaw.WantedFaction {} uses faction {}, which has a reputation "
                "entry. Player-vs-player reaction is answered from the attacker's standing "
                "with it and returns FRIENDLY before the faction is compared, so NOBODY will "
                "be able to attack a wanted outlaw - while their client still shows a hostile "
                "cursor. Use a template whose faction has no reputation.",
                g_wantedFaction, templ->faction);
            return;
        }

        LOG_INFO("module.sanctuaryoutlaw",
            "Wanted outlaws wear faction template {} (faction {}, group 0x{:X}, friend 0x{:X}, "
            "enemy 0x{:X}).",
            g_wantedFaction, templ->faction, templ->ourMask, templ->friendlyMask,
            templ->hostileMask);
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>("SanctuaryOutlaw.Enable", true);
        g_hostileFaction = sConfigMgr->GetOption<uint32>("SanctuaryOutlaw.Faction", 14);
        g_wantedFaction = sConfigMgr->GetOption<uint32>("SanctuaryOutlaw.WantedFaction", 2300);
        g_escalateOnAttack = sConfigMgr->GetOption<bool>("SanctuaryOutlaw.EscalateOnAttack", true);
        g_hostileMinutes = sConfigMgr->GetOption<uint32>("SanctuaryOutlaw.HostileMinutes", 0);
        g_setPvpFlag = sConfigMgr->GetOption<bool>("SanctuaryOutlaw.SetPvPFlag", true);
        g_allowStrikingUnflagged = sConfigMgr->GetOption<bool>("SanctuaryOutlaw.AllowStrikingUnflagged", true);
        g_forceReaction = sConfigMgr->GetOption<bool>("SanctuaryOutlaw.ForceReaction", true);
        g_guardsAttack = sConfigMgr->GetOption<bool>("SanctuaryOutlaw.GuardsAttack", true);
        g_requireOutOfCombat = sConfigMgr->GetOption<bool>("SanctuaryOutlaw.RequireOutOfCombat", true);
        g_releaseSeconds = sConfigMgr->GetOption<uint32>("SanctuaryOutlaw.ReleaseSeconds", 60);
        g_maxMinutes = sConfigMgr->GetOption<uint32>("SanctuaryOutlaw.MaxSentenceMinutes", 1440);
        g_prefix = sConfigMgr->GetOption<std::string>("SanctuaryOutlaw.Addon.Prefix", "SOUTLAW");

        LOG_INFO("module.sanctuaryoutlaw",
                 "Sanctuary outlaw {}: faction {}, pvp flag {}, strike unflagged {}, forced reaction {}, guards attack {}.",
                 g_enabled ? "enabled" : "disabled",
                 g_wantedFaction ? std::to_string(g_wantedFaction) : "unchanged",
                 g_setPvpFlag ? "on" : "off",
                 g_allowStrikingUnflagged ? "on" : "off",
                 g_forceReaction ? "on" : "off",
                 g_guardsAttack ? "on" : "off");
    }
};

void AddSC_sanctuary_outlaw_scripts()
{
    new sanctuary_outlaw_playerscript();
    new sanctuary_outlaw_unitscript();
    new sanctuary_outlaw_worldscript();
}
