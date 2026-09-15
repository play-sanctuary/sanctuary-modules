# mod-sanctuary-faction

Player factions: an organisation a character belongs to, with a ladder of ranks, where the
rank is what grants things.

Not guilds — a character may hold a guild *and* a faction, which is the whole reason this
is not built on the guild tables. Not `Faction.dbc` either: nothing here decides who may
attack whom.

## The one thing to understand

Everything in this module is **server rows**. That is not an implementation detail, it is
the feature: a faction can be created mid-session with `.faction create`, given ranks, and
have people put in it, with no restart, no rebuild, and no client patch. There is no id
budget and no ceiling, because faction and rank names are strings this module owns rather
than client DBC text.

The one thing that is *not* free is a spell, because a spell has to exist in the client.
Draw rank rewards from the ~50,000 stock spells and it stays free; invent one and it is a
patch and a redownload.

## Ranks

**Rank 0 is the lowest and higher is senior** — the opposite of the core's guild ranks.
Deliberate, and explained in `2026_09_08_01_sanctuary_faction_rank.sql`: grants are
cumulative upward, so "everything rank 2 and above may do" has to compile to `rank_id >= 2`
or every future reader inverts it in their head, and getting it backwards once hands a
recruit the captain's powers.

Each rank carries one bitmask, `permissions` — what you may do *to the faction*: invite,
kick, promote, demote. That is the delegation: a faction runs its own roster while a game
master keeps the ladder. `PERM_EDIT` and `PERM_DISBAND` exist in the mask and are
deliberately unwired, because writing the ladder is a balance decision.

An earlier draft had a second mask, `capabilities`, letting a rank grant powers the other
modules check for — shackling without irons, opening any strongbox. It was cut. What a
faction gives its members is a **spell**: one kind of reward is easier to balance and to
explain than two, and a spell in the spellbook is something the player can see, whereas a
capability is an invisible boolean that quietly changes what a door does.

## The spellbook trap

`Player::learnSpell` works live — no trainer, no relog, and it persists by itself. But the
client files a known spell into a spellbook tab through its `SkillLineAbility` row, and
`Player::addSpell` only grants the underlying skill line when `AcquireMethod` is
`LEARNED_ON_SKILL_LEARN` (Player.cpp:3378). A spell tied to a skill line the character does
not have can end up genuinely known and castable from a macro, but never *drawn*.

It is a property of the individual spell. Check each one with `.learn <id>` on a
**non-game-master** character of the wrong class — that runs the identical code path this
module uses.

## Why `sanctuary_faction_granted` exists

Because `learnSpell` is permanent and the core does not know the grant was conditional.
Revoke reads the receipt table, **never** the rank table. Deriving the revoke set from the
ladder is wrong in two directions and both are silent: it unlearns the wrong spells after
any rank edit, and it strips spells the player earned elsewhere. A spell the player already
knew when the faction offered it records no receipt, so leaving never takes it.

## Commands

Game master (`RBAC_PERM_COMMAND_MODIFY_FACTION`):

    .faction create <name>
    .faction disband <name>
    .faction rank <faction id> <rank> <permissions> <name>
    .faction spell <add|remove> <faction id> <rank> <spell id>
    .faction set <faction id> <rank>        on the selected player; 0 removes them
    .faction list
    .faction reload

Player:

    .faction            what you belong to
    .faction roster
    .faction leave


Delegated to a rank that holds the bit (`RBAC 100007`, granted to role 199 so everybody
holds it — the *rank*, not the account, is what decides):

    .faction invite         asks the selected player; they answer with .faction accept
    .faction accept
    .faction kick    [name|guid]
    .faction promote [name|guid]
    .faction demote  [name|guid]

with no argument meaning the selected player. Three rules guard them, and the last two are
what stop the system being pointless: you may only touch somebody in your own faction; you
may not touch anybody of your own rank or higher; and you may not promote anybody **to**
your own rank or higher — otherwise promotion is self-elevation with one extra step.

## The addon

`addon/SanctuaryFaction/` ships a minimap button (next along the Sanctuary arc, at −190.6°)
and a window: your faction and rank, what your rank has taught you, the roster, and buttons
for the four delegated actions. `/faction` opens it too.

It decides nothing. Every button sends the same `.faction` command over AzerothCore's addon
command channel, so a player who edits the Lua to un-grey a button gets the same refusal
they would have got from typing it. Greying out is politeness; the rules are on the server.

**Commands on that channel carry no leading dot.**
`AddonChannelCommandHandler::ParseCommands` hands `str.substr(17)` straight to
`_ParseCommands` without stripping one, so `.faction create` is looked up as a command
named `.faction` and fails. Every button in this window was silently dead for exactly that
reason — send `faction create`, the way every other Sanctuary addon already does.

Three details worth knowing:

- Roster rows carry a **guid** as well as a label, and the buttons act on the guid. A label
  is an alias for anybody you have not been introduced to, and an alias resolves to no
  character — so acting by name would work on friends and fail on strangers, in your own
  faction, which is where it would be least expected. A guid gives the disguise away to
  nobody.
- Members who are **not logged in are counted, not named**. `SanctuaryIdentity::LabelFor`
  needs both people present to choose between a real name and an alias, so there is no
  honest label for an absent member yet.

## Not built yet

- **Offline names in the roster.** The fix is a guid-taking overload in the identity
  module — its introduction and alias tables are both keyed by guid already, so the data is
  there. Until then the window says how many are away and no more.
- **Hostility.** Not attempted. If it is ever wanted the lever is
  `UnitScript::IfNormalReaction`, which mod-sanctuary-outlaw already proves works for two
  players, and it can be added without disturbing anything here.
