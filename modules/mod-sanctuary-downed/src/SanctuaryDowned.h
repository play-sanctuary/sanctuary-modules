/*
 * mod-sanctuary-downed - carrying a fallen player
 *
 * The carry is the part worth proving before anything else, because the one question the
 * data cannot answer is what it looks like. Everything here works on a living player, which
 * is deliberate: a downed player is kept technically alive precisely so this works.
 */

#ifndef MOD_SANCTUARY_DOWNED_H
#define MOD_SANCTUARY_DOWNED_H

#include "Define.h"
#include "ObjectGuid.h"

class Player;
class Unit;

namespace SanctuaryDowned
{
    enum class Result
    {
        Ok,
        Disabled,
        NoTarget,
        Self,
        TooFar,
        AlreadyCarrying,
        AlreadyCarried,
        CarrierIsCarried,
        NotCarrying,
        Mounted,
        InVehicle,
        Busy,
        // The core will not seat a passenger who is in combat when the vehicle is another
        // player (Unit.cpp:15653), and it refuses in silence - the ride aura is applied,
        // _EnterVehicle returns early, and the seat is simply never taken. Caught here so
        // it reads as a rule with a reason rather than a lift that does nothing.
        TargetInCombat,
        // Told apart because they fail for opposite reasons: NoSeat is a configuration
        // fault on this realm, SeatRefused is the core declining at the last moment.
        NoSeat,
        SeatRefused,
        Failed
    };

    /// Turns a refusal into something worth reading. Ok returns nullptr.
    char const* Explain(Result result);

    /// Whether this lift would be allowed, without attempting it.
    Result CanPickUp(Player* carrier, Player* target);

    /// Teaches the lift to anybody who does not have it. Every class gets it.
    void TeachCarrySpell(Player* player);

    /// Starts the cast. The lift itself happens when the bar finishes.
    Result BeginPickUp(Player* carrier, Player* target);

    /// Lifts the target onto the carrier's shoulder, skipping the cast.
    Result PickUp(Player* carrier, Player* target);

    /// Sets down whoever the carrier is holding.
    Result PutDown(Player* carrier);

    /// The carrier holding this player, or an empty guid.
    ObjectGuid CarrierOf(Player* passenger);

    /// Whoever this player is holding, or an empty guid.
    ObjectGuid CarriedBy(Player* carrier);

    /// True while this player is down: alive at 1 HP, rooted and prone.
    bool IsDowned(Player const* player);

    /// Puts a player out of the fight instead of killing them.
    void GoDown(Player* player, Unit* feller);

    /// Back on their feet. Safe to call on somebody who was never down.
    void StandUp(Player* player, bool announce);

    /// Brings a downed player round, restoring some health. False if they were not down.
    bool Revive(Player* player, Player* rescuer);

    /// The downed player stops waiting and dies. False if they were not down.
    bool GiveUp(Player* player);

    /// Removes the auras that advertise the downed state, and nothing else. For a login
    /// sweep, where a previous session's auras can outlive the state itself.
    void StripDownedAuras(Player* player);

    /// Forgets the downed state without touching the player. For logout and death.
    void ClearDowned(Player* player);

    /// Advances every bleed-out clock. Carried players are skipped.
    void BleedOut(uint32 diff);

    /// Gets a conscious passenger down. False if they are down, or nobody is carrying
    /// them - being unable to is what makes somebody carryable against their will.
    bool GetDown(Player* player);

    /// Hands a downed player's clock to their next session. Called at logout, before
    /// StandUp, which is what erases the state it reads.
    void SaveDowned(Player* player);

    /// Puts back a state saved at logout, and takes the row away. Does nothing if there
    /// is none, which is every normal login.
    void RestoreDowned(Player* player);

    /// Drops any carry this player is part of, either end. Safe to call blind.
    void Release(Player* player);

    /// Puts back anybody who has fallen out of their seat - dismounting ejects them.
    void ReseatLapsed();

    /// Overrides which vehicle this carrier uses, for trying offsets. 0 clears it.
    void SetSeatOverride(Player* carrier, uint32 vehicleId);
    uint32 GetSeatOverride(Player* carrier);

    /// The same, consulted only while the carrier is mounted. 0 clears it.
    void SetMountSeatOverride(Player* carrier, uint32 vehicleId);
    uint32 GetMountSeatOverride(Player* carrier);

    /// Moves whoever is already carried onto the currently selected seat.
    bool ReseatCarried(Player* carrier);

    /// Spawns morphed dummies for every carrier/passenger pair, or one passenger row.
    /// Returns how many pairs were made. -1 spawns the whole 20x20 grid.
    uint32 SpawnTestGrid(Player* gm, int32 onlyPassenger);
    void ClearTestGrid(Player* anchor);

    /// The first seat of a vehicle and its attachment offsets, read from the DBC.
    bool DescribeSeat(uint32 vehicleId, uint32& seatOut, float& x, float& y, float& z);
}

#endif // MOD_SANCTUARY_DOWNED_H
