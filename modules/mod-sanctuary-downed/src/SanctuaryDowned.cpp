/*
 * mod-sanctuary-downed - carrying a fallen player
 *
 * One player kneels over another for three seconds, then lifts them onto their shoulder and
 * walks off with them.
 *
 * The whole thing rests on a fact that is easy to miss: a Player can be a vehicle. The core
 * builds the kit and broadcasts SMSG_PLAYER_VEHICLE_DATA in Unit::Mount (Unit.cpp:10510),
 * which is how two-seater mounts carry a second player, so every 3.3.5a client already
 * understands it. What is copied here is that block without the mount - we want the seat,
 * not a horse appearing underneath somebody.
 *
 * The pose is stock data, not a client patch. Animation 132 is "Drowned", and eighteen stock
 * vehicle seats use it as their ride loop. Seat 2528 (vehicle 284) attaches at the right
 * shoulder with offsets (1.80, -0.50, -2.50): dropped well below the shoulder bone and a
 * little to the right, which is what a body slung over somebody looks like.
 *
 * The offsets live in the client's VehicleSeat.dbc and cannot be set from here - the server
 * copies them into the passenger's transport position but the client welds the model to the
 * bone from its own data. That is why per-race tuning is a mapping from race to *vehicle
 * id* rather than to a set of numbers: a different body length needs a different seat row.
 *
 * The passenger must be ALIVE. Unit::_EnterVehicle refuses a dead unit on its first line
 * (Unit.cpp:15626), which is the entire reason the downed state keeps players at 1 HP
 * instead of letting them die.
 */

#include "SanctuaryDowned.h"

#include "AllSpellScript.h"
#include "Chat.h"
#include "TemporarySummon.h"
#include "Creature.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "UnitScript.h"
#include "Vehicle.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // Not an "s_" prefix: winsock2.h defines s_host and friends as macros on in_addr, and a
    // file-scope name colliding with one expands into nonsense before the compiler sees it.
    bool g_enabled = true;

    /// Vehicle 284 / seat 2528: right shoulder, "Drowned" ride loop, offsets (1.8, -0.5, -2.5).
    uint32 g_vehicleId = 284;
    int8 g_seatId = 0;

    /*
     * Per-carrier override, indexed by race then gender (0 male, 1 female).
     *
     * This is the first vehicle of the carrier's block: the anchor, derived from where
     * attachment 5 sits in that character's own M2 and never tuned by hand. The passenger
     * and the trim pick which entry of the block is actually used.
     *
     * Offsets were at one point assumed to be scaled by the carrier's render scale, and
     * they are not - an orc female renders at 1.00 and was wrong in the same direction as
     * a tauren at 1.35, which rules that out. 0 means "use g_vehicleId".
     */
    std::array<std::array<uint32, 2>, 12> g_vehicleByCarrier{};

    /*
     * The seat used while the carrier is mounted, if it needs to differ.
     *
     * The passenger hangs off attachment 5 - a bone on the carrier model - so a mount
     * should carry them up with it. If it does not, the offsets still cannot be changed
     * at runtime, but which vehicle is used can be, so a second seat covers that case.
     * 0 keeps whatever the on-foot rules chose.
     */
    uint32 g_mountedVehicleId = 0;

    /*
     * How far below the carrier's shoulder the body sits, indexed by the PASSENGER's
     * race then gender - the opposite key to g_vehicleByCarrier, and deliberately so.
     *
     * The carrier decides where the shoulder is; the passenger decides how far under it
     * their body has to hang, because the Drowned pose puts a large model's mesh further
     * above its own origin than a small one's. An orc and a tauren carrying the same body
     * agreed on the delta, which is what established that this belongs to the passenger.
     */
    std::array<std::array<uint32, 2>, 12> g_deltaByPassenger{};

    /*
     * A small vertical correction per CARRIER, on top of the measured anchor.
     *
     * Attachment 5 is not in quite the same place relative to the visible shoulder on
     * every model - an orc male and an orc female carrying the same body do not match -
     * and nothing in the model data predicts the difference, so it has to be seen and
     * then written down. Rungs are generated either side of no-correction so that doing
     * so costs a config reload rather than a client patch.
     */
    std::array<std::array<uint32, 2>, 12> g_trimByCarrier{};

    /// Deltas generated per passenger body type, and trims per delta. Together these give
    /// the stride from one carrier block to the next.
    uint32 g_deltaCount = 8;
    uint32 g_trimCount = 1;


    /*
     * The downed.
     *
     * A player here is alive at 1 HP, rooted, silenced and lying in the Drowned pose - the
     * same pose the carry seat puts them in, so being lifted changes where the body is and
     * nothing about how it looks.
     */
    bool g_downEnabled = true;
    bool g_downFromPlayers = true;
    bool g_downFromCreatures = false;
    uint32 g_bleedOutMs = 120000;
    /// Health restored by a rescue. They are on 1 HP, and standing up on 1 HP is a
    /// formality before dying again.
    uint32 g_reviveHealthPct = 15;
    uint32 g_downImmunityMs = 5000;

    /// Faction 2300: neutral to every playable race both ways, and still attackable.
    /// Shipped already for wanted outlaws, and the behaviour needed here is identical.
    uint32 g_downedFaction = 2300;

    /*
     * Close enough to hold a vial under somebody's nose - and to kneel over them.
     *
     * Both revives use it. The spells carry the client's shortest range, two yards,
     * because there is no shorter row; this is the distance that actually decides, and
     * it is measured with bounding radii like the carry's reach, so it means "stood
     * over them" rather than "somewhere nearby".
     */
    float g_reviveRange = 0.5f;

    /// Shown in the debuff tray while the grace lasts, so the timer is visible rather
    /// than something the player has to count in their head. 0 disables it.
    uint32 g_downImmunitySpell = 81002;

    /// The bleed-out countdown, worn for as long as they are down. It is also what the
    /// addon watches to know when to offer the button, so it earns its keep twice.
    uint32 g_bleedOutSpell = 81003;

    /*
     * Resuscitate: our own revive spell, and the reason the retail rez spells are left
     * alone.
     *
     * Widening those was tried and the client refused it outright - an instrumented
     * server logged nothing at all when one was cast, so the refusal happened before the
     * packet was sent, where no server hook can reach. A spell we own has no such
     * argument to lose: it is in the spellbook, it targets whoever is selected, and what
     * it does is entirely ours.
     */
    uint32 g_reviveSpell = 81004;

    /// What Hearthdown casts. Ten seconds against Resuscitate's five: fumbling a vial out
    /// of a bag is not the same as knowing what you are doing.
    uint32 g_hearthdownSpell = 81005;

    /// The item consumed when that cast lands.
    uint32 g_hearthdownItem = 990004;

    /// Classes that already carry a resurrection, so the spell lands where it belongs:
    /// paladin, priest, shaman, druid.
    std::vector<uint32> g_reviveClasses = { 2, 5, 7, 11 };


    struct DownedState
    {
        uint32 remainingMs;
        uint32 immuneMs;            ///< counts down separately from the bleed-out
        uint32 formerFaction;       ///< restored on the way back up
        uint32 downedHealth;        ///< what they are pinned to; regeneration cannot pass it
        bool neutral;               ///< true once the grace has expired and 2300 is on
        ObjectGuid feller;
    };

    std::unordered_map<ObjectGuid, DownedState> g_downed;

    float g_reach = 0.5f;
    float g_carrySpeed = 0.7f;
    bool g_announce = true;

    /*
     * The cast: five seconds, cancelled by movement or a hit.
     *
     * Our own spell now. It used to borrow 56992 and reshape it at load, which worked
     * until the borrowed spell turned out to be an inscription recipe and demanded
     * Moonglow Ink in front of a player - a requirement invisible in the cast time and
     * channel flags that had been checked when choosing it. Owning the row means nothing
     * is inherited that nobody looked at.
     *
     * Cast on the carrier themselves; the reach to whoever is being lifted is checked in
     * Check(), which is why the spell carries no range of its own.
     */
    uint32 g_castSpell = 81006;

    /// Worn by whoever is over the shoulder. The addon watches it to offer the struggle
    /// button, and it is what tells a conscious captive what has happened to them.
    uint32 g_carriedSpell = 81007;

    /// Looping animation held by the carrier. 428 is STATE_LOOT, whose pose is LootHold.
    uint32 g_carrierEmote = 428;

    /// carrier guid -> passenger guid, and the reverse, so either end resolves in one step.
    std::unordered_map<ObjectGuid, ObjectGuid> g_carrying;
    std::unordered_map<ObjectGuid, ObjectGuid> g_carriedBy;

    /// Per-player override of the vehicle, for trying offsets. Session only.
    std::unordered_map<ObjectGuid, uint32> g_seatOverride;

    /// The same, consulted only while the carrier is mounted. Session only.
    std::unordered_map<ObjectGuid, uint32> g_mountSeatOverride;

    /// Who each in-flight cast was aimed at, so a finished bar lifts the right person.
    std::unordered_map<ObjectGuid, ObjectGuid> g_pending;

    char const* const RaceNames[12] =
    {
        // Race 9 is Goblin, which is not playable in 3.3.5a. Left blank so the loop below
        // skips it rather than asking for a config key that has no reason to exist.
        "", "Human", "Orc", "Dwarf", "NightElf", "Undead",
        "Tauren", "Gnome", "Troll", "", "BloodElf", "Draenei"
    };

    /*
     * Both parties matter, and they pick different halves of the answer: the carrier
     * chooses the anchor (their own measured shoulder), the passenger chooses the delta
     * (how far under it their body hangs). A tauren body does hang further than a gnome
     * one, which is what the second table is for.
     */
    uint32 VehicleForPassenger(Player const* carrier, Player const* passenger)
    {
        if (!carrier)
            return g_vehicleId;

        bool const mounted = carrier->IsMounted();

        // A carrier who is trying offsets overrides everything, so the same body can be
        // looked at in a dozen positions without a rebuild between each. The mounted
        // override is read first so both states can be tuned in one sitting.
        if (mounted)
        {
            auto itr = g_mountSeatOverride.find(carrier->GetGUID());
            if (itr != g_mountSeatOverride.end() && itr->second)
                return itr->second;
        }

        auto itr = g_seatOverride.find(carrier->GetGUID());
        if (itr != g_seatOverride.end() && itr->second)
            return itr->second;

        if (mounted && g_mountedVehicleId)
            return g_mountedVehicleId;

        uint8 const race = carrier->getRace();
        uint8 const sex = carrier->getGender() == GENDER_FEMALE ? 1 : 0;

        if (race >= g_vehicleByCarrier.size() || !g_vehicleByCarrier[race][sex])
            return g_vehicleId;

        // The configured id is the carrier's first seat; the passenger picks which of the
        // deltas that follow it is used. Guarded so a bad config value cannot walk off
        // the end of one body type's block and into the next carrier's seats.
        uint32 delta = 0;

        if (passenger)
        {
            uint8 const passRace = passenger->getRace();
            uint8 const passSex = passenger->getGender() == GENDER_FEMALE ? 1 : 0;

            if (passRace < g_deltaByPassenger.size())
                delta = g_deltaByPassenger[passRace][passSex];
        }

        if (delta >= g_deltaCount)
            delta = 0;

        uint32 trim = g_trimByCarrier[race][sex];

        if (trim >= g_trimCount)
            trim = g_trimCount / 2;      // the middle rung is no correction

        return g_vehicleByCarrier[race][sex] + delta * g_trimCount + trim;
    }

    /*
     * Gives the carrier a vehicle kit without making them look mounted.
     *
     * Lifted from Unit::Mount (Unit.cpp:10525-10535), minus UNIT_FIELD_MOUNTDISPLAYID and
     * UNIT_FLAG_MOUNT. Accessories are deliberately not installed: this vehicle's stock
     * accessory list belongs to the creature it was authored for.
     */
    bool GiveVehicleKit(Player* carrier, uint32 vehicleId)
    {
        if (carrier->GetVehicleKit())
            return true;

        if (!carrier->CreateVehicleKit(vehicleId, 0))
            return false;

        carrier->GetVehicleKit()->Reset();

        WorldPacket data(SMSG_PLAYER_VEHICLE_DATA, carrier->GetPackGUID().size() + 4);
        data << carrier->GetPackGUID();
        data << uint32(vehicleId);
        carrier->SendMessageToSet(&data, true);

        // Without this the carrier's own client waits on a ride aura that never comes and
        // leaves the vehicle UI half-open.
        data.Initialize(SMSG_ON_CANCEL_EXPECTED_RIDE_VEHICLE_AURA, 0);
        carrier->SendDirectMessage(&data);

        return true;
    }

    void TakeVehicleKit(Player* carrier)
    {
        if (!carrier->GetVehicleKit())
            return;

        carrier->RemoveVehicleKit();

        // Tell everyone it is gone, or other clients keep drawing the seat.
        WorldPacket data(SMSG_PLAYER_VEHICLE_DATA, carrier->GetPackGUID().size() + 4);
        data << carrier->GetPackGUID();
        data << uint32(0);
        carrier->SendMessageToSet(&data, true);
    }

    void SetCarryPose(Player* carrier, bool carrying)
    {
        // Carrying somebody should cost something, or there is no decision in it.
        carrier->SetSpeed(MOVE_RUN, carrying ? g_carrySpeed : 1.0f, true);
        carrier->SetSpeed(MOVE_SWIM, carrying ? g_carrySpeed : 1.0f, true);

        // A looping emote is the only way to hold a player in a pose. The client resolves
        // the animation from the emote's own Emotes.dbc row, so the choice of pose is
        // limited to animations some emote already points at.
        if (g_carrierEmote)
            carrier->SetUInt32Value(UNIT_NPC_EMOTESTATE, carrying ? g_carrierEmote : 0);
    }

    void Forget(ObjectGuid carrier, ObjectGuid passenger)
    {
        g_carrying.erase(carrier);
        g_carriedBy.erase(passenger);

        // Every way a carry can end runs through here - put down, walked out of range,
        // logged out, died - so the aura comes off here rather than in each of them.
        if (g_carriedSpell)
            if (Player* body = ObjectAccessor::FindConnectedPlayer(passenger))
                body->RemoveAurasDueToSpell(g_carriedSpell);
    }

    /// Everything that must be true both when the cast starts and when it lands.
    SanctuaryDowned::Result Check(Player* carrier, Player* target)
    {
        using Result = SanctuaryDowned::Result;

        if (!g_enabled)
            return Result::Disabled;

        if (!carrier || !target)
            return Result::NoTarget;

        if (carrier == target)
            return Result::Self;

        if (!SanctuaryDowned::CarriedBy(carrier).IsEmpty())
            return Result::AlreadyCarrying;

        if (!SanctuaryDowned::CarrierOf(target).IsEmpty())
            return Result::AlreadyCarried;

        // Somebody already over a shoulder cannot pick a third person up.
        if (!SanctuaryDowned::CarrierOf(carrier).IsEmpty())
            return Result::CarrierIsCarried;

        // GM mode is exempt from the two rules that are there for roleplay rather than
        // for correctness. Staging a scene generally means doing this from a flying
        // mount, at whatever distance the camera is happiest at, and both of those are
        // otherwise refused. Note this is `.gm on`, not the account level: a GM playing
        // a character with gm off is held to the same rules as anyone else.
        bool const staging = carrier->IsGameMaster();

        if (!staging && carrier->IsMounted())
            return Result::Mounted;

        // Either of them already in a vehicle would fight this one for the seat - which
        // is a real conflict rather than a rule, so it holds for GMs too.
        if (carrier->GetVehicle() || target->GetVehicle())
            return Result::InVehicle;

        // Deliberately tight, and measured with bounding radii, so it means "stood over
        // them" rather than "somewhere nearby".
        if (!staging && !carrier->IsWithinDistInMap(target, g_reach))
            return Result::TooFar;

        return Result::Ok;
    }
}

namespace SanctuaryDowned
{
    ObjectGuid CarrierOf(Player* passenger)
    {
        if (!passenger)
            return ObjectGuid::Empty;

        auto itr = g_carriedBy.find(passenger->GetGUID());
        return itr == g_carriedBy.end() ? ObjectGuid::Empty : itr->second;
    }

    ObjectGuid CarriedBy(Player* carrier)
    {
        if (!carrier)
            return ObjectGuid::Empty;

        auto itr = g_carrying.find(carrier->GetGUID());
        return itr == g_carrying.end() ? ObjectGuid::Empty : itr->second;
    }

    Result BeginPickUp(Player* carrier, Player* target)
    {
        Result const check = Check(carrier, target);
        if (check != Result::Ok)
            return check;

        // No cast configured means lift now. The config has always offered this; it just
        // was not wired up, so setting it to 0 quietly did nothing.
        if (!g_castSpell)
            return PickUp(carrier, target);

        if (carrier->IsNonMeleeSpellCast(false))
            return Result::Busy;

        // Remembered so the finished bar lifts whoever it was aimed at, rather than
        // whatever happens to be selected three seconds later.
        g_pending[carrier->GetGUID()] = target->GetGUID();

        carrier->CastSpell(carrier, g_castSpell, false);

        if (g_announce)
            ChatHandler(carrier->GetSession()).PSendSysMessage("You stoop to lift them...");

        return Result::Ok;
    }

    Result PickUp(Player* carrier, Player* target)
    {
        Result const check = Check(carrier, target);
        if (check != Result::Ok)
            return check;

        uint32 const vehicleId = VehicleForPassenger(carrier, target);

        if (!GiveVehicleKit(carrier, vehicleId))
            return Result::Failed;

        target->EnterVehicle(carrier, g_seatId);

        // EnterVehicle works through a spell, so the seat is not guaranteed to be taken by
        // the time it returns. Checking rather than assuming means a failure is reported as
        // one instead of leaving a phantom carry recorded.
        if (target->GetVehicle() != carrier->GetVehicleKit())
        {
            TakeVehicleKit(carrier);
            return Result::Failed;
        }

        g_carrying[carrier->GetGUID()] = target->GetGUID();
        g_carriedBy[target->GetGUID()] = carrier->GetGUID();

        SetCarryPose(carrier, true);

        if (g_carriedSpell)
            target->AddAura(g_carriedSpell, target);

        if (g_announce)
        {
            ChatHandler(carrier->GetSession()).PSendSysMessage("You lift them onto your shoulder.");

            if (target->GetSession())
                ChatHandler(target->GetSession()).PSendSysMessage("Someone lifts you onto their shoulder.");
        }

        return Result::Ok;
    }

    Result PutDown(Player* carrier)
    {
        if (!carrier)
            return Result::NoTarget;

        ObjectGuid const passengerGuid = CarriedBy(carrier);
        if (passengerGuid.IsEmpty())
            return Result::NotCarrying;

        if (Player* passenger = ObjectAccessor::FindConnectedPlayer(passengerGuid))
        {
            passenger->ExitVehicle();

            if (g_announce && passenger->GetSession())
                ChatHandler(passenger->GetSession()).PSendSysMessage("You are set down.");
        }

        Forget(carrier->GetGUID(), passengerGuid);

        TakeVehicleKit(carrier);
        SetCarryPose(carrier, false);

        if (g_announce)
            ChatHandler(carrier->GetSession()).PSendSysMessage("You set them down.");

        return Result::Ok;
    }

    /*
     * Gets a conscious passenger down off somebody's shoulder.
     *
     * No contest and nothing to roll - anybody awake simply climbs down. The one refusal is
     * the point of the whole system: somebody genuinely unconscious cannot, because being
     * unable to is what makes them carryable against their will in the first place.
     */
    bool GetDown(Player* player)
    {
        if (!player || CarrierOf(player).IsEmpty())
            return false;

        if (IsDowned(player))
            return false;

        ObjectGuid const carrierGuid = CarrierOf(player);
        Release(player);

        if (g_announce)
        {
            if (player->GetSession())
                ChatHandler(player->GetSession()).PSendSysMessage("You get down.");

            if (Player* carrier = ObjectAccessor::FindConnectedPlayer(carrierGuid))
                if (carrier->GetSession())
                    ChatHandler(carrier->GetSession()).PSendSysMessage(
                        "They get down off your shoulder.");
        }

        return true;
    }

    void Release(Player* player)
    {
        if (!player)
            return;

        g_pending.erase(player->GetGUID());

        // Carrying somebody: put them down properly.
        if (!CarriedBy(player).IsEmpty())
        {
            PutDown(player);
            return;
        }

        // Being carried: get off, and tidy the carrier's end too.
        ObjectGuid const carrierGuid = CarrierOf(player);
        if (carrierGuid.IsEmpty())
            return;

        player->ExitVehicle();
        Forget(carrierGuid, player->GetGUID());

        if (Player* carrier = ObjectAccessor::FindConnectedPlayer(carrierGuid))
        {
            TakeVehicleKit(carrier);
            SetCarryPose(carrier, false);
        }
    }
}

namespace SanctuaryDowned
{
    /*
     * Puts back anybody who has fallen out of their seat.
     *
     * Called on a timer rather than from a hook because the thing that ejects them is
     * Unit::Dismount stripping the vehicle kit, which no script hook covers - and the same
     * lapse can come from a teleport or a forced exit. Re-checking the truth is simpler and
     * catches all of them.
     */
    void ReseatLapsed()
    {
        if (!g_enabled || g_carrying.empty())
            return;

        std::vector<ObjectGuid> lost;

        for (auto const& [carrierGuid, passengerGuid] : g_carrying)
        {
            Player* carrier = ObjectAccessor::FindConnectedPlayer(carrierGuid);
            Player* passenger = ObjectAccessor::FindConnectedPlayer(passengerGuid);

            if (!carrier || !passenger || !carrier->IsInWorld() || !passenger->IsInWorld())
            {
                lost.push_back(carrierGuid);
                continue;
            }

            // Still seated on us: nothing to do, which is the common case - unless the
            // seat that should be under them has changed, which is what mounting does.
            if (carrier->GetVehicleKit() && passenger->GetVehicle() == carrier->GetVehicleKit())
            {
                VehicleEntry const* info = carrier->GetVehicleKit()->GetVehicleInfo();

                if (!info || info->m_ID == VehicleForPassenger(carrier, passenger))
                    continue;

                if (!ReseatCarried(carrier))
                    lost.push_back(carrierGuid);

                continue;
            }

            // Wandered off, or died: let the carry end rather than dragging them back.
            if (!passenger->IsAlive() || !carrier->IsWithinDistInMap(passenger, 20.0f))
            {
                lost.push_back(carrierGuid);
                continue;
            }

            // Somebody else has them now.
            if (passenger->GetVehicle())
                continue;

            if (!GiveVehicleKit(carrier, VehicleForPassenger(carrier, passenger)))
            {
                lost.push_back(carrierGuid);
                continue;
            }

            passenger->EnterVehicle(carrier, g_seatId);

            if (passenger->GetVehicle() == carrier->GetVehicleKit())
            {
                // The pose and the speed penalty go with the kit, so both come back too.
                SetCarryPose(carrier, true);
            }
        }

        for (ObjectGuid const& carrierGuid : lost)
        {
            if (Player* carrier = ObjectAccessor::FindConnectedPlayer(carrierGuid))
                PutDown(carrier);
            else if (auto itr = g_carrying.find(carrierGuid); itr != g_carrying.end())
                Forget(carrierGuid, itr->second);
        }
    }
}

namespace SanctuaryDowned
{
    void SetSeatOverride(Player* carrier, uint32 vehicleId)
    {
        if (!carrier)
            return;

        if (vehicleId)
            g_seatOverride[carrier->GetGUID()] = vehicleId;
        else
            g_seatOverride.erase(carrier->GetGUID());
    }

    uint32 GetSeatOverride(Player* carrier)
    {
        if (!carrier)
            return 0;

        auto itr = g_seatOverride.find(carrier->GetGUID());
        return itr == g_seatOverride.end() ? 0 : itr->second;
    }

    void SetMountSeatOverride(Player* carrier, uint32 vehicleId)
    {
        if (!carrier)
            return;

        if (vehicleId)
            g_mountSeatOverride[carrier->GetGUID()] = vehicleId;
        else
            g_mountSeatOverride.erase(carrier->GetGUID());
    }

    uint32 GetMountSeatOverride(Player* carrier)
    {
        if (!carrier)
            return 0;

        auto itr = g_mountSeatOverride.find(carrier->GetGUID());
        return itr == g_mountSeatOverride.end() ? 0 : itr->second;
    }

    bool DescribeSeat(uint32 vehicleId, uint32& seatOut, float& x, float& y, float& z)
    {
        VehicleEntry const* vehicle = sVehicleStore.LookupEntry(vehicleId);
        if (!vehicle)
            return false;

        uint32 const seatId = vehicle->m_seatID[0];
        VehicleSeatEntry const* seat = sVehicleSeatStore.LookupEntry(seatId);
        if (!seat)
            return false;

        seatOut = seatId;
        x = seat->m_attachmentOffsetX;
        y = seat->m_attachmentOffsetY;
        z = seat->m_attachmentOffsetZ;
        return true;
    }
}

namespace SanctuaryDowned
{
    /*
     * Moves whoever is already over the shoulder onto the currently selected seat.
     *
     * The offsets belong to the vehicle, so changing which vehicle is used means tearing
     * the kit down and building it again. Doing that here rather than making the player
     * drop and re-lift is the difference between trying eighteen candidates and giving up
     * after four.
     */
    bool ReseatCarried(Player* carrier)
    {
        if (!carrier)
            return false;

        ObjectGuid const passengerGuid = CarriedBy(carrier);
        if (passengerGuid.IsEmpty())
            return false;

        Player* passenger = ObjectAccessor::FindConnectedPlayer(passengerGuid);
        if (!passenger)
            return false;

        passenger->ExitVehicle();
        TakeVehicleKit(carrier);

        uint32 const vehicleId = VehicleForPassenger(carrier, passenger);

        if (!GiveVehicleKit(carrier, vehicleId))
        {
            Forget(carrier->GetGUID(), passengerGuid);
            SetCarryPose(carrier, false);
            return false;
        }

        passenger->EnterVehicle(carrier, g_seatId);

        if (passenger->GetVehicle() != carrier->GetVehicleKit())
        {
            Forget(carrier->GetGUID(), passengerGuid);
            TakeVehicleKit(carrier);
            SetCarryPose(carrier, false);
            return false;
        }

        SetCarryPose(carrier, true);
        return true;
    }
}

namespace SanctuaryDowned
{
    /*
     * Turns a refusal into something worth reading. Ok returns nullptr.
     *
     * Used by the chat commands and by the spell's own check-cast, which is why it lives
     * here rather than beside the commands: both need to say the same thing, and a refusal
     * phrased two ways is a refusal players learn to distrust.
     */
    char const* Explain(Result result)
    {
        switch (result)
        {
            case Result::Ok:               return nullptr;
            case Result::Disabled:         return "Carrying is switched off on this realm.";
            case Result::NoTarget:         return "Target the person you mean to lift.";
            case Result::Self:             return "You cannot carry yourself.";
            case Result::TooFar:           return "You need to be stood over them.";
            case Result::AlreadyCarrying:  return "You already have somebody over your shoulder.";
            case Result::AlreadyCarried:   return "Somebody else already has them.";
            case Result::CarrierIsCarried: return "You are being carried yourself.";
            case Result::NotCarrying:      return "You are not carrying anybody.";
            case Result::Busy:             return "You are already casting something.";
            case Result::Mounted:          return "Not while mounted.";
            case Result::InVehicle:        return "One of you is already in a vehicle.";
            default:                       return "That did not work.";
        }
    }
}

namespace SanctuaryDowned
{
    bool IsDowned(Player const* player)
    {
        return player && g_downed.count(player->GetGUID()) != 0;
    }

    /*
     * Puts a player out of the fight without killing them.
     *
     * Combat is dropped as well as movement, because a rooted player still in combat is
     * still being hit by everything that was already swinging, and cannot be rescued in any
     * meaningful sense.
     */
    void GoDown(Player* player, Unit* feller)
    {
        if (!player || IsDowned(player))
            return;

        g_downed[player->GetGUID()] =
            { g_bleedOutMs, g_downImmunityMs, player->GetFaction(), 1, false,
              feller ? feller->GetGUID() : ObjectGuid::Empty };

        // Root holds them in place but leaves them free to spin, which reads as very
        // much awake. Stun is what stops the turn.
        player->SetControlled(true, UNIT_STATE_ROOT);
        player->SetControlled(true, UNIT_STATE_STUNNED);
        player->SetUnitFlag(UnitFlags(UNIT_FLAG_PACIFIED | UNIT_FLAG_SILENCED));

        player->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_STATE_DROWNED);
        player->CombatStop(true);
        player->AttackStop();

        /*
         * Everything hostile comes off.
         *
         * They tick for nothing now that damage is clamped, but a body lying under a stack
         * of curses reads as one still being killed, and a snare or a knockback would still
         * take effect - those are not damage, so the clamp never sees them.
         *
         * Ids are collected first and removed afterwards: GetAppliedAuras hands back a const
         * map, and removing while walking it would invalidate the iterator anyway.
         */
        std::vector<uint32> hostile;

        for (auto const& [spellId, application] : player->GetAppliedAuras())
            if (application && !application->IsPositive())
                hostile.push_back(spellId);

        for (uint32 spellId : hostile)
            player->RemoveAurasDueToSpell(spellId);

        // A few seconds where nothing can touch them at all, so an area effect already on
        // the ground does not immediately undo the rescue somebody is about to attempt.
        if (g_downImmunityMs)
        {
            player->SetUnitFlag(UnitFlags(UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC));

            // The aura's duration is overwritten from the config rather than left at
            // the five seconds baked into the spell row, so the icon cannot say one
            // thing while the immunity does another.
            if (g_downImmunitySpell)
                if (Aura* grace = player->AddAura(g_downImmunitySpell, player))
                    grace->SetDuration(int32(g_downImmunityMs));
        }

        /*
         * The bleed-out clock goes on AFTER the strip above, not before it.
         *
         * It is flagged as a debuff - it has to be, or it would sit among the buffs and
         * could be right-clicked away - which means the "remove everything hostile" pass
         * treats it as hostile and takes it straight back off again. Applied first, it
         * lasted a few microseconds: long enough to exist, never long enough to be seen,
         * which is exactly what the missing window looked like from the outside.
         */
        if (g_bleedOutSpell)
            if (Aura* clock = player->AddAura(g_bleedOutSpell, player))
                clock->SetDuration(int32(g_bleedOutMs));

        if (g_announce)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "You have fallen unconscious.");

            if (Player* by = feller ? feller->ToPlayer() : nullptr)
                ChatHandler(by->GetSession()).PSendSysMessage("They go down, but they are not dead.");
        }
    }

    /// Back on their feet. Safe to call on somebody who was never down.
    void StandUp(Player* player, bool announce)
    {
        if (!player || !IsDowned(player))
            return;

        auto itr = g_downed.find(player->GetGUID());
        uint32 const formerFaction = itr != g_downed.end() && itr->second.neutral
            ? itr->second.formerFaction : 0;

        g_downed.erase(player->GetGUID());

        // Their own faction goes back on before anything else, so they are not left standing
        // as a neutral nobody will defend.
        if (formerFaction)
            player->SetFaction(formerFaction);

        player->SetControlled(false, UNIT_STATE_STUNNED);
        player->SetControlled(false, UNIT_STATE_ROOT);
        player->RemoveUnitFlag(UnitFlags(UNIT_FLAG_PACIFIED | UNIT_FLAG_SILENCED |
                                         UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC));
        player->SetUInt32Value(UNIT_NPC_EMOTESTATE, 0);

        if (g_downImmunitySpell)
            player->RemoveAurasDueToSpell(g_downImmunitySpell);

        if (g_bleedOutSpell)
            player->RemoveAurasDueToSpell(g_bleedOutSpell);

        if (announce && g_announce && player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage("You get your feet under you again.");
    }

    /*
     * Somebody has brought them round.
     *
     * Health comes back with them: standing up on the 1 HP they were left with is a
     * formality before dying again, and makes a rescue worth nothing.
     */
    bool Revive(Player* player, Player* rescuer)
    {
        if (!player || !IsDowned(player))
            return false;

        StandUp(player, false);

        if (uint32 const pct = g_reviveHealthPct)
            player->SetHealth(std::max<uint32>(1, player->GetMaxHealth() * pct / 100));

        if (g_announce)
        {
            if (player->GetSession())
                ChatHandler(player->GetSession()).PSendSysMessage("You come round, and the world with you.");

            if (rescuer && rescuer->GetSession())
                ChatHandler(rescuer->GetSession()).PSendSysMessage("You bring them round.");
        }

        return true;
    }

    /// Stops waiting. The only way out of the downed state that the player controls.
    bool GiveUp(Player* player)
    {
        if (!player || !IsDowned(player))
            return false;

        StandUp(player, false);
        Release(player);
        player->KillSelf();
        return true;
    }

    void ClearDowned(Player* player)
    {
        if (player)
            g_downed.erase(player->GetGUID());
    }

    /*
     * Hands the state to the next session.
     *
     * Written at logout and read back at login. It is a handover rather than a record: the
     * row is deleted the moment it is used, because a row left behind would put somebody
     * back on the floor every time they logged in, however they got up the first time.
     *
     * The auras are deliberately not the thing that persists. They come off at logout with
     * everything else and are rebuilt from these numbers, so the icon and the module cannot
     * drift apart - and auras found at login with no row behind them are what a crash looks
     * like, which is the case StripDownedAuras already handles.
     */
    void SaveDowned(Player* player)
    {
        if (!player)
            return;

        auto itr = g_downed.find(player->GetGUID());

        if (itr == g_downed.end())
            return;

        DownedState const& state = itr->second;

        // Only a player feller is worth keeping. A creature's guid means nothing after a
        // restart, and crediting the wolf that mauled somebody an hour ago is not worth
        // the lookup it would cost.
        uint32 const feller = state.feller.IsPlayer() ? state.feller.GetCounter() : 0;

        CharacterDatabase.Execute(
            "REPLACE INTO `sanctuary_downed` "
            "(`guid`, `remaining_ms`, `immune_ms`, `downed_health`, `feller`) "
            "VALUES ({}, {}, {}, {}, {})",
            player->GetGUID().GetCounter(), state.remainingMs, state.immuneMs,
            state.downedHealth, feller);
    }

    /*
     * Puts them back where they were left.
     *
     * Everything GoDown does except the two things that only make sense the first time: the
     * pass that strips hostile auras (there is nothing left to strip after a logout) and the
     * announcement that they have fallen, which they did some time ago.
     *
     * Whether the grace still has time on it decides the rest. Logging out during those few
     * seconds is rare but possible, and it is the difference between coming back untouchable
     * and coming back neutral with anybody free to finish the job.
     */
    void RestoreDowned(Player* player)
    {
        if (!player)
            return;

        uint32 const guid = player->GetGUID().GetCounter();

        QueryResult result = CharacterDatabase.Query(
            "SELECT `remaining_ms`, `immune_ms`, `downed_health`, `feller` "
            "FROM `sanctuary_downed` WHERE `guid` = {}", guid);

        if (!result)
            return;

        // Taken out whether or not it is used below, so that switching the module off does
        // not leave rows waiting to catch people out when it comes back on.
        CharacterDatabase.Execute("DELETE FROM `sanctuary_downed` WHERE `guid` = {}", guid);

        if (!g_enabled || !g_downEnabled)
            return;

        Field* fields = result->Fetch();

        uint32 const remainingMs = fields[0].Get<uint32>();
        uint32 const immuneMs = fields[1].Get<uint32>();
        uint32 const downedHealth = std::max<uint32>(1, fields[2].Get<uint32>());
        uint32 const fellerLow = fields[3].Get<uint32>();

        if (!remainingMs || !player->IsAlive())
            return;

        // Their real faction is read here rather than saved: it is not a stored column, so
        // the core rebuilds it from their race at load and this is it, before anything of
        // ours goes on top.
        g_downed[player->GetGUID()] =
            { remainingMs, immuneMs, player->GetFaction(), downedHealth, immuneMs == 0,
              fellerLow ? ObjectGuid::Create<HighGuid::Player>(fellerLow) : ObjectGuid::Empty };

        player->SetControlled(true, UNIT_STATE_ROOT);
        player->SetControlled(true, UNIT_STATE_STUNNED);
        player->SetUnitFlag(UnitFlags(UNIT_FLAG_PACIFIED | UNIT_FLAG_SILENCED));
        player->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_STATE_DROWNED);
        player->SetHealth(downedHealth);

        // Guards ignore them for the whole state, grace or no grace.
        player->SetUnitFlag(UNIT_FLAG_IMMUNE_TO_NPC);

        if (immuneMs)
        {
            player->SetUnitFlag(UNIT_FLAG_IMMUNE_TO_PC);

            if (g_downImmunitySpell)
                if (Aura* grace = player->AddAura(g_downImmunitySpell, player))
                    grace->SetDuration(int32(immuneMs));
        }
        else if (g_downedFaction)
            player->SetFaction(g_downedFaction);

        if (g_bleedOutSpell)
            if (Aura* clock = player->AddAura(g_bleedOutSpell, player))
                clock->SetDuration(int32(remainingMs));

        if (g_announce && player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "You are where you fell, and still unconscious.");
    }

    /// Removes the advertising auras without touching anything else. For a login sweep,
    /// where the player is not down and there is no state to unwind.
    /*
     * Gives Resuscitate to a class that ought to have it.
     *
     * Done at login rather than by a trainer because it is not really a class ability -
     * it is the rule that somebody who can resurrect the dead can also rouse the merely
     * unconscious, and a rule should not depend on anyone finding a trainer.
     */
    /*
     * Every class, at login. No class owns picking a person up off the floor, and a
     * mechanic that needs a chat command to reach is one most players never discover.
     * `.carry` still works, and is still how you set somebody down.
     */
    void TeachCarrySpell(Player* player)
    {
        if (!player || !g_castSpell)
            return;

        if (!player->HasSpell(g_castSpell))
            player->learnSpell(g_castSpell);
    }

    void TeachReviveSpell(Player* player)
    {
        if (!player || !g_reviveSpell)
            return;

        bool const eligible = std::find(g_reviveClasses.begin(), g_reviveClasses.end(),
            uint32(player->getClass())) != g_reviveClasses.end();

        if (eligible)
        {
            if (!player->HasSpell(g_reviveSpell))
                player->learnSpell(g_reviveSpell);
        }
        // Taken away again if they should not have it - a config change, or a spell
        // handed out by an earlier build to a class that is no longer on the list.
        else if (player->HasSpell(g_reviveSpell))
            player->removeSpell(g_reviveSpell, SPEC_MASK_ALL, false);
    }

    void StripDownedAuras(Player* player)
    {
        if (!player)
            return;

        if (g_downImmunitySpell)
            player->RemoveAurasDueToSpell(g_downImmunitySpell);

        if (g_bleedOutSpell)
            player->RemoveAurasDueToSpell(g_bleedOutSpell);

        // Looking dead is a display flag, and a session that ended badly can leave it on
        // somebody who is walking around perfectly alive.
    }

    /*
     * Counts the downed towards dying, and notices anyone who has been patched up.
     *
     * Carried players are skipped rather than ticked: being carried out is what buys time,
     * so the clock has to stop while somebody is doing it.
     */
    void BleedOut(uint32 diff)
    {
        if (g_downed.empty())
            return;

        std::vector<ObjectGuid> expired;
        std::vector<ObjectGuid> recovered;

        for (auto& [guid, state] : g_downed)
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(guid);

            if (!player || !player->IsInWorld())
                continue;

            if (!player->IsAlive())
            {
                recovered.push_back(guid);      // died some other way; nothing left to do
                continue;
            }

            /*
             * Held at the health they went down on.
             *
             * Out-of-combat regeneration would otherwise carry them from 1 HP to full over
             * the ten minutes they are lying there, which quietly undoes the rest of the
             * design: a body that has healed to full is no longer something an onlooker can
             * finish with a deliberate blow, and it reads as recovering when the whole point
             * is that they cannot recover unaided.
             *
             * Applied for the whole downed state rather than only the immunity window. The
             * grace is where it was noticed, but regeneration does not stop when the grace
             * expires, and neither should this.
             */
            if (player->GetHealth() > state.downedHealth)
                player->SetHealth(state.downedHealth);

            // Nothing here brings them round. Being incapacitated has to mean something,
            // so a passing heal or a tick of regeneration cannot undo it - only a
            // resurrection spell or smelling salts, both of which call Revive directly.

            // The grace runs on its own clock: it should expire while somebody is being
            // carried, not sit frozen and pop the moment they are set down.
            if (state.immuneMs)
            {
                if (state.immuneMs <= diff)
                {
                    state.immuneMs = 0;
                    /*
                     * Only the PLAYER half comes off.
                     *
                     * Finishing somebody who is already down should be a decision a person
                     * makes, so players get to reach them again. Guards and wildlife do not
                     * make decisions - a patrol walking past a body on the flagstones would
                     * simply beat it to death, which is neither drama nor anything the
                     * victim could answer. IMMUNE_TO_NPC therefore stays on for as long as
                     * they are down, and comes off in StandUp with everything else.
                     */
                    player->RemoveUnitFlag(UNIT_FLAG_IMMUNE_TO_PC);

                    if (g_downImmunitySpell)
                        player->RemoveAurasDueToSpell(g_downImmunitySpell);

                    // Neutral from here. Nothing treats them as an enemy any more, so a
                    // stray area effect leaves them alone and killing them takes somebody
                    // deliberately picking them out again.
                    if (g_downedFaction && !state.neutral)
                    {
                        state.neutral = true;
                        player->SetFaction(g_downedFaction);
                    }
                }
                else
                    state.immuneMs -= diff;
            }

            /*
             * Keep the icon and the clock telling the same story.
             *
             * The aura counts down on its own, but the module's timer stops while
             * somebody is carrying them - so without this the debuff would run out
             * while the player was in no danger at all. Re-synced only when the two
             * have drifted by more than a second, to avoid an aura update every tick.
             */
            if (g_bleedOutSpell)
                if (Aura* clock = player->GetAura(g_bleedOutSpell))
                    if (std::abs(clock->GetDuration() - int32(state.remainingMs)) > 1000)
                        clock->SetDuration(int32(state.remainingMs));

            if (!CarrierOf(player).IsEmpty())
                continue;

            if (state.remainingMs <= diff)
                expired.push_back(guid);
            else
                state.remainingMs -= diff;
        }

        for (ObjectGuid const& guid : recovered)
            if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
                StandUp(player, player->IsAlive());
            else
                g_downed.erase(guid);

        for (ObjectGuid const& guid : expired)
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(guid);
            if (!player)
            {
                g_downed.erase(guid);
                continue;
            }

            // Read before the state goes, because StandUp takes it with it.
            ObjectGuid fellerGuid;
            auto itr = g_downed.find(guid);

            if (itr != g_downed.end())
                fellerGuid = itr->second.feller;

            // Clear the state first: the death hooks run inside the kill and would otherwise
            // see a player who is still recorded as down. It also puts their own faction back
            // before they die, which is what the honour and kill-credit checks read - dying
            // as faction 2300 would be nobody killing nobody.
            StandUp(player, false);
            Release(player);

            /*
             * Credited to whoever put them there.
             *
             * KillSelf made every bleed-out a suicide, so nothing downstream that reads a
             * killer ever fired: no PvP credit, no bounty, no corpse worth looting. Downing
             * somebody and walking away is a way of killing them, and ten minutes later it
             * should count as one.
             *
             * A suicide is still the fallback, for a feller who has logged out, left the
             * map, or died first - which is most of them, ten minutes being a long time.
             */
            Unit* killer = fellerGuid.IsEmpty()
                ? nullptr : ObjectAccessor::GetUnit(*player, fellerGuid);

            if (killer && killer != player && killer->IsAlive())
                Unit::Kill(killer, player);
            else
                player->KillSelf();
        }
    }
}


/*
 * The cast landing.
 *
 * OnSpellCast is the last line of Spell::_cast, reached only after the bar filled AND the
 * second CheckCast passed. PlayerScript::OnPlayerSpellCast is the tempting one and is wrong:
 * it fires before that check.
 */
class sanctuary_downed_spellscript : public AllSpellScript
{
public:
    sanctuary_downed_spellscript() : AllSpellScript("sanctuary_downed_spellscript",
        { ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_SPELL_CHECK_CAST }) { }

    /*
     * Refuses a lift that was never going to work, before the bar starts.
     *
     * This hook can only DENY: it runs at the top of Spell::CheckCast with the result
     * already SPELL_CAST_OK and the next line returns early if a script changed it. That
     * is why it was useless for letting a resurrection through, and exactly what is
     * wanted here - without it you would stand over somebody for five seconds only to be
     * told you were too far away.
     */
    void OnSpellCheckCast(Spell* spell, bool /*strict*/, SpellCastResult& res) override
    {
        if (!g_enabled || !spell || !g_castSpell)
            return;

        SpellInfo const* info = spell->GetSpellInfo();

        if (!info || info->Id != g_castSpell)
            return;

        Player* carrier = spell->GetCaster() ? spell->GetCaster()->ToPlayer() : nullptr;

        if (!carrier)
            return;

        Player* target = ObjectAccessor::FindConnectedPlayer(carrier->GetTarget());
        SanctuaryDowned::Result const problem = Check(carrier, target);

        if (problem == SanctuaryDowned::Result::Ok)
            return;

        if (carrier->GetSession())
            ChatHandler(carrier->GetSession()).SendErrorMessage(
                SanctuaryDowned::Explain(problem));

        res = SPELL_FAILED_ERROR;
    }

    // ALLSPELLHOOK_ON_SPELL_CHECK_CAST is deliberately NOT taken. It is called at the
    // top of Spell::CheckCast with the result freshly set to SPELL_CAST_OK, and the very
    // next line returns early if a script changed it (Spell.cpp:5675). It can therefore
    // only refuse a cast, never permit one - so it cannot be used to let a resurrection
    // reach a living target, which is what it was briefly added for.


    void OnSpellCast(Spell* /*spell*/, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        if (!g_enabled || !spellInfo)
            return;

        /*
         * Resuscitate does the same work smelling salts do, and deliberately so: one
         * route for the classes trained in it, one for anybody who thought to carry a
         * vial, and no difference in the outcome.
         */
        bool const bySpell = g_reviveSpell && spellInfo->Id == g_reviveSpell;
        bool const byHearthdown = g_hearthdownSpell && spellInfo->Id == g_hearthdownSpell;

        if (bySpell || byHearthdown)
        {
            Player* healer = caster ? caster->ToPlayer() : nullptr;

            if (!healer)
                return;

            ChatHandler handler(healer->GetSession());
            Player* target = ObjectAccessor::FindConnectedPlayer(healer->GetTarget());

            if (!target || !SanctuaryDowned::IsDowned(target))
            {
                handler.SendErrorMessage(
                    "They are not down, and there is nothing to bring them back from.");
                return;
            }

            // The spells carry the client's shortest range, which is two yards; this is the
            // distance that decides, and it is deliberately tighter than the client's.
            if (!healer->IsWithinDistInMap(target, g_reviveRange))
            {
                handler.SendErrorMessage("You need to be right over them.");
                return;
            }

            if (!SanctuaryDowned::Revive(target, healer))
                return;

            // Only the vial is spent, and only once it has worked. Resuscitate costs
            // nothing but the five seconds.
            if (byHearthdown && g_hearthdownItem)
                healer->DestroyItemCount(g_hearthdownItem, 1, true);

            return;
        }

        if (spellInfo->Id != g_castSpell)
            return;

        Player* carrier = caster ? caster->ToPlayer() : nullptr;
        if (!carrier)
            return;

        /*
         * Two ways in, and they arrive differently.
         *
         * `.carry` remembers who was aimed at when the bar started, so five seconds of
         * casting cannot be redirected by clicking somebody else halfway through. Cast from
         * the spellbook there is no such record and the current target is all there is -
         * which is the same thing the client was showing while the bar ran.
         */
        ObjectGuid targetGuid;
        auto itr = g_pending.find(carrier->GetGUID());

        if (itr != g_pending.end())
        {
            targetGuid = itr->second;
            g_pending.erase(itr);
        }
        else
            targetGuid = carrier->GetTarget();

        // Re-tested from scratch: three seconds is long enough to walk out of reach, for
        // the target to be picked up by somebody else, or for either to die.
        Player* target = ObjectAccessor::FindConnectedPlayer(targetGuid);

        if (SanctuaryDowned::PickUp(carrier, target) != SanctuaryDowned::Result::Ok)
        {
            if (carrier->GetSession())
                ChatHandler(carrier->GetSession()).PSendSysMessage("You cannot lift them.");
        }
    }
};

class sanctuary_downed_playerscript : public PlayerScript
{
public:
    sanctuary_downed_playerscript() : PlayerScript("sanctuary_downed_playerscript") { }

    void OnPlayerLogout(Player* player) override
    {
        // Leaving while holding somebody would strand them in a seat on a player who is no
        // longer there, which no other event would ever clear.
        SanctuaryDowned::Release(player);

        // Before StandUp, which is what erases the state this reads.
        SanctuaryDowned::SaveDowned(player);

        /*
         * Stood up properly rather than merely forgotten.
         *
         * The downed state lives in memory and does not survive a logout, but the auras that
         * advertise it are written to character_aura and come back on login - so clearing
         * only the map left players wearing "Bleeding Out" while the server no longer
         * considered them down, and the addon dutifully showed the window for a state that
         * did not exist. StandUp takes the auras, the root, the stun and the faction off
         * together, which is what leaving the state should mean.
         */
        SanctuaryDowned::StandUp(player, false);
        SanctuaryDowned::ClearDowned(player);
        SanctuaryDowned::SetSeatOverride(player, 0);
    }

    /*
     * Sweeps up anything a previous session left behind.
     *
     * The logout hook covers an orderly exit, but a server restart or a crash never runs
     * it, and the auras are already saved by then. Those are leftovers and come off. A row
     * in sanctuary_downed is the opposite: an orderly exit while down, which goes back on.
     */
    void OnPlayerLogin(Player* player) override
    {
        SanctuaryDowned::StripDownedAuras(player);

        // And then back down, if they left that way. Stripping first is what makes a crash
        // - auras saved with no row behind them - end with the player on their feet.
        SanctuaryDowned::RestoreDowned(player);

        SanctuaryDowned::TeachReviveSpell(player);
        SanctuaryDowned::TeachCarrySpell(player);
    }

    void OnPlayerJustDied(Player* player) override
    {
        SanctuaryDowned::Release(player);
        SanctuaryDowned::ClearDowned(player);
    }

    void OnPlayerUpdateZone(Player* player, uint32 /*newZone*/, uint32 /*newArea*/) override
    {
        // A zone change usually means a teleport, and a seat does not survive one.
        if (!SanctuaryDowned::CarriedBy(player).IsEmpty() && !player->GetVehicleKit())
            SanctuaryDowned::Release(player);
    }
};

class sanctuary_downed_worldscript : public WorldScript
{
public:
    sanctuary_downed_worldscript() : WorldScript("sanctuary_downed_worldscript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE }) { }

    /*
     * Reported here rather than from OnAfterConfigLoad, which runs before the DBC stores
     * are populated - a cast time looked up there is always null, and the banner said
     * "0 ms cast" while the spell was in fact correct. A log line that lies costs more than
     * one that arrives slightly later.
     */
    void OnStartup() override
    {
        if (!g_enabled || !g_castSpell)
            return;

        SpellInfo const* spell = sSpellMgr->GetSpellInfo(g_castSpell);

        if (!spell)
        {
            LOG_ERROR("module.sanctuarydowned",
                "Carry cast spell {} does not exist; lifting will be instant.", g_castSpell);
            g_castSpell = 0;
            return;
        }

        LOG_INFO("module.sanctuarydowned", "Carry cast: spell {} at {} ms, cancelled by movement.",
            g_castSpell, spell->CastTimeEntry ? spell->CastTimeEntry->CastTime : 0);
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>("SanctuaryDowned.Enable", true);
        g_downEnabled = sConfigMgr->GetOption<bool>("SanctuaryDowned.Downed.Enable", true);
        g_downFromPlayers = sConfigMgr->GetOption<bool>("SanctuaryDowned.Downed.FromPlayers", true);
        g_downFromCreatures = sConfigMgr->GetOption<bool>("SanctuaryDowned.Downed.FromCreatures", false);
        g_bleedOutMs = sConfigMgr->GetOption<uint32>("SanctuaryDowned.Downed.BleedOutSeconds", 600) * 1000;
        g_reviveHealthPct = sConfigMgr->GetOption<uint32>("SanctuaryDowned.Downed.ReviveHealthPct", 15);
        g_downImmunityMs = sConfigMgr->GetOption<uint32>("SanctuaryDowned.Downed.ImmunitySeconds", 5) * 1000;
        g_downedFaction = sConfigMgr->GetOption<uint32>("SanctuaryDowned.Downed.NeutralFaction", 2300);
        g_reviveRange = sConfigMgr->GetOption<float>("SanctuaryDowned.Downed.ReviveRange", 0.5f);
        g_downImmunitySpell = sConfigMgr->GetOption<uint32>("SanctuaryDowned.Downed.ImmunitySpell", 81002);
        g_bleedOutSpell = sConfigMgr->GetOption<uint32>("SanctuaryDowned.Downed.BleedOutSpell", 81003);
        g_reviveSpell = sConfigMgr->GetOption<uint32>("SanctuaryDowned.Downed.ReviveSpell", 81004);
        g_hearthdownSpell = sConfigMgr->GetOption<uint32>("SanctuaryDowned.Downed.HearthdownSpell", 81005);
        g_hearthdownItem = sConfigMgr->GetOption<uint32>("SanctuaryDowned.Downed.HearthdownItem", 990004);

        // "2 5 7 11" - paladin, priest, shaman, druid. Separators are anything but digits,
        // so commas and spaces both work and nobody has to guess which.
        g_reviveClasses.clear();
        {
            std::string const raw = sConfigMgr->GetOption<std::string>(
                "SanctuaryDowned.Downed.ReviveSpellClasses", "2,5,7,11");
            uint32 value = 0;
            bool building = false;

            for (char c : raw + " ")
            {
                if (c >= '0' && c <= '9')
                {
                    value = value * 10 + uint32(c - '0');
                    building = true;
                }
                else if (building)
                {
                    g_reviveClasses.push_back(value);
                    value = 0;
                    building = false;
                }
            }
        }

        g_vehicleId = sConfigMgr->GetOption<uint32>("SanctuaryDowned.Carry.VehicleId", 284);
        g_seatId = int8(sConfigMgr->GetOption<int32>("SanctuaryDowned.Carry.SeatId", 0));
        g_reach = sConfigMgr->GetOption<float>("SanctuaryDowned.Carry.Reach", 0.5f);
        g_carrySpeed = sConfigMgr->GetOption<float>("SanctuaryDowned.Carry.SpeedFactor", 0.7f);
        g_announce = sConfigMgr->GetOption<bool>("SanctuaryDowned.Announce", true);

        g_castSpell = sConfigMgr->GetOption<uint32>("SanctuaryDowned.Carry.CastSpell", 81006);
        g_carriedSpell = sConfigMgr->GetOption<uint32>("SanctuaryDowned.Carry.CarriedSpell", 81007);
        g_carrierEmote = sConfigMgr->GetOption<uint32>("SanctuaryDowned.Carry.CarrierEmote", 428);

        g_mountedVehicleId = sConfigMgr->GetOption<uint32>("SanctuaryDowned.Carry.MountedVehicleId", 0);
        g_deltaCount = sConfigMgr->GetOption<uint32>("SanctuaryDowned.Carry.DeltaCount", 8);
        g_trimCount = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("SanctuaryDowned.Carry.TrimCount", 1));

        for (auto& byGender : g_deltaByPassenger)
            byGender.fill(0);

        for (auto& byGender : g_trimByCarrier)
            byGender.fill(g_trimCount / 2);

        // <Race>Female is read on top of <Race> rather than instead of it, because only
        // tauren split by gender - every other race renders both at the same scale, so one
        // key covers them and the second never has to be written.
        for (auto& byGender : g_vehicleByCarrier)
            byGender.fill(0);

        for (uint8 race = 1; race < g_vehicleByCarrier.size(); ++race)
        {
            if (!RaceNames[race][0])
                continue;

            std::string const key = std::string("SanctuaryDowned.Carry.VehicleId.") + RaceNames[race];
            uint32 const both = sConfigMgr->GetOption<uint32>(key, 0);

            g_vehicleByCarrier[race][0] = both;
            g_vehicleByCarrier[race][1] = sConfigMgr->GetOption<uint32>(key + "Female", both);

            std::string const dkey = std::string("SanctuaryDowned.Carry.Delta.") + RaceNames[race];
            uint32 const dboth = sConfigMgr->GetOption<uint32>(dkey, 0);

            g_deltaByPassenger[race][0] = dboth;
            g_deltaByPassenger[race][1] = sConfigMgr->GetOption<uint32>(dkey + "Female", dboth);

            std::string const tkey = std::string("SanctuaryDowned.Carry.Trim.") + RaceNames[race];
            uint32 const tboth = sConfigMgr->GetOption<uint32>(tkey, g_trimCount / 2);

            g_trimByCarrier[race][0] = tboth;
            g_trimByCarrier[race][1] = sConfigMgr->GetOption<uint32>(tkey + "Female", tboth);
        }

        LOG_INFO("module.sanctuarydowned", "Sanctuary carrying {}: vehicle {} seat {}, reach {} yards.",
            g_enabled ? "enabled" : "disabled", g_vehicleId, g_seatId, g_reach);
    }

    void OnUpdate(uint32 diff) override
    {
        SanctuaryDowned::BleedOut(diff);

        _sinceSweep += diff;

        // Twice a second. Fast enough that a dismount barely shows, cheap enough to run
        // against a map of carries that is almost always empty.
        if (_sinceSweep < 500)
            return;

        _sinceSweep = 0;
        SanctuaryDowned::ReseatLapsed();
    }

private:
    uint32 _sinceSweep = 0;
};

/*
 * Turns a killing blow into a knockdown.
 *
 * OnDamage runs before the damage lands, so clamping here leaves the player on 1 HP rather
 * than resurrecting a corpse afterwards - which matters because a corpse cannot be picked
 * up: Unit::_EnterVehicle refuses a dead unit outright.
 */
class sanctuary_downed_unitscript : public UnitScript
{
public:
    sanctuary_downed_unitscript() : UnitScript("sanctuary_downed_unitscript", true,
        { UNITHOOK_ON_DAMAGE }) { }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!g_enabled || !g_downEnabled || !damage)
            return;

        Player* player = victim ? victim->ToPlayer() : nullptr;

        if (!player || !player->IsAlive())
            return;

        // Already down. During the grace they are flagged immune and nothing reaches them;
        // after it they are neutral, so anyone still swinging has deliberately picked them
        // out again - and that should finish them rather than being absorbed.
        if (SanctuaryDowned::IsDowned(player))
            return;

        if (damage < player->GetHealth())
            return;

        // The core already clamps lethal duel damage and ends the duel; going down here
        // instead would leave the loser prone with no duel left to lose.
        if (player->duel)
            return;

        // A GM being shot at while setting a scene should not end up on the floor.
        if (player->IsGameMaster())
            return;

        bool const byPlayer = attacker && attacker->GetTypeId() == TYPEID_PLAYER;

        if (byPlayer ? !g_downFromPlayers : !g_downFromCreatures)
            return;

        damage = player->GetHealth() - 1;
        SanctuaryDowned::GoDown(player, attacker);
    }
};


void AddSC_sanctuary_downed_commandscript();

void AddSC_sanctuary_downed_scripts()
{
    new sanctuary_downed_spellscript();
    new sanctuary_downed_playerscript();
    new sanctuary_downed_worldscript();
    new sanctuary_downed_unitscript();
    AddSC_sanctuary_downed_commandscript();
}

namespace
{
    /*
     * The twenty playable body types, in the order carry_anchors.py generates them.
     *
     * This ordering IS the delta index and the carrier block order, so it has to match the
     * generator exactly: races in the order below, male then female for each. Race 9 is
     * goblin and is not playable in 3.3.5a, so it is absent from both.
     */
    struct BodyType
    {
        uint8 race;
        uint8 sex;
    };

    constexpr BodyType TestBodyTypes[20] =
    {
        { 1, 0 }, { 1, 1 },   // human
        { 2, 0 }, { 2, 1 },   // orc
        { 3, 0 }, { 3, 1 },   // dwarf
        { 4, 0 }, { 4, 1 },   // night elf
        { 5, 0 }, { 5, 1 },   // undead
        { 6, 0 }, { 6, 1 },   // tauren
        { 7, 0 }, { 7, 1 },   // gnome
        { 8, 0 }, { 8, 1 },   // troll
        { 10, 0 }, { 10, 1 }, // blood elf
        { 11, 0 }, { 11, 1 }, // draenei
    };

    /// Faction 35, no movement, no flags - so twenty rows of them just stand there.
    constexpr uint32 TEST_DUMMY_ENTRY = 30527;
    constexpr float TEST_SPACING = 7.0f;

    std::vector<ObjectGuid> g_testGrid;

    /*
     * A textured NPC display per body type, and the scale that makes it player-sized.
     *
     * ChrRaces' own display ids cannot be used here. Those are character displays: the
     * client expects per-player customization to arrive with them and an NPC has none, so a
     * creature morphed to one renders untextured - flat blue. Each id below is an NPC
     * display built on the SAME model, whose CreatureDisplayInfoExtra carries a baked
     * texture, so it renders dressed.
     *
     * The scale is the player display's CreatureModelScale over this display's. It is 1.0
     * for eighteen of the twenty; tauren male and gnome female are the exceptions, and a
     * grid rendering at the wrong size would not be testing what it is meant to test.
     */
    struct TestDisplay
    {
        uint32 display;
        float scale;
    };

    constexpr TestDisplay TestDisplays[20] =
    {
        {  1276, 1.0000f },   // human male
        {   176, 1.0000f },   // human female
        {  1139, 1.0000f },   // orc male
        {  1312, 1.0000f },   // orc female
        {   115, 1.0000f },   // dwarf male
        {  1286, 1.0000f },   // dwarf female
        {  1285, 1.0000f },   // night elf male
        {  1543, 1.0000f },   // night elf female
        {  1027, 1.0000f },   // undead male
        {  1029, 1.0000f },   // undead female
        { 21200, 0.9643f },   // tauren male  - renders 1.40 against the player's 1.35
        {  1905, 1.0000f },   // tauren female
        { 21711, 1.0000f },   // gnome male
        {  6982, 1.0455f },   // gnome female - renders 1.10 against the player's 1.15
        {  1976, 1.0000f },   // troll male
        {  1882, 1.0000f },   // troll female
        { 10375, 1.0000f },   // blood elf male
        { 15505, 1.0000f },   // blood elf female
        { 16199, 1.0000f },   // draenei male
        { 16200, 1.0000f },   // draenei female
    };


    /// Same composition the live resolver uses, but from ids rather than from two Players.
    uint32 TestVehicleFor(BodyType carrier, BodyType passenger)
    {
        uint32 const base = g_vehicleByCarrier[carrier.race][carrier.sex];
        if (!base)
            return 0;

        uint32 delta = g_deltaByPassenger[passenger.race][passenger.sex];
        if (delta >= g_deltaCount)
            delta = 0;

        uint32 trim = g_trimByCarrier[carrier.race][carrier.sex];
        if (trim >= g_trimCount)
            trim = g_trimCount / 2;

        return base + delta * g_trimCount + trim;
    }

    Creature* SpawnDummy(Player* gm, uint32 body, float x, float y, float z, float o)
    {
        TempSummon* summon = gm->SummonCreature(TEST_DUMMY_ENTRY, x, y, z, o,
            TEMPSUMMON_MANUAL_DESPAWN);

        if (!summon)
            return nullptr;

        summon->SetDisplayId(TestDisplays[body].display);
        summon->SetObjectScale(TestDisplays[body].scale);

        // Inert scenery: they exist to be looked at, not interacted with.
        summon->SetReactState(REACT_PASSIVE);
        summon->SetUnitFlag(UnitFlags(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC |
                                      UNIT_FLAG_IMMUNE_TO_NPC));
        summon->SetImmuneToAll(true);

        g_testGrid.push_back(summon->GetGUID());
        return summon;
    }
}

namespace SanctuaryDowned
{
    /// Needs a world anchor to resolve guids, so it takes whoever asked for the clear.
    void ClearTestGrid(Player* anchor)
    {
        if (anchor)
            for (ObjectGuid const& guid : g_testGrid)
                if (Creature* creature = ObjectAccessor::GetCreature(*anchor, guid))
                    creature->DespawnOrUnsummon();

        g_testGrid.clear();
    }

    /*
     * Spawns one column per carrier body type, and one row per passenger body type.
     *
     * A whole grid is 400 pairs and so 800 creatures, which is a lot to look at and a lot to
     * load; passing a single passenger index spawns just that row instead, which is the
     * usual case when one body is misbehaving.
     */
    uint32 SpawnTestGrid(Player* gm, int32 onlyPassenger)
    {
        if (!gm)
            return 0;

        ClearTestGrid(gm);

        float const baseX = gm->GetPositionX();
        float const baseY = gm->GetPositionY();
        float const z = gm->GetPositionZ();
        float const o = gm->GetOrientation();

        uint32 spawned = 0;

        // Rows are counted as they are laid down, not taken from the passenger index. Using
        // the index put a single filtered row as far north as its place in the full grid
        // would have been - row 13 landed 91 yards west, which from where you are standing
        // is indistinguishable from nothing having spawned at all.
        uint32 row = 0;

        for (uint32 p = 0; p < 20; ++p)
        {
            if (onlyPassenger >= 0 && uint32(onlyPassenger) != p)
                continue;

            for (uint32 c = 0; c < 20; ++c)
            {
                uint32 const vehicle = TestVehicleFor(TestBodyTypes[c], TestBodyTypes[p]);
                if (!vehicle)
                    continue;

                float const x = baseX + float(c) * TEST_SPACING;
                float const y = baseY + float(row) * TEST_SPACING;

                Creature* carrier = SpawnDummy(gm, c, x, y, z, o);
                if (!carrier)
                    continue;

                if (!carrier->CreateVehicleKit(vehicle, TEST_DUMMY_ENTRY))
                    continue;

                Creature* body = SpawnDummy(gm, p, x, y, z, o);
                if (!body)
                    continue;

                body->EnterVehicle(carrier, 0);

                // A creature does not take the seat's ride animation the way a player does,
                // so without this every body in the grid stands to attention on the
                // shoulder and the pose being judged is not the one players will see.
                // EMOTE_STATE_DROWNED is the core's own enum: emote 383, animation 132.
                body->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_STATE_DROWNED);
                ++spawned;
            }

            ++row;
        }

        return spawned;
    }
}
