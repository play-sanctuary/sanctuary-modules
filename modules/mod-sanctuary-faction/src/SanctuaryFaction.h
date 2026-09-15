/*
 * mod-sanctuary-faction - the bit other modules need
 *
 * Player factions: an organisation a character belongs to, with ranks, where the rank is
 * what grants things. Deliberately NOT built on the core's guild system - a character may
 * hold a guild and a faction at once, and the guild tables are untouched here.
 *
 * Deliberately NOT related to Faction.dbc or FactionTemplate.dbc either. Nothing in this
 * module decides who may attack whom; it is membership and rank, which is server data all
 * the way down and therefore creatable at runtime with no client patch and no restart.
 *
 * In a static build every module compiles into one `modules` target with every module's
 * source directory on the public include path, so including this from another module needs
 * no CMake change - the same way mod-proximity-voice includes SanctuaryIdentity.h.
 *
 * Callers should tolerate the module being switched off, which IsEnabled() reports; every
 * query below answers "no" rather than throwing when it is.
 */

#ifndef MOD_SANCTUARY_FACTION_H
#define MOD_SANCTUARY_FACTION_H

#include "Define.h"

#include <string>

class Player;

namespace SanctuaryFaction
{
    /// False when the module is switched off, so callers need no config of their own.
    bool IsEnabled();

    /*
     * What a rank lets you do to the FACTION.
     *
     * Held in `sanctuary_faction_rank`.`permissions`. These never leave this module.
     */
    enum Permission : uint32
    {
        PERM_NONE    = 0x00,
        PERM_INVITE  = 0x01,
        PERM_KICK    = 0x02,
        PERM_PROMOTE = 0x04,
        PERM_DEMOTE  = 0x08,
        PERM_EDIT    = 0x10,   ///> rename, redescribe, rewrite the rank ladder
        PERM_DISBAND = 0x20
    };

    /*
     * There is no second mask.
     *
     * An earlier draft had a `capabilities` mask beside this one, letting a rank grant
     * powers the other modules already check for - shackling without irons, opening any
     * strongbox. It was removed deliberately: what a faction gives you is a SPELL, and
     * one kind of reward is easier to reason about, to balance and to explain than two.
     *
     * A power that is worth granting is worth being a spell somebody can see in their
     * spellbook, rather than an invisible boolean that changes what a door does.
     */

    /// The faction this character belongs to, or 0 for none.
    uint32 FactionOf(Player const* player);

    /// A faction's name, or an empty string if there is no such faction.
    std::string NameOf(uint32 faction);

    /// Their rank within it. Meaningless unless FactionOf() is non-zero; 0 is the lowest.
    uint8 RankOf(Player const* player);

    /// The name their faction gives that rank - "Recruit", "Underboss" - or empty.
    std::string RankNameOf(Player const* player);

    /*
     * Whether this character's rank may do this to the faction.
     *
     * False for a player in no faction, for a rank that does not carry it, and when the
     * module is off - so a call site needs no config or null checks of its own.
     */
    bool May(Player const* player, Permission permission);
}

#endif // MOD_SANCTUARY_FACTION_H
