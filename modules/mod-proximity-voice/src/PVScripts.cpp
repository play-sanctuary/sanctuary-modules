/*
 * mod-proximity-voice - world and player hooks
 */

#include "ProximityVoice.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "WorldScript.h"

namespace
{
    /// Learning a language is just learning its spell, so this is the cheapest
    /// place to notice that a character can now understand the other faction.
    bool IsLanguageSpell(uint32 spellId)
    {
        for (LanguageDesc const& desc : lang_description)
            if (desc.spell_id == spellId)
                return true;

        return false;
    }
}

class ProximityVoice_WorldScript : public WorldScript
{
public:
    ProximityVoice_WorldScript() : WorldScript("ProximityVoice_WorldScript",
        {
            WORLDHOOK_ON_AFTER_CONFIG_LOAD,
            WORLDHOOK_ON_STARTUP,
            WORLDHOOK_ON_SHUTDOWN,
            WORLDHOOK_ON_UPDATE
        }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        sProximityVoice->LoadConfig(reload);
    }

    void OnStartup() override
    {
        sProximityVoice->Startup();
    }

    void OnShutdown() override
    {
        sProximityVoice->Shutdown();
    }

    void OnUpdate(uint32 diff) override
    {
        sProximityVoice->Update(diff);
    }
};

class ProximityVoice_PlayerScript : public PlayerScript
{
public:
    ProximityVoice_PlayerScript() : PlayerScript("ProximityVoice_PlayerScript",
        {
            PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_LOGOUT,
            PLAYERHOOK_ON_LEARN_SPELL,
            PLAYERHOOK_ON_SET_SKILL,
            PLAYERHOOK_ON_UPDATE_SKILL,
            PLAYERHOOK_ON_MAP_CHANGED,
            PLAYERHOOK_ON_UPDATE_ZONE,
            PLAYERHOOK_ON_PLAYER_ENTER_COMBAT,
            PLAYERHOOK_ON_PLAYER_LEAVE_COMBAT,
            // Addon traffic is a whisper to self, so it is the private-chat overload.
            PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT
        }) { }

    //[[ Addon traffic arrives as a whisper the player sends to themselves. ]]
    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg, Player* /*receiver*/) override
    {
        if (lang != LANG_ADDON || !player)
            return true;

        // Returning false swallows the message, so our own traffic never shows in chat.
        return !sProximityVoice->HandleAddonMessage(player, msg);
    }

    void OnPlayerLogin(Player* player) override
    {
        sProximityVoice->OnLogin(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        sProximityVoice->OnLogout(player);
    }

    void OnPlayerLearnSpell(Player* player, uint32 spellID) override
    {
        if (IsLanguageSpell(spellID))
            sProximityVoice->OnLanguagesChanged(player);
    }

    // A language skill set or raised - .voice teach, .setskill, or anything that ever
    // trains one - changes how much of it the character understands. The core fires
    // these after the value is written, so what the manager reads is the new one.
    void OnPlayerSetSkill(Player* player, uint32 skillId, uint32 /*value*/, uint32 /*max*/, uint32 /*step*/, uint32 /*newValue*/) override
    {
        if (ProximityVoice::IsLanguageSkill(skillId))
            sProximityVoice->OnLanguagesChanged(player);
    }

    void OnPlayerUpdateSkill(Player* player, uint32 skillId, uint32 /*value*/, uint32 /*max*/, uint32 /*step*/, uint32 /*newValue*/) override
    {
        if (ProximityVoice::IsLanguageSkill(skillId))
            sProximityVoice->OnLanguagesChanged(player);
    }

    void OnPlayerMapChanged(Player* player) override
    {
        sProximityVoice->OnWorldChanged(player);
    }

    void OnPlayerUpdateZone(Player* player, uint32 /*newZone*/, uint32 /*newArea*/) override
    {
        sProximityVoice->OnWorldChanged(player);
    }

    // Whoever heard this character recently is told, so their addon can keep the plate
    // through the fight. The manager decides whether anyone did.
    void OnPlayerEnterCombat(Player* player, Unit* /*enemy*/) override
    {
        sProximityVoice->OnCombatChanged(player, true);
    }

    void OnPlayerLeaveCombat(Player* player) override
    {
        sProximityVoice->OnCombatChanged(player, false);
    }
};

void AddSC_proximity_voice_scripts()
{
    new ProximityVoice_WorldScript();
    new ProximityVoice_PlayerScript();
}
