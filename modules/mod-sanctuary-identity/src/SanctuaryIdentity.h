/*
 * mod-sanctuary-identity - the bit other modules need
 *
 * Anything that puts a player's name in front of another player has to ask first, or it
 * becomes a way around the disguise. mod-proximity-voice is the live example: it names the
 * speaker to everyone in earshot over the addon channel, which would hand out real names
 * no matter what the client was told to draw.
 *
 * In a static build every module compiles into one `modules` target with every module's
 * source directory on the public include path, so including this from another module needs
 * no CMake change. Callers should still tolerate the disguise being switched off, which
 * IsDisguising() reports.
 */

#ifndef MOD_SANCTUARY_IDENTITY_H
#define MOD_SANCTUARY_IDENTITY_H

#include <string>

class Player;

namespace SanctuaryIdentity
{
    /// Whether names are being disguised at all. False means LabelFor is always the real name.
    bool IsDisguising();

    /// Whether this viewer has been introduced to this subject. A viewer always knows itself.
    bool KnowsName(Player* viewer, Player* subject);

    /// What this viewer should be shown: the real name if they know it, otherwise the alias.
    std::string LabelFor(Player* viewer, Player* subject);
}

#endif // MOD_SANCTUARY_IDENTITY_H
