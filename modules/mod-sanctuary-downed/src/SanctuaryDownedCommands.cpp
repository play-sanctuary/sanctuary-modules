/*
 * mod-sanctuary-downed - in-game commands
 *
 * Chat commands are how the carry gets exercised while it is being proved out. They stay
 * afterwards, so everything remains reachable without an addon.
 */

#include "SanctuaryDowned.h"

#include "Chat.h"
#include "CommandScript.h"
#include "Player.h"
#include "RBAC.h"

using namespace Acore::ChatCommands;

namespace
{
    // Added by this module's auth SQL and linked under "Role: Player Commands".
    // 100001 voice, 100002 outlaw, 100003 lawman, 100005 disguise.
    constexpr uint32 RBAC_PERM_COMMAND_CARRY = 100004;

    // And the staging tools, under "Role: Gamemaster Commands" instead.
    //
    // `.carry testgrid` summons four hundred dummies with TEMPSUMMON_MANUAL_DESPAWN, so
    // they stay until somebody clears them. Sharing the player permission meant anybody
    // could leave four hundred creatures standing in a field, twice over if they ran it
    // again. `.carry seat` and `.carry mountseat` come along because they take an
    // arbitrary vehicle id, which is a measuring tool rather than a thing to play with.
    constexpr uint32 RBAC_PERM_COMMAND_CARRY_STAGING = 100006;

    bool Report(ChatHandler* handler, SanctuaryDowned::Result result)
    {
        if (char const* problem = SanctuaryDowned::Explain(result))
        {
            handler->SendErrorMessage(problem);
            return false;
        }

        return true;
    }
}

class sanctuary_downed_commandscript : public CommandScript
{
public:
    sanctuary_downed_commandscript() : CommandScript("sanctuary_downed_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable carryCommandTable =
        {
            { "drop", HandleCarryDropCommand, RBAC_PERM_COMMAND_CARRY, Console::No },
            { "seat", HandleCarrySeatCommand, RBAC_PERM_COMMAND_CARRY_STAGING, Console::No },
            { "mountseat", HandleCarryMountSeatCommand, RBAC_PERM_COMMAND_CARRY_STAGING, Console::No },
            { "testgrid", HandleCarryTestGridCommand, RBAC_PERM_COMMAND_CARRY_STAGING, Console::No },
            // Bare `.carry` lifts your target, which is the common case.
            { "",     HandleCarryCommand,     RBAC_PERM_COMMAND_CARRY, Console::No }
        };

        static ChatCommandTable commandTable =
        {
            { "carry", carryCommandTable },
            // Its own command rather than a subcommand of carry: the person using it is
            // the one on the floor, and they should not have to think about carrying.
            { "bleedout", HandleBleedOutCommand, RBAC_PERM_COMMAND_CARRY, Console::No },
            // Also its own command: the person using it is the one over somebody else's
            // shoulder, and asking them to think about "carry" would be backwards. The
            // addon puts a button on this, which is how anybody will actually use it.
            { "getdown", HandleGetDownCommand, RBAC_PERM_COMMAND_CARRY, Console::No }
        };
        return commandTable;
    }

    static bool HandleCarryCommand(ChatHandler* handler, Optional<PlayerIdentifier> who)
    {
        Player* carrier = handler->GetSession()->GetPlayer();

        // Already carrying somebody: treat a second `.carry` as putting them down, so one
        // key can do both without the player having to remember which state they are in.
        if (!SanctuaryDowned::CarriedBy(carrier).IsEmpty())
            return Report(handler, SanctuaryDowned::PutDown(carrier));

        // A name is taken as well as a selection. GM mode makes clicking somebody
        // unreliable - a GM staging a scene is exactly who needs to lift a body they
        // cannot conveniently target - so `.carry Brok` is accepted alongside `.carry`.
        if (!who)
            who = PlayerIdentifier::FromTarget(handler);

        // Begins the cast; the lift happens when the bar finishes, and moving cancels it.
        return Report(handler, SanctuaryDowned::BeginPickUp(
            carrier, who ? who->GetConnectedPlayer() : nullptr));
    }

    /*
     * Climb down off somebody's shoulder.
     *
     * Only works while conscious. Somebody genuinely unconscious has no say in where
     * they are taken, which is the whole reason the downed state is worth having.
     */
    static bool HandleGetDownCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();

        if (!SanctuaryDowned::GetDown(player))
        {
            handler->SendErrorMessage(
                SanctuaryDowned::IsDowned(player)
                    ? "You are in no state to."
                    : "Nobody is carrying you.");
            return false;
        }

        return true;
    }

    /*
     * Stop waiting.
     *
     * Ten minutes is a long time to lie still if nobody is coming, and the alternative -
     * logging out - would leave the body in the world with a clock still running on it.
     */
    static bool HandleBleedOutCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();

        if (!SanctuaryDowned::GiveUp(player))
        {
            handler->SendErrorMessage("You are not down.");
            return false;
        }

        handler->PSendSysMessage("You stop holding on.");
        return true;
    }

    /*
     * Tries a different seat, which is the only way to move the body.
     *
     * The offsets are in the client's VehicleSeat.dbc, so they cannot be set from here -
     * but which seat is used can be, and a spread of candidates ships in the patch. This
     * turns tuning from a rebuild each time into a session of looking.
     */
    static bool HandleCarrySeatCommand(ChatHandler* handler, Optional<uint32> vehicleId)
    {
        Player* carrier = handler->GetSession()->GetPlayer();

        if (!vehicleId)
        {
            uint32 const current = SanctuaryDowned::GetSeatOverride(carrier);

            if (!current)
            {
                handler->PSendSysMessage("No seat override; your race mapping is in use.");
                handler->PSendSysMessage("Try one with |cffffffff.carry seat <vehicle>|r, or 0 to clear.");
                return true;
            }

            Describe(handler, current);
            return true;
        }

        if (*vehicleId == 0)
        {
            SanctuaryDowned::SetSeatOverride(carrier, 0);
            handler->PSendSysMessage("Seat override cleared.");
            return true;
        }

        uint32 seat = 0;
        float x = 0.f, y = 0.f, z = 0.f;

        if (!SanctuaryDowned::DescribeSeat(*vehicleId, seat, x, y, z))
        {
            handler->SendErrorMessage("No vehicle with that id, or it has no first seat.");
            return false;
        }

        SanctuaryDowned::SetSeatOverride(carrier, *vehicleId);
        Describe(handler, *vehicleId);

        // Applied to whoever is already up, so candidates can be compared back to back.
        if (SanctuaryDowned::ReseatCarried(carrier))
            handler->PSendSysMessage("Moved them onto it.");

        return true;
    }

    /*
     * The seat used while mounted, which only matters if a mount moves the body.
     *
     * Kept apart from `.carry seat` rather than folded into it so both states can be
     * held at once: set one on foot, set the other mounted, and mounting swaps between
     * them without either value being lost.
     */
    static bool HandleCarryMountSeatCommand(ChatHandler* handler, Optional<uint32> vehicleId)
    {
        Player* carrier = handler->GetSession()->GetPlayer();

        if (!vehicleId)
        {
            uint32 const current = SanctuaryDowned::GetMountSeatOverride(carrier);

            if (!current)
            {
                handler->PSendSysMessage("No mounted seat override; the on-foot seat is used throughout.");
                return true;
            }

            Describe(handler, current);
            return true;
        }

        if (*vehicleId == 0)
        {
            SanctuaryDowned::SetMountSeatOverride(carrier, 0);
            handler->PSendSysMessage("Mounted seat override cleared.");
            return true;
        }

        uint32 seat = 0;
        float x = 0.f, y = 0.f, z = 0.f;

        if (!SanctuaryDowned::DescribeSeat(*vehicleId, seat, x, y, z))
        {
            handler->SendErrorMessage("No vehicle with that id, or it has no first seat.");
            return false;
        }

        SanctuaryDowned::SetMountSeatOverride(carrier, *vehicleId);
        Describe(handler, *vehicleId);

        // Only bites once mounted; the sweep swaps it in when that happens.
        if (carrier->IsMounted() && SanctuaryDowned::ReseatCarried(carrier))
            handler->PSendSysMessage("Moved them onto it.");
        else if (!carrier->IsMounted())
            handler->PSendSysMessage("Applies when you mount.");

        return true;
    }

    /*
     * Spawns the whole matrix as morphed dummies so it can be judged in one pass.
     *
     * Carriers run north (+X), passengers run west (+Y). A bad ROW therefore means a
     * passenger's delta is wrong, and a bad COLUMN means a carrier's trim is - a far
     * quicker read than four hundred separate relogs.
     */
    static bool HandleCarryTestGridCommand(ChatHandler* handler, Optional<std::string> arg)
    {
        Player* gm = handler->GetSession()->GetPlayer();

        if (arg && *arg == "clear")
        {
            SanctuaryDowned::ClearTestGrid(gm);
            handler->PSendSysMessage("Test grid cleared.");
            return true;
        }

        int32 only = -1;
        if (arg && !arg->empty())
            only = int32(atoi(arg->c_str()));

        uint32 const pairs = SanctuaryDowned::SpawnTestGrid(gm, only);

        // The extent is spelled out because a grid that lands off in the distance looks
        // exactly like one that never spawned, and that cost a debugging round trip once.
        uint32 const rows = only >= 0 ? 1 : 20;
        uint32 const columns = pairs && rows ? pairs / rows : 0;

        handler->PSendSysMessage("Spawned {} carried pairs: {} carriers north x {} passengers west.",
            pairs, columns, rows);
        handler->PSendSysMessage("They start where you stand and run {} yards north, {} west.",
            columns ? (columns - 1) * 7 : 0, rows ? (rows - 1) * 7 : 0);
        handler->PSendSysMessage("Clear them with |cffffffff.carry testgrid clear|r.");
        return true;
    }

    static void Describe(ChatHandler* handler, uint32 vehicleId)
    {
        uint32 seat = 0;
        float x = 0.f, y = 0.f, z = 0.f;

        if (SanctuaryDowned::DescribeSeat(vehicleId, seat, x, y, z))
            handler->PSendSysMessage("Vehicle {} seat {}: forward {:.2f}, right {:.2f}, up {:.2f}",
                vehicleId, seat, x, -y, z);
    }

    static bool HandleCarryDropCommand(ChatHandler* handler)
    {
        return Report(handler, SanctuaryDowned::PutDown(handler->GetSession()->GetPlayer()));
    }
};

void AddSC_sanctuary_downed_commandscript()
{
    new sanctuary_downed_commandscript();
}
