# Design: RTS combat and the topographic minimap

## Combat lives in the session, not in the game

A Swift game could compute hits itself in `Fixed` and only move units through the session. Then the
follower peer — which sees the command log and nothing else — would not know who died, the state
hash would not cover the battle, and a desync in damage would go unseen. So the battle is part of the
session: the game *configures* it (`arm`, `fortify`, before the first tick) and *orders* it (two new
order kinds in the same command stream), and every tick decides targets, chases, blows, flights,
hits and deaths deterministically. The order payload is unchanged — `kind`, `group` and a raw
`Fixed` point — so an attack names the enemy nearest a point, as a player's right-click does,
rather than an index the payload has no integer field for.

Reaches are gaps between footprints, so a melee unit's reach does not depend on either radius; the
battle's swordsmen reach 1.25 m, past the crowd's 2 m separation between two half-metre units, or
the crowd would push two fighters out of each other's reach every tick.

A unit that leaves its way to fight remembers it (`diverted`): an attack-move resumes its path once
nothing is left to fight, a chase that reaches the end of its path plans again instead of arriving.

## The game reads what happened and draws it

Effects, lights and sounds are presentation and may not be played from a fixed step. The session
therefore records events — strike, launch, impact, damage, death, destroyed — numbered from zero, and
the adapter keeps a bounded history after every tick. A game reads from a cursor in its frame
update, however many ticks ran since, and decides what to show: blue's fog of war (`fog_cell`) says
whether a place is seen, and nothing in the fog is played.

## The minimap is baked once and fogged where it changed

The ground of a map does not change while it is played, so its picture — colours, hillshade,
contours, water, roads — is baked once. The fog changes every frame, but only near units: the fog
stage compares the field with the last one it drew and redraws the texels over changed cells and
their neighbours (a sample reads the cells within one of its point), which is exactly the whole
stage's answer (`unit.render_minimap` holds the two equal) at a fraction of its cost. Icons are
stamped on a copy each composition.

A contour line is placed on the HIGHER texel of every neighbouring pair a level crosses, which makes
it one texel wide and exactly checkable against the heights.

## A page updated inside the frame

`upload_atlas` makes a texture, submits a graph of its own and waits for the device: right for a
font page made once, wrong for a page that changes while the game runs. `stage_atlas` keeps the
page's texture, stages its texels in a per-slot buffer when the frame declares the interface's
stage, and copies them in a transfer pass the interface's passes read after.

While building the battle, a run that updated its minimap through `upload_atlas` every sixth frame
lost the device (NVRM Xid 109, a context-switch timeout) at a fixed frame on this host's RTX 5060 —
with the GI, ambient occlusion and the sun's shadows all on, and only in the battle. No validation,
synchronisation-validation or AddressSanitizer finding preceded it, and under AddressSanitizer it did
not occur. It was recorded, with how to reproduce it, as an open finding; the battle's minimap uses
`stage_atlas`, which adds no device frame and is the cheaper path in any case.

**Resolved by `fix-ui-atlas-upload-frame`.** `upload_atlas` ran its copy in a device frame of its own
between two of the game's frames, so the game's next frame ran in the slot its previous frame had
used; that frame is the one the device was lost in, and an upload without its own frame never lost
it. `upload_atlas` now stages its texels into the next frame's graph as `stage_atlas` does; that
change's design records the bisection.
