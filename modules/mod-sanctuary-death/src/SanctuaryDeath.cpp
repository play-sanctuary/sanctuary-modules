/*
 * mod-sanctuary-death
 *
 * You cannot walk back to your own body and stand up again. Death sends you to a spirit
 * healer, or you wait where you fell and hope somebody raises you.
 *
 * The obvious implementation - delete the corpse, so there is nothing to return to - is
 * wrong, and quietly so. A released player is resurrected by another player *through their
 * corpse*: the client sends a corpse target and Spell.cpp resolves it with
 * corpseTarget->GetOwnerGUID() to find the ghost. Remove the corpse and nobody can ever
 * raise you again, which would turn "wait for help" into a lie and make every death
 * spirit-healer-only. So the body stays exactly where it fell, and only the player's own
 * reclaim is refused.
 *
 * The second half is the spirit healer. Its stock dialogue warns about resurrection
 * sickness, which this realm does not apply, so the one thing the popup says is false.
 * Rather than suppress the popup - which would leave the player no way to confirm - the
 * module takes the conversation over before it starts, and says something true instead.
 */

#include "Chat.h"
#include "Config.h"
#include "Corpse.h"
#include "Creature.h"
#include "CreatureScript.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "ServerScript.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <string>

namespace
{
    // Not an "s_" prefix: winsock2.h defines s_host and friends as macros on in_addr, and a
    // file-scope name colliding with one expands into nonsense before the compiler sees it.
    bool g_enabled = true;
    bool g_blockCorpseResurrection = true;
    bool g_excludeBattleground = true;
    bool g_excludeInstances = false;
    uint32 g_strandedMinutes = 10;

    bool g_replaceHealerDialog = true;
    std::string g_healerBody;
    std::string g_healerConfirm;
    std::string g_healerDecline;
    std::string g_blockedMessage;

    // Matches the notice board's palette, so the two custom windows read as one realm.
    constexpr char const* Ink = "|cff2f2114";      ///< body text, near-black brown
    constexpr char const* Muted = "|cff6b5a45";    ///< asides
    constexpr char const* Accent = "|cff7a4e10";   ///< the action worth spotting

    constexpr uint32 GOSSIP_SENDER_SANCTUARY_DEATH = 0x5344;   ///< 'SD', ours alone
    constexpr uint32 ACTION_RESURRECT = 1;
    constexpr uint32 ACTION_DECLINE = 2;

    /// Whether this player's own reclaim should be refused right now.
    bool ShouldBlockReclaim(Player* player)
    {
        if (!g_enabled || !g_blockCorpseResurrection || !player)
            return false;

        // Arenas are already refused by the core before this ever matters.
        if (g_excludeBattleground && player->InBattleground())
            return false;

        if (g_excludeInstances && player->GetMap() && player->GetMap()->Instanceable())
            return false;

        /*
         * The safety valve, and it is not decoration.
         *
         * Instances are in scope, and a wipe somewhere the entrance graveyard has no
         * reachable spirit healer would otherwise leave a whole group as ghosts with no
         * way back at all - not stuck for a while, stuck permanently. After long enough
         * as a ghost the reclaim is allowed through, so being stranded is always
         * temporary however badly the rest of this is configured.
         */
        if (g_strandedMinutes > 0)
        {
            if (Corpse* corpse = player->GetCorpse())
            {
                time_t const stranded = GameTime::GetGameTime().count() - corpse->GetGhostTime();

                if (stranded >= time_t(g_strandedMinutes * MINUTE))
                    return false;
            }
        }

        return true;
    }
}

/*
 * WorldSession::HandleReclaimCorpseOpcode has no script hook of its own, so the packet is
 * the only place to intervene. Refusing it here means the handler never runs.
 */
class sanctuary_death_serverscript : public ServerScript
{
public:
    sanctuary_death_serverscript() : ServerScript("sanctuary_death_serverscript",
        { SERVERHOOK_CAN_PACKET_RECEIVE }) { }

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        if (packet.GetOpcode() != CMSG_RECLAIM_CORPSE || !session)
            return true;

        Player* player = session->GetPlayer();
        if (!player || !ShouldBlockReclaim(player))
            return true;

        /*
         * The client draws its own "Resurrect Now" button and has no idea we refused, so a
         * silently dropped packet looks exactly like a broken server. Saying so - and
         * naming both routes back, because the second one is easy to forget - is the
         * difference between a rule and a bug.
         */
        ChatHandler(session).PSendSysMessage("{}", g_blockedMessage);

        return false;
    }
};

/*
 * The spirit healer, in full.
 *
 * Taking the whole conversation rather than suppressing the popup: selecting the stock
 * option casts spell 17251, whose script sends SMSG_SPIRIT_HEALER_CONFIRM, and the client
 * answers that with its sickness warning. Never letting the stock option exist means the
 * spell never casts and the warning never happens - with no addon involved, so this works
 * for a player who has installed nothing at all.
 */
class sanctuary_death_spirithealer : public CreatureScript
{
public:
    sanctuary_death_spirithealer() : CreatureScript("sanctuary_spirit_healer") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        // A living player clicking a healer should behave exactly as it always has.
        if (!g_enabled || !g_replaceHealerDialog || !player || player->IsAlive())
            return false;

        ClearGossipMenuFor(player);

        // Body text as non-selectable rows, the same trick the notice board uses: gossip
        // body text otherwise has to live in npc_text, and this keeps it in the config
        // where it can be reworded without touching the database.
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, Ink + g_healerBody + "|r",
            GOSSIP_SENDER_SANCTUARY_DEATH, ACTION_DECLINE);

        AddGossipItemFor(player, GOSSIP_ICON_TALK, Accent + g_healerConfirm + "|r",
            GOSSIP_SENDER_SANCTUARY_DEATH, ACTION_RESURRECT);

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, Muted + g_healerDecline + "|r",
            GOSSIP_SENDER_SANCTUARY_DEATH, ACTION_DECLINE);

        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());

        // True stops PrepareGossipMenu and SendPreparedGossip, so the stock spirit healer
        // option is never offered and spell 17251 is never reached.
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* /*creature*/, uint32 sender, uint32 action) override
    {
        if (sender != GOSSIP_SENDER_SANCTUARY_DEATH)
            return false;

        CloseGossipMenuFor(player);

        if (action != ACTION_RESURRECT)
            return true;

        // Re-checked rather than trusted: the menu was built when they were a ghost, and
        // somebody may have raised them in the meantime.
        if (player->IsAlive())
            return true;

        // The core's own spirit resurrection: restores, applies durability loss, spawns
        // the bones and teleports to the graveyard nearest the corpse. Everything the
        // stock path does, minus the dialog.
        player->GetSession()->SendSpiritResurrect();

        return true;
    }
};

class sanctuary_death_worldscript : public WorldScript
{
public:
    sanctuary_death_worldscript() : WorldScript("sanctuary_death_worldscript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>("SanctuaryDeath.Enable", true);
        g_blockCorpseResurrection = sConfigMgr->GetOption<bool>("SanctuaryDeath.BlockCorpseResurrection", true);
        g_excludeBattleground = sConfigMgr->GetOption<bool>("SanctuaryDeath.Exclude.Battleground", true);
        g_excludeInstances = sConfigMgr->GetOption<bool>("SanctuaryDeath.Exclude.Instances", false);
        g_strandedMinutes = sConfigMgr->GetOption<uint32>("SanctuaryDeath.StrandedMinutes", 10);

        g_replaceHealerDialog = sConfigMgr->GetOption<bool>("SanctuaryDeath.ReplaceSpiritHealerDialog", true);

        g_healerBody = sConfigMgr->GetOption<std::string>("SanctuaryDeath.Healer.Body",
            "This is not your only road back. A companion may still raise you where you fell.");
        g_healerConfirm = sConfigMgr->GetOption<std::string>("SanctuaryDeath.Healer.Confirm",
            "Return me to life.");
        g_healerDecline = sConfigMgr->GetOption<std::string>("SanctuaryDeath.Healer.Decline",
            "Not yet. I will wait.");

        g_blockedMessage = sConfigMgr->GetOption<std::string>("SanctuaryDeath.BlockedMessage",
            "You cannot return to your body. Find a spirit healer, or wait - someone may still raise you.");

        LOG_INFO("module.sanctuarydeath", "Sanctuary death rules {}: corpse resurrection {}, spirit healer dialogue {}.",
            g_enabled ? "enabled" : "disabled",
            g_blockCorpseResurrection ? "refused" : "allowed",
            g_replaceHealerDialog ? "replaced" : "stock");
    }
};

void AddSC_sanctuary_death_scripts()
{
    new sanctuary_death_serverscript();
    new sanctuary_death_spirithealer();
    new sanctuary_death_worldscript();
}
