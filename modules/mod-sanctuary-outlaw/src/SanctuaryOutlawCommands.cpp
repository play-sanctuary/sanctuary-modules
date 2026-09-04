/*
 * mod-sanctuary-outlaw - in-game commands
 *
 * The addon button is the control surface players will actually use, but it only issues
 * these, so everything it can do stays reachable from chat by anyone who has turned the
 * addon off.
 */

#include "SanctuaryOutlaw.h"

#include "Chat.h"
#include "CommandScript.h"
#include "Player.h"
#include "RBAC.h"

using namespace Acore::ChatCommands;

namespace
{
    // Added by this module's auth SQL and linked under "Role: Player Commands".
    // 100001 belongs to mod-proximity-voice.
    constexpr uint32 RBAC_PERM_COMMAND_OUTLAW = 100002;

    /// Turns a refusal into something worth reading. Ok returns nullptr.
    char const* Explain(SanctuaryOutlaw::Result result)
    {
        using Result = SanctuaryOutlaw::Result;

        switch (result)
        {
            case Result::Ok:
                return nullptr;
            case Result::Disabled:
                return "Outlawry is switched off on this realm.";
            case Result::NoPlayer:
                return "That character is not online.";
            case Result::GameMaster:
                return "Game masters cannot be outlaws. Try |cffffffff.gm off|r first.";
            case Result::AlreadyOutlaw:
                return "You are already an outlaw.";
            case Result::NotOutlaw:
                return "That character is not an outlaw.";
            case Result::AlreadyReleasing:
                return "You have already asked to stand down. Wait for it to take.";
            case Result::InCombat:
                return "Not in the middle of a fight. Break off first.";
            case Result::Sentenced:
                return "You are serving a sentence. It ends when it ends.";
            default:
                return "That did not work.";
        }
    }
}

class sanctuary_outlaw_commandscript : public CommandScript
{
public:
    sanctuary_outlaw_commandscript() : CommandScript("sanctuary_outlaw_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable outlawCommandTable =
        {
            { "on",     HandleOutlawOnCommand,     RBAC_PERM_COMMAND_OUTLAW,                 Console::No },
            { "off",    HandleOutlawOffCommand,    RBAC_PERM_COMMAND_OUTLAW,                 Console::No },
            { "status", HandleOutlawStatusCommand, RBAC_PERM_COMMAND_OUTLAW,                 Console::No },
            { "set",    HandleOutlawSetCommand,    rbac::RBAC_PERM_COMMAND_MODIFY_FACTION,   Console::No },
            { "clear",  HandleOutlawClearCommand,  rbac::RBAC_PERM_COMMAND_MODIFY_FACTION,   Console::No },
            { "check",  HandleOutlawCheckCommand,  rbac::RBAC_PERM_COMMAND_MODIFY_FACTION,   Console::No },
            // Bare `.outlaw` reports rather than toggling: a toggle you cannot see the
            // state of is a good way to walk into a city still flagged.
            { "",       HandleOutlawStatusCommand, RBAC_PERM_COMMAND_OUTLAW,                 Console::No }
        };

        static ChatCommandTable commandTable = { { "outlaw", outlawCommandTable } };
        return commandTable;
    }

    static bool HandleOutlawStatusCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        handler->PSendSysMessage("{}", SanctuaryOutlaw::Describe(player));
        return true;
    }

    static bool HandleOutlawOnCommand(ChatHandler* handler, Optional<uint32> minutes)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        uint32 requested = minutes.value_or(0);

        if (requested > SanctuaryOutlaw::MaxSentenceMinutes())
            requested = SanctuaryOutlaw::MaxSentenceMinutes();

        SanctuaryOutlaw::Result result = SanctuaryOutlaw::Flag(player, requested);

        if (char const* problem = Explain(result))
        {
            handler->SendErrorMessage(problem);
            return false;
        }

        // One line, not two. Describe() opens with "You are an outlaw" as well, so calling
        // both said the same thing twice. It stays the single source of truth for state -
        // `.outlaw` on its own - and the action handlers no longer restate it.
        if (requested)
            handler->PSendSysMessage("|cffff4040You are an outlaw|r for the next {} minute(s). "
                                     "Anyone may raise a hand to you, the guards included.", requested);
        else
            handler->PSendSysMessage("|cffff4040You are an outlaw.|r Anyone may raise a hand to you now, "
                                     "and the guards will not look kindly on you.");

        return true;
    }

    static bool HandleOutlawOffCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        SanctuaryOutlaw::Result result = SanctuaryOutlaw::Release(player);

        if (char const* problem = Explain(result))
        {
            handler->SendErrorMessage(problem);
            return false;
        }

        handler->PSendSysMessage("{}", SanctuaryOutlaw::Describe(player));
        return true;
    }

    /// `.outlaw set <player> [minutes]` - sentencing somebody who will not flag themselves.
    static bool HandleOutlawSetCommand(ChatHandler* handler, Optional<PlayerIdentifier> target, Optional<uint32> minutes)
    {
        if (!target)
            target = PlayerIdentifier::FromTargetOrSelf(handler);

        if (!target)
            return false;

        Player* subject = target->GetConnectedPlayer();

        if (!subject)
        {
            handler->SendErrorMessage("{} is not online. A sentence has to be handed down in person.",
                                      target->GetName());
            return false;
        }

        uint32 requested = minutes.value_or(0);

        if (requested > SanctuaryOutlaw::MaxSentenceMinutes())
            requested = SanctuaryOutlaw::MaxSentenceMinutes();

        SanctuaryOutlaw::Result result = SanctuaryOutlaw::Flag(subject, requested);

        if (char const* problem = Explain(result))
        {
            handler->SendErrorMessage(problem);
            return false;
        }

        if (requested)
        {
            ChatHandler(subject->GetSession()).PSendSysMessage(
                "|cffff4040You have been declared an outlaw|r for the next {} minute(s). "
                "You cannot stand down early.", requested);

            handler->PSendSysMessage("{} is an outlaw for {} minute(s).", subject->GetName(), requested);
        }
        else
        {
            ChatHandler(subject->GetSession()).PSendSysMessage(
                "|cffff4040You have been declared an outlaw.|r Anyone may raise a hand to you, "
                "the guards included.");

            handler->PSendSysMessage("{} is now an outlaw.", subject->GetName());
        }

        return true;
    }

    /*
     * `.outlaw check <player>` - why you can or cannot raise a hand to them.
     *
     * "They are flagged but I still cannot attack them" is the report this exists to
     * answer, and it has several unrelated causes that look identical from in game: the
     * two of you are in a group, one of you is standing in a sanctuary, one of you is in
     * game master mode, or a flag genuinely did not take. Rather than guess, this prints
     * the whole chain and finishes with the core's own verdict.
     */
    static bool HandleOutlawCheckCommand(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        Player* self = handler->GetPlayer();

        if (!self)
            return false;

        if (!target)
            target = PlayerIdentifier::FromTarget(handler);

        if (!target)
        {
            handler->SendErrorMessage("Select somebody, or name them.");
            return false;
        }

        Player* other = target->GetConnectedPlayer();

        if (!other)
        {
            handler->SendErrorMessage("{} is not online.", target->GetName());
            return false;
        }

        auto yesno = [](bool value) { return value ? "|cff7fb069yes|r" : "|cffff4040no|r"; };

        handler->PSendSysMessage("--- {} ---", other->GetName());
        handler->PSendSysMessage("outlaw: {}   state: {}   faction: {}   (yours: {})",
            yesno(SanctuaryOutlaw::IsOutlaw(other)),
            !SanctuaryOutlaw::IsOutlaw(other) ? "-"
                : (SanctuaryOutlaw::IsHostile(other) ? "|cffff4040hostile|r - the watch is hunting them"
                                                     : "|cffd9a441wanted|r - attackable, not hunted"),
            other->GetFaction(), self->GetFaction());
        handler->PSendSysMessage("ffa: {}   pvp: {}   strike-unflagged: {}   ignores-reputation: {}",
            yesno(other->HasByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_FFA_PVP)),
            yesno(other->IsPvP()),
            yesno(other->HasByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_UNK1)),
            yesno(other->HasUnitFlag2(UNIT_FLAG2_IGNORE_REPUTATION)));

        // The three that silently beat every flag above, in the order the core checks them.
        handler->PSendSysMessage("in your group: {}   sanctuary (them/you): {} / {}   gm mode (them/you): {} / {}",
            yesno(self->IsInRaidWith(other)),
            yesno(other->IsInSanctuary()), yesno(self->IsInSanctuary()),
            yesno(other->IsGameMaster()), yesno(self->IsGameMaster()));

        handler->PSendSysMessage("you may strike them: {}   they may strike you: {}",
            yesno(self->IsValidAttackTarget(other)), yesno(other->IsValidAttackTarget(self)));

        return true;
    }

    /// `.outlaw clear <player>` - a pardon, with none of the release delay.
    static bool HandleOutlawClearCommand(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        if (!target)
            target = PlayerIdentifier::FromTargetOrSelf(handler);

        if (!target)
            return false;

        Player* subject = target->GetConnectedPlayer();

        if (!subject)
        {
            handler->SendErrorMessage("{} is not online.", target->GetName());
            return false;
        }

        SanctuaryOutlaw::Result result = SanctuaryOutlaw::Revoke(subject);

        if (char const* problem = Explain(result))
        {
            handler->SendErrorMessage(problem);
            return false;
        }

        handler->PSendSysMessage("{} is pardoned.", subject->GetName());
        return true;
    }
};

void AddSC_sanctuary_outlaw_commandscript()
{
    new sanctuary_outlaw_commandscript();
}
