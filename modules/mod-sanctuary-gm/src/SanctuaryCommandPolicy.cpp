/*
 * mod-sanctuary-gm - who may TYPE Sanctuary's commands
 *
 * The realm is meant to be driven by its windows. Every player-facing thing Sanctuary adds
 * - the voice panel, the outlaw flag, the lawman's writ, carrying somebody, the disguise,
 * the faction roster - has a button, and this refuses the typed form of the same command so
 * that the buttons are the way in rather than one of two ways.
 *
 *
 * WHAT THIS IS NOT. It is not a security measure and must never be mistaken for one. Every
 * one of these commands still checks its own rules when it runs: rank, range, whether you
 * hold the office, whether the person is even there. A player who edits their addon to send
 * whatever they like gets exactly what they would have got by typing it before this
 * existed. This is about there being one obvious way to do a thing, not about stopping
 * anybody.
 *
 *
 * HOW IT TELLS THEM APART. AddonChannelCommandHandler::IsHumanReadable() returns false for
 * the 'i' opcode, which is what every Sanctuary addon sends and what a person typing never
 * produces - a typed command arrives on a plain ChatHandler, whose IsHumanReadable() is the
 * base's `true`. So the button and the keyboard are distinguishable, and only the keyboard
 * is refused.
 *
 * That is also the whole reason the addons had to stop sending SendChatMessage(".search")
 * before this could exist: four buttons were typing on the player's behalf, and would have
 * been refused along with the player.
 *
 *
 * WHO IS EXEMPT.
 *
 *   the console        - has no window to use, and is not a player
 *   game masters       - the panels do not cover a fraction of what they need, and the
 *                        realm is run from a chat box. Tested with the same permission the
 *                        game master half of these command trees is gated on.
 *
 * A game master will therefore notice no difference at all, which is worth knowing when
 * testing this: log in as an ordinary character to see it work.
 */

#include "AllCommandScript.h"
#include "Chat.h"
#include "Config.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <string>
#include <unordered_set>

namespace
{
    bool g_guiOnly = true;

    /*
     * The commands the windows own.
     *
     * Top-level names only. Sub-commands are not listed because a game master is exempt
     * anyway and an ordinary player cannot reach the game master sub-commands of these
     * trees - `.faction create` is already refused by rbac long before it reaches here.
     *
     * `search`, `bleedout` and `getdown` are in the list rather than under a parent because
     * that is how they are registered: they are their own commands, not sub-commands of
     * `lawman` and `carry`.
     */
    std::unordered_set<std::string> const g_windowed = {
        "voice",
        "outlaw",
        "lawman",
        "search",
        "carry",
        "bleedout",
        "getdown",
        "disguise",
        "faction"
    };

    /// The first word of the command, lowercased. Empty if there is not one.
    std::string FirstWord(std::string_view text)
    {
        size_t const start = text.find_first_not_of(" \t");

        if (start == std::string_view::npos)
            return {};

        size_t const end = text.find_first_of(" \t", start);

        std::string word{ text.substr(start, end == std::string_view::npos ? end : end - start) };

        for (char& c : word)
            c = char(std::tolower(static_cast<unsigned char>(c)));

        return word;
    }
}

class sanctuary_command_policy : public AllCommandScript
{
public:
    sanctuary_command_policy() : AllCommandScript("sanctuary_command_policy",
        { ALLCOMMANDHOOK_ON_TRY_EXECUTE_COMMAND }) { }

    bool OnTryExecuteCommand(ChatHandler& handler, std::string_view cmdStr) override
    {
        if (!g_guiOnly)
            return true;

        // Arrived from an addon, which is the way we want it. Nothing to do.
        if (!handler.IsHumanReadable())
            return true;

        if (handler.IsConsole())
            return true;

        Player* player = handler.GetPlayer();

        if (!player || !player->GetSession())
            return true;

        // Game masters keep their keyboards.
        if (player->GetSession()->HasPermission(rbac::RBAC_PERM_COMMAND_MODIFY_FACTION))
            return true;

        if (!g_windowed.count(FirstWord(cmdStr)))
            return true;

        /*
         * Told, not ignored.
         *
         * Somebody typing one of these has either come from a guide written before the
         * windows existed or has an addon that is not loading, and both of those want a
         * sentence rather than silence. A command that simply did nothing would be reported
         * as the realm being broken, which is very nearly what it would mean.
         */
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cffd8b46aSanctuary:|r that is done from its window, not typed. "
            "If no window opened, the addons may not have loaded - restart the launcher.");

        return false;
    }
};

class sanctuary_command_policy_worldscript : public WorldScript
{
public:
    sanctuary_command_policy_worldscript() : WorldScript("sanctuary_command_policy_worldscript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_guiOnly = sConfigMgr->GetOption<bool>("SanctuaryGM.GuiOnlyCommands", true);
    }
};

void AddSC_sanctuary_command_policy()
{
    new sanctuary_command_policy();
    new sanctuary_command_policy_worldscript();
}
