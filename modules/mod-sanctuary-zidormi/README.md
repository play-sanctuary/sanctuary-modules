# mod-sanctuary-zidormi

Zidormi stands on the road outside the Ruins of Lordaeron and moves players between two
readings of the same ground: **the memory of the past** and **the present day**.

She does exactly one thing — set the player's phase mask, the same thing `.modify phase`
does. The two timelines themselves live in the world, not in this module.

## The NPC

Entry **31848**, which already exists in 3.3.5a as the Caverns of Time traveller in the
Bronze Dragonshrine. Right name, right model, right faction, so there is no custom
creature and no lookalike to maintain.

The script is attached to the **spawn**, not the template — `creature`.`ScriptName`, which
`Creature::GetScriptId` prefers over `creature_template`'s. That matters: putting it on the
template would give the Dragonblight Zidormi this dialogue too, replacing *"Take me to the
Caverns of Time"* with it and quietly removing a travel service players still use.

To add another timeway anywhere else, spawn her and name the script on that spawn:

```sql
UPDATE `creature` SET `ScriptName` = 'sanctuary_zidormi' WHERE `guid` = <the new guid>;
```

A `.reload` will not pick that up — spawn scripts are bound when the creature is created,
so respawn her or restart worldserver. The module logs how many timeways it found at
startup, and warns if none carry the name.

## What the two phases actually mean

| | Phase mask | Sees |
| --- | --- | --- |
| Memory of the past | `1` | spawns in phase 1 |
| Present day | `4294967295` | spawns in **every** phase |

Phase 1 is what every character and every spawn starts in, so **the past is the baseline
world** and the present is that baseline *plus* anything you put in another phase.

Two consequences worth knowing before you build anything:

- **To add present-day content**, spawn it in a phase other than 1 — `2` is the obvious
  one. It appears for players in the present and is invisible in the past.
- **Nothing can be hidden from the present.** `4294967295` matches every phase, so a spawn
  cannot be past-only. If you ever want that, give the present its own bit instead —
  `SanctuaryZidormi.PresentPhase = 2` — and move the present-day spawns onto it. That is a
  config change, not a code change, and stored player choices that no longer match are
  dropped rather than restored.

Zidormi herself sits in phase 1 on purpose, which makes her visible from both sides: `1 & 1`
for a player in the past, and `4294967295 & 1` for one in the present.

## Remembering the choice

The core keeps no phase column on `characters`, so a phase mask lives only in memory and is
lost at logout. `sanctuary_timeline` in the characters database holds one row per character
who has actually spoken to her, and the choice is reapplied at login.

Turn it off with `SanctuaryZidormi.Remember = 0` if you would rather every session begin in
the past. Game masters are skipped either way — they are on `PHASEMASK_ANYWHERE` for their
own reasons and it is not this module's business to undo that.

## Files

| Path | What |
| --- | --- |
| `src/SanctuaryZidormi.cpp` | gossip, phase change, login restore, startup checks |
| `conf/sanctuary_zidormi.conf.dist` | the four options above |
| `data/sql/db-world/…` | `npc_text` 990000–990001, and the spawn's `ScriptName` |
| `data/sql/db-characters/…` | `sanctuary_timeline` |
