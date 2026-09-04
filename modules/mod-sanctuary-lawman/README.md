# mod-sanctuary-lawman

The other half of outlawry. A game master appoints a character to the office — **Guard** on
the Alliance side, **Grunt** on the Horde — and while on duty they wear their city's tabard
and carry a **Writ of Accusation**.

```
.lawman set <player>     appoint      (game master)
.lawman remove <player>  dismiss      (game master)
.lawman on / off         take up or leave your post
.lawman                  where you stand
```

**The module hands out nothing.** The office is paperwork; the four items are given out by
game masters with `.additem`, which is what lets a guard captain issue them, a corrupt one
sell them, and a criminal end up holding a pair.

| Item | Entry | Icon (Blizzard's own) |
| --- | --- | --- |
| Writ of Accusation | **990000** | `INV_Scroll_04` |
| Writ of Pardon | **990003** | `INV_Scroll_03` |
| Iron Shackles | **990001** | `INV_Belt_18` — a plain iron chain |
| Iron Shackle Key | **990002** | `INV_Misc_Key_03` — the stock Shackle Key |

### These need the client patch

The 3.3.5a client reads an item's class, subclass and **display** from its own `Item.dbc`
and takes only the name, tooltip and stats from the server. An entry it has never heard of
renders as a question mark, and nothing the server sends can change that.

`tools/assets/make-patch.py` builds **`patch-enUS-4.MPQ`**, which adds rows to three
tables and nothing else:

| Table | Rows | For |
| --- | --- | --- |
| `Item.dbc` | 990000–990003 | the four items above |
| `Spell.dbc` | 81000 | the chain a shackled prisoner wears, named `Iron Shackles` |
| `SpellIcon.dbc` | 4400 | so that debuff wears the item's own iron chain |

**No artwork ships with it** — every row points at art the client already has, so the
icons and the chain are Blizzard's own: no new texture to get wrong. The archive is
compressed, which matters more than it sounds: `Spell.dbc` is 46MB flat and 2.6MB
deflated. It goes in `<client>/Data/enUS/` — the locale folder, because the file it must
win against is `patch-enUS-3.MPQ` and the locale archives load last.

Without it the items still work and still read correctly; they just have no icon, and a
shackled prisoner wears no chain.

The spell row has a **server half too** — `spell_dbc`, added by this module's world SQL.
AzerothCore overlays that table on top of `Spell.dbc` and grows its index table to fit
ids the DBC never had, so a custom spell costs one INSERT rather than a rebuilt server
DBC. Both halves must be installed: the client decides what the aura is called and what
it looks like, the server decides what it does. The module logs which spell it found at
startup so a missing half says so instead of quietly drawing nothing.

### Borrowing retail entries was tried first, and is a trap

Four attempts, four different inherited properties, none of them visible until it bit:

| What was inherited | How it looked |
| --- | --- |
| `ITEM_FLAG_DEPRECATED` | the item never appeared in the bag at all |
| `spellcharges_1 = -1` | the item was consumed on the first use |
| subclass *Item Enhancement* | the client asked for a weapon to enchant and never ran the script |
| the icon | of every chain-shaped icon, all belonged to weapon-chain consumables carrying the three problems above |

Repurposing means inheriting **everything** not explicitly overwritten. Owning the rows
ends the whole category, at the cost of a patch the players have to install.

> The builder refuses to run against a client that already has the patch installed. It reads
> `Item.dbc` out of the client's own archives, and if the previous patch is one of them it
> would append four rows to the four already there — duplicates that are not obvious
> afterwards. Remove `Data/enUS/patch-enUS-4.MPQ` before rebuilding.

### Why they are not custom entries

They were, and they rendered as question marks. The 3.3.5a client reads an item's
`DisplayInfoID` from its **own `Item.dbc`**, not from the query response — and that file
stops at entry 56806, so nothing in a custom range has any display and no server setting
can give it one. The name and tooltip still arrive from the server, which is exactly why
the items worked and read correctly while showing no icon.

**Repurposing means inheriting.** Everything about the old row that is not explicitly
overwritten survives — and these four all carried `ITEM_FLAG_DEPRECATED` (0x10), which the
client honours by not rendering the item at all. That is worth knowing before taking a
fifth entry: clear `Flags`.

There are no free ids either: `item_template` already defines every entry `Item.dbc`
contains. So all four ride on **deprecated** rows — dead content nothing spawns, sells,
drops or asks for — keeping the client's display while the server supplies the rest.
`class` and `subclass` are deliberately left alone, since the core rewrites them to match
`Item.dbc` anyway. Three are quest-class, which does not restrict trading: `Item::CanBeTraded`
checks soulbinding, never class, and all four are unbound.

## What the writ is for

Outlawry only ever covered people who declared themselves. A criminal could switch the mode
off and become untouchable, whatever they had just done and whoever had hold of them.

Using the writ on somebody **makes them an outlaw** for a fixed sentence — 30 minutes by
default. Because the sentence carries an expiry, `mod-sanctuary-outlaw` refuses to let them
stand down from it early; that refusal already existed for game-master sentences and the
writ simply uses it. The accusation is the whole feature. Everything else is dressing.

## Why a lawman needs no combat flags at all

This is the part worth understanding before changing anything, because it looks like an
omission and is not.

**Anyone can already attack an outlaw.** An outlaw carries faction 14, so every client
renders them hostile, and `target->IsPvP()` at `Unit.cpp:10866` passes them server-side.
A lawman therefore needs nothing whatsoever to be able to fight criminals.

So the entire "doesn't make them hostile to guards" requirement is met by *withholding* two
things `mod-sanctuary-outlaw` applies: the faction override, and
`UNIT_FLAG2_IGNORE_REPUTATION`. A lawman on their native faction with no ignore-reputation
flag reads as `REP_FRIENDLY` to a city guard through the reputation branch at
`Unit.cpp:7266`, exactly as any ordinary player does. Add either of them and the guards of
their own city will turn on them, which is precisely the bug this module exists not to have.

## The writs, mechanically

`item_template` **990000** (accusation) and **990003** (pardon). Neither is soulbound and
neither is handed back at login: they are issued **once**, with the irons and the key, as
one kit. Everything the office carries is a physical thing that can be lost, given away or
taken off a body — which is the point, and which is also why the row remembers having
issued it. `.lawman kit <player>` is a game master replacing a genuinely lost set.

The accusation writ's on-use spell, **38067**, is never cast. `ItemScript::OnUse` returns
`true`, suppressing it, so the spell's name and visual never appear and the item's own name
and tooltip are all the player sees.

> **Suppressing the cast also skips every core check.** `OnItemUse` fires in
> `HandleUseItemOpcode` *before* `CastItemUseSpell`, so returning `true` means
> `Spell::CheckCast` never runs — no range, no line of sight, no target validation.
> The module does all of it by hand. Without that a modified client could serve a writ
> across the zone.
>
> Every refusal also calls `SendEquipError`, or the client leaves the item greyed out and
> the player reads a refusal as a cooldown.

A genuinely custom *spell* was considered and is impossible: the client builds its
spellbook from its own `Spell.dbc` and never emits a cast for an id it does not know, so a
new spell would need a patched MPQ. An item needs none — `item_template` reaches the client
at runtime, so the name, icon and tooltip are ours.

### When an item does not appear

Worth knowing, because three unrelated causes look identical from in game and none of them
says anything on its own:

- **It was never issued.** The kit is issued once, so anyone appointed before a piece of it
  existed had nothing handed over. This is now covered — login issues the kit if the row
  says it never arrived — but it is the first thing to check on an old character.
- **The bag was full.** `Player::AddItem` returns false and says nothing at all, which is
  indistinguishable from the item not existing. The module now says so and logs it.
- **The row is not loaded.** The module writes one line per item at startup naming the
  entry it found, or an error naming the one it did not, so this is a fact in the log
  rather than a guess.

`maxcount` is 0 on every one of them. A limit of one is wrong for something meant to change
hands, and it makes adding a second copy fail with a bare inventory error.

## The tabard

Stormwind **45574** for Alliance, Orgrimmar **45581** for Horde, chosen by
`GetTeamId(true)` — the *racial* side, so a faction override cannot flip a lawman's title
or tabard mid-sentence.

`Player::SetVisibleItemSlot` takes an `Item*` and so cannot express a tabard the player does
not own; the public appearance field is written directly instead. The player never receives
the item and it costs them nothing.

Four places in the core will fight it, all in `PlayerStorage.cpp` — `VisualizeItem:2975`,
`RemoveItem:3040`, `DestroyItem:3166`, and `LoadFromDB:5131`, which clears all nineteen
slots on every login. `OnPlayerAfterSetVisibleItemSlot` catches the first three,
`OnPlayerLogin` the fourth, and a once-a-second comparison catches anything else — the same
tactic `mod-sanctuary-outlaw` settled on for faction, and for the same reason.

**A tabard rather than anything written by the name**: it carries no text, so it cannot leak
the identity `mod-sanctuary-identity` otherwise hides. For the same reason both messages an
accusation produces are built per recipient through `SanctuaryIdentity::LabelFor`, never
from a raw name.

## Pardons

A pardon is the **Writ of Pardon** and nothing else. There is no command for it: an
on-duty lawman uses the writ on an outlaw within range, which calls
`SanctuaryOutlaw::Revoke(player, why)`. The reason parameter was added for this, so a
pardoned player is not told a game master released them when it was the watch.

`.lawman pardon` existed first and has been removed. Letting somebody go should cost a
piece of paper somebody had to be given, and be a thing done in the world where it can
be seen, rather than a line typed from across the map.

## For other modules

```cpp
#include "SanctuaryLawman.h"

if (SanctuaryLawman::IsOnDuty(player))
    ...
```

Static builds put every module's source directory on the include path, so this needs no
CMake change — the same way this module includes `SanctuaryOutlaw.h` and
`SanctuaryIdentity.h`.

## Iron Shackles

A second item, and a very different one: **nothing about it checks for the office.** The
shackles and the Iron Shackle Key are tradeable, and possession is the whole authority,
because a criminal obtaining a pair from a corrupt guard is the reason they exist. Every
use is written to the log instead.

Used on a player, they take a **five second cast at five yards**, disarm the target, and
tether them within ten yards of whoever put them on. A key undoes them — any key, in
anyone's hands.

### Why the cast is on the user, not the prisoner

Aiming it does not work, and the way it fails is worth recording. Every candidate spell
with a usable range carries an **empty `Targets` mask** in the client's own DBC, and the
ones the server would accept are *positive* spells — so target selection runs
`IsValidAssistTarget`, which refuses a hostile unit. An outlaw carries faction 14 and is
hostile to everybody, so an aimed shackle failed with **"Invalid Target" on exactly the
person it exists for**, while working perfectly on an innocent.

Cast on the user there is nothing to validate on either side of the wire, and the prisoner
is read from the caster's **selection** instead. Range and line of sight are then the
module's job — checked when the item is used and **again when the bar fills**, since five
seconds is long enough to walk apart, break line of sight, or pick a different target.

The cast bar shows the base spell's **name**, which the client reads from its own files —
no server setting can change that text. So the spell is chosen for its name: **62646**, which
is called "Shackle". Everything else about it is wrong for us and is corrected at load
through `OnLoadSpellCustomAttr`: its target becomes the caster, its effect becomes a dummy
so nothing lands on whoever used it, and its cast time becomes five seconds.

Borrowed spells also bring their **descriptions** with them, and that description is what an
item's tooltip prints as flavour text. The first attempt used 38067, so every item read out
the Hunter's Mark wording. The suppressed-cast items now use **61410 "Bind"** — instant,
self-targeted, inert, and with an empty description, so the tooltip shows only what we
wrote.

### The cast is real, and that is the trick

The writ *suppresses* its spell by returning `true` from `OnUse`. The shackles return
**`false`**, so `Player::CastItemUseSpell` builds a genuine `Spell`. That buys the cast
bar, interruption on movement, and range, line of sight and target validation from the core
for nothing — including a **second** `CheckCast` when the bar fills (`Spell.cpp:3851`), so
walking out of range during the five seconds fails the cast. It is the opposite trade from
the writ, and the reason the two items are written so differently.

Completion is caught with `AllSpellScript::OnSpellCast`, the last line of `Spell::_cast`.
`PlayerScript::OnPlayerSpellCast` is the tempting one and is wrong — it fires *before* that
second check, so it would also fire for a cast about to be refused for range.

**Spell 10617 "Release Rageclaw"** was chosen by reading the client's own `Spell.dbc`, not
for its name, which nobody ever sees. Range index 12 is 0–5 yards **in the client's copy** —
and the client gates range from its own file before it will even send the packet, so a base
spell with the wrong range desyncs and no server setting can fix it. Its target type is 25
(`TARGET_UNIT_TARGET_ANY`, the one that skips the friend-or-foe check), its effect is a
dummy, and its interrupt flags already include movement. Only the cast time is wrong at 10
seconds, and `GlobalScript::OnLoadSpellCustomAttr` rewrites it to five at load — a hook that
hands over a *non-const* `SpellInfo*`, so no SQL is involved. Not `spell_dbc`: that is loaded
with `SELECT *` and replaces the whole row, so a partial insert would zero every column it
omitted.

The disarm is applied with `Unit::AddAura`, not `CastSpell`, and that is load-bearing: the
aura spell carries `MECHANIC_DISARM`, and `SpellInfo::CheckTarget` refuses to disarm a
target with an **empty main hand** — exactly the unarmed prisoner this is most likely used
on. `AddAura` skips `CheckCast`, which is safe because the real cast already validated
everything. Set `SetMaxDuration` before `SetDuration`, or the client draws a bar longer than
its own maximum.

### The tether

Checked every 200ms, not the second the tabard re-assert uses — a running player covers
about seven yards a second, and a one second check would let them get most of the way to
the next zone before the chain noticed.

Two thresholds, not one: rooted past ten yards, released again inside nine. A single
boundary flaps as somebody shuffles across it, and every flap is a rubber-band on their
screen.

> **Every way out goes through `Release()`, because every one of them has to clear the
> root.** A prisoner left rooted cannot walk, cannot be freed by a key that no longer knows
> about them, and can do nothing but find a game master. The ways out are: the key, the
> duration, the captor logging out, dying, or leaving the map, and the prisoner logging out.

Nothing is persisted. A shackle dies when either party leaves the world, which is a
decision rather than an oversight — it means there is no way to end up with a prisoner
rooted to a row that outlived the people in it.

### Issuing

A lawman is given one set when appointed, and **only once**. The writ is soulbound so
re-granting it at every login costs nothing; these are tradeable, so re-granting would be a
farm — give them away, relog, get another. `kit_issued` on the row remembers, and
`.lawman kit <player>` is a game master replacing a set that was genuinely lost.

## The Writ of Pardon

`item_template` **990003**, and deliberately **not** soulbound and **not** re-granted at
login. It is issued once with the irons and the key, so it is a physical thing that can be
carried, lost, handed to somebody else or taken off a body.

Only the Writ of Accusation reappears on its own — that one is soulbound office paper, so
handing it back every login costs nothing.

Used on an outlaw within range it lifts the accusation, so letting somebody go is something
done in the world rather than typed. It calls the same
`SanctuaryOutlaw::Revoke(player, why)`, with the reason that tells the pardoned player it
was the watch and not a game master. It is the only way to grant a pardon - the command
that used to do it has been removed.

## When somebody is flagged but cannot be attacked

There are several unrelated causes and they look identical from in game. `.outlaw check
<player>` prints the whole chain — the flags, the faction, and then the three things that
silently beat all of them:

- **You are in a group or raid together.** `Unit.cpp:7167` returns friendly before any FFA
  or faction test is reached. No flag can override it.
- **Either of you is in a sanctuary area.** `Unit.cpp:10857` blocks player-versus-player
  outright. Capital cities are not sanctuaries; Dalaran and Shattrath are.
- **Either of you is in game master mode.**

It finishes with the core's own verdict in both directions, so there is nothing left to
guess at.

## Two things the shackles do that are not obvious

**The chain is a triggered cast.** `SendPlaySpellVisual` was tried first and drew nothing:
it takes a **SpellVisualKit** index, and it was being handed a `SpellVisual` id out of
`Spell.dbc` — a different table, so the client had nothing to look up. A triggered cast of
29909 "Elven Manacles" plays the whole animation and skips `CheckCast`, which is what makes
it safe to aim at a prisoner at all: an outlaw is hostile to everybody, and an ordinary cast
is refused for exactly the person this is used on. That spell's own effect is a dummy, so
nothing but the chain is borrowed from it.

**The cast can actually be interrupted.** The borrowed base spell ships with
`InterruptFlags = 0`, so the core never cancelled it — walking stopped the bar on the
client, which predicts a cancel locally, while the server carried on and applied the
shackle five seconds later regardless. `SPELL_INTERRUPT_FLAG_MOVEMENT` and
`ABORT_ON_DMG` are set at load, and a per-tick watchdog cancels it if the prisoner leaves
reach or line of sight, since a self-cast means the core validates nothing about the far
end of the chain for the whole five seconds.

**Being shackled ends outlawry.** Someone in irons is in custody, which is the opposite of
being at large — and it is also what makes them movable. An outlaw carries a hostile
faction, and hauling a hostile player around fights every check the core makes about who
may touch whom.

> The base spell is borrowed for its **name** and nothing else, so everything about it is
> corrected at load: its target becomes the caster, its effect becomes a dummy, its cast
> time becomes five seconds, and its **channel flags are cleared**. That last one is not
> cosmetic — a channel takes its length from a duration rather than a cast time, so with
> the effect turned inert there was nothing to end it and the caster was left stuck in a
> casting animation that never finished.
