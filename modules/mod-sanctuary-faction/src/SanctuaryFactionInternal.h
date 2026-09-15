/*
 * mod-sanctuary-faction - the store, shared between this module's own source files.
 *
 * NOT for other modules. SanctuaryFaction.h is the public face and answers questions;
 * this one changes things, and the difference matters: every mutation below assumes its
 * caller has already decided the actor was allowed to ask. The permission check lives at
 * the command and addon entry points, never in here.
 */

#ifndef MOD_SANCTUARY_FACTION_INTERNAL_H
#define MOD_SANCTUARY_FACTION_INTERNAL_H

#include "SanctuaryFaction.h"

#include "ObjectGuid.h"

#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;

namespace SanctuaryFactionStore
{
    struct Rank
    {
        std::string name;
        uint32 permissions = 0;
        std::unordered_set<uint32> spells;     ///> granted at this rank AND ABOVE
    };

    struct Faction
    {
        uint32 id = 0;
        std::string name;
        std::string description;
        uint32 founded = 0;
        ObjectGuid::LowType founder = 0;

        // Ordered, because the ladder is shown as a ladder and because Reconcile walks it
        // from the bottom to the member's rank. A gap is legal - a game master editing
        // ranks live will make one - and reads as a rank with no name and no powers.
        std::map<uint8, Rank> ranks;
    };

    struct Member
    {
        uint32 faction = 0;
        uint8 rank = 0;
        uint32 joined = 0;
    };

    /// Every faction on the realm, by id. Loaded once at startup, then kept in step.
    Faction const* Find(uint32 id);

    /// By name, case-insensitively, because it is typed at a command. Null if no match.
    Faction const* FindByName(std::string const& name);

    /// Membership for a character guid, online or not. Null for somebody in no faction.
    Member const* MemberOf(ObjectGuid::LowType guid);

    /// Everybody in a faction, lowest rank first. Guids, so the caller can decide what to
    /// show for each - which on this realm means asking mod-sanctuary-identity.
    std::vector<ObjectGuid::LowType> Roster(uint32 faction);

    /// Reads both tables from scratch. Startup, and `.faction reload`.
    void LoadAll();

    // --- mutations. Each writes through to the database and to the cache together. ---

    /// Creates a faction with an empty ladder. Returns its id, or 0 if the name is taken.
    uint32 Create(std::string const& name, std::string const& description,
                  ObjectGuid::LowType founder);

    /// Removes a faction, its ladder and its roster. Members online are reconciled, so the
    /// spells the faction was lending them are taken back on the spot.
    bool Disband(uint32 faction);

    /// Creates or rewrites one rung of the ladder.
    bool SetRank(uint32 faction, uint8 rank, std::string const& name, uint32 permissions);

    /*
     * Renames an existing rung, leaving what it grants alone.
     *
     * Split from SetRank because the two are different acts with different owners: a game
     * master decides what a rank may DO, and the faction decides what it is CALLED. Refuses
     * a rung that does not exist rather than creating one - adding rungs is still a game
     * master's, and a typo in a rank number should not silently grow the ladder.
     */
    bool SetRankName(uint32 faction, uint8 rank, std::string const& name);

    bool AddRankSpell(uint32 faction, uint8 rank, uint32 spell);
    bool RemoveRankSpell(uint32 faction, uint8 rank, uint32 spell);

    /// Puts a character in a faction at a rank, moving them out of any other first.
    bool SetMembership(Player* player, uint32 faction, uint8 rank);

    /// Same, for somebody who is not logged in. Their spells are settled at their next
    /// login rather than now, which is the whole reason Reconcile runs there.
    bool SetMembershipOffline(ObjectGuid::LowType guid, uint32 faction, uint8 rank);

    /// Takes them out of whatever they are in. Safe on somebody in nothing.
    bool ClearMembership(Player* player);
    bool ClearMembershipOffline(ObjectGuid::LowType guid);

    /*
     * Settles what this character should know against what this module has lent them.
     *
     * The one function that must never be skipped. Ranks change while people are offline,
     * so this runs at every login as well as at every change - and it is written to be
     * idempotent so that running it twice costs nothing.
     */
    void Reconcile(Player* player);

    /// Reconciles every member of one faction who is currently online. What a rank edit
    /// needs: the people it affects are already standing in the world holding spells the
    /// ladder no longer says they may have.
    void ReconcileFaction(uint32 faction);

    /*
     * The rung above or below one that exists, or nothing at the end of the ladder.
     *
     * Promotion moves to the next DEFINED rank rather than to rank + 1. A ladder is allowed
     * to have holes in it - a game master editing ranks live will make one - and a raw +1
     * would land somebody on a rung with no name and no powers, which reads as the module
     * having eaten their rank.
     */
    bool NextRankUp(uint32 faction, uint8 from, uint8& next);
    bool NextRankDown(uint32 faction, uint8 from, uint8& next);

    /*
     * Tells this player's addon window what it now is.
     *
     * Implemented in SanctuaryFactionAddon.cpp and called from Reconcile(), so a window
     * that is open when somebody is promoted redraws itself. Without it the window is
     * correct only at the moment it was opened, and a player who was just promoted has to
     * close and reopen it to find out - which reads as the promotion not having worked.
     *
     * Safe for a player with no addon: it is a whisper to themselves in the addon
     * language, which a client with nothing listening drops.
     */
    void PushState(Player* player);

    /*
     * Pads this player's next roster with invented members. A testing aid, nothing else.
     *
     * A roster of thirty cannot be produced by hand: every member has to be a real
     * character, and the window only names people who are logged in, so thirty rows would
     * mean thirty clients. The padding is generated when the roster is sent and stored
     * nowhere - no rows, no guids, no character records - so it cannot outlive the session
     * or be mistaken for a real member by anything that reads the database.
     *
     * `count` of 0 turns it off.
     */
    void SetDummyRoster(ObjectGuid::LowType guid, uint32 count);
}

#endif // MOD_SANCTUARY_FACTION_INTERNAL_H
