/*
 * mod-sanctuary-outlaw - what the rest of the realm may ask about outlaws.
 *
 * Static builds put every module's source directory on the include path, so a sibling
 * module can include this by name with no CMake change - the same way
 * mod-proximity-voice includes SanctuaryIdentity.h.
 */

#ifndef SANCTUARY_OUTLAW_H
#define SANCTUARY_OUTLAW_H

#include "Define.h"

#include <string>

class Player;

namespace SanctuaryOutlaw
{
    /// False when the module is switched off, so callers need no config of their own.
    bool IsEnabled();

    /// Whether this character is currently open to violence. Offline characters are
    /// never outlaws as far as this returns: the set is built at login.
    bool IsOutlaw(Player const* player);

    /// Whether they have struck somebody since being flagged, which is what turns the
    /// town watch against them. An outlaw who has not is attackable but not hunted.
    bool IsHostile(Player const* player);

    /// Why a request did not do what was asked. Ok also covers "that was already true".
    enum class Result
    {
        Ok,
        Disabled,
        NoPlayer,
        GameMaster,
        AlreadyOutlaw,
        NotOutlaw,
        AlreadyReleasing,
        InCombat,
        Sentenced
    };

    /// Flags a character. `minutes` of 0 means until they ask to stop.
    Result Flag(Player* player, uint32 minutes = 0);

    /// Begins the release timer. Refused in PvP combat, so the flag cannot be used as an
    /// escape hatch mid-fight.
    Result Release(Player* player);

    /// Drops the flag immediately, ignoring the release timer.
    ///
    /// `why` is appended to what the pardoned player is told, so a lawman's pardon does
    /// not claim to have come from a game master. Null keeps the game master wording.
    Result Revoke(Player* player, char const* why = nullptr);

    /// One line of plain English about where this character stands.
    std::string Describe(Player const* player);

    /// Pushes machine-readable state to the character's addon.
    void SendState(Player* player);

    /// Longest sentence the realm allows, in minutes. Commands clamp to it rather than
    /// refusing, so a fat-fingered `.outlaw set bob 99999` is a long sentence, not a
    /// permanent one nobody notices.
    uint32 MaxSentenceMinutes();
}

#endif // SANCTUARY_OUTLAW_H
