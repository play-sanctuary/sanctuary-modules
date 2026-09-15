# Sanctuary — AzerothCore modules

Server-side source for **Sanctuary**, a roleplaying realm running on
[AzerothCore](https://github.com/azerothcore/azerothcore-wotlk) (WotLK 3.3.5a).

This repository contains **the modules Sanctuary runs** — all written for Sanctuary except
one, which is the AzerothCore project's own (see [Third-party](#third-party)). It is not a
game server on its own — every module here compiles into, and links against, an unmodified
AzerothCore world server. See [Credits and licence](#credits-and-licence).

---

## Credits

Sanctuary is built on **AzerothCore**, an open-source MMORPG server framework maintained
by the AzerothCore community.

- Upstream project: <https://github.com/azerothcore/azerothcore-wotlk>
- Website: <https://www.azerothcore.org>

AzerothCore itself descends from **TrinityCore** and, before that, **MaNGOS**. All of the
server functionality these modules hook into — the world server, the map and grid system,
the spell engine, the vehicle kit, the gossip and loot systems — is their work, not mine.
The modules in this repository are additions on top of that foundation.

None of this code is affiliated with or endorsed by Blizzard Entertainment. No Blizzard
game data, client files or artwork is contained in this repository.

---

## What is in here

Sixteen AzerothCore modules, fifteen of them written for Sanctuary. Each is a
self-contained directory in the layout AzerothCore expects — `src/` for C++, `data/sql/`
for database migrations, `conf/` for a config template, and `addon/` where the module
ships a client-side Lua addon of its own.

### Identity and presence

| Module | What it does |
| --- | --- |
| `mod-sanctuary-identity` | Players are strangers until they introduce themselves. A stranger reads as an alias — "Hooded Orc" — everywhere their name would otherwise appear. Applied server-side, in the name the client is sent, because the nameplate above a character's head is drawn by the client from its own cache and no addon can reach it. |
| `mod-sanctuary-profile` | Four short lines describing a character — appearance, condition, bearing, and one detail worth noticing — shown when you look closer at someone standing in front of you. Deliberately capped short. |
| `mod-sanctuary-emote` | `/do` — an emote for the room rather than for a person, carrying no name. It has to originate on the server: a client can only produce a line with its own name welded to the front. |
| `mod-sanctuary-faction` | Player factions: an organisation a character belongs to, with a ladder of ranks, where the rank is what grants things. Not guilds — a character can hold a guild and a faction at once — and made entirely of server rows, so a faction can be created, ranked and staffed mid-session with no restart, rebuild or client patch. |

### Law and violence

| Module | What it does |
| --- | --- |
| `mod-sanctuary-outlaw` | A flag a player puts on themselves to say *I am open to violence* — for a robbery, a duel to the death, or an execution. Not the core's FFA PvP flag, which fails silently in both directions. |
| `mod-sanctuary-lawman` | The other half of outlawry. A game master appoints a character to the office — Guard on the Alliance side, Grunt on the Horde — and while on duty they wear their city's tabard and carry a Writ of Accusation. Includes shackles. |
| `mod-sanctuary-pvploot` | A player killed in open-world PvP drops a sack where they fell: a slice of their coin and occasionally an item, openable only by their killer. The corpse itself is never touched, so the victim still releases and resurrects normally. |
| `mod-sanctuary-death` | No corpse runs. Death sends you to a spirit healer, or you wait where you fell and hope somebody raises you. The corpse is deliberately left intact — it is the target another player resurrects you *through*. |
| `mod-sanctuary-downed` | One player kneels over a fallen one for three seconds, then lifts them onto their shoulder and carries them off. Built on the core's vehicle seat, which every 3.3.5a client already understands. |

### World and objects

| Module | What it does |
| --- | --- |
| `mod-sanctuary-board` | A notice board in town. Players pin up bills — goods for sale, work wanted, a warning, an invitation. Built on gossip rather than an addon on purpose, so it works for someone who has just arrived and installed nothing. |
| `mod-sanctuary-stash` | Strongboxes: shared containers standing in the world, each opened by whoever carries its key. A thieves' cache, the watch's evidence locker, a merchant's lockup. The guild bank cannot be borrowed for this. |
| `mod-sanctuary-zidormi` | Zidormi of the bronze flight, outside the Ruins of Lordaeron, moving players between two readings of the same ground — the memory of the past, and the present day — by phase mask. |
| `mod-sanctuary-minimap` | Tracks several kinds of place on the minimap at once — mailbox, banker, trainer — where the 3.3.5a client allows only one. The client gives an addon no world position at all, so the server sends each pin as an offset in yards from the player and the addon draws it. |

### Voice and administration

| Module | What it does |
| --- | --- |
| `mod-proximity-voice` | Proximity voice chat with distance attenuation and the game's own language barrier applied to speech: speak Orcish at a Human who never learned it and they hear a person talking, without a single intelligible word. This module is the **world-server half only** — see [What is not in here](#what-is-not-in-here). |
| `mod-sanctuary-gm` | The server half of the Sanctuary game master panel. The addon is a convenience, never the authority — every request is re-checked against the sender's account security, because the addon channel is just chat and a player with no addon can send the same message. |

### Third-party

| Module | What it does |
| --- | --- |
| `mod-skip-dk-starting-area` | Lets a new Death Knight skip the Acherus starting zone. **Not written for Sanctuary**: this is the AzerothCore project's own module, [azerothcore/mod-skip-dk-starting-area](https://github.com/azerothcore/mod-skip-dk-starting-area) at commit `cd0bac4`, included unmodified because Sanctuary runs it. It keeps its own **MIT** licence — see its `LICENSE`. |

### Build order

Most modules are independent. Six have compile-time dependencies on their siblings, which
work because a static AzerothCore build puts every module's `src/` directory on the include
path:

```
mod-sanctuary-identity   ← mod-proximity-voice, mod-sanctuary-downed, mod-sanctuary-faction,
                           mod-sanctuary-lawman, mod-sanctuary-profile
mod-sanctuary-outlaw     ← mod-sanctuary-lawman
mod-sanctuary-downed     ← mod-sanctuary-lawman
mod-proximity-voice      ← mod-sanctuary-gm
```

If you take only some of these modules, take everything to the left of any module you want
along with it — `mod-sanctuary-lawman` brings identity, outlaw and downed, and
`mod-sanctuary-gm` brings proximity-voice and, through it, identity.

---

## What is not in here

Left out deliberately, and none of it is needed to build the modules:

- **The game client.** No Blizzard files of any kind.
- **The Sanctuary launcher** — a WPF desktop application, client-side.
- **The companion voice server and account service** — separate .NET programs that talk to
  the world server over a socket. `mod-proximity-voice` expects a voice server on the other
  end of its bridge; without one it logs a failed connection and does nothing else.
- **Build output, extracted DBC/map data, logs, and live configuration.** Configuration
  ships here only as `.conf.dist` templates with placeholder values.

---

## Installing

These are ordinary AzerothCore modules.

```bash
# from your azerothcore-wotlk checkout
cp -r /path/to/sanctuary-modules/modules/mod-sanctuary-identity modules/
```

Then re-run CMake **before** building — module discovery happens at configure time, not at
build time, and a module added without reconfiguring compiles into nothing with no error
anywhere:

```bash
cmake <path-to-azerothcore-wotlk> -DSCRIPTS=static -DMODULES=static
```

Confirm the module is named in `build/modules/gen_scriptloader/static/ModulesLoader.cpp`
before trusting the build.

After building, apply each module's SQL from `data/sql/` to the matching database
(`db-auth`, `db-characters`, `db-world`), and copy any `conf/*.conf.dist` into your server's
`configs/modules/` directory as `.conf`, filling in the placeholder values.

Modules that ship an addon have it under `addon/` — those go in the client's
`Interface/AddOns/`.

### A note on custom entry IDs

Several modules create custom `gameobject_template` and `item_template` rows. If you
already have custom entries of your own, check for collisions before importing — the SQL
files state the ranges they use.

---

## Credits and licence

AzerothCore is distributed under the **GNU Affero General Public License v3.0**. Its source
tree carries GPL-2.0-**or-later** headers on most files, inherited from MaNGOS and
TrinityCore, alongside AGPL-3.0 headers on files taken from TrinityCore; the "or later"
grant is what allows the combined work to be distributed under AGPL-3.0, which is the
licence upstream declares for the project as a whole.

Every module in this repository is compiled into and linked against the AzerothCore server.
They are derivative works of it, and so they are released under the same licence:

**GNU Affero General Public License v3.0** — see [LICENSE](LICENSE).

The one exception is `mod-skip-dk-starting-area`, which is the AzerothCore project's own work
under the **MIT** licence. Its `LICENSE` travels with it, and MIT permits it to be distributed
alongside AGPL code.

The practical consequence, and the reason this repository exists: the AGPL requires that if
you run modified AzerothCore code as a network service, the users of that service are
offered its source. Sanctuary runs these modules, so here they are.

See [NOTICE](NOTICE) for attribution details, and
[core-patches/](core-patches/) for the one change made to the AzerothCore tree itself.
