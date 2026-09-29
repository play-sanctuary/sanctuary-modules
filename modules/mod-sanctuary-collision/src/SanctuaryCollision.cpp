/*
 * mod-sanctuary-collision
 *
 * Players bump into one another.
 *
 * The 3.3.5a client never collides with another character - only with the world and with
 * gameobjects - and nothing the server sends can change that. So collision is built from the
 * two things the server does control:
 *
 *   Standing still. A player who stops is solid: every player near them is sent an invisible,
 *   person-sized box where they stand - Blizzard's own "Collision PC Size" model, display
 *   7735, which every stock client already has. Their clients collide with it properly,
 *   smoothly, with no lag. Each nearby player gets a copy of their own, turned to face them.
 *   The standing player is never sent their own. The copies are taken back the moment they
 *   move.
 *
 *   Moving. A box cannot follow a moving player on this client: a gameobject's position is
 *   sent once, when it is created; a re-created one takes a moment to become solid, which is
 *   yards at a run; and a movement update for a gameobject crashes the client. All three were
 *   tested. So two people who meet while moving are glided past each other instead: a short
 *   move the server drives, the way Charge moves a player, with no falling in it.
 *
 * See README.md for what each half can and cannot do.
 */

#include "Cell.h"
#include "CellImpl.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "MovementHandlerScript.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "ServerScript.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include "UpdateData.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    /// How a moving player is moved out of someone's way.
    enum class SwerveStyle
    {
        Glide,  // a short server-driven move: no falling
        Slide,  // a very low knockback: a brief hop
        Bounce  // a knockback back the way they came
    };

    bool g_enabled = true;
    bool g_debug = false;
    uint32 g_checkIntervalMs = 50;
    std::unordered_set<uint32> g_disabledAreas;

    bool g_blockEnabled = true;
    uint32 g_blockEntry = 990300;
    uint32 g_blockIdleMs = 0;
    float g_blockScale = 1.0f;
    uint32 g_blockSolidMs = 1000;

    bool g_bumpEnabled = true;
    SwerveStyle g_style = SwerveStyle::Glide;
    uint32 g_glideMs = 150;
    float g_bumpSpeedXY = 3.0f;
    float g_bumpSpeedZ = 1.5f;
    float g_slideHop = 0.7f;
    uint32 g_bumpCooldownMs = 600;
    float g_bumpRadiusScale = 1.0f;

    std::string const StateKey = "SanctuaryCollision";

    /// Two players this far apart in height are on different floors, not in each other's way.
    constexpr float HeightTolerance = 2.0f;

    /// A copy of a box already sent is kept up to this difference in height - and at any height
    /// while its viewer is in the air - so a jump from a step beside someone, or a few stairs,
    /// does not take it away and send it again.
    constexpr float HeightKeepTolerance = 3.0f;

    /// How far to look for anyone to bump into, or to send a box to. Two runners closing head
    /// on can be ten yards apart in their last reported positions and still be touching now;
    /// the extra two cover BoxKeepRange.
    constexpr float SearchRange = 12.0f;

    /// A standing player's box is sent to the players who come within this range of them...
    constexpr float BoxRange = 10.0f;

    /// ...and taken back only once they are further than this, so someone standing right at
    /// the edge is not sent it and relieved of it on alternate checks.
    constexpr float BoxKeepRange = 12.0f;

    /*
     * The box's collision, at scale 1, read from its model (Collision_PCSize, one closed
     * block): it spans -0.616 to 0.634 along its own X axis and +-0.764 across, and its
     * corners are 0.993 from its centre. A copy is turned so its +X face - 0.634 out - faces
     * the viewer it is sent to.
     */
    constexpr float BoxHalfDepth = 0.634f;
    constexpr float BoxBackDepth = 0.616f;
    constexpr float BoxHalfWidth = 0.764f;
    constexpr float BoxCornerRadius = 0.993f;

    /// Room left between a viewer and a copy sent to them: a copy only goes out to someone
    /// at least this much clear of its face, so it never closes around them.
    constexpr float BoxGap = 0.15f;

    /*
     * Every copy ever sent goes out under a guid no client has seen before. A gameobject guid
     * a client has had taken away never becomes solid on it again: in testing, copies re-sent
     * under a guid the client had lost - even 26 seconds earlier - were walked through every
     * time, and fresh ones never were. The low part comes from the module's own counter, not
     * the map's: a box guid carries the box's entry, which nothing else uses, so the two can
     * never meet, and the map's gameobject guids are not used up. 24 bits, wrapping after
     * 16.7 million copies, long after any client could still have the first.
     */
    std::atomic<uint32> g_boxCounter{ 0 };

    ObjectGuid::LowType NextBoxLow()
    {
        return g_boxCounter.fetch_add(1, std::memory_order_relaxed) % 0xFFFFFF + 1;
    }

    /// A viewer reported this far inside a copy - or passing this far inside it between two
    /// packets - has shown that it is not solid on their client.
    constexpr float LeakSlack = 0.05f;
    constexpr float CrossSlack = 0.2f;

    /// A copy found not to be solid is sent again, under another guid, once its viewer is this
    /// much further than touching distance away from its owner.
    constexpr float LeakClearance = 1.0f;

    /// How far a standing player may be moved without walking - a blink, a summon - before
    /// the copies of their box no longer stand where they do.
    constexpr float AnchorSlack = 0.1f;

    /// A client moving in a straight line reports about every half second. A packet older
    /// than this is late rather than old, and extrapolating it further only invents motion.
    constexpr uint64 MaxPredictMs = 750;

    /// A client on the move reports at least every half second. One quiet for longer than
    /// this is lagging or has lost its connection, and is taken to be where the server has it.
    constexpr uint64 StalePacketMs = 1500;

    /// How often a moving player's progress is compared with their speed, to notice them
    /// pressing into a wall rather than walking - in the client's own time.
    constexpr uint32 StallSampleMs = 250;

    /// A longer gap between two of a client's packets is a freeze or a clock jump, and says
    /// nothing about whether they are getting anywhere.
    constexpr uint32 StallMaxSampleMs = 2000;

    /// The client's gravity, in yards per second squared. A knockback's time off the ground
    /// is twice its vertical speed over this, which is what sizes a knockback swerve.
    constexpr float Gravity = 19.2911f;

    /// How much further apart than touching a swerve leaves two people, so the next check
    /// does not find them grazing and swerve them again.
    constexpr float SlideMargin = 0.1f;

    /// A position this close to a box's contact circle is already touching it: a client that a
    /// box has stopped reports itself right at its surface, give or take its own radius.
    constexpr float SurfaceSlack = 0.05f;

    /// The fastest shove a knockback swerve gives, and the highest hop it may use to stay under.
    constexpr float MaxShoveSpeed = 20.0f;
    constexpr float MaxSlideHop = 5.0f;

    /*
     * A glide is a move the server drives, and the client follows it blindly - through walls,
     * off ledges, into water. So its path is walked first: the ground every half yard, no step
     * up or down steeper than GlideMaxStepZ, no more than GlideMaxRiseZ over all, no water, and
     * nothing solid in the way at knee or head height. A path cut short by something is still
     * used if at least GlideMinFraction of it is clear.
     */
    constexpr float GlideSampleStep = 0.5f;
    constexpr float GlideMaxStepZ = 0.6f;
    constexpr float GlideMaxRiseZ = 1.0f;
    constexpr float GlideKneeHeight = 0.5f;
    constexpr float GlideMinFraction = 0.6f;

    /// A glide starts where their client will be when it arrives: their last packet, carried on
    /// by one round trip (no more than this).
    constexpr uint32 GlideMaxLeadMs = 300;

    /// Someone moving whose last packet is older than this is not known well enough to glide.
    constexpr uint64 GlideFreshPacketMs = 600;

    /// A speed change still waiting for their client's acknowledgement stops a glide - one sent
    /// during it would be lost - but only for this long: the core loses some on its own, and
    /// the count would otherwise never come down.
    constexpr uint64 SpeedAckGraceMs = 1500;

    /// After a glide, their client is expected to report within this long. One that has not
    /// is taken to have stopped during it.
    constexpr uint64 GlideAwaitMs = 800;

    /// A packet whose client time is this much before the glide reached them was sent before
    /// it, and is dropped if it arrives after it.
    constexpr int32 GlideStaleSlackMs = 20;

    /// How far from the glide's end their client may report the end of it and still be taken
    /// at its word.
    constexpr float GlideDoneTolerance = 3.0f;

    constexpr uint32 DirectionFlags =
        MOVEMENTFLAG_FORWARD | MOVEMENTFLAG_BACKWARD | MOVEMENTFLAG_STRAFE_LEFT | MOVEMENTFLAG_STRAFE_RIGHT;

    constexpr uint32 AirborneFlags = MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR;

    /// The movement flags that are the player's keys: what they are asking to do.
    constexpr uint32 KeyFlags = DirectionFlags | MOVEMENTFLAG_MASK_TURNING | MOVEMENTFLAG_WALKING;

    /*
     * A player's box, as sent to the players around them.
     *
     * Never added to the map. A gameobject in the map is sent by the core to everyone who can
     * see it, from the one position it has - including the player it stands for, who is
     * inside it. This one is built into a create packet by hand, separately for each viewer,
     * placed and turned for that viewer and under a guid of its own, and taken off their client
     * by hand.
     *
     * A subclass because the position a gameobject's create packet carries is
     * m_stationaryPosition, which has no public setter, and its guid is only set by the
     * protected _Create.
     *
     * There is deliberately no way to move a copy in place. A movement-only update
     * (UPDATETYPE_MOVEMENT) for a gameobject crashes the 3.3.5a client outright - found by
     * testing on 2026-09-28. A copy is only ever created, and later removed; the next one is a
     * new object on the client, under a new guid (see NextBoxLow).
     */
    class CollisionProxy : public GameObject
    {
    public:
        void Place(float x, float y, float z, float o)
        {
            Relocate(x, y, z, o);
            m_stationaryPosition.Relocate(x, y, z, o);
            SetWorldRotationAngles(o, 0.0f, 0.0f);
        }

        /// The next copy built from it goes out under this guid.
        void Rename(ObjectGuid::LowType low)
        {
            Object::_Create(low, GetEntry(), HighGuid::GameObject);
        }
    };

    /// A glide on foot. The core would give anyone who can fly - a flying mount standing on
    /// the ground - a flying path, with the flying animation.
    class GlideInit : public Movement::MoveSplineInit
    {
    public:
        explicit GlideInit(Unit* unit) : Movement::MoveSplineInit(unit)
        {
            args.flags.flying = false;
        }
    };

    /// A copy of a box on one viewer's client.
    struct SentCopy
    {
        // The guid it went out under, and the map that guid belongs to.
        ObjectGuid guid;
        uint32 mapId = 0;
        uint32 instanceId = 0;

        float x = 0.0f;
        float y = 0.0f;
        float o = 0.0f;
        uint32 viewerLogin = 0;
        uint64 sentMs = 0;

        // Where the viewer was reported at the last check, to catch them passing through it.
        float seenX = 0.0f;
        float seenY = 0.0f;

        // Shown not to be solid on their client: they were found inside it.
        bool leaky = false;
    };

    /// Why a glide could not be had.
    enum class GlideRefusal
    {
        None,
        NotFree,       // not on their own feet, or the server is moving them some other way
        TooFar,        // their last packet is too far from where they must be by now
        LeadBlocked,   // something is in the way of where their client will be
        Nowhere,       // nowhere to go
        Blocked        // something is in the way of the glide
    };

    char const* Describe(GlideRefusal refusal)
    {
        switch (refusal)
        {
            case GlideRefusal::NotFree: return "not free to glide";
            case GlideRefusal::TooFar: return "too far from their last packet";
            case GlideRefusal::LeadBlocked: return "the way to where they are is blocked";
            case GlideRefusal::Nowhere: return "nowhere to go";
            case GlideRefusal::Blocked: return "blocked";
            default: return "not moving";
        }
    }

    /// Where a player is now, and how fast they are going, as far as the server can tell.
    struct Motion
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float vx = 0.0f;
        float vy = 0.0f;
        bool moving = false;
    };

    /// A glide worked out and checked, ready to go.
    struct GlidePlan
    {
        Movement::PointsArray points;
        float vx = 0.0f;
        float vy = 0.0f;
        float length = 0.0f;
    };

    /// Which way, and how far, a swerve takes two players apart: `player` steps along minus
    /// (sideX, sideY), the other along it, and `step` is the whole of it, to share.
    struct Swerve
    {
        float sideX = 0.0f;
        float sideY = 0.0f;
        float step = 0.0f;
    };

    /*
     * What the module remembers about one player. Kept on the player itself, in CustomData,
     * so it lives and dies with them. It is touched from their map's thread, and from the
     * world thread (logout, commands, teleports ordered from there), which never runs while
     * the maps update.
     */
    struct CollisionState : public DataMap::Base
    {
        // The last movement packet - everything between packets is predicted from it.
        Position lastPos;
        uint32 lastFlags = 0;
        uint64 lastPacketMs = 0;

        // Their client's clock at lastPacketMs, if the module has heard from it at all.
        uint32 lastClientMs = 0;
        bool hasClientClock = false;

        // Their pending speed-change acknowledgements, as last seen, and when that changed.
        std::array<uint8, MAX_MOVE_TYPE> speedChanges{};
        uint64 speedChangedMs = 0;

        // Set when the player has been "moving" without getting anywhere: holding forward
        // into a wall, a door, or someone's box. Predicting them forward would be wrong.
        bool stalled = false;
        bool hasStallRef = false;
        Position stallRefPos;
        uint32 stallRefClientMs = 0;

        // Thrown by a knockback - a swerve, a bounce, or a spell's - and not landed yet.
        bool knockedBack = false;

        uint64 stillSinceMs = 0;
        uint64 nextCheckMs = 0;
        uint64 bumpReadyMs = 0;

        // This player's box on this map, and the players who have a copy of it.
        std::unique_ptr<CollisionProxy> box;
        std::unordered_map<ObjectGuid, SentCopy> sentTo;

        // Where this player stood, and how big they were, when the copies out there were sent.
        Position anchor;
        float anchorScale = 0.0f;

        // This tick's prediction, stopped at the boxes their client has, worked out once per
        // tick however many players ask for it.
        Motion blocked;
        uint64 blockedAtMs = 0;
        bool blockedValid = false;

        // The last glide: its spline, whether the server is still driving it, when it ends,
        // how fast it goes, the keys held going into it, and when their client should have
        // got it by its own clock.
        uint32 glideId = 0;
        bool glideRunning = false;
        uint64 glideEndMs = 0;
        float glideVx = 0.0f;
        float glideVy = 0.0f;
        uint32 glideKeys = 0;
        uint32 glideArriveClientMs = 0;
        bool glideHasClock = false;

        // Waiting for their client's first word since the glide, until this time.
        bool awaitingPacket = false;
        uint64 expectPacketByMs = 0;

        // Their client's report of the glide's end, kept until the server's own end of it.
        bool hasPendingDone = false;
        MovementInfo pendingDone;

        // .collision probe: the box sent to this game master, and the login it went to.
        std::unique_ptr<CollisionProxy> probe;
        uint32 probeLogin = 0;
    };

    uint64 Now()
    {
        return uint64(GameTime::GetGameTimeMS().count());
    }

    CollisionState* StateOf(Player* player)
    {
        return player->CustomData.GetDefault<CollisionState>(StateKey);
    }

    CollisionState* FindState(Player const* player)
    {
        return player->CustomData.Get<CollisionState>(StateKey);
    }

    bool IsGliding(CollisionState const& state)
    {
        return state.glideRunning || state.awaitingPacket;
    }

    bool IsInDisabledArea(Player const* player)
    {
        return !g_disabledAreas.empty()
            && (g_disabledAreas.count(player->GetZoneId()) || g_disabledAreas.count(player->GetAreaId()));
    }

    /*
     * Whether a player takes part in collision at all - as someone solid, and as someone who
     * is stopped by others.
     *
     * Each exclusion is a case where a box or a shove would be wrong, not merely odd:
     *  - stealth and invisibility, because bumping into somebody tells you where they are;
     *  - game masters, and anyone the core hides from players;
     *  - the dead, whose ghosts walk through the living;
     *  - anyone the client is not steering - feared, confused, charmed - where a knockback
     *    would fight the server's own movement of them;
     *  - passengers, which includes a player being carried, and anyone on a taxi, a boat or
     *    a zeppelin;
     *  - swimming and flying, where a person-shaped box standing on nothing makes no sense;
     *  - anyone being teleported: the server still has them where they were until their client
     *    acknowledges it, and everyone else already sees them where they are going.
     */
    bool IsEligible(Player const* player)
    {
        return player->IsInWorld()
            && !player->IsBeingTeleported()
            && player->IsAlive()
            && !player->IsGameMaster()
            && player->isGMVisible()
            && !player->HasStealthAura()
            && !player->HasInvisibilityAura()
            && player->IsClientControlled(player)
            && !player->GetVehicle()
            && !player->GetTransport()
            && !player->IsInFlight()
            && !player->IsInWater()
            && !player->IsFlying()
            && !IsInDisabledArea(player);
    }

    /// A player's body radius. The core sets it from their scale, so a character shrunk or
    /// enlarged with .modify scale is the size they appear.
    float BodyRadiusOf(Player const* player)
    {
        float const radius = player->GetFloatValue(UNIT_FIELD_BOUNDINGRADIUS);
        return radius > 0.0f ? radius : DEFAULT_WORLD_OBJECT_SIZE;
    }

    /// How close two players come before they are swerved. Bump.RadiusScale applies to this
    /// alone; a box is where it is, whatever the swerve thinks.
    float BumpRadiusOf(Player const* player)
    {
        return BodyRadiusOf(player) * g_bumpRadiusScale;
    }

    float BlockScaleFor(Player const* player)
    {
        return g_blockScale * player->GetObjectScale();
    }

    bool OnSameFloor(float z1, float z2)
    {
        return std::fabs(z1 - z2) < HeightTolerance;
    }

    /// In the air by their last packet: jumping, falling, or thrown.
    bool IsAirborne(CollisionState const& state)
    {
        return (state.lastFlags & AirborneFlags) != 0;
    }

    float SpeedFor(Player const* player, uint32 flags)
    {
        if (flags & MOVEMENTFLAG_WALKING)
            return player->GetSpeed(MOVE_WALK);

        if ((flags & MOVEMENTFLAG_BACKWARD) && !(flags & MOVEMENTFLAG_FORWARD))
            return player->GetSpeed(MOVE_RUN_BACK);

        return player->GetSpeed(MOVE_RUN);
    }

    /// Everyone near enough to walk into this player, to be sent their box, or to have sent
    /// them one.
    void FindNearby(Player* player, std::list<Player*>& nearby)
    {
        Acore::AnyPlayerInObjectRangeCheck check(player, SearchRange, false);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(player, nearby, check);
        Cell::VisitObjects(player, searcher, SearchRange);
    }

    /*
     * Where a player is at `now`, extrapolated from their last movement packet the way the
     * other clients extrapolate them: direction keys, facing, turning and speed.
     *
     * A player being glided is where the server has them, going the glide's way. A player who
     * is not moving, is pressing into something, has gone quiet, or whose last packet does not
     * match where the server has them (a teleport since) is simply where the server has them.
     */
    Motion Predict(Player const* player, CollisionState const& state, uint64 now)
    {
        Motion motion;
        motion.x = player->GetPositionX();
        motion.y = player->GetPositionY();
        motion.z = player->GetPositionZ();

        if (state.glideRunning && !player->movespline->Finalized() && player->movespline->GetId() == state.glideId)
        {
            motion.vx = state.glideVx;
            motion.vy = state.glideVy;
            motion.moving = true;
            return motion;
        }

        uint32 const flags = state.lastFlags;
        float const forward = float((flags & MOVEMENTFLAG_FORWARD) != 0)
            - float((flags & MOVEMENTFLAG_BACKWARD) != 0);
        float const left = float((flags & MOVEMENTFLAG_STRAFE_LEFT) != 0)
            - float((flags & MOVEMENTFLAG_STRAFE_RIGHT) != 0);

        if (!state.lastPacketMs || state.stalled || (forward == 0.0f && left == 0.0f))
            return motion;

        if (player->GetExactDist2d(state.lastPos) > 5.0f)
            return motion;

        uint64 const elapsed = now > state.lastPacketMs ? now - state.lastPacketMs : 0;

        if (elapsed > StalePacketMs)
            return motion;

        float const dt = float(std::min(elapsed, MaxPredictMs)) / 1000.0f;

        // Turning while moving bends the path; the heading halfway round the arc is close enough.
        float facing = state.lastPos.GetOrientation();
        float const turn = player->GetSpeed(MOVE_TURN_RATE) * dt / 2.0f;

        if (flags & MOVEMENTFLAG_LEFT)
            facing += turn;

        if (flags & MOVEMENTFLAG_RIGHT)
            facing -= turn;

        // Strafing left is a quarter turn anticlockwise from facing, backward a half turn.
        float const heading = facing + std::atan2(left, forward);
        float const speed = SpeedFor(player, flags);

        motion.vx = std::cos(heading) * speed;
        motion.vy = std::sin(heading) * speed;
        motion.x = state.lastPos.GetPositionX() + motion.vx * dt;
        motion.y = state.lastPos.GetPositionY() + motion.vy * dt;
        motion.z = state.lastPos.GetPositionZ();
        motion.moving = true;
        return motion;
    }

    /// Predicts any player, including one the module has not heard from yet.
    Motion PredictAny(Player const* player, uint64 now)
    {
        if (CollisionState const* state = FindState(player))
            return Predict(player, *state, now);

        Motion motion;
        motion.x = player->GetPositionX();
        motion.y = player->GetPositionY();
        motion.z = player->GetPositionZ();
        return motion;
    }

    /*
     * Shortens a straight stretch of movement, from (fromX, fromY) to (toX, toY), to the point
     * where it first touches a circle of `radius` round (cx, cy). True if it was shortened.
     *
     * A stretch that starts out touching the circle already - where a client that a box has
     * stopped reports itself - is stopped where it starts if it heads further in, and left
     * alone if it heads out.
     */
    bool StopAtCircle(float fromX, float fromY, float& toX, float& toY, float cx, float cy, float radius)
    {
        float const dx = toX - fromX;
        float const dy = toY - fromY;
        float const fx = fromX - cx;
        float const fy = fromY - cy;

        float const a = dx * dx + dy * dy;
        float const b = 2.0f * (fx * dx + fy * dy);

        if (a < 1e-6f)
            return false;

        if (std::sqrt(fx * fx + fy * fy) <= radius + SurfaceSlack)
        {
            if (b >= 0.0f)
                return false;

            toX = fromX;
            toY = fromY;
            return true;
        }

        float const c = fx * fx + fy * fy - radius * radius;
        float const discriminant = b * b - 4.0f * a * c;

        if (discriminant < 0.0f)
            return false;

        float const t = (-b - std::sqrt(discriminant)) / (2.0f * a);

        if (t < 0.0f || t > 1.0f)
            return false;

        toX = fromX + dx * t;
        toY = fromY + dy * t;
        return true;
    }

    /// A point in a copy's own frame: along the side it turns to its viewer, and across.
    void ToCopyFrame(SentCopy const& copy, float x, float y, float& along, float& across)
    {
        float const dx = x - copy.x;
        float const dy = y - copy.y;
        float const c = std::cos(copy.o);
        float const s = std::sin(copy.o);
        along = dx * c + dy * s;
        across = dy * c - dx * s;
    }

    /// Whether (x, y) is inside a copy's footprint, by more than `margin`.
    bool InsideCopy(SentCopy const& copy, float scale, float x, float y, float margin)
    {
        float along = 0.0f;
        float across = 0.0f;
        ToCopyFrame(copy, x, y, along, across);

        return along < BoxHalfDepth * scale - margin && along > -BoxBackDepth * scale + margin
            && std::fabs(across) < BoxHalfWidth * scale - margin;
    }

    /// Whether the straight line from (ax, ay) to (bx, by) passes inside a copy's footprint,
    /// by more than `margin`.
    bool CrossesCopy(SentCopy const& copy, float scale, float ax, float ay, float bx, float by, float margin)
    {
        float lo[2] = { -BoxBackDepth * scale + margin, -BoxHalfWidth * scale + margin };
        float hi[2] = { BoxHalfDepth * scale - margin, BoxHalfWidth * scale - margin };

        if (lo[0] >= hi[0] || lo[1] >= hi[1])
            return false;

        float from[2] = { 0.0f, 0.0f };
        float to[2] = { 0.0f, 0.0f };
        ToCopyFrame(copy, ax, ay, from[0], from[1]);
        ToCopyFrame(copy, bx, by, to[0], to[1]);

        float enter = 0.0f;
        float leave = 1.0f;

        for (int axis = 0; axis < 2; ++axis)
        {
            float const d = to[axis] - from[axis];

            if (std::fabs(d) < 1e-6f)
            {
                if (from[axis] <= lo[axis] || from[axis] >= hi[axis])
                    return false;

                continue;
            }

            float enterAxis = (lo[axis] - from[axis]) / d;
            float leaveAxis = (hi[axis] - from[axis]) / d;

            if (enterAxis > leaveAxis)
                std::swap(enterAxis, leaveAxis);

            enter = std::max(enter, enterAxis);
            leave = std::min(leave, leaveAxis);

            if (enter > leave)
                return false;
        }

        return true;
    }

    /// The copy of `owner`'s box that `viewer`'s client has now, however young, solid or not.
    /// Null if there is none.
    SentCopy const* HeldCopy(Player const* owner, Player* viewer)
    {
        CollisionState const* state = FindState(owner);

        if (!state)
            return nullptr;

        auto const copy = state->sentTo.find(viewer->GetGUID());

        if (copy == state->sentTo.end() || copy->second.viewerLogin != viewer->GetInGameTime())
            return nullptr;

        return &copy->second;
    }

    /*
     * The copy of `owner`'s box that `viewer`'s client has and can be relied on to collide
     * with: long enough ago to have become solid (Block.SolidMs), and not shown since to let
     * them through. Null if there is none.
     */
    SentCopy const* SolidCopy(Player const* owner, Player* viewer, uint64 now)
    {
        SentCopy const* copy = HeldCopy(owner, viewer);

        if (!copy || copy->leaky || now - copy->sentMs < g_blockSolidMs)
            return nullptr;

        return copy;
    }

    /*
     * Cuts a stretch of `player`'s movement short where their client would stop it: at the
     * first solid box of someone in `nearby` that their client has. The part of (vx, vy)
     * heading into a box they are stopped at goes too, if given.
     */
    void StopAtSolidCopies(Player* player, std::list<Player*> const& nearby, uint64 now, float fromX, float fromY,
        float& toX, float& toY, float* vx = nullptr, float* vy = nullptr)
    {
        for (Player* owner : nearby)
        {
            if (owner == player || !owner->InSamePhase(player))
                continue;

            SentCopy const* copy = SolidCopy(owner, player, now);

            if (!copy)
                continue;

            float const contact = BoxHalfDepth * BlockScaleFor(owner) + BodyRadiusOf(player);

            if (!StopAtCircle(fromX, fromY, toX, toY, copy->x, copy->y, contact) || !vx || !vy)
                continue;

            // Stopped against it: what is left of their motion runs along it, not into it.
            float const towardX = copy->x - fromX;
            float const towardY = copy->y - fromY;
            float const length = std::hypot(towardX, towardY);

            if (length < 0.001f)
                continue;

            float const into = (*vx * towardX + *vy * towardY) / length;

            if (into > 0.0f)
            {
                *vx -= into * towardX / length;
                *vy -= into * towardY / length;
            }
        }
    }

    /*
     * Where a player is predicted to be, stopped where their own client would stop them.
     *
     * Plain prediction assumes someone holding a direction key keeps going. But a client that
     * has walked into a box stops there, and the server does not hear so until its next
     * movement packet, up to half a second later. Predicted on regardless, someone pressing
     * into a standing player would seem to walk straight into them. So the predicted path is
     * cut short where it first meets a solid box that player's client has, exactly as the
     * client cuts it, and the part of their motion heading into that box goes with it.
     *
     * The boxes are looked for around the player themselves: `nearby` is taken only when it
     * is their own list. Worked out once per player per tick, however many others ask.
     */
    Motion PredictBlocked(Player* player, uint64 now, std::list<Player*> const* nearby = nullptr)
    {
        CollisionState* state = FindState(player);

        if (!state)
            return PredictAny(player, now);

        if (state->blockedValid && state->blockedAtMs == now)
            return state->blocked;

        Motion motion = Predict(player, *state, now);

        if (motion.moving && !state->glideRunning)
        {
            std::list<Player*> found;

            if (!nearby)
            {
                FindNearby(player, found);
                nearby = &found;
            }

            StopAtSolidCopies(player, *nearby, now, state->lastPos.GetPositionX(), state->lastPos.GetPositionY(),
                motion.x, motion.y, &motion.vx, &motion.vy);
        }

        state->blocked = motion;
        state->blockedAtMs = now;
        state->blockedValid = true;
        return motion;
    }

    /*
     * Notices a player holding a direction key without getting anywhere.
     *
     * Compared over a quarter second rather than packet to packet: turning with the mouse
     * sends packets far more often than that, and over a few milliseconds nobody has moved.
     * Timed by the client's own clock, which the hook sees before the core adjusts it, so
     * packets the network held back and delivered together are not taken for no progress.
     */
    void UpdateStall(Player const* player, CollisionState& state, MovementInfo const& movementInfo)
    {
        if (!(movementInfo.flags & DirectionFlags))
        {
            state.stalled = false;
            state.hasStallRef = false;
            return;
        }

        if (state.hasStallRef)
        {
            // Unsigned, so a clock that wraps still gives the right gap, and one that runs
            // backwards gives a huge one.
            uint32 const elapsed = movementInfo.time - state.stallRefClientMs;

            if (elapsed < StallSampleMs)
                return;

            if (elapsed <= StallMaxSampleMs)
            {
                float const expected = SpeedFor(player, movementInfo.flags) * float(elapsed) / 1000.0f;
                state.stalled = state.stallRefPos.GetExactDist2d(movementInfo.pos) < expected * 0.25f;
            }
            else
                state.stalled = false;
        }
        else
            state.stalled = false;

        state.stallRefPos = movementInfo.pos;
        state.stallRefClientMs = movementInfo.time;
        state.hasStallRef = true;
    }

    /// Takes a copy of a box off one viewer's client.
    void SendRemoval(Player* viewer, ObjectGuid box)
    {
        UpdateData data;
        data.AddOutOfRangeGUID(box);

        WorldPacket packet;
        data.BuildPacket(packet);
        viewer->SendDirectMessage(&packet);
    }

    /// Gives one viewer a copy of a box, already placed for them.
    void SendCopy(Player* viewer, CollisionProxy* box)
    {
        UpdateData data;
        box->BuildCreateUpdateBlockForPlayer(&data, viewer);

        WorldPacket packet;
        data.BuildPacket(packet);
        viewer->SendDirectMessage(&packet);
    }

    /*
     * Takes a copy of a box off a viewer's client, wherever the viewer has got to since.
     *
     * Only while they are still on the map the copy was sent on. A client that has changed map
     * has dropped every object it had already.
     */
    void RemoveCopy(ObjectGuid viewerGuid, SentCopy const& copy)
    {
        Player* viewer = ObjectAccessor::FindConnectedPlayer(viewerGuid);

        if (viewer && viewer->IsInWorld() && viewer->GetMapId() == copy.mapId
            && viewer->GetInstanceId() == copy.instanceId)
            SendRemoval(viewer, copy.guid);
    }

    /// Takes a player's box off every client that has a copy.
    void WithdrawBox(Player const* player, CollisionState& state)
    {
        if (state.sentTo.empty())
            return;

        uint64 const now = Now();

        for (auto const& copy : state.sentTo)
        {
            if (g_debug)
                LOG_INFO("module", "SanctuaryCollision [debug {}]: {}'s box withdrawn from {}.", now,
                         player->GetName(), copy.first.ToString());

            RemoveCopy(copy.first, copy.second);
        }

        state.sentTo.clear();
    }

    /// The player's box on their current map, made the first time it is wanted. Null if it
    /// cannot be.
    CollisionProxy* EnsureBox(Player* player, CollisionState& state)
    {
        if (state.box && state.box->GetEntry() == g_blockEntry && state.box->GetMapId() == player->GetMapId()
            && state.box->GetInstanceId() == player->GetInstanceId())
            return state.box.get();

        WithdrawBox(player, state);
        state.box.reset();

        if (!sObjectMgr->GetGameObjectTemplate(g_blockEntry))
            return nullptr;

        Map* map = player->GetMap();
        float const o = player->GetOrientation();
        G3D::Quat const rotation = G3D::Quat::fromAxisAngleRotation(G3D::Vector3::unitZ(), o);
        auto box = std::make_unique<CollisionProxy>();

        if (!box->Create(NextBoxLow(), g_blockEntry, map, player->GetPhaseMask(), player->GetPositionX(),
                player->GetPositionY(), player->GetPositionZ(), o, rotation, 0, GO_STATE_READY))
        {
            // Tried again at every check while they stand, so only worth a line when debugging.
            if (g_debug)
                LOG_INFO("module", "SanctuaryCollision [debug {}]: could not create the box for {} on map {}.", Now(),
                         player->GetName(), map->GetId());

            return nullptr;
        }

        state.box = std::move(box);
        return state.box.get();
    }

    /*
     * Keeps a standing player solid to everyone near them.
     *
     * A player who has stood still for Block.IdleMs (by default, straight away) sends each
     * eligible player who comes within BoxRange a copy of their box, once: where they stand,
     * turned so its narrow side faces that viewer. Never on top of anyone - someone closer than
     * that gets nothing until they have stepped clear, and in the meantime is swerved like
     * anyone else walking into a player who has no box.
     *
     * Every copy goes out under a guid of its own, never used before. A copy is left alone
     * while the owner stands: a new one would not be solid for a moment. It is sent again only
     * when it has let its viewer through - they were found inside it - once they are clear. All
     * copies are withdrawn as soon as the owner moves, is moved, changes size or stops
     * qualifying, and a viewer's is withdrawn once they are out of range or stop qualifying.
     */
    void UpdateStanding(Player* player, CollisionState& state, bool eligible, std::list<Player*> const& nearby,
        uint64 now)
    {
        bool const moving = player->isMoving() || IsGliding(state);

        if (moving)
            state.stillSinceMs = 0;
        else if (!state.stillSinceMs)
            state.stillSinceMs = now;

        if (!g_blockEnabled || !eligible || moving || now - state.stillSinceMs < g_blockIdleMs)
        {
            WithdrawBox(player, state);
            return;
        }

        CollisionProxy* box = EnsureBox(player, state);

        if (!box)
            return;

        float const scale = BlockScaleFor(player);

        if (box->GetObjectScale() != scale)
            box->SetObjectScale(scale);

        // The copies out there stand where they were sent. If this player has been moved
        // without walking since, or has changed size, they no longer fit: they all come off
        // now, and go out again, as things are, at the next check.
        if (state.sentTo.empty())
        {
            state.anchor = player->GetPosition();
            state.anchorScale = scale;
        }
        else if (player->GetExactDist(state.anchor) > AnchorSlack || state.anchorScale != scale)
        {
            if (g_debug)
                LOG_INFO("module", "SanctuaryCollision [debug {}]: {} was moved or resized while standing.", now,
                         player->GetName());

            WithdrawBox(player, state);
            return;
        }

        float const ownerX = player->GetPositionX();
        float const ownerY = player->GetPositionY();
        float const ownerZ = player->GetPositionZ();
        std::unordered_set<ObjectGuid> inRange;

        for (Player* viewer : nearby)
        {
            // A character whose player has lost their connection stays in the world a while,
            // with no client to hold a copy.
            if (viewer == player || !viewer->InSamePhase(player) || !IsEligible(viewer)
                || viewer->GetSession()->IsSocketClosed())
                continue;

            ObjectGuid const viewerGuid = viewer->GetGUID();

            // A copy sent to an earlier login of theirs - before a relog, or before a reconnect
            // to the character still standing in the world - is not on the client they have now.
            uint32 const viewerLogin = viewer->GetInGameTime();
            auto sent = state.sentTo.find(viewerGuid);

            if (sent != state.sentTo.end() && sent->second.viewerLogin != viewerLogin)
            {
                state.sentTo.erase(sent);
                sent = state.sentTo.end();
            }

            bool const known = sent != state.sentTo.end();
            float const distance = viewer->GetExactDist2d(ownerX, ownerY);
            float const height = std::fabs(ownerZ - viewer->GetPositionZ());
            bool const inReach = known
                ? distance <= BoxKeepRange
                    && (height < HeightKeepTolerance || viewer->HasUnitMovementFlag(AirborneFlags))
                : distance <= BoxRange && height < HeightTolerance;

            if (!inReach)
                continue;

            /*
             * Where the viewer is: whichever is nearer of where they last reported being and
             * where they are predicted to be by now. Someone walking up is further along than
             * their last packet says; someone who has just stopped is exactly where it says.
             */
            float viewerX = viewer->GetPositionX();
            float viewerY = viewer->GetPositionY();
            float viewerDistance = distance;
            Motion const predicted = PredictAny(viewer, now);
            float const predictedDistance = std::hypot(predicted.x - ownerX, predicted.y - ownerY);

            if (predictedDistance < viewerDistance)
            {
                viewerX = predicted.x;
                viewerY = predicted.y;
                viewerDistance = predictedDistance;
            }

            float const touching = BoxHalfDepth * scale + BodyRadiusOf(viewer) + BoxGap;

            if (known)
            {
                SentCopy& copy = sent->second;
                float const reportedX = viewer->GetPositionX();
                float const reportedY = viewer->GetPositionY();
                CollisionState const* viewerState = FindState(viewer);
                bool const beingGlided = viewerState && viewerState->glideRunning;

                // Found inside it, or gone clean through it since the last check: it is not
                // stopping them. Their client may never have made it solid. Not judged while the
                // server is gliding them - that path is the server's, not their client's.
                if (!copy.leaky && !beingGlided && (InsideCopy(copy, scale, reportedX, reportedY, LeakSlack)
                    || CrossesCopy(copy, scale, copy.seenX, copy.seenY, reportedX, reportedY, CrossSlack)))
                {
                    copy.leaky = true;

                    if (g_debug)
                        LOG_INFO("module", "SanctuaryCollision [debug {}]: {}'s box let {} through, {} ms after it "
                                           "was sent.", now, player->GetName(), viewer->GetName(), now - copy.sentMs);
                }

                copy.seenX = reportedX;
                copy.seenY = reportedY;

                if (copy.leaky && viewerDistance > touching + LeakClearance)
                {
                    if (g_debug)
                        LOG_INFO("module", "SanctuaryCollision [debug {}]: {}'s box taken back from {}, to be sent "
                                           "again under another guid.", now, player->GetName(), viewer->GetName());

                    RemoveCopy(viewerGuid, copy);
                    state.sentTo.erase(sent);
                    continue;
                }

                inRange.insert(viewerGuid);
                continue;
            }

            // Never on top of them: nothing until they are clear of where it would stand.
            if (viewerDistance < touching)
                continue;

            float const o = Position::NormalizeOrientation(std::atan2(viewerY - ownerY, viewerX - ownerX));

            box->Rename(NextBoxLow());
            box->Place(ownerX, ownerY, ownerZ, o);
            SendCopy(viewer, box);

            SentCopy& copy = state.sentTo[viewerGuid];
            copy = SentCopy();
            copy.guid = box->GetGUID();
            copy.mapId = box->GetMapId();
            copy.instanceId = box->GetInstanceId();
            copy.x = ownerX;
            copy.y = ownerY;
            copy.o = o;
            copy.viewerLogin = viewerLogin;
            copy.sentMs = now;
            copy.seenX = viewer->GetPositionX();
            copy.seenY = viewer->GetPositionY();
            inRange.insert(viewerGuid);

            if (g_debug)
                LOG_INFO("module", "SanctuaryCollision [debug {}]: {}'s box {} sent to {} at ({:.2f}, {:.2f}, {:.2f}); "
                                   "they are {:.2f} yd apart.", now, player->GetName(), copy.guid.ToString(),
                         viewer->GetName(), ownerX, ownerY, ownerZ, viewerDistance);
        }

        // Anyone holding a copy who is out of range now, or no longer collides, loses it.
        for (auto itr = state.sentTo.begin(); itr != state.sentTo.end();)
        {
            if (inRange.count(itr->first))
            {
                ++itr;
                continue;
            }

            if (g_debug)
                LOG_INFO("module", "SanctuaryCollision [debug {}]: {}'s box withdrawn from {}.", now,
                         player->GetName(), itr->first.ToString());

            RemoveCopy(itr->first, itr->second);
            itr = state.sentTo.erase(itr);
        }
    }

    /// Knocks a player along (dirX, dirY) at the given speeds. KnockbackFrom pushes away from
    /// a point, so the point is put a yard behind them on that line.
    void Shove(Player* player, float dirX, float dirY, float speedXY, float speedZ)
    {
        player->KnockbackFrom(player->GetPositionX() - dirX, player->GetPositionY() - dirY,
            speedXY, speedZ);
    }

    /*
     * Sends one moving player on past another with a very low knockback, swerving `step`
     * yards along (sideX, sideY), all of their own motion kept.
     *
     * The step has to happen while they are off the ground, and a low knockback is off the
     * ground only briefly - under a tenth of a second at the default Slide.Hop. That brief
     * fall is what a glide avoids; this is the swerve for when a glide cannot be had.
     */
    void SlidePast(Player* player, Motion const& motion, float sideX, float sideY, float step)
    {
        float hop = g_slideHop;
        float airborne = 2.0f * hop / Gravity;

        // The shove is capped at MaxShoveSpeed. Rather than let the cap cut the step short -
        // and their own pace with it - a step too quick to fit under it gets a longer hop.
        float const along = motion.vx * sideX + motion.vy * sideY;
        float const own = motion.vx * motion.vx + motion.vy * motion.vy;
        float const room = -along + std::sqrt(std::max(0.0f, along * along + MaxShoveSpeed * MaxShoveSpeed - own));

        if (room > 0.01f && step / airborne > room)
        {
            hop = std::min(Gravity * step / (2.0f * room), MaxSlideHop);
            airborne = 2.0f * hop / Gravity;
        }

        float const vx = motion.vx + sideX * step / airborne;
        float const vy = motion.vy + sideY * step / airborne;
        float const speed = std::sqrt(vx * vx + vy * vy);

        if (speed < 0.01f)
            return;

        Shove(player, vx / speed, vy / speed, std::min(speed, MaxShoveSpeed), hop);
    }

    /*
     * How two players who have met are to step past each other.
     *
     * Only as far as needed: enough to take the other person off this one's path, measured
     * along how the two are moving relative to each other. A glancing touch is a nudge;
     * walking straight into someone is a full step aside.
     */
    Swerve SwerveFor(Motion const& mine, Motion const& them, float dx, float dy, float reach)
    {
        // How this player moves as the other sees it. Never zero here: the caller only gets
        // this far for two players who are closing.
        float const relX = mine.vx - them.vx;
        float const relY = mine.vy - them.vy;
        float const rel = std::sqrt(relX * relX + relY * relY);
        float const pathX = relX / rel;
        float const pathY = relY / rel;

        // Where the other stands to the side of that path, and how far.
        float const ahead = dx * pathX + dy * pathY;
        float sideX = dx - ahead * pathX;
        float sideY = dy - ahead * pathY;
        float const miss = std::sqrt(sideX * sideX + sideY * sideY);

        Swerve swerve;

        if (miss < 0.05f)
        {
            // Dead centre. Treat the other as standing just left of the path, so this player
            // steps right - and, the path being reversed for them, so do they: two people
            // walking straight at each other both keep right.
            swerve.sideX = -pathY;
            swerve.sideY = pathX;
        }
        else
        {
            swerve.sideX = sideX / miss;
            swerve.sideY = sideY / miss;
        }

        swerve.step = std::max(0.0f, reach + SlideMargin - miss);
        return swerve;
    }

    /*
     * How much room there is from (x, y, z) along (dirX, dirY), up to `reach`: the nearest
     * thing solid at knee or head height, in the map's own geometry or its gameobjects.
     */
    float RoomAlong(Map* map, uint32 phase, float height, float x, float y, float z, float dirX, float dirY,
        float reach)
    {
        auto const& statics = map->GetMapCollisionData().GetStaticTree();
        auto const& dynamics = map->GetMapCollisionData().GetDynamicTree();
        float room = reach;

        for (float const above : { GlideKneeHeight, height - 0.2f })
        {
            float hitX = 0.0f;
            float hitY = 0.0f;
            float hitZ = 0.0f;
            float const toX = x + dirX * reach;
            float const toY = y + dirY * reach;

            if (statics.GetObjectHitPos(x, y, z + above, toX, toY, z + above, hitX, hitY, hitZ, 0.0f))
                room = std::min(room, std::hypot(hitX - x, hitY - y));

            if (dynamics.GetObjectHitPos(phase, x, y, z + above, toX, toY, z + above, hitX, hitY, hitZ, 0.0f))
                room = std::min(room, std::hypot(hitX - x, hitY - y));
        }

        return room;
    }

    /*
     * Walks a straight line over the ground the way a client would, to see how much of it a
     * server-driven move can safely follow - a client being glided follows blindly, through
     * walls, off ledges and into water. Sampled every half yard: the ground there, no step up
     * or down too steep, no water, and nothing solid in between at knee or head height. The
     * point it ends at must leave their body room - ahead and to both sides - at least as much
     * as they had where it started, so it never leaves them closer to a wall than their own
     * client would. The points are appended to `points`.
     *
     * Returns how far along it is clear. The core knows nothing of other players or of the
     * boxes; the caller sees to those.
     */
    float WalkLine(Player* player, float fromX, float fromY, float fromZ, float toX, float toY,
        Movement::PointsArray& points)
    {
        float const length = std::hypot(toX - fromX, toY - fromY);

        if (length < 0.01f)
            return 0.0f;

        Map* map = player->GetMap();
        uint32 const phase = player->GetPhaseMask();
        float const height = player->GetCollisionHeight();
        float const radius = BodyRadiusOf(player);
        float const dirX = (toX - fromX) / length;
        float const dirY = (toY - fromY) / length;
        uint32 const samples = std::max<uint32>(1, uint32(std::ceil(length / GlideSampleStep)));

        auto const& statics = map->GetMapCollisionData().GetStaticTree();
        auto const& dynamics = map->GetMapCollisionData().GetDynamicTree();

        std::size_t const base = points.size();
        std::vector<float> reached;
        float prevX = fromX;
        float prevY = fromY;
        float prevZ = fromZ;

        for (uint32 i = 1; i <= samples; ++i)
        {
            float const t = length * float(i) / float(samples);
            float const x = fromX + dirX * t;
            float const y = fromY + dirY * t;

            if (!Acore::IsValidMapCoord(x, y, prevZ))
                break;

            float const z = player->GetMapHeight(x, y, prevZ);

            if (z <= INVALID_HEIGHT || std::fabs(z - prevZ) > GlideMaxStepZ || std::fabs(z - fromZ) > GlideMaxRiseZ)
                break;

            if (map->GetLiquidData(phase, x, y, z, height, {}).Status & MAP_LIQUID_STATUS_IN_CONTACT)
                break;

            bool blocked = false;

            for (float const above : { GlideKneeHeight, height - 0.2f })
            {
                float hitX = 0.0f;
                float hitY = 0.0f;
                float hitZ = 0.0f;

                if (statics.GetObjectHitPos(prevX, prevY, prevZ + above, x, y, z + above, hitX, hitY, hitZ, 0.0f)
                    || dynamics.GetObjectHitPos(phase, prevX, prevY, prevZ + above, x, y, z + above, hitX, hitY, hitZ,
                        0.0f))
                {
                    blocked = true;
                    break;
                }
            }

            if (blocked)
                break;

            points.emplace_back(x, y, z);
            reached.push_back(t);
            prevX = x;
            prevY = y;
            prevZ = z;
        }

        // Room for their body at the end: ahead, and to either side, no less than they had at
        // the start (someone already brushing a wall may glide along it, never into it).
        float const sides[3][2] = { { dirX, dirY }, { -dirY, dirX }, { dirY, -dirX } };
        float need[3] = { 0.0f, 0.0f, 0.0f };

        for (int side = 0; side < 3; ++side)
            need[side] = std::min(radius, RoomAlong(map, phase, height, fromX, fromY, fromZ, sides[side][0],
                sides[side][1], radius)) - SurfaceSlack;

        while (points.size() > base)
        {
            G3D::Vector3 const& end = points.back();
            bool roomy = true;

            for (int side = 0; side < 3 && roomy; ++side)
                roomy = RoomAlong(map, phase, height, end.x, end.y, end.z, sides[side][0], sides[side][1], radius)
                    >= need[side];

            if (roomy)
                break;

            points.pop_back();
            reached.pop_back();
        }

        return reached.empty() ? 0.0f : reached.back();
    }

    /*
     * Whether a player may be glided at all.
     *
     * Only someone moving on their own feet, under their own control, that the server is not
     * already moving some other way, and whose movement is known well enough. A glide is a
     * move the server drives; it must never fight a charge, a fear, a root or a teleport, nor
     * leave the air, the water or a transport. Nor may it start while a speed change is waiting
     * for their client's acknowledgement: the core drops acknowledgements during a glide.
     */
    bool CanGlide(Player* player, CollisionState const& state, uint64 now)
    {
        Unit const* mover = player->m_mover;

        if (mover != player || !player->IsAlive() || !player->movespline->Finalized() || player->IsBeingTeleported()
            || player->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE
            || player->HasUnitState(UNIT_STATE_NOT_MOVE | UNIT_STATE_LOST_CONTROL | UNIT_STATE_CASTING
                | UNIT_STATE_IN_FLIGHT)
            || player->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE)
            || player->HasUnitMovementFlag(AirborneFlags | MOVEMENTFLAG_ONTRANSPORT | MOVEMENTFLAG_SWIMMING
                | MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY | MOVEMENTFLAG_HOVER))
            return false;

        if (IsGliding(state) || IsAirborne(state) || state.stalled)
            return false;

        if ((state.lastFlags & DirectionFlags)
            && (!state.lastPacketMs || now < state.lastPacketMs || now - state.lastPacketMs > GlideFreshPacketMs))
            return false;

        // A count the core has lost track of - it drops some acknowledgements on its own - is
        // not waited on for ever.
        if (now - state.speedChangedMs < SpeedAckGraceMs)
        {
            for (uint8 type = 0; type < MAX_MOVE_TYPE; ++type)
                if (player->m_forced_speed_changes[type])
                    return false;
        }

        return true;
    }

    /*
     * Works out a glide for one player: their own motion carried on for Glide.Ms, plus `step`
     * yards along (sideX, sideY). False, with the reason in `refusal`, if it cannot be had
     * safely.
     *
     * It starts where their client will be when it arrives - a spline starts from wherever the
     * server has them, which for someone running is their last packet, up to half a second
     * behind - and that stretch is checked like the glide itself. `partner` is the one they
     * are gliding past: the step is already sized for them.
     */
    bool PlanGlide(Player* player, CollisionState const& state, Motion const& motion, float sideX, float sideY,
        float step, Player const* partner, std::list<Player*> const* nearby, uint64 now, GlidePlan& plan,
        GlideRefusal& refusal)
    {
        if (!CanGlide(player, state, now))
        {
            refusal = GlideRefusal::NotFree;
            return false;
        }

        std::list<Player*> found;

        if (!nearby)
        {
            FindNearby(player, found);
            nearby = &found;
        }

        uint32 const lead = std::min<uint32>(player->GetSession()->GetLatency(), GlideMaxLeadMs);
        Motion const at = Predict(player, state, now + lead);
        float const serverX = player->GetPositionX();
        float const serverY = player->GetPositionY();
        float const serverZ = player->GetPositionZ();

        // Where their client will be - but no further than a solid box it has would let it go.
        float atX = at.x;
        float atY = at.y;
        StopAtSolidCopies(player, *nearby, now, serverX, serverY, atX, atY);

        // Only a sanity bound: as far as their own speed could carry them from their last
        // packet, a round trip on.
        float const shift = std::hypot(atX - serverX, atY - serverY);
        float const maxShift = SpeedFor(player, state.lastFlags)
            * float(std::min<uint64>(GlideFreshPacketMs + GlideMaxLeadMs, MaxPredictMs)) / 1000.0f + 0.5f;

        if (shift > maxShift)
        {
            refusal = GlideRefusal::TooFar;
            return false;
        }

        float startX = serverX;
        float startY = serverY;
        float startZ = serverZ;

        if (shift >= 0.05f)
        {
            Movement::PointsArray leg;

            if (WalkLine(player, serverX, serverY, serverZ, atX, atY, leg) < shift - 0.01f || leg.empty())
            {
                refusal = GlideRefusal::LeadBlocked;
                return false;
            }

            startX = leg.back().x;
            startY = leg.back().y;
            startZ = leg.back().z;
        }

        Movement::PointsArray points;
        points.emplace_back(startX, startY, startZ);

        // Where the glide would take them, cut short at anyone else, and at any box their
        // client has - solid yet or not: the client follows a glide straight through both.
        float const seconds = float(g_glideMs) / 1000.0f;
        float endX = startX + motion.vx * seconds + sideX * step;
        float endY = startY + motion.vy * seconds + sideY * step;
        float const wanted = std::hypot(endX - startX, endY - startY);

        if (wanted < 0.05f)
        {
            refusal = GlideRefusal::Nowhere;
            return false;
        }

        for (Player* other : *nearby)
        {
            if (other == player || other == partner || !other->InSamePhase(player) || !IsEligible(other))
                continue;

            Motion const them = PredictAny(other, now);

            if (OnSameFloor(them.z, startZ))
                StopAtCircle(startX, startY, endX, endY, them.x, them.y, BodyRadiusOf(other) + BodyRadiusOf(player));

            if (SentCopy const* copy = HeldCopy(other, player))
                StopAtCircle(startX, startY, endX, endY, copy->x, copy->y,
                    BoxCornerRadius * BlockScaleFor(other) + BodyRadiusOf(player));
        }

        float const clear = WalkLine(player, startX, startY, startZ, endX, endY, points);

        if (points.size() < 2 || clear < wanted * GlideMinFraction)
        {
            refusal = GlideRefusal::Blocked;
            return false;
        }

        plan.length = 0.0f;

        for (std::size_t i = 1; i < points.size(); ++i)
            plan.length += (points[i] - points[i - 1]).length();

        plan.vx = (points.back().x - startX) / seconds;
        plan.vy = (points.back().y - startY) / seconds;
        plan.points = std::move(points);
        refusal = GlideRefusal::None;
        return true;
    }

    /*
     * Sets a planned glide going: the server is put where it starts, and the player - and
     * everyone who can see them - is sent a short spline to follow, with their facing kept.
     *
     * The server hears nothing from their client while it drives them, so what the module
     * knows of them is set to where the glide leaves them, going on as they were.
     */
    bool LaunchGlide(Player* player, CollisionState& state, GlidePlan const& plan, uint64 now)
    {
        G3D::Vector3 const& start = plan.points.front();
        G3D::Vector3 const& end = plan.points.back();

        if (player->GetExactDist(start.x, start.y, start.z) > 0.01f)
            player->UpdatePosition(start.x, start.y, start.z, player->GetOrientation());

        GlideInit init(player);
        init.MovebyPath(plan.points);
        init.SetOrientationFixed(true);
        init.SetVelocity(std::max(plan.length * 1000.0f / float(g_glideMs), 0.1f));

        int32 const duration = init.Launch();

        if (duration <= 0)
            return false;

        player->SetFallInformation(GameTime::GetGameTime().count(), end.z);
        sScriptMgr->AnticheatSetUnderACKmount(player);

        // Their client clock when the glide reached them: their last packet's time, carried on
        // to now, plus the trip there and back. Only if their client has been heard from.
        uint32 const lead = std::min<uint32>(player->GetSession()->GetLatency(), GlideMaxLeadMs);
        uint64 const sinceLast = now > state.lastPacketMs ? now - state.lastPacketMs : 0;

        state.glideId = player->movespline->GetId();
        state.glideRunning = true;
        state.glideEndMs = now + uint32(duration);
        state.glideVx = plan.vx;
        state.glideVy = plan.vy;
        state.glideKeys = state.lastFlags & KeyFlags;
        state.glideHasClock = state.hasClientClock && state.lastPacketMs != 0;
        state.glideArriveClientMs = state.lastClientMs + uint32(sinceLast) + lead;
        state.awaitingPacket = true;
        state.expectPacketByMs = state.glideEndMs + GlideAwaitMs;
        state.hasPendingDone = false;

        // Prediction goes on from the glide's end. Their client's clock is moved on with it, so
        // the two still belong together.
        if (state.hasClientClock && state.glideEndMs > state.lastPacketMs)
            state.lastClientMs += uint32(state.glideEndMs - state.lastPacketMs);

        state.lastPos.Relocate(end.x, end.y, end.z);
        state.lastPacketMs = state.glideEndMs;
        state.stalled = false;
        state.hasStallRef = false;
        state.blockedValid = false;

        if (g_debug)
            LOG_INFO("module", "SanctuaryCollision [debug {}]: {} glides {:.2f} yd in {} ms.", now, player->GetName(),
                     plan.length, duration);

        return true;
    }

    /// Lets go of direction and turning keys the module has on the server for a player, and
    /// tells everyone watching.
    void ReleaseKeys(Player* player)
    {
        if (!player->movespline->Finalized()
            || !player->HasUnitMovementFlag(DirectionFlags | MOVEMENTFLAG_MASK_TURNING))
            return;

        player->RemoveUnitMovementFlag(DirectionFlags | MOVEMENTFLAG_MASK_TURNING);
        player->SendMovementFlagUpdate();
    }

    /*
     * Keeps track of a glide once it is going, every update.
     *
     * When the server's end of it is over, the core has let go of their direction keys - so
     * everyone watching would see them stop at the end, then jump on at their next packet. If
     * their client has told us how it ended, that is passed on; otherwise they are taken to be
     * going on as they were - if the glide ran its course and they are still free to move. And
     * if their client has not been heard from within GlideAwaitMs, they stopped during it.
     */
    void UpdateGlide(Player* player, CollisionState& state, uint64 now)
    {
        bool const finalized = player->movespline->Finalized();

        if (state.glideRunning && !finalized && player->movespline->GetId() == state.glideId)
            return;

        bool const justEnded = state.glideRunning;
        state.glideRunning = false;

        if (!state.awaitingPacket)
            return;

        Unit const* mover = player->m_mover;
        bool const endedOwn = finalized && player->movespline->GetId() == state.glideId;
        bool const free = player->IsAlive() && mover == player
            && !player->HasUnitState(UNIT_STATE_NOT_MOVE | UNIT_STATE_LOST_CONTROL)
            && !player->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE);
        bool const canTell = endedOwn && free;

        if (state.hasPendingDone)
        {
            state.hasPendingDone = false;
            MovementInfo const& done = state.pendingDone;

            if (canTell && player->GetExactDist(done.pos) <= GlideDoneTolerance)
            {
                player->UpdatePosition(done.pos.GetPositionX(), done.pos.GetPositionY(), done.pos.GetPositionZ(),
                    done.pos.GetOrientation());
                player->SetUnitMovementFlags((player->GetUnitMovementFlags() & ~KeyFlags) | (done.flags & KeyFlags));
                player->SendMovementFlagUpdate();

                state.lastPos = done.pos;
                state.lastFlags = done.flags;
                state.lastPacketMs = now;
                state.lastClientMs = done.time;
                state.hasClientClock = true;
                state.awaitingPacket = false;
                state.blockedValid = false;

                if (g_debug)
                    LOG_INFO("module", "SanctuaryCollision [debug {}]: {}'s client ended the glide {}.", now,
                             player->GetName(), (done.flags & DirectionFlags) ? "still moving" : "standing");

                return;
            }
        }

        // Cut short by a stop, a stun, a root or a death: they are not going on.
        if (justEnded && !free)
        {
            state.awaitingPacket = false;
            state.lastFlags &= ~(DirectionFlags | MOVEMENTFLAG_MASK_TURNING);
            state.blockedValid = false;
            ReleaseKeys(player);
            return;
        }

        if (justEnded && canTell && (state.glideKeys & DirectionFlags))
        {
            player->AddUnitMovementFlag(state.glideKeys & DirectionFlags);
            player->SendMovementFlagUpdate();
        }

        if (now <= state.expectPacketByMs)
            return;

        // Nothing from their client since: they let go of the keys during it.
        state.awaitingPacket = false;
        state.lastFlags &= ~(DirectionFlags | MOVEMENTFLAG_MASK_TURNING);
        state.blockedValid = false;
        ReleaseKeys(player);

        if (g_debug)
            LOG_INFO("module", "SanctuaryCollision [debug {}]: nothing from {}'s client after the glide; taken to "
                               "have stopped.", now, player->GetName());
    }

    /// Forgets any glide: for a teleport, or anything else that ends one outright.
    void ForgetGlide(CollisionState& state)
    {
        state.glideRunning = false;
        state.awaitingPacket = false;
        state.hasPendingDone = false;
    }

    /*
     * Glides two players who have met past each other.
     *
     * Each one who may be moved gets their share of the step; one who cannot be glided - a
     * wall at their side, a ledge, water - leaves the whole step to the other. Where the other
     * cannot take it all, or neither can be glided, the knockback swerve makes up the rest: a
     * brief hop, but the client's own collision keeps it out of walls, and nobody walks
     * through anybody. It is not used on someone a wall is in the way of, whom a hop would not
     * get past either.
     */
    void GlideApart(Player* player, CollisionState& state, Player* other, CollisionState& theirs,
        Motion const& mine, Motion const& them, Swerve const& swerve, bool moveMe, bool moveThem,
        std::list<Player*> const& nearby, uint64 now, bool& movedMe, bool& movedThem)
    {
        float const share = moveMe && moveThem ? swerve.step / 2.0f : swerve.step;
        GlideRefusal whyMe = GlideRefusal::None;
        GlideRefusal whyThem = GlideRefusal::None;
        GlidePlan planMe;
        GlidePlan planThem;

        bool const okMe = moveMe
            && PlanGlide(player, state, mine, -swerve.sideX, -swerve.sideY, share, other, &nearby, now, planMe, whyMe);
        bool const okThem = moveThem
            && PlanGlide(other, theirs, them, swerve.sideX, swerve.sideY, share, player, nullptr, now, planThem,
                whyThem);
        bool whole = false;

        if (moveMe && moveThem && okMe != okThem)
        {
            GlidePlan full;
            GlideRefusal why = GlideRefusal::None;

            if (okMe && PlanGlide(player, state, mine, -swerve.sideX, -swerve.sideY, swerve.step, other, &nearby, now,
                    full, why))
            {
                planMe = std::move(full);
                whole = true;
            }
            else if (okThem && PlanGlide(other, theirs, them, swerve.sideX, swerve.sideY, swerve.step, player,
                    nullptr, now, full, why))
            {
                planThem = std::move(full);
                whole = true;
            }
        }

        movedMe = okMe && LaunchGlide(player, state, planMe, now);
        movedThem = okThem && LaunchGlide(other, theirs, planThem, now);

        auto const walled = [](GlideRefusal why)
        {
            return why == GlideRefusal::Blocked || why == GlideRefusal::LeadBlocked;
        };

        if (movedMe || movedThem)
        {
            // One glided only their share and the other could not glide: the other's share by
            // knockback.
            if (moveMe && moveThem && !whole)
            {
                if (movedThem && !movedMe && !walled(whyMe))
                {
                    SlidePast(player, mine, -swerve.sideX, -swerve.sideY, share);
                    movedMe = true;
                }
                else if (movedMe && !movedThem && !walled(whyThem))
                {
                    SlidePast(other, them, swerve.sideX, swerve.sideY, share);
                    movedThem = true;
                }
            }

            return;
        }

        if (g_debug)
            LOG_INFO("module", "SanctuaryCollision [debug {}]: no glide for {} ({}) or {} ({}); knockback instead.",
                     now, player->GetName(), Describe(whyMe), other->GetName(), Describe(whyThem));

        if (moveMe)
            SlidePast(player, mine, -swerve.sideX, -swerve.sideY, share);

        if (moveThem)
            SlidePast(other, them, swerve.sideX, swerve.sideY, share);

        movedMe = moveMe;
        movedThem = moveThem;
    }

    /*
     * Looks for anyone this player is walking into, or who is walking into them.
     *
     * Only the players in motion are moved. Someone standing still is never moved: a runner
     * must not be able to knock idle people about. And someone standing whose box the other's
     * client has, solid by now, is left to that box - it stops them properly. The swerve is
     * for what the boxes cannot do: two people who are both moving, and a runner reaching
     * someone who has only just stopped.
     *
     * Nobody is moved while in the air - a knockback would cut their jump short, and a glide
     * would flatten it; the other person takes the whole swerve. Someone already thrown or
     * being glided is left to finish.
     */
    void CheckBumps(Player* player, CollisionState& state, std::list<Player*> const& nearby, uint64 now)
    {
        if (state.knockedBack || IsGliding(state))
            return;

        Motion const mine = PredictBlocked(player, now, &nearby);
        float const myRadius = BumpRadiusOf(player);
        bool myReady = now >= state.bumpReadyMs && !IsAirborne(state);

        for (Player* other : nearby)
        {
            // Each pair is looked at once, from the lower guid's side.
            if (other == player || !(player->GetGUID() < other->GetGUID()))
                continue;

            if (!other->InSamePhase(player) || !IsEligible(other))
                continue;

            CollisionState* theirs = FindState(other);

            if (!theirs || theirs->knockedBack || IsGliding(*theirs))
                continue;

            Motion const them = PredictBlocked(other, now);

            // Two people standing still and overlapping got there by teleport or login, not
            // by walking into each other. Leave them be.
            if (!mine.moving && !them.moving)
                continue;

            if ((!them.moving && SolidCopy(other, player, now)) || (!mine.moving && SolidCopy(player, other, now)))
                continue;

            if (!OnSameFloor(mine.z, them.z))
                continue;

            float const dx = them.x - mine.x;
            float const dy = them.y - mine.y;
            float const distance = std::sqrt(dx * dx + dy * dy);
            float const reach = myRadius + BumpRadiusOf(other);

            if (distance >= reach)
                continue;

            // Only while they are closing. Two people already stepping apart are left to do so.
            float const closing = (mine.vx - them.vx) * dx + (mine.vy - them.vy) * dy;

            if (closing <= 0.0f || distance < 0.001f)
                continue;

            // Who may be moved: whoever is moving, on their feet, and not moved too recently.
            bool const moveMe = mine.moving && myReady;
            bool const moveThem = them.moving && now >= theirs->bumpReadyMs && !IsAirborne(*theirs);

            if (!moveMe && !moveThem)
                continue;

            bool movedMe = false;
            bool movedThem = false;

            if (g_style == SwerveStyle::Bounce)
            {
                float const awayX = -dx / distance;
                float const awayY = -dy / distance;

                if (moveMe)
                    Shove(player, awayX, awayY, g_bumpSpeedXY, g_bumpSpeedZ);

                if (moveThem)
                    Shove(other, -awayX, -awayY, g_bumpSpeedXY, g_bumpSpeedZ);

                movedMe = moveMe;
                movedThem = moveThem;
            }
            else
            {
                // Past someone standing whose box the other's client has - not solid yet, or
                // this would not be happening - far enough to clear the box's side, not just
                // their body: it becomes solid around anyone left inside it.
                float clearance = reach;

                if (!them.moving && HeldCopy(other, player))
                    clearance = std::max(clearance, BoxHalfWidth * BlockScaleFor(other) + BodyRadiusOf(player));

                if (!mine.moving && HeldCopy(player, other))
                    clearance = std::max(clearance, BoxHalfWidth * BlockScaleFor(player) + BodyRadiusOf(other));

                Swerve const swerve = SwerveFor(mine, them, dx, dy, clearance);

                if (g_style == SwerveStyle::Glide)
                    GlideApart(player, state, other, *theirs, mine, them, swerve, moveMe, moveThem, nearby, now,
                        movedMe, movedThem);
                else
                {
                    float const share = moveMe && moveThem ? swerve.step / 2.0f : swerve.step;

                    if (moveMe)
                        SlidePast(player, mine, -swerve.sideX, -swerve.sideY, share);

                    if (moveThem)
                        SlidePast(other, them, swerve.sideX, swerve.sideY, share);

                    movedMe = moveMe;
                    movedThem = moveThem;
                }
            }

            if (movedMe)
            {
                state.bumpReadyMs = now + g_bumpCooldownMs;
                myReady = false;
            }

            if (movedThem)
                theirs->bumpReadyMs = now + g_bumpCooldownMs;

            if (g_debug)
                LOG_INFO("module", "SanctuaryCollision [debug {}]: bump: {}{} and {}{} at {:.2f} yd.", now,
                         player->GetName(), movedMe ? " (moved)" : "", other->GetName(), movedThem ? " (moved)" : "",
                         distance);

            if (IsGliding(state))
                return;
        }
    }

    /// Whether an opcode is one of the client's own movement reports.
    bool IsMovementReport(uint16 opcode)
    {
        switch (opcode)
        {
            case MSG_MOVE_START_FORWARD:
            case MSG_MOVE_START_BACKWARD:
            case MSG_MOVE_STOP:
            case MSG_MOVE_START_STRAFE_LEFT:
            case MSG_MOVE_START_STRAFE_RIGHT:
            case MSG_MOVE_STOP_STRAFE:
            case MSG_MOVE_JUMP:
            case MSG_MOVE_START_TURN_LEFT:
            case MSG_MOVE_START_TURN_RIGHT:
            case MSG_MOVE_STOP_TURN:
            case MSG_MOVE_START_PITCH_UP:
            case MSG_MOVE_START_PITCH_DOWN:
            case MSG_MOVE_STOP_PITCH:
            case MSG_MOVE_SET_RUN_MODE:
            case MSG_MOVE_SET_WALK_MODE:
            case MSG_MOVE_FALL_LAND:
            case MSG_MOVE_START_SWIM:
            case MSG_MOVE_STOP_SWIM:
            case MSG_MOVE_SET_FACING:
            case MSG_MOVE_SET_PITCH:
            case MSG_MOVE_HEARTBEAT:
            case MSG_MOVE_START_ASCEND:
            case MSG_MOVE_STOP_ASCEND:
            case MSG_MOVE_START_DESCEND:
                return true;
            default:
                return false;
        }
    }

    /// Whether an opcode is the client acknowledging something the server did to its movement.
    bool IsMovementAck(uint16 opcode)
    {
        switch (opcode)
        {
            case CMSG_FORCE_RUN_SPEED_CHANGE_ACK:
            case CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK:
            case CMSG_FORCE_SWIM_SPEED_CHANGE_ACK:
            case CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK:
            case CMSG_FORCE_WALK_SPEED_CHANGE_ACK:
            case CMSG_FORCE_TURN_RATE_CHANGE_ACK:
            case CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK:
            case CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK:
            case CMSG_FORCE_PITCH_RATE_CHANGE_ACK:
            case CMSG_FORCE_MOVE_ROOT_ACK:
            case CMSG_FORCE_MOVE_UNROOT_ACK:
            case CMSG_MOVE_KNOCK_BACK_ACK:
            case CMSG_MOVE_HOVER_ACK:
            case CMSG_MOVE_FEATHER_FALL_ACK:
            case CMSG_MOVE_WATER_WALK_ACK:
            case CMSG_MOVE_SET_CAN_FLY_ACK:
            case CMSG_MOVE_GRAVITY_DISABLE_ACK:
            case CMSG_MOVE_GRAVITY_ENABLE_ACK:
            case CMSG_MOVE_SET_COLLISION_HGT_ACK:
                return true;
            default:
                return false;
        }
    }
}

class sanctuary_collision_playerscript : public PlayerScript
{
public:
    sanctuary_collision_playerscript() : PlayerScript("sanctuary_collision_playerscript",
        { PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_BEFORE_TELEPORT, PLAYERHOOK_ON_MAP_CHANGED }) { }

    void OnPlayerUpdate(Player* player, uint32 /*diff*/) override
    {
        if (!player->IsInWorld())
            return;

        if (!g_enabled)
        {
            // Switched off by a config reload: whatever is still out comes back, once.
            CollisionState* state = FindState(player);

            if (state && !state->sentTo.empty())
                WithdrawBox(player, *state);

            return;
        }

        uint64 const now = Now();
        CollisionState* state = StateOf(player);

        // When their pending speed changes last changed (see CanGlide).
        for (uint8 type = 0; type < MAX_MOVE_TYPE; ++type)
        {
            if (state->speedChanges[type] != player->m_forced_speed_changes[type])
            {
                state->speedChanges[type] = player->m_forced_speed_changes[type];
                state->speedChangedMs = now;
            }
        }

        // Every update, not every check: the end of a glide is passed on the moment it comes.
        if (IsGliding(*state))
            UpdateGlide(player, *state, now);

        if (now < state->nextCheckMs)
            return;

        state->nextCheckMs = now + g_checkIntervalMs;

        std::list<Player*> nearby;
        FindNearby(player, nearby);

        bool const eligible = IsEligible(player);

        UpdateStanding(player, *state, eligible, nearby, now);

        if (eligible && g_bumpEnabled)
            CheckBumps(player, *state, nearby, now);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (CollisionState* state = FindState(player))
            WithdrawBox(player, *state);
    }

    /*
     * A teleport - to another map, or across this one: a blink, a summon, .tele - takes the
     * box off everyone's client before the player goes. To another map, the box itself goes
     * too: it was made on this one. Their movement is predicted afresh once they arrive, and
     * any glide is over (the core ends it).
     *
     * Never refuses the teleport.
     */
    bool OnPlayerBeforeTeleport(Player* player, uint32 mapId, float /*x*/, float /*y*/, float /*z*/,
        float /*orientation*/, uint32 /*options*/, Unit* /*target*/) override
    {
        if (CollisionState* state = FindState(player))
        {
            WithdrawBox(player, *state);

            if (mapId != player->GetMapId())
                state->box.reset();

            ForgetGlide(*state);
            state->stillSinceMs = 0;
            state->lastFlags = 0;
            state->lastPacketMs = 0;
            state->hasClientClock = false;
            state->stalled = false;
            state->hasStallRef = false;
            state->knockedBack = false;
            state->blockedValid = false;
        }

        return true;
    }

    /// A login or a far teleport, on the world thread like the command: the client has
    /// dropped every object it had, a probe included.
    void OnPlayerMapChanged(Player* player) override
    {
        if (CollisionState* state = FindState(player))
            state->probe.reset();
    }
};

class sanctuary_collision_movementscript : public MovementHandlerScript
{
public:
    sanctuary_collision_movementscript() : MovementHandlerScript("sanctuary_collision_movementscript",
        { MOVEMENTHOOK_ON_PLAYER_MOVE }) { }

    void OnPlayerMove(Player* player, MovementInfo movementInfo, uint32 opcode) override
    {
        if (!g_enabled)
        {
            // Nothing is recorded while switched off, so nothing recorded before may be
            // predicted from once it is back on.
            if (CollisionState* state = FindState(player))
            {
                state->lastFlags = 0;
                state->lastPacketMs = 0;
                state->hasClientClock = false;
                state->stalled = false;
                state->hasStallRef = false;
                state->knockedBack = false;
                state->blockedValid = false;
                ForgetGlide(*state);
            }

            return;
        }

        uint64 const now = Now();
        CollisionState* state = StateOf(player);

        if (state->awaitingPacket && g_debug)
            LOG_INFO("module", "SanctuaryCollision [debug {}]: {}'s client, {} ms after the glide ended: {}.", now,
                     player->GetName(), int64(now) - int64(state->glideEndMs),
                     (movementInfo.flags & DirectionFlags) ? "still moving" : "not moving");

        state->awaitingPacket = false;
        state->hasPendingDone = false;

        UpdateStall(player, *state, movementInfo);

        state->lastPos = movementInfo.pos;
        state->lastFlags = movementInfo.flags;
        state->lastPacketMs = now;
        state->lastClientMs = movementInfo.time;
        state->hasClientClock = true;
        state->blockedValid = false;

        if (opcode == CMSG_MOVE_KNOCK_BACK_ACK)
            state->knockedBack = true;
        else if (!(movementInfo.flags & AirborneFlags))
            state->knockedBack = false;

        // Their box comes off everyone's client on the first step, rather than at the next
        // check: the other clients hear about it one network trip later as it is.
        if (movementInfo.flags & MOVEMENTFLAG_MASK_MOVING)
        {
            state->stillSinceMs = 0;
            WithdrawBox(player, *state);
        }
    }
};

/*
 * Three things about a glide that only the packets tell, looked at before the core handles
 * them (movement packets are handled on the player's map thread):
 *
 *  - Their client's report that it has finished the glide (CMSG_MOVE_SPLINE_DONE), with where
 *    it ended and which keys are held. The core ignores it; it is kept to pass on.
 *  - A movement packet their client sent before the glide reached it, arriving after the
 *    server has finished the glide - more likely the longer their ping. The core would take
 *    it, and everyone would see them snap back to where they were before it. Dropped.
 *  - An acknowledgement - of a speed change, a root, a knockback - arriving while the server
 *    is still driving the glide. The core drops every movement packet then, acknowledgements
 *    included, and a lost one is never made good. The glide is ended there instead.
 */
class sanctuary_collision_serverscript : public ServerScript
{
public:
    sanctuary_collision_serverscript() : ServerScript("sanctuary_collision_serverscript",
        { SERVERHOOK_CAN_PACKET_RECEIVE }) { }

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        if (!g_enabled)
            return true;

        uint16 const opcode = packet.GetOpcode();
        bool const done = opcode == CMSG_MOVE_SPLINE_DONE;
        bool const ack = IsMovementAck(opcode);

        if (!done && !ack && !IsMovementReport(opcode))
            return true;

        Player* player = session->GetPlayer();

        if (!player || !player->IsInWorld())
            return true;

        CollisionState* state = FindState(player);

        if (!state || !IsGliding(*state))
            return true;

        if (ack)
        {
            if (state->glideRunning && !player->movespline->Finalized()
                && player->movespline->GetId() == state->glideId)
            {
                if (g_debug)
                    LOG_INFO("module", "SanctuaryCollision [debug {}]: {}'s glide ended early for an "
                                       "acknowledgement.", Now(), player->GetName());

                player->StopMoving();
            }

            return true;
        }

        // Everything after their client's own end of the glide was sent after it.
        if (!done && state->hasPendingDone)
            return true;

        try
        {
            WorldPacket copy(packet);
            copy.rpos(0);

            ObjectGuid guid;
            copy >> guid.ReadAsPacked();

            if (guid != player->GetGUID())
                return true;

            MovementInfo info;
            info.guid = guid;
            session->ReadMovementInfo(copy, &info);

            if (done)
            {
                uint32 splineId = 0;
                copy >> splineId;

                if (splineId == state->glideId)
                {
                    state->pendingDone = info;
                    state->hasPendingDone = true;
                }

                return true;
            }

            if (!state->glideRunning && state->glideHasClock
                && int32(info.time - state->glideArriveClientMs) < -GlideStaleSlackMs)
            {
                if (g_debug)
                    LOG_INFO("module", "SanctuaryCollision [debug {}]: dropped a packet {} sent before the glide "
                                       "reached them.", Now(), player->GetName());

                return false;
            }
        }
        catch (ByteBufferException const&)
        {
        }

        return true;
    }
};

class sanctuary_collision_worldscript : public WorldScript
{
public:
    sanctuary_collision_worldscript() : WorldScript("sanctuary_collision_worldscript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>("SanctuaryCollision.Enable", true);
        g_debug = sConfigMgr->GetOption<bool>("SanctuaryCollision.Debug", false);
        g_checkIntervalMs = std::max<uint32>(10,
            sConfigMgr->GetOption<uint32>("SanctuaryCollision.CheckIntervalMs", 50));

        g_blockEnabled = sConfigMgr->GetOption<bool>("SanctuaryCollision.Block.Enable", true);
        g_blockEntry = sConfigMgr->GetOption<uint32>("SanctuaryCollision.Block.Entry", 990300);
        g_blockIdleMs = sConfigMgr->GetOption<uint32>("SanctuaryCollision.Block.IdleMs", 0);
        g_blockScale = std::clamp(sConfigMgr->GetOption<float>("SanctuaryCollision.Block.Scale", 1.0f), 0.25f, 4.0f);
        g_blockSolidMs = std::min<uint32>(5000,
            sConfigMgr->GetOption<uint32>("SanctuaryCollision.Block.SolidMs", 1000));

        g_bumpEnabled = sConfigMgr->GetOption<bool>("SanctuaryCollision.Bump.Enable", true);

        std::string style = sConfigMgr->GetOption<std::string>("SanctuaryCollision.Bump.Style", "glide");
        std::transform(style.begin(), style.end(), style.begin(),
            [](unsigned char c) { return char(std::tolower(c)); });

        if (style == "slide")
            g_style = SwerveStyle::Slide;
        else if (style == "bounce")
            g_style = SwerveStyle::Bounce;
        else
        {
            if (style != "glide")
                LOG_ERROR("module", "SanctuaryCollision: Bump.Style '{}' is not glide, slide or bounce - using glide.",
                          style);

            g_style = SwerveStyle::Glide;
        }

        g_glideMs = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("SanctuaryCollision.Glide.Ms", 150), 80, 250);
        g_slideHop = std::clamp(sConfigMgr->GetOption<float>("SanctuaryCollision.Slide.Hop", 0.7f), 0.3f, 5.0f);
        g_bumpSpeedXY = std::clamp(sConfigMgr->GetOption<float>("SanctuaryCollision.Bump.SpeedXY", 3.0f), 0.0f, 20.0f);
        g_bumpSpeedZ = std::clamp(sConfigMgr->GetOption<float>("SanctuaryCollision.Bump.SpeedZ", 1.5f), 0.1f, 20.0f);
        g_bumpCooldownMs = sConfigMgr->GetOption<uint32>("SanctuaryCollision.Bump.CooldownMs", 600);
        g_bumpRadiusScale = std::clamp(
            sConfigMgr->GetOption<float>("SanctuaryCollision.Bump.RadiusScale", 1.0f), 0.1f, 4.0f);

        g_disabledAreas.clear();
        std::string const areas = sConfigMgr->GetOption<std::string>("SanctuaryCollision.DisabledAreas", "");

        for (std::string_view token : Acore::Tokenize(areas, ',', false))
        {
            std::size_t const first = token.find_first_not_of(' ');

            if (first == std::string_view::npos)
                continue;

            token = token.substr(first, token.find_last_not_of(' ') - first + 1);

            if (Optional<uint32> id = Acore::StringTo<uint32>(token))
                g_disabledAreas.insert(*id);
            else
                LOG_ERROR("module", "SanctuaryCollision: '{}' in DisabledAreas is not a zone or area id - ignored.",
                          token);
        }
    }

    /// Checked at startup rather than on config load, because gameobject templates are not
    /// loaded yet when the config is read.
    void OnStartup() override
    {
        if (!g_enabled)
            return;

        if (g_blockEnabled && !sObjectMgr->GetGameObjectTemplate(g_blockEntry))
        {
            LOG_ERROR("module", "SanctuaryCollision: gameobject_template {} is missing, so nobody standing still can "
                                "be walked into - only the swerve works. Apply the module's world SQL.", g_blockEntry);
            g_blockEnabled = false;
        }

        char const* const style = g_style == SwerveStyle::Glide ? "glide" : g_style == SwerveStyle::Slide ? "slide"
            : "bounce";

        LOG_INFO("module", "SanctuaryCollision: standing players {}, moving players {}, {} area(s) exempt.",
                 g_blockEnabled ? "solid" : "off", g_bumpEnabled ? style : "off", g_disabledAreas.size());
    }
};

using namespace Acore::ChatCommands;

/*
 * Diagnostics for game masters. Commands run on the world thread, which never overlaps the
 * map updates, so they can use the game master's own state.
 *
 * `.collision probe` sends the game master their own copy of the box three yards ahead,
 * built and delivered exactly the way a standing player's copy is. Walking into it answers
 * whether the client finds a box sent this way solid. Run it again to take the box away.
 *
 * `.collision glide [yards]` glides the game master that far to their right (1 by default),
 * exactly as a swerve would, standing or on the move. It answers how a glide looks and feels
 * - above all, whether running carries on after it with the key still held.
 */
class sanctuary_collision_commandscript : public CommandScript
{
public:
    sanctuary_collision_commandscript() : CommandScript("sanctuary_collision_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable collisionTable =
        {
            { "probe", HandleProbe, rbac::RBAC_PERM_COMMAND_GOBJECT_ADD_TEMP, Console::No },
            { "glide", HandleGlide, rbac::RBAC_PERM_COMMAND_GOBJECT_ADD_TEMP, Console::No },
        };

        static ChatCommandTable commandTable =
        {
            { "collision", collisionTable },
        };

        return commandTable;
    }

    static bool HandleProbe(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        CollisionState* state = StateOf(player);

        // One sent to an earlier login - before a reconnect - is not on the client they have now.
        if (state->probe && state->probeLogin != player->GetInGameTime())
            state->probe.reset();

        if (state->probe)
        {
            SendRemoval(player, state->probe->GetGUID());
            state->probe.reset();
            handler->SendSysMessage("Collision probe removed.");
            return true;
        }

        Map* map = player->GetMap();
        float const o = player->GetOrientation();
        float const x = player->GetPositionX() + std::cos(o) * 3.0f;
        float const y = player->GetPositionY() + std::sin(o) * 3.0f;
        float const z = player->GetPositionZ();
        G3D::Quat const rotation = G3D::Quat::fromAxisAngleRotation(G3D::Vector3::unitZ(), o);

        auto probe = std::make_unique<CollisionProxy>();

        if (!probe->Create(NextBoxLow(), g_blockEntry, map, player->GetPhaseMask(), x, y, z, o, rotation, 0,
                GO_STATE_READY))
        {
            handler->SendSysMessage("Could not create the probe - is gameobject_template 990300 present?");
            return true;
        }

        // Its narrow side toward the player, as a standing player's copy would be.
        probe->Place(x, y, z, Position::NormalizeOrientation(o + float(M_PI)));
        SendCopy(player, probe.get());

        handler->PSendSysMessage("Collision probe {} sent 3 yards ahead of you, at ({:.1f}, {:.1f}, {:.1f}). Walk "
                                 "into it; .collision probe again removes it.", probe->GetGUID().ToString(), x, y, z);
        LOG_INFO("module", "SanctuaryCollision: probe {} sent to {} at ({:.2f}, {:.2f}, {:.2f}).",
                 probe->GetGUID().ToString(), player->GetName(), x, y, z);

        state->probe = std::move(probe);
        state->probeLogin = player->GetInGameTime();
        return true;
    }

    static bool HandleGlide(ChatHandler* handler, Optional<float> yards)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        uint64 const now = Now();
        CollisionState* state = StateOf(player);
        float const step = std::clamp(yards.value_or(1.0f), 0.2f, 3.0f);
        float const right = player->GetOrientation() - float(M_PI) / 2.0f;
        Motion const motion = Predict(player, *state, now);
        GlidePlan plan;
        GlideRefusal why = GlideRefusal::None;

        if (!PlanGlide(player, *state, motion, std::cos(right), std::sin(right), step, nullptr, nullptr, now, plan,
                why))
        {
            handler->PSendSysMessage("No glide: {}.", Describe(why));
            return true;
        }

        if (!LaunchGlide(player, *state, plan, now))
        {
            handler->SendSysMessage("No glide: the core refused the spline.");
            return true;
        }

        handler->PSendSysMessage("Gliding {:.2f} yd over {} ms, {}.", plan.length, g_glideMs,
                                 motion.moving ? "on the move" : "standing");
        LOG_INFO("module", "SanctuaryCollision [{}]: {} glides {:.2f} yd by command ({}).", now, player->GetName(),
                 plan.length, motion.moving ? "moving" : "standing");
        return true;
    }
};

void AddSC_sanctuary_collision_scripts()
{
    new sanctuary_collision_playerscript();
    new sanctuary_collision_movementscript();
    new sanctuary_collision_serverscript();
    new sanctuary_collision_worldscript();
    new sanctuary_collision_commandscript();
}
