/*
 * mod-sanctuary-lawman - in-game commands
 *
 * The addon button is what a lawman will actually press, but it only issues these, so
 * everything stays reachable from chat by anyone who has turned the addon off.
 */

#include "SanctuaryLawman.h"

#include "SanctuaryIdentity.h"
#include "SanctuaryOutlaw.h"

#include "Chat.h"
#include "CommandScript.h"
#include "Player.h"
#include "RBAC.h"
#include "WorldSession.h"

using namespace Acore::ChatCommands;

namespace
{
    // Added by this module's auth SQL and linked under "Role: Player Commands".
    // 100001 is voice, 100002 is outlaw.
    constexpr uint32 RBAC_PERM_COMMAND_LAWMAN = 100003;

    char const* Explain(SanctuaryLawman::Result result)
    {
        using Result = SanctuaryLawman::Result;

        switch (result)
        {
            case Result::Ok:
                return nullptr;
            case Result::Disabled:
                return "The watch is not keeping order on this realm.";
            case Result::NoPlayer:
                return "That character is not online.";
            case Result::NotLawman:
                return "That character holds no office.";
            case Result::AlreadyLawman:
                return "They already hold the office.";
            case Result::AlreadyOnDuty:
                return "You are already on duty.";
            case Result::AlreadyOffDuty:
                return "You are already off duty.";
            default:
                return "That did not work.";
        }
    }
}

class sanctuary_lawman_commandscript : public CommandScript
{
public:
    sanctuary_lawman_commandscript() : CommandScript("sanctuary_lawman_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable lawmanCommandTable =
        {
            { "on",     HandleLawmanOnCommand,     RBAC_PERM_COMMAND_LAWMAN,               Console::No },
            { "off",    HandleLawmanOffCommand,    RBAC_PERM_COMMAND_LAWMAN,               Console::No },
            { "status", HandleLawmanStatusCommand, RBAC_PERM_COMMAND_LAWMAN,               Console::No },
            { "set",    HandleLawmanSetCommand,    rbac::RBAC_PERM_COMMAND_MODIFY_FACTION, Console::No },
            { "remove", HandleLawmanRemoveCommand, rbac::RBAC_PERM_COMMAND_MODIFY_FACTION, Console::No },
            // Bare `.lawman` reports rather than toggling, for the same reason `.outlaw`
            // does: a toggle whose state you cannot see is one you walk around wearing.
            { "",       HandleLawmanStatusCommand, RBAC_PERM_COMMAND_LAWMAN,               Console::No }
        };

        static ChatCommandTable commandTable = { { "lawman", lawmanCommandTable } };
        return commandTable;
    }

    static bool HandleLawmanStatusCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        handler->PSendSysMessage("{}", SanctuaryLawman::Describe(player));
        return true;
    }

    static bool HandleLawmanOnCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        SanctuaryLawman::Result result = SanctuaryLawman::StartDuty(player);

        if (char const* problem = Explain(result))
        {
            handler->SendErrorMessage(problem);
            return false;
        }

        handler->PSendSysMessage("You are on duty as a |cff7fb069{}|r. Wear the tabard well.",
                                 SanctuaryLawman::TitleOf(player));
        return true;
    }

    static bool HandleLawmanOffCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        SanctuaryLawman::Result result = SanctuaryLawman::EndDuty(player);

        if (char const* problem = Explain(result))
        {
            handler->SendErrorMessage(problem);
            return false;
        }

        handler->PSendSysMessage("Off duty. Your writ stays in your pack.");
        return true;
    }

    /// `.lawman set <player>` - appointing someone to the office.
    static bool HandleLawmanSetCommand(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        if (!target)
            target = PlayerIdentifier::FromTargetOrSelf(handler);

        if (!target)
            return false;

        Player* subject = target->GetConnectedPlayer();

        if (!subject)
        {
            handler->SendErrorMessage("{} is not online. The office is handed over in person.",
                                      target->GetName());
            return false;
        }

        SanctuaryLawman::Result result = SanctuaryLawman::Appoint(subject);

        if (char const* problem = Explain(result))
        {
            handler->SendErrorMessage(problem);
            return false;
        }

        std::string const title = SanctuaryLawman::TitleOf(subject);

        ChatHandler(subject->GetSession()).PSendSysMessage(
            "|cff7fb069You have been appointed a {}.|r", title);

        handler->PSendSysMessage("{} is now a {}.", subject->GetName(), title);
        return true;
    }

    /// `.lawman remove <player>` - taking the office back.
    static bool HandleLawmanRemoveCommand(ChatHandler* handler, Optional<PlayerIdentifier> target)
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

        SanctuaryLawman::Result result = SanctuaryLawman::Dismiss(subject);

        if (char const* problem = Explain(result))
        {
            handler->SendErrorMessage(problem);
            return false;
        }

        ChatHandler(subject->GetSession()).PSendSysMessage("You have been relieved of your office.");
        handler->PSendSysMessage("{} no longer holds the office.", subject->GetName());
        return true;
    }
};

void AddSC_sanctuary_lawman_commandscript()
{
    new sanctuary_lawman_commandscript();
}
