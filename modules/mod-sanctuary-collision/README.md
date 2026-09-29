# mod-sanctuary-collision

Players bump into one another.

The 3.3.5a client works out its own movement and collides only with the world and with
gameobjects — never with another character. Nothing the server sends changes that, so this
module builds collision out of the two things the server does control. No client patch, no
launcher change.

## Standing still: solid

A player who stops is solid (`Block.IdleMs`, default at once). Every player within ten yards is
sent an invisible box where they stand: Blizzard's own **188215 "Collision PC Size"** model
(display 7735, `World\Generic\Collision\Collision_PCSize.mdx`), a closed block 1.25 × 1.53
yards across and 3.2 tall, under our own entry 990300. Blizzard placed that model some 1,470
times in the stock world, so every client already has it and collides with it.

Walking into a standing player is therefore real client collision: smooth, like terrain, with
no lag. The box comes off everyone's client on the owner's first movement packet.

- **Each viewer gets a copy of their own**, where the player stands, turned so its narrow side
  faces that viewer — and **only once they are clear of it**, so it never lands on top of
  anyone. Someone standing shoulder to shoulder with the player gets theirs when they step
  away; until then, walking into the player swerves them like anyone else.
- **The owner never gets their own box**, so they can always walk off.
- **The box never enters the map.** A gameobject in the map is sent by the core to everyone
  from the one position it has, the owner included. This one (`CollisionProxy`, a `GameObject`
  subclass — the position a create packet carries is `m_stationaryPosition`, which has no public
  setter) is built into a create packet by hand for each viewer, and taken off their client by
  hand. It never touches the server's line of sight or pathing.
- **Every copy ever sent goes out under a guid no client has seen before.** A gameobject guid a
  client has had taken away never becomes solid on it again: in testing, copies re-sent under a
  guid the client had lost — even 26 seconds earlier — were walked through every time, and fresh
  ones never were. The guids come from the module's own counter; a box guid carries the box's
  entry, which nothing else uses, so they never clash with anything, and the map's gameobject
  guids are left alone.
- **A copy is left alone once sent**: a re-created one is not solid for a moment. It is sent
  again only if it let its viewer through — they were reported inside it, or passing through it
  between two packets — once they are clear of the player, under another guid. Until then
  that viewer is swerved round the player instead.
- **A fresh copy is not relied on at once.** A client takes a moment to make a box solid, so for
  `Block.SolidMs` (a second by default) after a copy is sent, someone walking into that player
  is swerved round them.
- **It is sent within ten yards and taken back beyond twelve** (and when heights differ by
  more than three yards, unless the viewer is in mid-jump). It is taken back from a viewer who
  stops colliding themselves (below), and from everyone when the owner moves, is teleported
  (a blink, a summon, `.tele`, another map), changes size, stops colliding, or logs out. A
  viewer whose connection has dropped is sent nothing until they are back.

## Moving: the glide

A moving player cannot carry a solid box on this client — every way of moving one was tried
and failed:

- **Re-create the box where they are, ten times a second.** A freshly created gameobject takes
  a moment to become solid on the client — yards, at a run. Players walked through each other,
  then caught on the box behind them.
- **Remove and re-create it under the same guid in one packet.** The client is left with no
  box at all.
- **Move it in place with a movement-only update (`UPDATETYPE_MOVEMENT`).** Crashes the
  client.

So when a moving player meets someone, they are moved past them. What that looks like is
`Bump.Style`:

- **`glide`** (default) — they brush past each other. Each moving player keeps going at their
  own pace and steps just far enough aside to clear the other person, in a short move the
  server drives (`Glide.Ms`, 150 ms): the way Charge or Death Grip moves a player, a spline the
  client follows, with no falling in it and their facing kept. A glancing touch is a nudge;
  walking straight into someone is a full step aside. When both are moving they share it, and
  head on both keep right.
- **`slide`** — the same swerve done with a very low knockback (`Unit::KnockbackFrom`, which
  sends `SMSG_MOVE_KNOCK_BACK`): about 70 ms off the ground (`Slide.Hop`). A knockback always
  puts the client into its falling state for that moment, which is what the glide avoids.
- **`bounce`** — each moving player is knocked back the way they came, a small stumble
  (`Bump.SpeedXY` / `Bump.SpeedZ`).

A spline is followed blindly — through walls, off ledges, into water — so a glide is checked
on the server first:

- **The way is walked every half yard**: the ground there, no step up or down steeper than a
  stair, no more than a yard of rise over all, no water, and nothing solid across it at knee or
  head height. Where it ends leaves their body room ahead and to both sides — never closer to a
  wall than they already were. It must not run into anyone else, or into a standing player's
  box the glider's client has, solid yet or not. A glide cut short is still used if most of it
  is clear.
- **It starts where their client will be when it arrives.** A spline starts from where the
  server has them, which for a runner is their last packet, up to half a second behind; so the
  server is moved to their predicted position one round trip on, and that stretch is checked
  the same way.
- **Only someone free to be moved is glided**: on their feet, under their own control, not
  charging, rooted, stunned, casting, swimming, flying, on a transport, in the air, or waiting
  on a speed change, with a recent enough packet to know where they are. Where one of two
  cannot be glided, the other takes the whole step; where neither can, the `slide` knockback is
  used instead.
- **While it lasts, the server hears nothing from their client** (the core ignores movement
  packets during a spline). Afterwards, everyone watching is told they carry on as they were —
  or as their client reports it ended (`CMSG_MOVE_SPLINE_DONE`, which the core ignores) — so
  nobody sees them stop and then jump on at their next packet. A client heard from within
  0.8 s is believed; one that is not was taken to have stopped. A packet their client sent
  before the glide reached it, arriving after it, is dropped so they do not snap back.

The rules, whatever the style:

- **Someone standing still is never moved.** A runner cannot push idle people about.
- **A standing player's box does the work once it is solid**, and the swerve does the rest.
- **Where a player is between packets is predicted** from their last movement packet
  (direction keys, facing, turning, speed), as the other clients extrapolate them — a client
  moving in a straight line only reports about every half second. The prediction stops where
  that player's client would stop them: at any solid standing player's box it has.
- **Pressing into a wall is not walking.** A player holding a direction key without getting
  anywhere is treated as standing, measured in the client's own time.
- **Only players closing on each other are moved.** Two people already stepping apart are
  left alone, as are two who were standing overlapped to begin with.
- **Nobody is moved in the air.** A knockback would cut a jump short and a glide would flatten
  it: whoever is on their feet takes the whole swerve, and two people who are both in the air
  pass. Someone already thrown — by a knockback or a spell — or being glided is left to finish.
- **`Bump.CooldownMs` is per player.** Someone just moved is not moved again for that long,
  but whoever walks into them still is.

### What it feels like

A swerve reaches the player one network trip after the contact, so they overlap by a little
before they are moved aside — more at a run, more with a higher ping. Turning sharply with the
mouse makes the prediction less accurate. A glide takes the controls away for its 150 ms, as a
Charge does.

## Who does not collide

Nobody collides while stealthed or invisible (a bump would give them away), in GM mode or GM
invisible, dead, feared, confused or charmed, on a vehicle — which includes being carried —
on a taxi, boat or zeppelin, swimming, or flying. Nobody collides in a zone or area listed in
`DisabledAreas`. Such a player is neither solid to others nor stopped by them.

Turning the module off with `Enable = 0` and `.reload config` takes every box back.

## What is deliberately not here

- **A solid box for moving players** — see the list above.
- **A "blocked" feel for moving players** — a short root when walking into someone. It was
  left out because `mod-sanctuary-downed` holds players with raw `SetControlled` roots and
  stuns, and releasing a collision root would release theirs too.

## Before trusting it — test in game

The server half is checked against the core; these are client behaviour and need two
characters (a game master can do the glide ones alone with `.collision glide`):

1. **The glide itself.** `.collision glide` while standing, then while running with the key
   held. Expect a short sideways step with no hop or fall pose, facing kept — and, above all,
   that the character **keeps running afterwards** without pressing the key again. If it stops
   dead instead, set `Bump.Style = slide`. Also watch from the second character: the glider
   should not stop and then jump on.
2. **A standing player is solid, every time.** Stop one character; walk the other into them
   straight away (they glide round), then again a second later (stopped at the shoulders), and
   again and again after walking away and back: stopped every time, not only the first.
3. **The owner can walk away** with no hitch.
4. **A crowd.** Stand the two characters shoulder to shoulder, then walk one a few yards away
   and back into the other. Stopped at the shoulders.
5. **Moving.** Run both characters into each other, head on and at an angle, walking and
   running, and once while one of them jumps. Each should step aside and carry on.
6. **Near things.** Glide past someone beside a wall, a fence, a doorway, stairs, a ledge and
   the water's edge. Nobody goes through anything or falls; where there is no room, the small
   knockback hop is used instead.
7. **Clean-up.** Walk one character over twelve yards away and back; log one out, and teleport
   one away, while the other walks where they stood. Nobody bumps into empty air.
8. **Scale.** Stand one character still, `.modify scale 2` on them, and check the box grows.
9. **Ping.** If you can, repeat 1 and 5 with a couple of hundred milliseconds of added latency
   on one client (for example with *clumsy*): no snapping back after a glide.

### Diagnosing

- **`SanctuaryCollision.Debug = 1`** logs, at info level and stamped with the server's
  milliseconds, every box sent (with its guid) or withdrawn, every box someone walked through
  (and how long after it was sent), every bump and glide, why a glide was refused, and what the
  glider's client said afterwards.
- **`.collision probe`** (game masters) sends you one box three yards ahead, built and
  delivered exactly as a standing player's copy is. Run it again to take it away.
- **`.collision glide [yards]`** (game masters) glides you that far to your right (1 by
  default), exactly as a swerve would, and says why if it cannot.

## Files

| Path | What |
| --- | --- |
| `src/SanctuaryCollision.cpp` | boxes, prediction, glides, swerves, config, the commands |
| `conf/sanctuary_collision.conf.dist` | the options above |
| `data/sql/db-world/…` | `gameobject_template` 990300 (and a `disables` row from when it was spawned in the map) |
