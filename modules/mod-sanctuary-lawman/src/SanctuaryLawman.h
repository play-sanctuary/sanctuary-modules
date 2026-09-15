/*
 * mod-sanctuary-lawman - what the rest of the realm may ask about lawmen.
 *
 * Static builds put every module's source directory on the include path, so a sibling
 * module can include this by name with no CMake change.
 */

#ifndef SANCTUARY_LAWMAN_H
#define SANCTUARY_LAWMAN_H

#include "Define.h"

#include <string>

class Player;

namespace SanctuaryLawman
{
    bool IsEnabled();

    /// Whether this character holds the office at all, on duty or not.
    bool IsLawman(Player const* player);

    /// Whether this character is currently on duty and carrying the office's powers.
    bool IsOnDuty(Player const* player);

    /// "Guard" or "Grunt", by the character's racial side. Empty when they hold no office.
    std::string TitleOf(Player const* player);

    enum class Result
    {
        Ok,
        Disabled,
        NoPlayer,
        NotLawman,
        AlreadyLawman,
        AlreadyOnDuty,
        AlreadyOffDuty
    };

    /// Grants or revokes the office. Game masters only; the commands enforce that.
    Result Appoint(Player* player);
    Result Dismiss(Player* player);

    /// Goes on or off duty. Only meaningful for someone who holds the office.
    Result StartDuty(Player* player);
    Result EndDuty(Player* player);

    /// One line of plain English about where this character stands.
    std::string Describe(Player const* player);

    /// Pushes machine-readable state to the character's addon.
    void SendState(Player* player);

    /// Whether this character is currently shackled by somebody.
    bool IsShackled(Player const* player);

    /// Whether this search would be allowed, without doing it. Returns the refusal, or
    /// null if it would go ahead. The addon asks this to decide whether to offer a button.
    char const* SearchRefusal(Player* searcher, Player* prisoner);

    /// Reads a prisoner's pack out to the searcher. Returns the refusal, or null if the
    /// search happened. Every rule lives here so the gloves and the addon's button agree.
    char const* Search(Player* searcher, Player* prisoner);
}

#endif // SANCTUARY_LAWMAN_H
