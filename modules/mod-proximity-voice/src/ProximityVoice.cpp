/*
 * mod-proximity-voice - core manager
 */

#include "ProximityVoice.h"

// Sibling module. Static builds put every module's source dir on the include path.
#include "SanctuaryIdentity.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "PVBridge.h"
#include "PVWire.h"
#include "Player.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <sstream>

namespace ProximityVoice
{
    namespace
    {
        constexpr char const* AddonPrefix = "SVOICE";

        /// Long enough for a voice client to attach after login, short enough to still
        /// read as part of logging in.
        constexpr uint32 AnnounceDelayMs = 4000;

        /// Floor between addon-initiated syncs from one character.
        constexpr uint32 SyncCooldownMs = 1000;

        std::string JoinLanguages(std::vector<uint32> const& languages)
        {
            std::string out;
            for (uint32 language : languages)
            {
                if (!out.empty())
                    out += ',';
                out += std::to_string(language);
            }
            return out;
        }

        // Crockford-style base32, with I, L, O and U removed: nothing in this
        // alphabet can be misread as something else when a player copies a code
        // off the screen by hand.
        constexpr char const* TokenAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
        constexpr std::size_t TokenCharacters = 8;

        /// Eight base32 characters is 40 bits. Against a code that expires in
        /// minutes and can only be tried one TCP connection at a time, that is a
        /// far larger margin than it needs, and it fits on one line of chat.
        std::string MakeToken()
        {
            // random_device rather than a seeded PRNG: this is a bearer
            // credential, and observing one code must not reveal the next.
            static thread_local std::random_device source;
            std::uniform_int_distribution<uint32> dist(0, 31);

            std::string token;
            token.reserve(TokenCharacters + 1);

            for (std::size_t i = 0; i < TokenCharacters; ++i)
            {
                // A dash halfway makes it much easier to read back aloud.
                if (i == TokenCharacters / 2)
                    token += '-';

                token += TokenAlphabet[dist(source)];
            }

            return token;
        }

        void SendAddonPacket(Player* player, std::string const& payload)
        {
            if (!player || !player->GetSession())
                return;

            // 3.3.5a carries addon traffic as "PREFIX\tBODY" inside a whisper to self.
            std::string message = std::string(AddonPrefix) + "\t" + payload;

            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, message);
            player->GetSession()->SendPacket(&data);
        }
    }

    Manager* Manager::instance()
    {
        static Manager instance;
        return &instance;
    }

    void Manager::LoadConfig(bool reload)
    {
        ModuleConfig config;

        config.Enable = sConfigMgr->GetOption<bool>("ProximityVoice.Enable", false);
        config.Announce = sConfigMgr->GetOption<bool>("ProximityVoice.Announce", true);

        config.BridgeHost = sConfigMgr->GetOption<std::string>("ProximityVoice.Bridge.Host", "127.0.0.1");
        config.BridgePort = uint16(sConfigMgr->GetOption<uint32>("ProximityVoice.Bridge.Port", 7788));
        config.BridgeSecret = sConfigMgr->GetOption<std::string>("ProximityVoice.Bridge.Secret", "change-me");
        config.BridgeReconnectMs = sConfigMgr->GetOption<uint32>("ProximityVoice.Bridge.ReconnectMs", 5000);

        config.UpdateIntervalMs = std::max<uint32>(50, sConfigMgr->GetOption<uint32>("ProximityVoice.Update.IntervalMs", 150));
        config.PositionEpsilon = sConfigMgr->GetOption<float>("ProximityVoice.Update.PositionEpsilon", 0.35f);
        config.OrientationEpsilon = sConfigMgr->GetOption<float>("ProximityVoice.Update.OrientationEpsilon", 0.15f);
        config.KeepAliveMs = std::max<uint32>(config.UpdateIntervalMs, sConfigMgr->GetOption<uint32>("ProximityVoice.Update.KeepAliveMs", 2000));

        config.RangeMin = std::max(1.0f, sConfigMgr->GetOption<float>("ProximityVoice.Range.Min", 5.0f));
        config.RangeMax = std::max(config.RangeMin, sConfigMgr->GetOption<float>("ProximityVoice.Range.Max", 100.0f));
        config.RangeDefault = std::clamp(sConfigMgr->GetOption<float>("ProximityVoice.Range.Default", 25.0f), config.RangeMin, config.RangeMax);

        config.EnforceLanguage = sConfigMgr->GetOption<bool>("ProximityVoice.Language.Enforce", true);
        config.GameMastersUnderstandAll = sConfigMgr->GetOption<bool>("ProximityVoice.Language.GameMastersUnderstandAll", true);
        config.DeadCanSpeak = sConfigMgr->GetOption<bool>("ProximityVoice.Dead.CanSpeak", false);
        config.DeadHearOnlyDead = sConfigMgr->GetOption<bool>("ProximityVoice.Dead.HearOnlyDead", true);

        config.TokenTtlSeconds = sConfigMgr->GetOption<uint32>("ProximityVoice.Token.TTLSeconds", 300);
        config.ClientHost = sConfigMgr->GetOption<std::string>("ProximityVoice.Client.Host", "127.0.0.1");
        config.ClientPort = uint16(sConfigMgr->GetOption<uint32>("ProximityVoice.Client.Port", 7789));

        bool const bridgeMoved = _started &&
            (config.BridgeHost != _config.BridgeHost ||
             config.BridgePort != _config.BridgePort ||
             config.BridgeSecret != _config.BridgeSecret);

        bool const toggled = _started && (config.Enable != _config.Enable);

        _config = config;

        if (!reload)
            return;

        if (toggled || bridgeMoved)
        {
            Shutdown();
            Startup();
        }
    }

    void Manager::Startup()
    {
        if (_started || !_config.Enable)
            return;

        if (_config.BridgeSecret == "change-me")
            LOG_WARN("module.proximityvoice", "ProximityVoice.Bridge.Secret is still the default value - set a real shared secret.");

        _bridge = std::make_unique<Bridge>(_config.BridgeHost, _config.BridgePort, _config.BridgeSecret, _config.BridgeReconnectMs);
        _bridge->Start();
        _started = true;

        LOG_INFO("module.proximityvoice", "Proximity voice enabled, bridging to {}:{} (clients connect to {}:{})",
            _config.BridgeHost, _config.BridgePort, _config.ClientHost, _config.ClientPort);
    }

    void Manager::Shutdown()
    {
        if (!_started)
            return;

        if (_bridge)
        {
            _bridge->Stop();
            _bridge.reset();
        }

        _sessions.clear();
        _started = false;
    }

    bool Manager::IsBridgeConnected() const
    {
        return _bridge && _bridge->IsConnected();
    }

    bool Manager::PlayRelaySound(Player* source, std::string const& sound, float range)
    {
        if (!source || sound.empty() || !IsBridgeConnected())
            return false;

        // Nothing but who and what is sent. The relay already tracks this character's
        // position and knows it better than any coordinates copied in here would.
        if (!FindSession(source->GetGUID()))
            return false;

        WireRecord record("PLAYSOUND");
        record.Set("guid", uint64(source->GetGUID().GetRawValue()));
        record.Set("name", sound);
        record.Set("range", range);
        Send(record);

        return true;
    }

    void Manager::StopSounds(Player* source)
    {
        if (!source || !IsBridgeConnected())
            return;

        WireRecord record("STOPSOUND");
        record.Set("guid", uint64(source->GetGUID().GetRawValue()));
        Send(record);
    }

    void Manager::RequestSoundReload()
    {
        if (!IsBridgeConnected())
            return;

        // No payload: the relay scans whichever folder it was configured with. Which one
        // that is comes back in the reply, so nothing here has to know it.
        Send(WireRecord("RELOADSOUNDS"));
    }

    void Manager::Send(WireRecord const& record)
    {
        if (_bridge)
            _bridge->Send(record.Encode());
    }

    Session const* Manager::FindSession(ObjectGuid guid) const
    {
        auto itr = _sessions.find(guid);
        return itr == _sessions.end() ? nullptr : &itr->second;
    }

    Session* Manager::FindSessionMutable(ObjectGuid guid)
    {
        auto itr = _sessions.find(guid);
        return itr == _sessions.end() ? nullptr : &itr->second;
    }

    // --- lifecycle ---------------------------------------------------------

    void Manager::OnLogin(Player* player)
    {
        if (!_started || !player || !player->GetSession())
            return;

        Session session;
        session.guid = player->GetGUID();
        session.accountId = player->GetSession()->GetAccountId();
        session.name = player->GetName();
        session.race = player->getRace();
        session.team = uint8(player->GetTeamId());
        session.level = player->GetLevel();
        session.isGameMaster = player->IsGameMaster();
        session.knownLanguages = CollectKnownLanguages(player);
        session.voiceLanguage = GetRacialLanguage(session.race);
        session.range = _config.RangeDefault;

        LoadPersistedSettings(player, session);

        // A stored language the character can no longer speak falls back to their racial one.
        if (_config.EnforceLanguage && session.voiceLanguage != LANG_UNIVERSAL && !KnowsLanguage(player, session.voiceLanguage))
            session.voiceLanguage = GetRacialLanguage(session.race);

        session.token = GenerateUniqueToken();
        session.tokenIssuedAt = uint32(GameTime::GetGameTime().count());

        ObjectGuid const guid = session.guid;
        _sessions[guid] = std::move(session);
        Session& stored = _sessions[guid];

        PushSession(stored);
        PushPosition(player, stored, _clockMs, true);

        // Sent now in case the client is ready, and queued as well: at this point it is
        // usually still on the loading screen and will not process an addon whisper. The
        // addon also asks for itself once it loads, so this is belt and braces.
        SendAddonUpdate(player);
        stored.needsAddonPush = true;

        if (_config.Announce)
        {
            // Deliberately no mention of .voice token. The launcher attaches on its own,
            // and telling everyone to type a command made a working setup look broken.
            // AnnounceVoiceState says the right thing once we know whether it attached.
            stored.announceAt = _clockMs + AnnounceDelayMs;
        }
    }

    void Manager::OnLogout(Player* player)
    {
        if (!_started || !player)
            return;

        auto itr = _sessions.find(player->GetGUID());
        if (itr == _sessions.end())
            return;

        PersistSettings(itr->second);

        WireRecord record("GONE");
        record.Set("guid", uint64(itr->first.GetRawValue()));
        Send(record);

        _sessions.erase(itr);
    }

    void Manager::OnLanguagesChanged(Player* player)
    {
        if (!_started || !player)
            return;

        Session* session = FindSessionMutable(player->GetGUID());
        if (!session)
            return;

        std::vector<uint32> languages = CollectKnownLanguages(player);
        if (languages == session->knownLanguages)
            return;

        session->knownLanguages = std::move(languages);
        PushLanguages(*session);
        SendAddonUpdate(player);
    }

    void Manager::OnWorldChanged(Player* player)
    {
        if (!_started || !player)
            return;

        Session* session = FindSessionMutable(player->GetGUID());
        if (!session)
            return;

        // A map or phase change must reach the mixer before the next audio frame,
        // otherwise a player stays briefly audible in the world they just left.
        PushPosition(player, *session, _clockMs, true);
    }

    // --- periodic work -----------------------------------------------------

    void Manager::Update(uint32 diff)
    {
        if (!_started || !_bridge)
            return;

        _clockMs += diff;

        for (WireRecord const& record : _bridge->DrainInbound())
            HandleInbound(record);

        if (_bridge->ConsumeResyncFlag())
        {
            // The voice server forgot everything; republish each live session.
            for (auto& [guid, session] : _sessions)
            {
                PushSession(session);
                session.sentMapId = 0xFFFFFFFF; // forces the next position push
            }
        }

        // Anything that could not be delivered when it happened. Done every tick rather
        // than on the position interval so a queued push lands promptly.
        for (auto& [guid, session] : _sessions)
        {
            if (!session.needsAddonPush && session.announceAt == 0)
                continue;

            Player* player = ObjectAccessor::FindConnectedPlayer(guid);
            if (!player || !player->IsInWorld())
                continue;

            if (session.needsAddonPush)
            {
                session.needsAddonPush = false;
                SendAddonUpdate(player);
            }

            if (session.announceAt != 0 && _clockMs >= session.announceAt)
            {
                session.announceAt = 0;

                ChatHandler handler(player->GetSession());

                if (session.clientConnected)
                {
                    handler.PSendSysMessage("|cff00ff96Proximity voice|r is connected.");
                }
                else
                {
                    // Only now, when we know they have no client attached, is the manual
                    // route worth mentioning.
                    handler.PSendSysMessage(
                        "|cff00ff96Proximity voice|r is active. Run the Sanctuary launcher, or connect a voice "
                        "client to {}:{} and use |cffffffff.voice token|r for a login code.",
                        _config.ClientHost, _config.ClientPort);
                }
            }
        }

        _accumulatorMs += diff;
        if (_accumulatorMs < _config.UpdateIntervalMs)
            return;

        _accumulatorMs = 0;

        if (!_bridge->IsConnected())
            return;

        for (auto& [guid, session] : _sessions)
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(guid);
            if (!player || !player->IsInWorld())
                continue;

            PushPosition(player, session, _clockMs, false);
        }
    }

    void Manager::PushPosition(Player* player, Session& session, uint32 nowMs, bool force)
    {
        if (!player || !player->IsInWorld())
            return;

        float const x = player->GetPositionX();
        float const y = player->GetPositionY();
        float const z = player->GetPositionZ();
        float const o = player->GetOrientation();
        bool const alive = player->IsAlive();
        uint32 const mapId = player->GetMapId();
        uint32 const instanceId = player->GetInstanceId();

        if (!force)
        {
            bool const worldChanged = mapId != session.sentMapId || instanceId != session.sentInstanceId;
            bool const stateChanged = alive != session.sentAlive;

            float const dx = x - session.sentX;
            float const dy = y - session.sentY;
            float const dz = z - session.sentZ;
            bool const moved = (dx * dx + dy * dy + dz * dz) > (_config.PositionEpsilon * _config.PositionEpsilon);

            float turn = std::fabs(o - session.sentO);
            if (turn > float(M_PI))
                turn = 2.0f * float(M_PI) - turn;
            bool const turned = turn > _config.OrientationEpsilon;

            bool const stale = (nowMs - session.lastPushMs) >= _config.KeepAliveMs;

            if (!worldChanged && !stateChanged && !moved && !turned && !stale)
                return;
        }

        WireRecord record("POS");
        record.Set("guid", uint64(session.guid.GetRawValue()));
        record.Set("map", mapId);
        record.Set("inst", instanceId);
        record.Set("phase", player->GetPhaseMask());
        record.Set("x", x);
        record.Set("y", y);
        record.Set("z", z);
        record.Set("o", o);
        record.Set("alive", alive);
        record.Set("zone", player->GetZoneId());
        record.Set("area", player->GetAreaId());
        Send(record);

        session.sentMapId = mapId;
        session.sentInstanceId = instanceId;
        session.sentX = x;
        session.sentY = y;
        session.sentZ = z;
        session.sentO = o;
        session.sentAlive = alive;
        session.lastPushMs = nowMs;
    }

    void Manager::PushSession(Session const& session)
    {
        WireRecord record("SESSION");
        record.Set("guid", uint64(session.guid.GetRawValue()));
        record.Set("acct", session.accountId);
        record.Set("name", session.name);
        record.Set("race", uint32(session.race));
        record.Set("team", uint32(session.team));
        record.Set("level", uint32(session.level));
        record.Set("gm", session.isGameMaster);
        record.Set("token", session.token);
        record.Set("lang", session.voiceLanguage);
        record.Set("langs", JoinLanguages(session.knownLanguages));
        record.Set("range", session.range);
        record.Set("muted", session.muted);
        record.Set("rmin", _config.RangeMin);
        record.Set("rmax", _config.RangeMax);
        record.Set("enforce", _config.EnforceLanguage);
        record.Set("gmall", _config.GameMastersUnderstandAll);
        record.Set("deadspeak", _config.DeadCanSpeak);
        record.Set("deadhear", _config.DeadHearOnlyDead);
        record.Set("ttl", _config.TokenTtlSeconds);
        Send(record);
    }

    void Manager::PushLanguages(Session const& session)
    {
        WireRecord record("LANGS");
        record.Set("guid", uint64(session.guid.GetRawValue()));
        record.Set("langs", JoinLanguages(session.knownLanguages));
        record.Set("lang", session.voiceLanguage);
        Send(record);
    }

    void Manager::PushSettings(Session const& session)
    {
        WireRecord record("CFG");
        record.Set("guid", uint64(session.guid.GetRawValue()));
        record.Set("range", session.range);
        record.Set("lang", session.voiceLanguage);
        record.Set("muted", session.muted);
        Send(record);
    }

    // --- player-facing operations -----------------------------------------

    float Manager::SetRange(Player* player, float yards)
    {
        if (!player)
            return 0.0f;

        Session* session = FindSessionMutable(player->GetGUID());
        if (!session)
            return 0.0f;

        session->range = std::clamp(yards, _config.RangeMin, _config.RangeMax);
        PushSettings(*session);
        PersistSettings(*session);
        SendAddonUpdate(player);
        return session->range;
    }

    bool Manager::SetVoiceLanguage(Player* player, uint32 language, std::string& error)
    {
        if (!player)
            return false;

        Session* session = FindSessionMutable(player->GetGUID());
        if (!session)
        {
            error = "No voice session for this character.";
            return false;
        }

        if (_config.EnforceLanguage && !KnowsLanguage(player, language))
        {
            error = "You have not learned " + GetLanguageName(language) + ".";
            return false;
        }

        session->voiceLanguage = language;
        PushSettings(*session);
        PersistSettings(*session);
        SendAddonUpdate(player);
        return true;
    }

    void Manager::SetMuted(Player* player, bool muted)
    {
        if (!player)
            return;

        Session* session = FindSessionMutable(player->GetGUID());
        if (!session)
            return;

        session->muted = muted;
        PushSettings(*session);
        PersistSettings(*session);
        SendAddonUpdate(player);
    }

    std::string Manager::IssueToken(Player* player, bool tellPlayer)
    {
        if (!player || !player->GetSession())
            return {};

        Session* session = FindSessionMutable(player->GetGUID());
        if (!session)
            return {};

        session->token = GenerateUniqueToken();
        session->tokenIssuedAt = uint32(GameTime::GetGameTime().count());

        WireRecord record("TOKEN");
        record.Set("guid", uint64(session->guid.GetRawValue()));
        record.Set("token", session->token);
        record.Set("ttl", _config.TokenTtlSeconds);
        Send(record);

        SendAddonUpdate(player);

        if (tellPlayer)
        {
            ChatHandler handler(player->GetSession());
            handler.PSendSysMessage("Voice server: |cffffffff{}:{}|r", _config.ClientHost, _config.ClientPort);
            handler.PSendSysMessage("Login code: |cff00ff96{}|r (valid for {} seconds, capitals and the dash optional)",
                session->token, _config.TokenTtlSeconds);
        }

        return session->token;
    }

    /*
     * The addon asking for its own state.
     *
     * Every other push happens at a moment the module chooses, and the addon has no way
     * to know whether it caught them - after a /reload it has nothing at all. Letting it
     * ask is what makes the HUD reliable rather than lucky.
     */
    bool Manager::HandleAddonMessage(Player* player, std::string const& message)
    {
        std::string const marker = std::string(AddonPrefix) + "	";

        if (message.rfind(marker, 0) != 0)
            return false;

        std::string const body = message.substr(marker.size());

        // Ours either way from here on, so it never surfaces as a whisper.
        Session* session = player ? FindSessionMutable(player->GetGUID()) : nullptr;
        if (!session)
            return true;

        if (body.rfind("SYNC", 0) == 0)
        {
            // Arrives on the chat path, so anyone can send it as fast as they like.
            if (_clockMs - session->lastSyncMs < SyncCooldownMs && session->lastSyncMs != 0)
                return true;

            session->lastSyncMs = _clockMs;
            SendAddonUpdate(player);
        }

        return true;
    }

    // --- inbound from the voice server -------------------------------------

    void Manager::HandleInbound(WireRecord const& record)
    {
        std::string const& verb = record.Verb();

        if (verb == "OK")
        {
            LOG_INFO("module.proximityvoice", "Voice server handshake accepted (protocol {})", record.GetUInt32("ver", 0));
            return;
        }

        if (verb == "SOUNDS")
        {
            // Replaced rather than merged: the relay may have been restarted with a
            // different library, and keeping names it no longer has would offer game
            // masters sounds that silently do nothing.
            _sounds.clear();

            std::stringstream stream(record.GetString("names"));
            std::string name;

            while (std::getline(stream, name, ','))
            {
                if (!name.empty())
                    _sounds.push_back(name);
            }

            _soundDirectory = record.GetString("dir");

            // Last, so anything watching this to learn a re-scan finished sees the new
            // names and folder together rather than a half-applied library.
            ++_soundsRevision;

            LOG_INFO("module.proximityvoice", "Voice server offers {} sound(s) from '{}'.",
                _sounds.size(), _soundDirectory.empty() ? "unknown folder" : _soundDirectory);
            return;
        }

        if (verb == "DENIED")
        {
            LOG_ERROR("module.proximityvoice", "Voice server rejected the bridge: {}",
                record.GetString("reason", "no reason given"));
            return;
        }

        ObjectGuid const guid = ObjectGuid(record.GetUInt64("guid", 0));
        if (!guid)
            return;

        Session* session = FindSessionMutable(guid);
        if (!session)
            return;

        Player* player = ObjectAccessor::FindConnectedPlayer(guid);

        if (verb == "SETRANGE")
        {
            // The client asks for a range; the world server decides what it gets.
            if (player)
                SetRange(player, record.GetFloat("range", session->range));
            return;
        }

        if (verb == "SETLANG")
        {
            if (player)
            {
                std::string error;
                uint32 const language = record.GetUInt32("lang", session->voiceLanguage);
                if (!SetVoiceLanguage(player, language, error))
                {
                    ChatHandler(player->GetSession()).PSendSysMessage("|cffff4444Voice:|r {}", error);
                    // Tell the client what it actually has, so its UI stops lying.
                    PushSettings(*session);
                }
            }
            return;
        }

        if (verb == "SETMUTE")
        {
            if (player)
                SetMuted(player, record.GetBool("muted", false));
            return;
        }

        if (verb == "CSTATE")
        {
            session->clientConnected = record.GetBool("conn", false);

            // A voice client attaching during login arrives before the player can be
            // found. Queue the push rather than losing it - this was the whole bug.
            if (!player)
            {
                session->needsAddonPush = true;
                return;
            }

            {
                SendAddonUpdate(player);
                if (_config.Announce)
                {
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        session->clientConnected
                            ? "|cff00ff96Voice:|r client connected."
                            : "|cffff8800Voice:|r client disconnected.");
                }
            }
            return;
        }

        if (verb == "SPEAK")
        {
            bool const speaking = record.GetBool("on", false);
            session->speaking = speaking;

            /*
             * Built per listener, not once.
             *
             * The addon needs a name to find the speaker's nameplate, because a 3.3.5a plate
             * carries no unit token and text is the only handle. But sending the real name to
             * everyone in earshot would hand out identities over the addon channel no matter
             * what the client had been told to draw - a leak the disguise itself cannot close.
             * So each listener is told the name they are actually shown.
             */
            std::string const header = "SPK " + std::to_string(session->guid.GetRawValue()) +
                " " + (speaking ? "1" : "0") + " ";

            // The voice server names the audience, because only it knows who was
            // actually in range and understood the speaker.
            std::stringstream stream(record.GetString("to"));
            std::string item;
            while (std::getline(stream, item, ','))
            {
                if (item.empty())
                    continue;

                // The cast is load-bearing on Linux and does nothing on Windows. strtoull
                // returns unsigned long long; uint64 is unsigned long there and unsigned
                // long long here, so without it the call matches neither ObjectGuid(uint64)
                // nor the deleted ObjectGuid(uint32) exactly and is ambiguous - a build
                // error that cannot happen on the machine this is usually compiled on.
                ObjectGuid const listener = ObjectGuid(static_cast<uint64>(std::strtoull(item.c_str(), nullptr, 10)));

                Player* target = ObjectAccessor::FindConnectedPlayer(listener);
                if (!target)
                    continue;

                // Falls back to the real name when the identity module is disabled, which is
                // also what LabelFor returns in that case.
                std::string const label = player
                    ? SanctuaryIdentity::LabelFor(target, player)
                    : session->name;

                SendAddonPacket(target, header + label);
            }

            return;
        }

        if (verb == "NOTIFY")
        {
            if (player)
                ChatHandler(player->GetSession()).PSendSysMessage("|cff00ff96Voice:|r {}", record.GetString("text"));
            return;
        }
    }

    // --- persistence -------------------------------------------------------

    void Manager::LoadPersistedSettings(Player* player, Session& session)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT `speak_range`, `voice_language`, `muted` FROM `character_voice_settings` WHERE `guid` = {}",
            player->GetGUID().GetCounter());

        if (!result)
            return;

        Field* fields = result->Fetch();
        session.range = std::clamp(fields[0].Get<float>(), _config.RangeMin, _config.RangeMax);
        session.voiceLanguage = fields[1].Get<uint32>();
        session.muted = fields[2].Get<uint8>() != 0;
    }

    std::string Manager::GenerateUniqueToken() const
    {
        for (int attempt = 0; attempt < 64; ++attempt)
        {
            std::string candidate = MakeToken();

            bool taken = false;
            for (auto const& [guid, session] : _sessions)
            {
                if (session.token == candidate)
                {
                    taken = true;
                    break;
                }
            }

            if (!taken)
                return candidate;
        }

        // 64 collisions in a row means the generator is broken, not unlucky.
        LOG_ERROR("module.proximityvoice", "Could not generate a unique voice login code after 64 attempts.");
        return MakeToken();
    }

    void Manager::PersistSettings(Session const& session)
    {
        CharacterDatabase.Execute(
            "REPLACE INTO `character_voice_settings` (`guid`, `speak_range`, `voice_language`, `muted`) VALUES ({}, {}, {}, {})",
            session.guid.GetCounter(), session.range, session.voiceLanguage, session.muted ? 1 : 0);
    }

    // --- addon channel -----------------------------------------------------

    void Manager::SendAddonUpdate(Player* player)
    {
        if (!player)
            return;

        Session const* session = FindSession(player->GetGUID());
        if (!session)
            return;

        std::ostringstream payload;
        payload << "CFG"
                << " range=" << session->range
                << " min=" << _config.RangeMin
                << " max=" << _config.RangeMax
                << " lang=" << session->voiceLanguage
                << " langs=" << JoinLanguages(session->knownLanguages)
                << " muted=" << (session->muted ? 1 : 0)
                << " conn=" << (session->clientConnected ? 1 : 0)
                << " host=" << _config.ClientHost
                << " port=" << _config.ClientPort
                << " token=" << session->token;

        SendAddonPacket(player, payload.str());
    }
}
