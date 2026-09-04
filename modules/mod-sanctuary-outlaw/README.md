# mod-sanctuary-outlaw

A flag a player puts on themselves to say *I am open to violence* — for a robbery, a duel
to the death, or an execution carried out by somebody else's hand.

`.outlaw on`, `.outlaw off`, `.outlaw` to see where you stand, or the button the addon
draws. Game masters get `.outlaw set <player> [minutes]` and `.outlaw clear <player>` for
sentencing someone who will not flag themselves.

## Why it is not just the FFA PvP flag

Because that does not work, and it fails silently in both directions.

**FFA PvP is mutual.** `Unit::_IsValidAttackTarget` computes both reactions first and
returns false for two friendly players at `Unit.cpp:10817` — long before it reaches the FFA
test at `10869`. Setting `UNIT_BYTE2_FLAG_FFA_PVP` on one player therefore does *nothing at
all* until the other player sets it too. Fine for a consensual duel; useless for robbing
someone who has not agreed to it.

**The client runs its own copy of that check.** `_IsValidAttackTarget` is, by its own
comment, "based on function `Unit::CanAttack` from 13850 client". Anything the server
permits that the client believes is friendly produces a target nobody can click, with
nothing logged anywhere. The only lever the client also sees is
`UNIT_FIELD_FACTIONTEMPLATE`.

## Two states

Being open to violence and being hunted by the town watch are not the same thing, and an
outlaw is in one of two states:

| State | Faction | The watch |
| --- | --- | --- |
| **Wanted** | 121, Booty Bay | Indifferent. Attackable by anyone, hunted by nobody. |
| **Hostile** | 14, Monster | Turns out in force. Earned by damaging a player who is not themselves an outlaw. |

**A neutral faction is attackable, and that is the whole trick.** A Human's `friendlyMask`
is `0x2` (ALLIANCE), *not* `0x1` (PLAYER) — so a template carrying only the player bit is
friendly to nobody and hostile to nobody, in both directions, for all ten playable races.
Template 121 is exactly that shape: `ourMask 0x1`, `friendlyMask 0x0`, `hostileMask 0x8`.
Neutral clears the check at `Unit.cpp:10801`, which only rejects a pair reading *actively
friendly*, and the PvP rules below it then allow the strike on their own terms. The
`hostileMask 0x8` keeps ordinary monsters as dangerous to an outlaw as to anybody else.

Gadgetzan (475), Ratchet (637), Everlook (854) and Steamwheedle (1615) are identical in
shape if you prefer the flavour.

Escalation triggers on **damage dealt**, not on entering combat: combat is entered by being
attacked as well as by attacking, and only one of those is a crime. Outlaw-on-outlaw never
escalates — both parties declared themselves open to it. Hostility is stored in
`sanctuary_outlaw`, not merely held in memory, so it cannot be laundered by logging out.

So an outlaw gets four things, each answering a different check:

| Applied | Answers |
| --- | --- |
| **The faction** | The only one the *client* sees. Wanted reads neutral and hostile reads hostile; either way the client renders an outlaw as attackable by anyone of either side. **This is the load-bearing one for PvP.** |
| `UNIT_FLAG2_IGNORE_REPUTATION` | What makes the *world* care — see below. Applied **only in the hostile state**, which is what keeps the guards out of it until blood is drawn. |
| `UNIT_BYTE2_FLAG_FFA_PVP` | The visual, and outlaw-vs-outlaw through the stock mutual path. |
| The ordinary PvP flag | `target->IsPvP()` at `Unit.cpp:10866` — what lets an ordinary player strike an outlaw. |
| `UNIT_BYTE2_FLAG_UNK1` | The either-side clause at `Unit.cpp:10872` — what lets an outlaw strike a victim who is not flagged. |

### Why the faction alone did not make guards react — and why that is now useful

It looks as though it should, and it silently does not. `Unit::GetFactionReactionTo`
answers a **creature's** reaction to a **player** from that player's *reputation* and
returns at `Unit.cpp:7278`; the faction template comparison at `:7285` is never reached.
Every city guard's faction has a real reputation entry, so an Exalted-with-Stormwind
outlaw still read as `REP_FRIENDLY` to a Stormwind guard however monstrous their faction
was, and `Creature::_IsTargetAcceptable` rejected them at `Creature.cpp:2606`.

`UNIT_FLAG2_IGNORE_REPUTATION` is the core's own switch for skipping that branch, which
makes it the natural lever for escalation. A **wanted** outlaw keeps their reputation, so
the watch goes on seeing a citizen in good standing and leaves them alone whatever their
faction says. A **hostile** one has the flag set, the faction is compared at last, and both
directions come out hostile — which matters, because `Unit.cpp:10801` rejects the pair if
*either* side reads friendly:

| | guard → outlaw | outlaw → guard |
| --- | --- | --- |
| Stormwind / Ironforge / Exodar | `hostileMask 0xC & ourMask 0x8` | `hostileMask 0x1 & ourMask 0x3` |
| Undercity / Orgrimmar / Silvermoon | `hostileMask 0xA & ourMask 0x8` | `hostileMask 0x1 & ourMask 0x5` |

City guards carry the *player* bit in `ourMask`, which is what makes the second column
work at all.

> `PLAYER_FLAGS_CONTESTED_PVP` is the obvious fix and would not have worked. It only moves
> creatures whose faction carries `FACTION_TEMPLATE_FLAG_ATTACK_PVP_ACTIVE_PLAYERS`, and
> **no city guard has it** — of the guards checked, only Sentinels do.

Plus `UnitScript::IfNormalReaction`, which stops two players of one side reading as friends
while either is an outlaw. It is the only hook that can bend `_IsValidAttackTarget`, which
has none of its own, and it is reachable for player-vs-player only because every playable
race's faction has `reputationListID = -1` so the reputation branch above it never returns.

Each is separately switchable in the config, because **which of them the 3.3.5a client
actually honours is a question only a live client can answer.** See "Still to be proven".

## Holding the flags on

The FFA byte is torn down constantly — by any zone change, resurrect, teleport, login, GM
toggle, spectator toggle, quest, taxi arrival or stat reset, and on *any* area change at
all, since `UpdateArea` passes `reset=false` with no PvP timer running. Chasing those one
at a time is hopeless.

What makes it tractable: **every one of the six removal sites is immediately followed by
`OnPlayerFfaPvpStateUpdate(player, false)`** — `Player.cpp:2263`, `2757`, `15784`,
`PlayerMisc.cpp:395`, `PlayerUpdates.cpp:1496`, `MiscHandler.cpp:745`. That single hook is
a complete interception point, so the module re-sets the bit there and nowhere else. It
uses `SetByteFlag` directly, never `UpdateFFAPvPState`, because the core is part way
through its own teardown at that moment.

> Hooking `OnPlayerUpdateArea` is the obvious answer and it is wrong: it fires at
> `PlayerUpdates.cpp:1228`, *before* the recalculation at `1235-1236` overwrites whatever
> it set.

The faction is the opposite — it survives area and zone churn, and is instead reset by
login, the GM toggle, `RestoreFaction` and instance scripts. Rather than chase each, it is
compared against the expected value once a second and put back.

Nothing is persisted by the core (`UNIT_FIELD_BYTES_2` is not a character column and the
faction is recomputed from race on login), so `sanctuary_outlaw` is the durable copy and
everything is reapplied from it at login.

## What it costs to be an outlaw

Worth saying out loud, because each of these reads as a bug if it is a surprise.

- **Guards and friendly creatures will attack you**, and vendors, flight masters and every
  other friendly NPC will refuse you — the city NPCs of *both* sides, not only the guards.
  An outlaw cannot walk through a town. That is `GuardsAttack`, and switching it off leaves
  outlawry purely a matter between players.
- **You cannot be healed, buffed or resurrected by ordinary players.** `Unit.cpp:10953`
  refuses assistance to an FFA target from a non-FFA source, and the faction refuses it
  outright. Accomplices have to be outlaws too.
- **Group and raid members can never fight each other**, flagged or not — `Unit.cpp:7167`
  returns friendly before any FFA check is reached. Leave the group first.
- **True sanctuary areas still block everything** (`Unit.cpp:10857`, keyed on the sanctuary
  byte). Capital cities are *not* sanctuary areas, so a robbery in Stormwind works;
  Dalaran and Shattrath do not.
- **Spoils are currently switched off.** `mod-sanctuary-pvploot` has `Enable = 0`, so an
  outlaw kill yields nothing. Turning it on needs no code change — that module has no
  faction test at all — but it also means accidental same-faction deaths would loot, with
  only `MinLevel`, the repeat cooldown and the same-account check as brakes.
- No honour is awarded: `Player::RewardHonor` already gates same-team kills on
  `IsFFAPvPRealm()`, which stays false.

## Standing down

`.outlaw off` does not take effect immediately, and is refused outright while you are in
PvP combat. Without that the flag is an escape button — strike, drop it, and stand there
untouchable while the person you hit is still swinging. The delay is
`SanctuaryOutlaw.ReleaseSeconds`, default 60.

A pending stand-down is deliberately **cancelled** by logging out, not honoured: otherwise
logging out during the countdown and back in after it would be exactly the escape the
countdown exists to prevent.

## Still to be proven

The one thing no amount of reading settles is whether the 3.3.5a client honours
`UNIT_BYTE2_FLAG_UNK1`. Test it with two same-faction characters:

1. Flag one. Can the **unflagged** one attack them? Expected yes — that is the faction
   override, and it is the basis of everything here.
2. Can the **outlaw** strike the unflagged one first? This is the unknown. The client may
   require the target to be PvP-flagged, exactly as it does for unflagged players of the
   opposite side.

If (2) fails, do not force it. The fix is to make the victim flag first — a "stand and
deliver" action that PvP-flags them for a minute and warns them — which puts both ends on
the stock mutual path that the client is known to honour.

## For other modules

```cpp
#include "SanctuaryOutlaw.h"

if (SanctuaryOutlaw::IsOutlaw(player))
    ...
```

Static builds put every module's source directory on the include path, so this needs no
CMake change — the same way `mod-proximity-voice` includes `SanctuaryIdentity.h`.

**Never put a player's name in an addon payload.** This module keys on nothing but the
viewer's own state for that reason; a broadcast naming an outlaw would be the same
enumeration oracle `mod-sanctuary-identity` already removed once.
