# After M9's gate closes — three things, none of them milestone work

> **UPDATE 2026-09-11, after M9 closed.** Item 1 is DONE and the cause was not what this note
> guessed. M9's own gate found half of it and the other half is fixed below; item 3 is still open;
> item 2 was done on the day this note was written. See "What item 1 actually was" at the end.

Authorised by the user 2026-09-11. All three are machinery around the work rather than the work, and
none belongs inside a milestone's scope. Do them between M9 closing and M10 launching.

---

## 1. Why continuous integration keeps cancelling

**This is the one that pays for itself**, because `delivery-roadmap` makes CI a precondition for the
audit ever getting shorter:

> **From M9 the audit MAY be reduced** … provided both of these hold, and the reduction SHALL be
> refused if either does not: continuous integration has actually executed, so that cross-platform
> and cross-configuration claims are verified by something other than an agent's reasoning; and the
> permanent gate set covers what the by-hand sweep would otherwise repeat.

Measured 2026-09-11 with `gh run list`:

```
queued      ci  main  schedule  34578492759  5h39m   <- still queued
cancelled   ci  main  push      34551346342  6h41m
cancelled   ci  main  push      34549531188  26m
```

Two consecutive **cancellations** and one run queued for over five and a half hours. So the first
condition does not hold today, the M9 gate should refuse the reduction, and every later milestone
keeps paying eight-hour gates until this is fixed.

**Where to look first, in order:** concurrency groups cancelling in-progress runs when a new push
lands (the two cancellations are both `push`, minutes apart, which is the signature); the 480-minute
timeout on the `milestone` job against a ledger that takes hours; and whether the hosted runner has
the capacity for a job that shape at all. A ledger that cannot finish inside a hosted runner's limits
is an argument for a self-hosted runner, not for a shorter ledger.

**Do not shorten the ledger to fit CI.** The ledger is the thing that has caught eighteen findings.

### What M9's closing gate then measured, which narrows this considerably

The cancellations are real and they are the second problem. **The first is that the jobs were
failing, and had been since M0.** `gh run list -L 100` returns sixty-three runs, from M0's first
commit to M8.c's last, and **not one of them is `success`**. The most recent push run is the record:
three jobs green (`spec validation`, `editor linux`, `roadmap-status`), **ten red**, twelve cancelled
when the next push superseded them, and `milestone — the closed milestones' exit criteria` **has
never started at all**.

Every Linux job died at the same line, at CONFIGURE, before compiling anything:

```
CMake Error at build/dev/_deps/sdl3-src/cmake/macros.cmake:433 (message):
  Couldn't find dependency package for XCURSOR.  Please install the needed packages
  or configure with -DSDL_X11_XCURSOR=OFF
```

The step above it installed `libx11-dev libxext-dev libwayland-dev libxkbcommon-dev`. README.md's
own "System libraries SDL3 builds against" list — the one a human runs on a fresh clone — also names
`libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev`, and `cmake/dependencies.cmake` says exactly
why those four: "What stays on is what DisplayServer needs — Xcursor, Xrandr for screen enumeration,
Xfixes and XInput2 for input."

**Fixed in M9's closing change**, along with the check that stops it recurring:
`tools/ci/check_workflows.py` now fails when a Linux job's package list drops one README documents,
with its own negative fixture. `libxss-dev` came *out* of README's list in the same change, because
`cmake/dependencies.cmake` forces `SDL_X11_XSCRNSAVER` off and this machine builds without it.

**Three failures are recorded and NOT fixed**, because this host runs neither operating system and a
blind edit to a leg nobody can execute is how the list got wrong in the first place:

| Leg | What it says |
|---|---|
| `build windows-x86_64`, `build windows-arm64` | `swift-actions/setup-swift@v2`: `Version "6.0" is not available` — while the Linux and macOS legs accept the same input |
| `editor macos` | `mapfile: command not found` — the macOS image's bash is 3.2 and a recipe uses a bash 4 builtin |
| `editor windows` | `rustfmt refused the generated src/generated/mod.rs` |

So the order is: fix those three, then the concurrency group, then ask whether a hosted runner can
carry the `milestone` job at all. Until a run finishes, **every `where = "ci"` criterion in this
repository is deferring its question to a machine that has never answered one** — which is why M9's
gate converted its own (`lockstep-cross-platform`) from `where = "ci"` into a declared gap.

## 2. The build directory grows without bound

Cleared 2026-09-11: **93 stale directories, 431 GB**, taking the disk from 92 % to 67 %. Verified
first that nothing under `just/`, `tools/`, `.github/` or any milestone ledger names them, and that
only `build/gate` and `build/swift-package` had been touched in the previous two hours.

**The cause is structural and the rule that causes it is a good rule.** Hard rule 11 tells every
agent to build into `build/<label>/`, which is what stopped agents corrupting each other's trees
after three contention incidents. Nothing removes them afterwards, so each milestone leaves five to
ten trees of 8–19 GB. At 171 directories the disk was one milestone from full — and a gate that runs
out of disk mid-ledger fails a milestone for a reason that has nothing to do with the milestone.

**Propose:** a `just build-reap` recipe taking an age threshold, refusing to touch anything modified
inside it, and listing what it will remove before removing it — the shape `maintenance-clean` already
uses. Not automatic on a timer: a tree an agent is still using must never vanish underneath it.

## 3. Virtual geometry has never been photographed

**The most visually demonstrable capability in this engine has no picture, and the reason is a
sequencing accident rather than neglect.**

`virtual-geometry` reached Working at M7 and is real: `samples/07-fidelity` puts **4,478,208 source
triangles in 1,915 clusters across 186 instances** on the device through cluster traversal and the
visibility buffer, with the crack-free check passing 5 of 5 shells over 120 thresholds each, and
22,118,326 interior plus 13,943,657 exterior pixels covered.

All of that is text, because **at M7 the engine could not capture a frame.** The pipeline layer that
turns a rendered frame into a PNG did not exist until M8.c — that is the black-versus-lit pair in the
build log. So `docs/design/images/fidelity-m7.png` is a chart, which was the most its own milestone
could produce.

**Now capturable.** Run the fidelity sample with M8.c's record callbacks and commit:

- the film-detail scene rendered at source resolution — the thing the number describes;
- a **cluster-boundary visualisation**, because "no manual level-of-detail authoring" is the claim
  and cluster colouring is how a person *sees* it rather than reads it;
- ideally the same view at two geometric-error thresholds, since the hierarchy choosing differently
  is the mechanism.

Then add them to both artifacts. Label anything that is not the engine's own output as a diagram,
per the rule M8.c's camera-cut panel already follows.

---

**Order:** 1 first — it is the only one that changes how long every future milestone takes. Then 3,
which is cheap and makes the engine legible to someone who has not read a ledger. Then 2, which is
now urgent again only when the disk climbs.

---

## What item 1 actually was

This note guessed "concurrency groups cancelling in-progress runs" from the two cancellations it
could see. That guess was right about the mechanism and wrong about the scale, and there was a second
cause underneath it.

**M9's gate found the first cause: every Linux job died at CONFIGURE.**
`Couldn't find dependency package for XCURSOR`, because the workflow installed four X11 packages
where `README.md`'s own list names eight. Fixed by the gate, with a check that fails when the two
lists drift.

**The second cause is the one this note guessed, and it is worse than two cancellations.**
`gh run list` over the repository's whole history:

```
65 cancelled · 0 success · 0 failure · 1 in progress     (66 runs, M0 to M9)
median run survived 115 minutes; the longest reached 14h31 before a push killed it
```

Not one run has ever reached a verdict. Three settings, each individually reasonable, combined badly:
the workflow runs on every push to `main`; one concurrency group covers all of `main`; and
`cancel-in-progress: true`. That last is ordinary good advice — it stops stale runs piling up on a
branch — and it is wrong for a workflow that takes hours on a trunk pushed to many times a day. This
project pushes to `main` constantly *by design*, because milestone workflows commit their agents'
work mid-flight.

**Fixed:** `cancel-in-progress: ${{ github.ref != 'refs/heads/main' }}`. Pull requests keep
cancelling, which is what the advice is for; `main` now finishes.

**And it is now checked.** `tools/ci/check_workflows.py` fails on a bare
`cancel-in-progress: true` in a workflow that runs on push, and the check was proved by reverting the
fix and watching it fire.

**The consequence, which is the reason this mattered:** `delivery-roadmap` makes "continuous
integration has actually executed" a precondition for a milestone's audit ever being reduced. One
setting is why every gate from M0 to M9 ran in full, by hand, for six to twelve hours each.

**What is NOT fixed, deliberately.** M9's gate recorded three further CI failures and declined to fix
them blind, which was right — none is reproducible on this machine: `setup-swift`'s
`Version "6.0" is not available` on both Windows legs, `mapfile: command not found` on macOS
(bash 3.2), and rustfmt refusing the generated `mod.rs` on Windows. **The first green run will say
which of these are real**, and that run is now possible for the first time.

## 4. The visibility buffer's depth test is not atomic with its payload write

**Found by rendering the M7 scene for documentation (item 3), not by a gate.** It is a correctness
defect, not only a determinism one.

`vgVisRaster` in `src/rendering/virtual_geometry/shaders/vg_visbuffer.slang` settles depth with
`InterlockedMin(depth[pixel], key, previous)` and then writes `visbuffer[pixel]` as a **separate,
unordered store**. The two are not atomic together, so for fragments at *different* depths:

1. the far fragment runs its `InterlockedMin` first, sees `kFarDepth`, wins its compare, and will store;
2. the near fragment then lowers `depth`, wins its own compare, and will store;
3. both store, and **whichever store lands last owns the pixel** — which can be the farther surface.

`depth` and `visbuffer` then disagree. The comment at that line claimed only coincident surfaces
race and that "either answer is a correct one"; both halves were wrong, and the comment is corrected
in place.

**Evidence**, two identical runs of `cy_fidelity_capture` at 1280x720, threshold 1.0:

| Quantity | Run to run |
|---|---|
| covered pixels | 921,593 — **stable** |
| visible clusters | 5,247 — **stable** |
| differing pixels | **110–150 (0.015%)**, at silhouettes |
| `materials_seen` | **flips 4 ↔ 5** across six runs |

Coverage and cluster count being bit-stable localises this to the payload pairing: not the traversal,
not the raster bounds. `materials_seen` flips because a thin material's last few pixels are exactly
the contested ones, and `fidelity.py` asserts only `materials > 1`, so the artefact never caught it.

**Fix**: one 64-bit atomic min over `(key << 32) | payloadIndex`, so depth and payload move together.
It needs shader int64 atomics (`VK_KHR_shader_atomic_int64`); where that is absent the fallback is a
depth-only prepass followed by a payload pass that writes where `key == depth[pixel]`, with a
deterministic tiebreak. **The regression test belongs on the PR that fixes it**: render one frame
twice and require the visibility buffer to be identical — it fails today, which is the point.

This is also why `docs/design/images/virtual-geometry-*.png` are reproducible only to within ~0.015%
of pixels, and why the capture tool colours clusters by the stable `(instance, cluster)` pair rather
than by `samples[].visible` — that index is atomic-append order and repaints the entire image on
every run.
