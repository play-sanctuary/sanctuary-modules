/*
 * mod-sanctuary-zidormi
 *
 * Zidormi of the bronze flight, standing outside the Ruins of Lordaeron, moving players
 * between two readings of the same ground: the memory of the past, and the present day.
 *
 * All she does is set the player's phase mask - the same thing `.modify phase` does - so
 * the world itself is where the two timelines actually live. See README.md for what that
 * implies about which spawns show up in which.
 *
 * The script is attached per *spawn* (`creature`.`ScriptName`), not to creature_template
 * 31848. Zidormi already exists in 3.3.5a as the Caverns of Time traveller in the Bronze
 * Dragonshrine, and putting the ScriptName on the template would replace "Take me to the
 * Caverns of Time" with this dialogue for her too, quietly removing a real travel service.
 */

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureScript.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Object.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "WorldScript.h"

#include <string>

namespace
{
    bool g_enabled = true;
    bool g_remember = true;
    uint32 g_pastPhase = PHASEMASK_NORMAL;
    uint32 g_presentPhase = PHASEMASK_ANYWHERE;

    /// The script name a spawn must carry to become a timeway. Also used by the startup
    /// check, so a mistyped ScriptName is reported rather than silently doing nothing.
    constexpr char const* ScriptName = "sanctuary_zidormi";

    /// Custom npc_text rows shipped with the module.
    constexpr uint32 TEXT_GREETING = 990000;
    constexpr uint32 TEXT_ABOUT = 990001;

    enum ZidormiAction : uint32
    {
        ACTION_PAST = GOSSIP_ACTION_INFO_DEF + 1,
        ACTION_PRESENT = GOSSIP_ACTION_INFO_DEF + 2,
        ACTION_ABOUT = GOSSIP_ACTION_INFO_DEF + 3,
        ACTION_MAIN = GOSSIP_ACTION_INFO_DEF + 4
    };

    // Gossip is parchment, so these are dark enough to read against it - the same palette
    // the notice board settled on.
    constexpr char const* Ink = "|cff2f2114";
    constexpr char const* Muted = "|cff6b5a45";
    constexpr char const* Accent = "|cff7a4e10";

    bool IsInPast(Player const* player) { return player->GetPhaseMask() == g_pastPhase; }
    bool IsInPresent(Player const* player) { return player->GetPhaseMask() == g_presentPhase; }

    /*
     * Where the player is standing in time, in words.
     *
     * A third answer is possible and worth saying out loud: a game master, or anyone a
     * `.modify phase` has left on some other mask, is in neither. Telling them so is
     * better than picking whichever of the two is closer and being wrong.
     */
    std::string DescribeNow(Player const* player)
    {
        if (IsInPast(player))
            return std::string(Muted) + "You are walking in the memory of the past.|r";

        if (IsInPresent(player))
            return std::string(Muted) + "You are in the present day.|r";

        return std::string(Muted) + "You are somewhere between - phase "
               + std::to_string(player->GetPhaseMask()) + ".|r";
    }

    void Remember(Player const* player, uint32 phaseMask)
    {
        if (!g_remember)
            return;

        CharacterDatabase.Execute(
            "REPLACE INTO `sanctuary_timeline` (`guid`, `phase_mask`, `changed`) VALUES ({}, {}, {})",
            player->GetGUID().GetCounter(), phaseMask, uint32(GameTime::GetGameTime().count()));
    }

    /*
     * Sends the player to one side or the other.
     *
     * SetPhaseMask with update = true is exactly what `.modify phase` performs, down to the
     * visibility refresh that makes the world around them change without a reload.
     */
    void SendTo(Player* player, Creature* zidormi, uint32 phaseMask, char const* whisper)
    {
        player->SetPhaseMask(phaseMask, true);
        Remember(player, phaseMask);

        CloseGossipMenuFor(player);

        if (zidormi)
            zidormi->Whisper(whisper, LANG_UNIVERSAL, player);
    }
}

class sanctuary_zidormi : public CreatureScript
{
public:
    sanctuary_zidormi() : CreatureScript(::ScriptName) { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!g_enabled)
            return false;   // fall through to whatever gossip the spawn otherwise has

        ShowMain(player, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        if (!g_enabled)
            return false;

        switch (action)
        {
            case ACTION_PAST:
                SendTo(player, creature, g_pastPhase,
                       "Close your eyes. When you open them, this will be Lordaeron as it was.");
                break;

            case ACTION_PRESENT:
                SendTo(player, creature, g_presentPhase,
                       "The hour you belong to. Do not linger too long in what has already happened.");
                break;

            case ACTION_ABOUT:
                ShowAbout(player, creature);
                break;

            case ACTION_MAIN:
            default:
                ShowMain(player, creature);
                break;
        }

        return true;
    }

private:
    static void ShowMain(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);

        // The first row is the answer to "where am I?", which is the question a player
        // arrives with and which nothing else on screen tells them.
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, DescribeNow(player), GOSSIP_SENDER_MAIN, ACTION_MAIN);

        if (!IsInPast(player))
            AddGossipItemFor(player, GOSSIP_ICON_TALK, std::string(Accent) + "Show me this land as it was.|r",
                             GOSSIP_SENDER_MAIN, ACTION_PAST);

        if (!IsInPresent(player))
            AddGossipItemFor(player, GOSSIP_ICON_TALK, std::string(Accent) + "Return me to the present day.|r",
                             GOSSIP_SENDER_MAIN, ACTION_PRESENT);

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, std::string(Ink) + "What is it you do, exactly?|r",
                         GOSSIP_SENDER_MAIN, ACTION_ABOUT);

        SendGossipMenuFor(player, TEXT_GREETING, creature->GetGUID());
    }

    static void ShowAbout(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);

        AddGossipItemFor(player, GOSSIP_ICON_TALK, std::string(Accent) + "< Back|r",
                         GOSSIP_SENDER_MAIN, ACTION_MAIN);

        SendGossipMenuFor(player, TEXT_ABOUT, creature->GetGUID());
    }
};

/*
 * Puts players back where they left off.
 *
 * A phase mask is not stored on the character - there is no column for it - so without
 * this every logout silently returns the player to the default phase. Someone who chose
 * the present day would find themselves back in the past next session with nothing to
 * explain it, which reads as a bug rather than as a rule.
 */
class sanctuary_zidormi_playerscript : public PlayerScript
{
public:
    sanctuary_zidormi_playerscript() : PlayerScript("sanctuary_zidormi_playerscript") { }

    void OnPlayerLogin(Player* player) override
    {
        if (!g_enabled || !g_remember)
            return;

        // A game master is on PHASEMASK_ANYWHERE deliberately, and it is not ours to undo.
        if (player->IsGameMaster())
            return;

        ObjectGuid::LowType guid = player->GetGUID().GetCounter();

        QueryResult result = CharacterDatabase.Query(
            "SELECT `phase_mask` FROM `sanctuary_timeline` WHERE `guid` = {}", guid);

        if (!result)
            return;

        uint32 stored = (*result)[0].Get<uint32>();

        /*
         * Only the two phases currently configured are honoured.
         *
         * Change PresentPhase in the config and every stored row becomes a mask that no
         * longer means anything - restoring it would strand players in a phase holding
         * none of the world. Dropping the row instead leaves them at the default, which
         * is somewhere real.
         */
        if (stored != g_pastPhase && stored != g_presentPhase)
        {
            CharacterDatabase.Execute("DELETE FROM `sanctuary_timeline` WHERE `guid` = {}", guid);
            return;
        }

        if (stored != player->GetPhaseMask())
            player->SetPhaseMask(stored, true);
    }
};

class sanctuary_zidormi_worldscript : public WorldScript
{
public:
    sanctuary_zidormi_worldscript() : WorldScript("sanctuary_zidormi_worldscript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>("SanctuaryZidormi.Enable", true);
        g_remember = sConfigMgr->GetOption<bool>("SanctuaryZidormi.Remember", true);
        g_pastPhase = sConfigMgr->GetOption<uint32>("SanctuaryZidormi.PastPhase", PHASEMASK_NORMAL);
        g_presentPhase = sConfigMgr->GetOption<uint32>("SanctuaryZidormi.PresentPhase", PHASEMASK_ANYWHERE);

        // Nothing below can work if the two sides of the conversation are the same place,
        // and a phase of 0 makes the world invisible rather than empty.
        if (!g_pastPhase || !g_presentPhase || g_pastPhase == g_presentPhase)
        {
            LOG_ERROR("module", "SanctuaryZidormi: PastPhase ({}) and PresentPhase ({}) must both be "
                                "non-zero and different from each other. Disabling.",
                      g_pastPhase, g_presentPhase);
            g_enabled = false;
        }
    }

    /*
     * Checked at startup rather than on config load, because creature spawns are not
     * loaded yet when the config is read.
     */
    void OnStartup() override
    {
        if (!g_enabled)
            return;

        QueryResult result = WorldDatabase.Query(
            "SELECT COUNT(*) FROM `creature` WHERE `ScriptName` = '{}'", ::ScriptName);

        uint32 spawns = result ? (*result)[0].Get<uint32>() : 0;

        if (!spawns)
        {
            LOG_WARN("module", "SanctuaryZidormi: enabled, but no creature spawn carries ScriptName '{}'. "
                               "Nobody can change phase until one does - see the module README.", ::ScriptName);
            return;
        }

        LOG_INFO("module", "SanctuaryZidormi: {} timeway(s) standing. Past = phase {}, present = phase {}.",
                 spawns, g_pastPhase, g_presentPhase);
    }
};

void AddSC_sanctuary_zidormi_scripts()
{
    new sanctuary_zidormi();
    new sanctuary_zidormi_playerscript();
    new sanctuary_zidormi_worldscript();
}
