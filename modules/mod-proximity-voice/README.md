# mod-proximity-voice

Proximity voice chat for AzerothCore 3.3.5a, with distance attenuation, an
adjustable speaking distance, and the game's own language barrier applied to
speech.

Stand next to someone and you hear them clearly. Walk away and they fade out.
Speak Orcish at a Human who never learned it and they hear a person talking —
volume, rhythm, pauses, emphasis — without a single intelligible word, exactly
as they would see garbled text in `/say`.

---

## Why there are three pieces

The 3.3.5a client cannot capture or play microphone audio, and addons cannot
touch audio hardware or the network. So voice needs a companion desktop app.

Everything spatial is decided by the **world server**, which is the only party
that knows where anyone actually is:

```
  worldserver                    SanctuaryVoice.Server              SanctuaryVoice.Client
  + mod-proximity-voice   TCP    routing + language                 mic, speakers,
  positions, languages  ───────▶ decisions                ◀────────▶ device pickers
  range limits          ◀─────── settings requests    TCP control    push to talk
                                                      UDP audio
        │ addon channel
        ▼
  WoW client + SanctuaryVoice addon
  (who is speaking, your settings)
```

The voice client never learns where anybody is, and never receives speech in a
language its character has not learned. Both are routing decisions made server
side, so a modified client gains nothing from ignoring them.

---

## Installing

### 1. The module

```bash
cd azerothcore-wotlk/modules
# this directory is already in place
```

Re-run CMake so the module is picked up, then rebuild:

```bash
cmake . && cmake --build . --target worldserver --config RelWithDebInfo
```

The module's SQL is applied automatically on the next worldserver start
(`Updates.EnableDatabases` must include the auth and character databases, which
is the default). It adds:

- `character_voice_settings` in the characters database — per-character speaking
  distance, language and mute state
- RBAC permission `100001` in the auth database, linked under *Role: Player
  Commands*, so ordinary players can use `.voice`

### 2. Configuration

Copy `configs/modules/proximity_voice.conf.dist` to `proximity_voice.conf` and
set at minimum:

```ini
ProximityVoice.Enable = 1
ProximityVoice.Bridge.Secret = "<a long random string>"
ProximityVoice.Client.Host = "voice.yourrealm.example"   # what players connect to
```

The secret must match `BridgeSecret` in the voice server's `voiceserver.json`.

### 3. The voice server

Runs alongside worldserver. It needs no database and holds no state of its own.

```bash
SanctuaryVoice.Server.exe          # reads voiceserver.json beside it
```

Ports: **7788/TCP** for the world server bridge (keep on loopback unless the
voice server is on another machine — this link carries every player position on
the realm), **7789/TCP+UDP** for players.

### 4. The player's side

Players need two things:

- `SanctuaryVoice.Client.exe` — the desktop app
- the `SanctuaryVoice` addon in `Interface/AddOns/` (optional, but it shows who
  is talking and hands you a copyable login code)

In game, `.voice token` prints a one-time login code and the server address —
something like `H48J-C72Z`. Type both into the client and press Connect.

The code is eight base32 characters using an alphabet with `I`, `L`, `O` and `U`
removed, so nothing in it can be misread. Capitals, the dash and stray spaces are
all ignored on the way in. It is spent on first use and expires after
`ProximityVoice.Token.TTLSeconds` (default five minutes); a wrong guess costs the
attacker a second per try against a keyspace of a trillion.

---

## How it behaves

### Distance

A speaker chooses how far their voice carries, between
`ProximityVoice.Range.Min` and `.Max`. The world server clamps whatever the
client asks for, so the slider cannot be pushed past the realm's limit by a
patched client.

Within `AttenuationReference` yards (default 8) a speaker is at full volume.
Past that, volume follows an inverse-distance rolloff, then fades smoothly to
silence over the last fifth of the range so nobody pops in or out. For scale:
`/say` reaches 25 yards and `/yell` reaches 300.

Voices are also placed in stereo relative to which way the listener is facing,
easing back to centre as a speaker gets close.

Beyond the speaking distance the server sends nothing at all — the packets are
never generated, so distance is not something a client can opt out of.

### The language barrier

A character speaks one language at a time, chosen in the client or with
`.voice lang <name>`, and can only pick languages they have actually learned —
the module checks the same skill the chat handler does, including Comprehend
Language effects.

When a listener does not know that language, the server **does not send them the
speech**. It sends four loudness bytes per 20 ms frame, and the listener's client
synthesises a voice from them: same rhythm, same emphasis, same volume, a
different speaker every time, and a distinct accent per language. Nothing
intelligible crosses the network, so there is no client setting or patch that
can turn it back into words.

Game masters hear everything and are heard by everyone
(`ProximityVoice.Language.GameMastersUnderstandAll`).

### Who can hear whom

Speech only travels between players on the same map, in the same instance, and
in overlapping phases. Dead players are silent by default; when
`ProximityVoice.Dead.CanSpeak` is on, `ProximityVoice.Dead.HearOnlyDead` keeps
their voices among the dead.

---

## Commands

| Command | Who | What |
| --- | --- | --- |
| `.voice` / `.voice status` | players | current distance, language, mute and client state |
| `.voice range <yards>` | players | change speaking distance (clamped to the realm's window) |
| `.voice lang <name>` | players | change the language you speak |
| `.voice langs` | players | list the languages you have learned |
| `.voice token` | players | issue a fresh login code |
| `.voice mute` / `.voice unmute` | players | mute the microphone from in game |
| `.voice info` | GM | bridge state, tracked sessions, realm settings |

The addon adds `/svoice` for the same things plus a copyable login code.

---

## Configuration reference

Everything the world server owns is in `proximity_voice.conf`. Everything about
how the mix sounds is in `voiceserver.json`:

| Setting | Default | Meaning |
| --- | --- | --- |
| `AttenuationReference` | 8 | Radius of the full-volume bubble, in yards |
| `AttenuationRolloff` | 1.0 | How sharply volume falls past the bubble |
| `MinimumAudibleGain` | 0.012 | Below this a listener is dropped from the route |
| `PanStrength` | 0.75 | Stereo placement strength; 0 disables it |
| `TalkTimeoutMs` | 300 | Silence after which someone stops showing as speaking |
| `MaxFramesPerSecondPerClient` | 60 | Uplink throttle; audio needs 50 |

---

## Operational notes

- **Bandwidth.** Opus at 24 kbit/s, 50 frames a second. A speaker heard by *n*
  people costs *n* × 24 kbit/s downstream from the voice server. A listener who
  does not understand the language costs almost nothing — four bytes a frame.
- **CPU.** The voice server never encodes or decodes audio. It copies packets
  and does distance arithmetic.
- **Restarts.** If worldserver drops, the voice server discards all state
  immediately: with no idea where anyone is, nobody may be audible. Everything
  is re-published when the bridge reconnects.
- **UDP.** A session id issued over the authenticated TCP channel is the
  credential on the UDP path, and is pinned to the first endpoint it is used
  from. Clients behind symmetric NAT are fine; they only ever send outbound
  first.

---

## Layout

```
modules/mod-proximity-voice/
  src/
    ProximityVoice.{h,cpp}     session tracking, config, persistence
    PVBridge.{h,cpp}           asio link to the voice server
    PVWire.{h,cpp}             line protocol codec
    PVLanguages.cpp            what a character may speak and understand
    PVScripts.cpp              world and player hooks
    PVCommands.cpp             .voice commands
  conf/proximity_voice.conf.dist
  data/sql/                    settings table + RBAC permission
  addon/SanctuaryVoice/        in-game addon
```

The C# side lives in `SanctuaryVoice/` next to the core checkout.
`PVWire.cpp` and `SanctuaryVoice.Shared/Wire.cs` implement the same format and
must be changed together.
