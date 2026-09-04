/*
 * mod-proximity-voice - in-game commands
 *
 * The companion app is the real control surface, but everything it can do is
 * also reachable from chat so a player can fix their settings without alt-tabbing.
 */

#include "ProximityVoice.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Player.h"
#include "RBAC.h"
#include <algorithm>

using namespace Acore::ChatCommands;

namespace
{
    // Added by this module's auth SQL and linked under "Role: Player Commands".
    constexpr uint32 RBAC_PERM_COMMAND_VOICE = 100001;
}

class proximity_voice_commandscript : public CommandScript
{
public:
    proximity_voice_commandscript() : CommandScript("proximity_voice_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable voiceCommandTable =
        {
            { "status", HandleVoiceStatusCommand, RBAC_PERM_COMMAND_VOICE,               Console::No  },
            { "range",  HandleVoiceRangeCommand,  RBAC_PERM_COMMAND_VOICE,               Console::No  },
            { "lang",   HandleVoiceLangCommand,   RBAC_PERM_COMMAND_VOICE,               Console::No  },
            { "langs",  HandleVoiceLangsCommand,  RBAC_PERM_COMMAND_VOICE,               Console::No  },
            { "token",  HandleVoiceTokenCommand,  RBAC_PERM_COMMAND_VOICE,               Console::No  },
            { "mute",   HandleVoiceMuteCommand,   RBAC_PERM_COMMAND_VOICE,               Console::No  },
            { "unmute", HandleVoiceUnmuteCommand, RBAC_PERM_COMMAND_VOICE,               Console::No  },
            { "info",   HandleVoiceInfoCommand,   rbac::RBAC_PERM_COMMAND_SERVER_INFO,   Console::Yes },
            { "",       HandleVoiceStatusCommand, RBAC_PERM_COMMAND_VOICE,               Console::No  }
        };

        static ChatCommandTable commandTable =
        {
            { "voice", voiceCommandTable }
        };

        return commandTable;
    }

    static bool HandleVoiceStatusCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        ProximityVoice::Session const* session = sProximityVoice->FindSession(player->GetGUID());
        if (!session)
        {
            handler->SendErrorMessage("Proximity voice is not active for this character.");
            return false;
        }

        auto const& config = sProximityVoice->Config();

        handler->PSendSysMessage("|cff00ff96Proximity voice|r");
        handler->PSendSysMessage("  Speaking distance: |cffffffff{:.0f}|r yards (allowed {:.0f} - {:.0f})",
            session->range, config.RangeMin, config.RangeMax);
        handler->PSendSysMessage("  Speaking in: |cffffffff{}|r", ProximityVoice::GetLanguageName(session->voiceLanguage));
        handler->PSendSysMessage("  Microphone: {}", session->muted ? "|cffff4444muted|r" : "|cff00ff96open|r");
        handler->PSendSysMessage("  Voice client: {}", session->clientConnected ? "|cff00ff96connected|r" : "|cffff8800not connected|r");
        handler->PSendSysMessage("  Voice server: |cffffffff{}:{}|r", config.ClientHost, config.ClientPort);
        return true;
    }

    static bool HandleVoiceRangeCommand(ChatHandler* handler, Optional<float> yards)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        auto const& config = sProximityVoice->Config();

        if (!yards)
        {
            handler->PSendSysMessage("Usage: .voice range <{:.0f} - {:.0f}>", config.RangeMin, config.RangeMax);
            return true;
        }

        float const applied = sProximityVoice->SetRange(player, *yards);
        if (applied <= 0.0f)
        {
            handler->SendErrorMessage("Proximity voice is not active for this character.");
            return false;
        }

        if (std::abs(applied - *yards) > 0.01f)
            handler->PSendSysMessage("Speaking distance clamped to |cffffffff{:.0f}|r yards.", applied);
        else
            handler->PSendSysMessage("Speaking distance set to |cffffffff{:.0f}|r yards.", applied);

        return true;
    }

    static bool HandleVoiceLangCommand(ChatHandler* handler, Optional<std::string> name)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        if (!name || name->empty())
        {
            handler->PSendSysMessage("Usage: .voice lang <language>. See .voice langs for what you know.");
            return true;
        }

        uint32 language = 0;
        if (!ProximityVoice::ParseLanguage(*name, language))
        {
            handler->SendErrorMessage("Unknown language \"{}\".", *name);
            return false;
        }

        std::string error;
        if (!sProximityVoice->SetVoiceLanguage(player, language, error))
        {
            handler->SendErrorMessage("{}", error);
            return false;
        }

        handler->PSendSysMessage("You will now speak |cffffffff{}|r.", ProximityVoice::GetLanguageName(language));
        return true;
    }

    static bool HandleVoiceLangsCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        std::vector<uint32> const known = ProximityVoice::CollectKnownLanguages(player);
        if (known.empty())
        {
            handler->PSendSysMessage("You have not learned any languages.");
            return true;
        }

        handler->PSendSysMessage("|cff00ff96Languages you can speak and understand:|r");
        for (uint32 language : known)
            handler->PSendSysMessage("  {}", ProximityVoice::GetLanguageName(language));

        handler->PSendSysMessage("Anyone speaking a language not on this list will sound like noise to you.");
        return true;
    }

    static bool HandleVoiceTokenCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        if (sProximityVoice->IssueToken(player, true).empty())
        {
            handler->SendErrorMessage("Proximity voice is not active for this character.");
            return false;
        }

        return true;
    }

    static bool HandleVoiceMuteCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        sProximityVoice->SetMuted(player, true);
        handler->PSendSysMessage("Voice microphone |cffff4444muted|r.");
        return true;
    }

    static bool HandleVoiceUnmuteCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        sProximityVoice->SetMuted(player, false);
        handler->PSendSysMessage("Voice microphone |cff00ff96open|r.");
        return true;
    }

    static bool HandleVoiceInfoCommand(ChatHandler* handler)
    {
        auto const& config = sProximityVoice->Config();

        handler->PSendSysMessage("Proximity voice: {}", config.Enable ? "enabled" : "disabled");
        handler->PSendSysMessage("  Bridge {}:{} - {}", config.BridgeHost, config.BridgePort,
            sProximityVoice->IsBridgeConnected() ? "connected" : "disconnected");
        handler->PSendSysMessage("  Tracked sessions: {}", sProximityVoice->SessionCount());
        handler->PSendSysMessage("  Range window: {:.0f} - {:.0f} yards (default {:.0f})",
            config.RangeMin, config.RangeMax, config.RangeDefault);
        handler->PSendSysMessage("  Language barrier: {}", config.EnforceLanguage ? "enforced" : "off");
        handler->PSendSysMessage("  Position push interval: {} ms", config.UpdateIntervalMs);
        return true;
    }
};

void AddSC_proximity_voice_commandscript()
{
    new proximity_voice_commandscript();
}
