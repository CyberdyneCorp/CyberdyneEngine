# `src/core/plugins/` — what a plugin is, which ones load, and in what order

Layer 0. `project-and-plugins`, and M11.b section 2.

## What was here before this module

Nothing. `find src tools -iname '*plugin*'` returned one `tools/layercheck` fixture, and had done
since M5. Four of the capability's eleven requirements — **Plugins**, **Plugin lifecycle**, **Plugin
resolution and lockfile**, **Trust tiers for extensions** — had no implementation at all. What
existed was `cy::config::ProjectPlugin`: a row in the generated project graph naming an id, a
version and an engine API range, which nothing read.

## Why this is the rung's first editor section rather than one of its last

`editor-architecture`'s **Specialised editors** requirement is normative about the order:

> Each SHALL be a plugin using the same panel and undo infrastructure as user plugins, so the
> extension API is exercised by the engine's own tooling

with a scenario — *"WHEN a built-in editor is implemented THEN it SHALL use only the public plugin
API"*. An extension API written after its first consumers is an API shaped by what those consumers
already did, which is the opposite of what the requirement is for.

## The four pieces

| Header | What it decides |
|---|---|
| `plugin.h` | What a plugin **is**: a stable identity, a version, an engine API range, what it contains, and the trust tier its contents require. Plus the manifest reader. |
| `resolve.h` | Which plugins load, at which versions, in **what order**, and the lockfile that makes that reproducible. |
| `host.h` | The eight phases, the containment when one fails, the extension points, and who owns which type. |

## Three decisions worth knowing before reading the code

**Identity is not a name and not a path.** `PluginId` is interned from a stable identifier the
author chooses once; `display_name` is a separate field; nothing in this module ever compares display
names. The requirement's scenario — renaming a plugin is safe — is a test rather than a convention,
because a convention is what gets broken by the first person who finds `display_name` convenient.

**Trust is a decision recorded, not a boundary enforced.** The requirement says so in as many words:
*"Native code loaded into the process has the process's privileges. The engine SHALL NOT claim to
sandbox it, and SHALL instead make the trust decision explicit."* `TrustTier::TrustedNative`
therefore guarantees exactly one thing — that nobody got there by accident. `PluginHost::add`
refuses a `TrustedNative` plugin with no explicit `trusted` argument, and there is no default that
grants it.

**Determinism is a tie-break, not a topological sort.** *"Load order SHALL follow the resolved
dependency graph and SHALL be deterministic."* A topological order alone gives the first half; the
second needs the walk to visit roots and dependencies in a fixed order, which here is by the plugin
identifier's **text**. `Name`'s own ordering is interning order — the order this process happened to
meet the strings in — so a lockfile or a load order sorted by `Name` would differ between two runs
that read the same manifests in a different order. Both halves of the tie-break have their own case
in `unit.project`, because dropping either one on its own leaves the other's case green.

## What this module deliberately does not do

**It does not `dlopen` anything.** Loading a binary is `src/abi/`'s, which already carries the
engine's reload model — *serialize, migrate by name, recreate, never `dlclose`* — and a second
loader would be a second answer to what happens to a retired image's string literals. This module
decides **which** plugins load and in what order; `PluginRuntime` is how it calls out to whatever
does the loading, and it does not know what one is.

**It does not count instances.** `TypeOwnership::may_unload` takes an `InstanceCounter` from the
caller, and it has to: this is layer 0 and `cy::ecs` is above it. A host that guessed zero would
permit exactly the unload the requirement exists to refuse, so a null counter is a refusal rather
than a yes.

**It does not read the project manifest.** That is `cy::core-config`, whose whole claim is that it
reads the project graph with no file I/O and no allocation because the generator rendered it into
`<cy_project.h>`. A plugin manifest is read at run time from a file the project did not compile, and
it allocates; putting one in the other would make that claim false for both halves.

## The suites

| CTest name | What it covers |
|---|---|
| `unit.project` | Manifests, identity, versions and constraints, trust tiers, resolution, the lockfile, extension points. No sequence, no world. |
| `integration.plugins` | The **lifecycle**: a set of plugins taken up through four phases each and back down through four more, with a failure contained in the middle of it. |

Nine mutations were run against these at M11.b and every one of them turned a suite red — including
the two halves of the order tie-break, which had to be pinned separately after the first attempt
found that dropping both at once was the only version the original cases could see.

## What is still owed here

* **Task 2.5's build-level check.** The requirement that a built-in specialised editor uses *only*
  the public plugin API is enforced today by there being nothing else to use, plus
  `tools/layercheck`. A built-in editor reaching past this API should be a **build** failure, and a
  program that links is a program whose layering was already accepted — so the check belongs in
  `tools/plugins/selftest.py` beside `tools/project/selftest.py`, which is where the project graph's
  own rejections live for the same reason. It is not written.
* **A loader.** `PluginRuntime`'s callbacks are filled in by hand today. The binary path — the
  engine's C ABI descriptor entry point taking the host's API version — is `native-abi`'s surface
  and `src/abi/`'s loading, joined here.
* **Nothing registers at an extension point yet.** `declare_standard_points` offers the twenty-one
  the requirement names at minimum; the engine's own features do not yet go through them, which is
  the "dogfoods its extension points" scenario and is the work each row does as it arrives.
