# `samples/11-ship` — the packaged project

**M11.d's closing artefact. Section 8.**

One project, **built, cooked, packaged, installed and launched from a single recipe**:

```
just run-ship                                   headless: the four acts that need no device
just run-ship --platform sdl3 --frames 240      a window, with the card presented on a swapchain
just run-ship --platform sdl3 --shot out.png --require-draw
```

`samples/` held fifteen entries before this one and **not one of them was a packaged project**.
Every earlier artefact reads its content off a path in the source tree or generates it in the
process that draws it. This one opens an *installation* — a content-addressed chunk store and the
manifest in force — verifies every byte it promises, reads its own provenance back out of it, reads
its card **by logical name**, and draws that.

---

## THIS IS NOT A GAME

`m11b:the-game-exists` is a **red** criterion. `ls -d samples/*game*` matches nothing in this
repository, and **a packaging proof does not close it and must not be read as closing it.** If you
arrived here looking for the game M11.b owes, it is not in this directory and this directory does
not stand in for it. The card says so on the screen it draws, `main.cpp` says so in its header, and
`ship.py` says so in its docstring, because this is exactly the kind of artefact a later reader
mistakes for the missing thing.

What this *is* is the proof that a project can be shipped: a derivation graph, a package, an
install, a provenance record and a launch that reproduces from it.

---

## What it covers, and what it does not

**The artefact states its own coverage on its own face** — the window carries this table, `--coverage
<path>` writes it, and the run prints it. Four of the five rows are not a pass:

| Leg | Verdict | Why |
|---|---|---|
| Linux / SDL3 | **RAN** | this host |
| Linux / native platform backend | **ABSENT** | M11.d section 4 had not landed when this was written |
| macOS / Metal | **NOT EVALUATED** | no Apple toolchain on this host |
| Windows / D3D12 | **NOT EVALUATED** | Linux host; both backends moved to a rung of their own |
| GPU vendors | **1** | one vendor, one driver, one operating system |

That is the honest shape of a rung deliberately scoped to what this machine can check.
`design.md` §1.4.3 records where sections 2 and 3 went and why: **Metal cannot be compiled on Linux
and neither can D3D12**, so neither backend could be written or judged where this rung was worked,
and `m11d:golden-images-across-three-backends` moved with them so that rung cannot close on "it
compiles somewhere".

**The native platform backend is an absence, not an omission.** Task 8.2 asks this artefact to draw
through it as well as through SDL3. When this directory was written `platform/` held `desktop-sdl3`,
`headless` and `host` and nothing else. Rather than invent a stand-in, the sample reports the leg
ABSENT with that reason and runs the SDL3 leg. `present.cpp`'s `available_platforms()` carries the
three lines that add it when section 4 lands, and nothing else in the sample knows which windowing
implementation it got — which is the whole claim `core-platform-abstraction`'s Complete cell rests
on.

---

## The first frame this engine ever presented to a window

Eleven milestones of rendering — the golden images, the beauty shot, the world, the animated
character — **every one of them drew into an offscreen image and read it back.**
`cy::rhi::Device::create_swapchain` has existed since M3 with a file of its own
(`src/backends/rhi/vulkan/src/vulkan_swapchain.cpp`), `GraphExecutor`'s `wait_acquire` and
`signal_present` have existed just as long, and **until `present.cpp` nothing in this repository had
ever called any of them.**

A packaging rung's artefact draws for that reason: presenting is the one thing a platform backend
does that a headless test cannot fake. The frame is an ordinary render-graph frame — the swapchain
image is *imported*, the copy declares `TransferWrite`, the last pass declares `Access::Present` and
records nothing, and the graph derives every transition. No pass emits a barrier, because none can.

**One interface change was needed and it is the finding.** A `VkSurfaceKHR` is created by the
platform against a `VkInstance`, and nothing in `cy::rhi::Device` exposed one — `native_handle()` is
the `VkDevice`, which is the wrong object. `vulkan_backend.h` now carries
`vulkan_instance_handle(Device&)`, on the *backend* rather than on `Device` so that it cannot become
an identity query the renderer branches on. Three milestones of rendering never noticed because
nothing had ever built a swapchain on a window.

---

## The five acts

| | |
|---|---|
| 1. **ship** | `cy_build` runs the card's four-node graph cold, then warm; the two manifests are byte-identical; the package installs; and the installation **verifies** — every chunk present and digesting |
| 2. **provenance** | the build identity, project, revision, platform, profile, toolchain fingerprint and content version, read back **out of the installed manifest** by the program that launched from it |
| 3. **launch** | the card by logical name, composed, and presented on a swapchain through a display server chosen at run time |
| 4. **report** | which legs ran, which reported NOT EVALUATED and why, and **which device answered** — hardware, software or null |
| 5. **content** | one line of the card changes; the palette's import is served from the cache and the card's is not; the same binary draws a different card out of the same installation directory, with no recompile |

`ship.py` checks every one of those against what the two programs printed and reports through
`samples/harness/artefact.py`, so a recorded gap is a non-zero exit and **NOT EVALUATED is neither a
pass nor a failure** — it is counted where a reader cannot miss it.

---

## Which device answered, and why the label is not a flag

`--coverage` and the card both name the device and classify it. The classification comes from the
device's **identity**, never from a capability bit, and `present.cpp` says why: M11.d's spike found
two D3D12 adapters on a hosted Windows runner, both `Microsoft Basic Render Driver`, and **adapter 0
did not set the software flag**. This engine has no device-type query at all —
`DeviceCapabilities` carries `device_name()` and `driver_version()` and nothing that says discrete,
integrated or software — so the name is the only evidence there is, and the report says that is what
it read. **That absence is a finding this artefact records rather than papers over.**

---

## Files

| | |
|---|---|
| `project/card/palette.cycard` | the palette — a separate source so that a colour change invalidates one import |
| `project/card/ship.cycard` | the card. **Content**: nothing in it is compiled into the program |
| `project/build/ship.cybuild` | the derivation graph: two imports, a cook, a package |
| `card.cpp` | the parser and the compositor, glyphs included |
| `present.cpp` | display server, device, surface, swapchain, frame, present, read back |
| `main.cpp` | the five acts and the report |
| `ship.py` | the driver `just run-ship` and `smoke.ship` both run |

**Governed by**: `delivery-roadmap` (milestone artefacts), `build-and-packaging` (provenance, the
install), `core-platform-abstraction` (the display server seam), `rhi-and-render-graph` (the
swapchain and the derived present transition).
