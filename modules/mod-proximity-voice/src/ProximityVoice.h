/*
 * mod-proximity-voice
 *
 * Server-authoritative proximity voice chat for AzerothCore.
 *
 * The world server owns every fact the voice mixer needs - who is where, on which
 * map/instance/phase, and which languages a character actually knows - and streams
 * that to a companion voice server. Voice clients never learn a position or a
 * language they were not entitled to, because the routing decision is made here.
 */

#ifndef MOD_PROXIMITY_VOICE_H
#define MOD_PROXIMITY_VOICE_H

#include "Define.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Player;

namespace ProximityVoice
{
    class Bridge;
    class WireRecord;

    struct ModuleConfig
    {
        bool Enable = false;
        bool Announce = true;

        std::string BridgeHost = "127.0.0.1";
        uint16 BridgePort = 7788;
        std::string BridgeSecret = "change-me";
        uint32 BridgeReconnectMs = 5000;

        /// How often a moving player's position is pushed to the voice server.
        uint32 UpdateIntervalMs = 150;
        /// Movement below this many yards does not warrant a push.
        float PositionEpsilon = 0.35f;
        /// Facing change below this many radians does not warrant a push.
        float OrientationEpsilon = 0.15f;
        /// A parked player is still refreshed this often so the mixer knows they are alive.
        uint32 KeepAliveMs = 2000;

        float RangeDefault = 25.0f;
        float RangeMin = 5.0f;
        float RangeMax = 100.0f;

        bool EnforceLanguage = true;
        bool GameMastersUnderstandAll = true;
        bool DeadCanSpeak = false;
        bool DeadHearOnlyDead = true;

        uint32 TokenTtlSeconds = 300;
        /// Advertised to the client so it knows where to connect; may differ from BridgeHost.
        std::string ClientHost = "127.0.0.1";
        uint16 ClientPort = 7789;
    };

    /// Everything the voice server needs to know about one online character.
    struct Session
    {
        ObjectGuid guid;
        uint32 accountId = 0;
        std::string name;
        uint8 race = 0;
        uint8 team = 0;
        uint8 level = 0;
        bool isGameMaster = false;

        std::string token;
        uint32 tokenIssuedAt = 0;

        /// Language this character's voice is carried in.
        uint32 voiceLanguage = LANG_UNIVERSAL;
        std::vector<uint32> knownLanguages;

        float range = 25.0f;
        bool muted = false;

        /// Whether the companion app is currently attached to this character.
        bool clientConnected = false;
        bool speaking = false;

        /*
         * State changed while the character could not be sent to.
         *
         * The addon is pushed to at moments the module chooses, and two of them can land
         * when nobody is listening: at login the client may still be on the loading
         * screen, and a voice client attaching mid-login arrives before the player is
         * resolvable through ObjectAccessor. Without this the addon keeps showing stale
         * state until something else happens to push, which is why typing .voice token
         * used to look like the fix.
         */
        bool needsAddonPush = false;

        /// Rate limit for addon-initiated syncs, which arrive on an unauthenticated path.
        uint32 lastSyncMs = 0;

        /*
         * When to tell the player about voice, or 0 for nothing pending.
         *
         * Deferred a few seconds past login so the message can say whether their voice
         * client actually attached, rather than instructing everyone to run a command
         * most of them do not need.
         */
        uint32 announceAt = 0;

        // Last snapshot pushed to the voice server, used to suppress redundant traffic.
        uint32 sentMapId = 0xFFFFFFFF;
        uint32 sentInstanceId = 0;
        float sentX = 0.0f, sentY = 0.0f, sentZ = 0.0f, sentO = 0.0f;
        bool sentAlive = true;
        uint32 lastPushMs = 0;
    };

    class Manager
    {
    public:
        static Manager* instance();

        ModuleConfig const& Config() const { return _config; }

        void LoadConfig(bool reload);
        void Startup();
        void Shutdown();
        void Update(uint32 diff);

        // --- world events -------------------------------------------------
        void OnLogin(Player* player);
        void OnLogout(Player* player);
        void OnLanguagesChanged(Player* player);
        void OnWorldChanged(Player* player);

        // --- player-facing operations, all validated here ------------------
        /// Clamps to the configured range window. Returns the value actually applied.
        float SetRange(Player* player, float yards);
        /// Fails if the character has not learned the language.
        bool SetVoiceLanguage(Player* player, uint32 language, std::string& error);
        void SetMuted(Player* player, bool muted);
        /// Issues a fresh single-use token and hands it to the player's addon + chat.
        std::string IssueToken(Player* player, bool tellPlayer);

        /// Handles an addon message from the player's client. Returns true if it was ours.
        bool HandleAddonMessage(Player* player, std::string const& message);

        Session const* FindSession(ObjectGuid guid) const;
        Session* FindSessionMutable(ObjectGuid guid);
        std::size_t SessionCount() const { return _sessions.size(); }

        bool IsBridgeConnected() const;

        // --- sounds played through the voice path --------------------------

        /*
         * A sound played this way is routed as though it were somebody speaking from the
         * player's position, so it inherits the voice attenuation, stereo placement and
         * range cutoff exactly.
         *
         * That routing is the only way to get a distance-faded sound at all: no 3.3.5a
         * sound packet carries a volume, so a client cannot be told how loudly to play
         * one. The cost is that only players with a voice client attached hear it.
         */
        /// Asks the relay to play a library sound at this player's position.
        ///
        /// Not named PlaySound: <mmsystem.h> defines that as a macro expanding to
        /// PlaySoundA, which renames the declaration and the definition inconsistently
        /// depending on include order and fails to link as a member of this class.
        bool PlayRelaySound(Player* source, std::string const& sound, float range);

        /// Stops the sounds this player started.
        void StopSounds(Player* source);

        /// Names the relay last published, for the game master panel to list.
        std::vector<std::string> const& AvailableSounds() const { return _sounds; }

        /// The folder the relay read those names from, so a game master can be told where
        /// to put files rather than left to guess. Empty until the relay says.
        std::string const& SoundDirectory() const { return _soundDirectory; }

        /// Bumped every time the relay publishes a library. A caller that asked for a
        /// re-scan watches this to know its answer arrived, rather than guessing at how
        /// long scanning a folder of unknown size takes.
        uint32 SoundsRevision() const { return _soundsRevision; }

        /// Asks the relay to re-scan its sound folder and publish the library again.
        ///
        /// The relay reads the folder once at startup, so a sound dropped in while the
        /// realm is up is otherwise invisible until it restarts - which would drop every
        /// voice connection to pick up one file.
        void RequestSoundReload();

        /// Called on the world thread with records the bridge received.
        void HandleInbound(WireRecord const& record);

    private:
        Manager() = default;

        void PushSession(Session const& session);
        void PushLanguages(Session const& session);
        void PushSettings(Session const& session);
        void PushPosition(Player* player, Session& session, uint32 nowMs, bool force);
        void Send(WireRecord const& record);

        void LoadPersistedSettings(Player* player, Session& session);
        void PersistSettings(Session const& session);

        /// Codes are short enough that a collision with a live one is worth
        /// ruling out rather than reasoning about - a duplicate would let one
        /// player authenticate as another.
        std::string GenerateUniqueToken() const;

        void SendAddonUpdate(Player* player);

        ModuleConfig _config;
        std::unique_ptr<Bridge> _bridge;
        std::unordered_map<ObjectGuid, Session> _sessions;

        /// The relay's sound library, refreshed every time it reconnects.
        std::vector<std::string> _sounds;
        std::string _soundDirectory;
        uint32 _soundsRevision = 0;
        uint32 _accumulatorMs = 0;
        uint32 _clockMs = 0;
        bool _started = false;
    };

    // --- language helpers -------------------------------------------------

    /// The language a character speaks by default, from their race.
    uint32 GetRacialLanguage(uint8 race);
    /// Every language this character may speak, including LANG_UNIVERSAL for GMs.
    std::vector<uint32> CollectKnownLanguages(Player* player);
    /// Whether this character has learned the given language.
    bool KnowsLanguage(Player* player, uint32 language);
    /// Human-readable name, for command output.
    std::string GetLanguageName(uint32 language);
    /// Resolves "common", "orcish", or a raw id. Returns false if unknown.
    bool ParseLanguage(std::string const& input, uint32& language);
    /// Ordered list of every language the module can carry.
    std::vector<uint32> AllLanguages();
}

#define sProximityVoice ProximityVoice::Manager::instance()

#endif // MOD_PROXIMITY_VOICE_H
