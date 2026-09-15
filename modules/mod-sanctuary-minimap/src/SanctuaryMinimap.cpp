/*
 * mod-sanctuary-minimap - the server half of tracking several places at once.
 *
 * WHY THIS EXISTS. The client tracks exactly one "place" kind at a time - mailbox, banker,
 * repair, and the rest - and that is not a rule anyone can lift: the client keeps a single
 * slot for it, remembers the name of it in the `minimapTrackedInfo` cvar between sessions,
 * and its SetTracking() takes one index. Several at once came in a later expansion.
 *
 * So the client's own tracking is left alone and the pins are drawn by the addon instead.
 * The only thing an addon cannot do for itself is know WHERE anything is: 3.3.5 gives no
 * world position to Lua at all. That is what this sends.
 *
 *
 * WHAT CROSSES THE WIRE, and why it is offsets rather than coordinates.
 *
 * Each pin is sent as a kind and an offset in yards, north and east of the player, rounded
 * to whole yards. Offsets because the addon can do nothing with a world coordinate: it
 * cannot convert one, having no position of its own to compare against. An offset it can
 * use directly - rotate it by the player's facing and scale it by the minimap's zoom.
 *
 * The cost of that is staleness: an offset is only true for the moment it was measured, so
 * they are re-sent once a second and the addon slides them in between using its own map
 * position. A second of walking is seven yards on a minimap a hundred yards across.
 *
 * Pins are batched - up to twenty to a message - because an addon message is capped at 255
 * bytes and one message per pin would be twenty a second for a city full of mailboxes.
 *
 *
 * WHAT IT COSTS THE SERVER. One grid search per tracking player per second, over a radius
 * wide enough for the widest zoom. Nothing is searched for a player who is tracking nothing,
 * which is nearly everybody nearly all the time.
 */

#include "Cell.h"
#include "CellImpl.h"
#include "Chat.h"
#include "DBCStores.h"
#include "Creature.h"
#include "Config.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    bool g_enabled = true;
    std::string g_prefix = "SMM";

    // Wide enough for the wildest zoom the minimap offers (its widest view is a little
    // over 230 yards across), with room for the second of movement between pushes.
    float g_radius = 150.0f;

    // A city has more mailboxes than a minimap has room for. The nearest of each kind are
    // kept, so a cap cannot leave one kind starved by another's crowd.
    uint32 g_perKind = 6;

    // And a cap on the whole answer, because every kind ticked at once in a city would be
    // fifteen times the above. The addon holds the same number (its MAX_PINS) and the two
    // are meant to agree; past this many a minimap is a wall of icons anyway.
    uint32 g_total = 40;

    uint32 g_interval = 1000;

    /*
     * The kinds, in the order their bits sit in the mask the addon sends. The addon has the
     * same list in the same order and neither half may be reordered without the other.
     *
     * Each is one npc flag, except the mailbox, which is a gameobject rather than a
     * creature - a mailbox has a flag of its own too, for the handful that are creatures.
     */
    enum Kind : uint32
    {
        KIND_MAILBOX = 0,
        KIND_BANKER,
        KIND_AUCTIONEER,
        KIND_INNKEEPER,
        KIND_FLIGHTMASTER,
        KIND_STABLEMASTER,
        KIND_BATTLEMASTER,
        KIND_REPAIR,
        KIND_TRAINER_CLASS,
        KIND_TRAINER_PROFESSION,
        KIND_VENDOR,
        KIND_VENDOR_AMMO,
        KIND_VENDOR_FOOD,
        KIND_VENDOR_POISON,
        KIND_VENDOR_REAGENT,
        KIND_MAX
    };

    constexpr uint32 KindFlag[KIND_MAX] =
    {
        UNIT_NPC_FLAG_MAILBOX,
        UNIT_NPC_FLAG_BANKER,
        UNIT_NPC_FLAG_AUCTIONEER,
        UNIT_NPC_FLAG_INNKEEPER,
        UNIT_NPC_FLAG_FLIGHTMASTER,
        UNIT_NPC_FLAG_STABLEMASTER,
        UNIT_NPC_FLAG_BATTLEMASTER,
        UNIT_NPC_FLAG_REPAIR,
        UNIT_NPC_FLAG_TRAINER_CLASS,
        UNIT_NPC_FLAG_TRAINER_PROFESSION,
        UNIT_NPC_FLAG_VENDOR,
        UNIT_NPC_FLAG_VENDOR_AMMO,
        UNIT_NPC_FLAG_VENDOR_FOOD,
        UNIT_NPC_FLAG_VENDOR_POISON,
        UNIT_NPC_FLAG_VENDOR_REAGENT,
    };

    // Who is tracking what, by mask. A player tracking nothing is not in here at all.
    struct Tracked
    {
        uint32 mask;
    };

    std::unordered_map<ObjectGuid, Tracked> g_tracking;

    struct Pin
    {
        uint32 kind;
        int32 north;
        int32 east;
        float distance;
    };

    /// The shape every Sanctuary addon uses: a whisper to self in the addon language,
    /// which no chat window ever shows.
    void Send(Player* to, std::string const& body)
    {
        if (!to || !to->GetSession())
            return;

        std::string payload = g_prefix + "\t" + body;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, to, to, payload);
        to->GetSession()->SendPacket(&data);
    }

    /*
     * The creatures a tracking player wants to see, as a check the grid search can run.
     *
     * Dead, invisible and hostile-to-nobody creatures are all still worth a pin - a dead
     * banker is a banker who will be back - but a creature the player cannot see at all is
     * not, so phase and visibility are left to the searcher and to this check in turn.
     */
    struct PoiCreatureCheck
    {
        PoiCreatureCheck(Player const* player, uint32 mask, float radius)
            : _player(player), _mask(mask), _radius(radius) { }

        bool operator()(Creature* creature) const
        {
            if (!creature || !creature->IsInWorld())
                return false;

            if (!_player->IsWithinDist(creature, _radius, false))
                return false;

            // A creature the player cannot see is not a landmark they can walk to: another
            // phase's innkeeper, or one a game master has hidden. Asked of the player
            // rather than the creature, which is where the core keeps this.
            if (!_player->CanSeeOrDetect(creature))
                return false;

            return KindOf(creature) != KIND_MAX;
        }

        uint32 KindOf(Creature const* creature) const
        {
            for (uint32 kind = 0; kind < KIND_MAX; ++kind)
                if ((_mask & (1u << kind)) && creature->HasNpcFlag(NPCFlags(KindFlag[kind])))
                    return kind;

            return KIND_MAX;
        }

        Player const* _player;
        uint32 _mask;
        float _radius;
    };

    struct MailboxCheck
    {
        MailboxCheck(Player const* player, float radius) : _player(player), _radius(radius) { }

        bool operator()(GameObject* object) const
        {
            return object && object->IsInWorld()
                && object->GetGoType() == GAMEOBJECT_TYPE_MAILBOX
                && _player->IsWithinDist(object, _radius, false)
                // Phased areas have their own mailboxes, and the other phase's is not one
                // this player can walk up to.
                && _player->CanSeeOrDetect(object);
        }

        Player const* _player;
        float _radius;
    };

    /*
     * North and east of the player, in TENTHS of a yard.
     *
     * The game's axes are not the compass: +x is north and +y is west, and the minimap is
     * drawn with north up. So east is -y, and that sign is the whole reason to do this here
     * rather than leave it to the addon to get wrong.
     *
     * Tenths rather than whole yards because the minimap is about a yard to the pixel at its
     * closest zoom, so rounding to a yard lets every pin jump up to half a pixel each time
     * an answer lands - a visible shimmer on something that is not moving.
     */
    Pin Offset(Player const* player, WorldObject const* object, uint32 kind)
    {
        float const north = object->GetPositionX() - player->GetPositionX();
        float const east = -(object->GetPositionY() - player->GetPositionY());

        return Pin{ kind, int32(std::lround(north * 10.0f)), int32(std::lround(east * 10.0f)),
                    std::sqrt((north * north) + (east * east)) };
    }

    /*
     * Yards per unit of the client's own map position, for the zone the player is in, on
     * each axis - the number the addon needs to slide its pins along between answers.
     *
     * It is sampled through the core's own converter rather than read out of
     * WorldMapArea.dbc, which DBCStores deliberately keeps to itself. Convert the player's
     * position, convert a point a hundred yards north and another a hundred yards east, and
     * the differences give the scale exactly.
     *
     * The addon used to work this out for itself, by dividing how far it had been told it
     * moved by how far its map position had changed. That worked, but both figures were
     * rounded to whole yards, so a slow second of walking taught a scale that was some
     * percent out, and the pins crept until the next answer. Asking the zone is exact.
     *
     * False for a place with no map of its own - an instance - where the client gives Lua
     * no map position either, so there is nothing to slide.
     */
    bool ZoneScale(Player const* player, int32& perNorth, int32& perEast,
                   int32& mapX, int32& mapY)
    {
        uint32 const zone = player->GetZoneId();

        float const x = player->GetPositionX();
        float const y = player->GetPositionY();

        // Map2ZoneCoordinates returns its input untouched when the zone has no entry, which
        // is how an instance shows up here.
        float ax = x, ay = y;
        Map2ZoneCoordinates(ax, ay, zone);

        if (ax == x && ay == y)
            return false;

        constexpr float Step = 100.0f;

        float nx = x + Step, ny = y;            // a hundred yards north
        Map2ZoneCoordinates(nx, ny, zone);

        float ex = x, ey = y - Step;            // and a hundred yards east, east being -y
        Map2ZoneCoordinates(ex, ey, zone);

        /*
         * The converter works in hundredths of the map and swaps the axes on the way out:
         * what it returns as y came from the world's x, which is north. The client's own
         * GetPlayerMapPosition is 0..1, so the hundredths are divided out here.
         */
        float const dNorth = std::fabs(ny - ay) / 100.0f;
        float const dEast = std::fabs(ex - ax) / 100.0f;

        if (dNorth < 0.000001f || dEast < 0.000001f)
            return false;

        perNorth = int32(std::lround(Step / dNorth));
        perEast = int32(std::lround(Step / dEast));

        /*
         * And where the player was when all this was measured, in hundred-thousandths of
         * the map - the converter works in hundredths, the client's own map position is
         * 0..1, so a thousand of these to one of its units.
         *
         * This is the anchor the addon slides its pins from, and it has to come from here
         * rather than from the addon reading its own position when the message lands. The
         * offsets were true at this instant; the message arrives a latency later, and at
         * epic flight speed that is a couple of yards of travel. Anchoring on arrival built
         * that whole error into every pin, and since latency wobbles and the error scales
         * with speed, it showed up as pins that shook when moving quickly.
         */
        mapX = int32(std::lround(ax * 1000.0f));
        mapY = int32(std::lround(ay * 1000.0f));
        return true;
    }

    std::vector<Pin> Gather(Player* player, uint32 mask)
    {
        std::vector<Pin> pins;

        if (mask & (1u << KIND_MAILBOX))
        {
            std::list<GameObject*> found;
            MailboxCheck check(player, g_radius);
            Acore::GameObjectListSearcher<MailboxCheck> searcher(player, found, check);
            Cell::VisitObjects(player, searcher, g_radius);

            for (GameObject* object : found)
                pins.push_back(Offset(player, object, KIND_MAILBOX));
        }

        // Everything else is a creature, and one search answers for all of them: the check
        // takes the whole mask and reports which kind each creature satisfies.
        uint32 const creatureMask = mask & ~(1u << KIND_MAILBOX);

        if (creatureMask)
        {
            std::list<Creature*> found;
            PoiCreatureCheck check(player, creatureMask, g_radius);
            Acore::CreatureListSearcher<PoiCreatureCheck> searcher(player, found, check);
            Cell::VisitObjects(player, searcher, g_radius);

            for (Creature* creature : found)
                pins.push_back(Offset(player, creature, check.KindOf(creature)));
        }

        // Nearest first, then the cap per kind, so a bank in a city of mailboxes still
        // gets its pin.
        std::sort(pins.begin(), pins.end(),
            [](Pin const& a, Pin const& b) { return a.distance < b.distance; });

        std::vector<Pin> kept;
        uint32 counted[KIND_MAX] = {};

        for (Pin const& pin : pins)
        {
            if (kept.size() >= g_total)
                break;

            if (pin.kind >= KIND_MAX || counted[pin.kind] >= g_perKind)
                continue;

            ++counted[pin.kind];
            kept.push_back(pin);
        }

        return kept;
    }
}

class sanctuary_minimap_playerscript : public PlayerScript
{
public:
    sanctuary_minimap_playerscript() : PlayerScript("sanctuary_minimap_playerscript",
        {
            PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
            PLAYERHOOK_ON_LOGOUT
        }) { }

    //[[ The addon speaks in whispers to itself, exactly as the others do. ]]
    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg, Player* /*receiver*/) override
    {
        if (lang != LANG_ADDON || !player || !g_enabled)
            return true;

        std::string const marker = g_prefix + "\t";

        if (msg.rfind(marker, 0) != 0)
            return true;

        std::istringstream stream(msg.substr(marker.size()));
        std::string verb;
        stream >> verb;

        /*
         * One verb, and it carries the whole answer: the mask of kinds the player wants.
         * Sending the mask rather than toggling one kind at a time means a reconnecting
         * addon says what it wants in one message and nothing has to be remembered across
         * a session on this side.
         */
        if (verb == "WANT")
        {
            uint32 mask = 0;
            stream >> mask;

            mask &= (1u << KIND_MAX) - 1;

            if (mask)
                g_tracking[player->GetGUID()] = Tracked{ mask };
            else
                g_tracking.erase(player->GetGUID());

            /*
             * And answer, always - even for a mask of zero, which sends no pins.
             *
             * This is what tells the addon the realm HAS a server half, and it does not
             * replace the minimap's tracking menu until it hears this. It cannot: its own
             * menu has no way to set the client's single place slot, so taking the stock
             * menu away on a realm running an older worldserver would leave a player
             * unable to track a mailbox at all. One addon directory serves every realm, so
             * the addon has to be told rather than assume.
             */
            Send(player, "OK");
        }

        // Ours either way: nothing addressed to this prefix should surface as a whisper.
        return false;
    }

    void OnPlayerLogout(Player* player) override
    {
        g_tracking.erase(player->GetGUID());
    }
};

class sanctuary_minimap_worldscript : public WorldScript
{
public:
    sanctuary_minimap_worldscript() : WorldScript("sanctuary_minimap_worldscript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>("SanctuaryMinimap.Enable", true);
        g_prefix = sConfigMgr->GetOption<std::string>("SanctuaryMinimap.Addon.Prefix", "SMM");
        g_radius = sConfigMgr->GetOption<float>("SanctuaryMinimap.Radius", 150.0f);
        g_perKind = sConfigMgr->GetOption<uint32>("SanctuaryMinimap.PinsPerKind", 6);
        g_total = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("SanctuaryMinimap.PinsTotal", 40));
        g_interval = std::max<uint32>(250, sConfigMgr->GetOption<uint32>("SanctuaryMinimap.IntervalMs", 1000));

        LOG_INFO("module.sanctuaryminimap", "Sanctuary minimap pins {} ({} yards, {} per kind, every {} ms).",
            g_enabled ? "enabled" : "disabled", g_radius, g_perKind, g_interval);
    }

    void OnUpdate(uint32 diff) override
    {
        if (!g_enabled || g_tracking.empty())
            return;

        _since += diff;

        if (_since < g_interval)
            return;

        _since = 0;

        for (auto itr = g_tracking.begin(); itr != g_tracking.end(); )
        {
            Player* player = ObjectAccessor::FindPlayer(itr->first);

            // Gone without a logout hook - a crashed session, say.
            if (!player || !player->IsInWorld())
            {
                itr = g_tracking.erase(itr);
                continue;
            }

            SendPins(player, itr->second);
            ++itr;

        }
    }

private:
    /*
     * "PINS <n> <perNorth> <perEast>:<kind>,<north>,<east>;..." in as few messages as fit,
     * with the offsets in tenths of a yard.
     *
     * The count comes first and describes the whole answer, so the addon knows when it has
     * all of them and can swap its pins over in one go rather than flickering through a
     * partial set. A count of zero is a message too: it is how the addon learns that the
     * mailbox it was pointing at is behind it now.
     *
     * THE FOUR NUMBERS AFTER THE COUNT are what the addon slides the pins by between
     * answers: the zone's scale - yards per unit of the client's own map position, north
     * and east - and then where the player was, on that same map, at the instant these
     * offsets were measured. A zero scale means there is no map to slide against, which is
     * an instance.
     */
    void SendPins(Player* player, Tracked& tracked)
    {
        std::vector<Pin> const pins = Gather(player, tracked.mask);

        int32 perNorth = 0;
        int32 perEast = 0;
        int32 mapX = 0;
        int32 mapY = 0;

        ZoneScale(player, perNorth, perEast, mapX, mapY);

        std::string const header = Acore::StringFormat("PINS {} {} {} {} {}:",
            uint32(pins.size()), perNorth, perEast, mapX, mapY);

        std::string batch;
        uint32 sent = 0;

        auto flush = [&]()
        {
            // An empty batch is still worth sending once - a count of zero is how the
            // addon learns to take its pins down - but not twice.
            if (batch.empty() && sent)
                return;

            Send(player, header + batch);
            batch.clear();
        };

        for (Pin const& pin : pins)
        {
            std::string const one = Acore::StringFormat("{},{},{};",
                pin.kind, pin.north, pin.east);

            // 255 bytes is the cap on an addon message; the prefix and count take a few.
            if (batch.size() + one.size() > 200)
                flush();

            batch += one;
            ++sent;
        }

        flush();
    }

    uint32 _since = 0;
};

void AddSC_sanctuary_minimap()
{
    new sanctuary_minimap_playerscript();
    new sanctuary_minimap_worldscript();
}
