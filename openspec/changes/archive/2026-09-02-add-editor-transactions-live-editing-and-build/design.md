# Design: CyberEditor and CyberBuild

## 1. Three worlds, not two

`editor-architecture` currently specifies two worlds: the editor's UI world and the edited scene's
world. That was right and is one short.

The **authoring world** holds what the editor needs and the runtime must never see — names, folder
organisation, prefab provenance, override conflicts, selection metadata, unresolved references. The
**preview world** serves asset editors: a material sphere, an animation rig, a VFX effect, each
isolated. The **runtime world** is what play mode instantiates, and it is produced *from* authoring
data rather than being it.

The separation is what makes the cooking rule enforceable. If the editor's live data were the
runtime's data, "editor-only data does not reach the runtime" would be a promise nothing could
check; with a compilation step between them, it is a property of the pipeline.

## 2. A transaction is a semantic operation, not a snapshot

The existing undo requirement stops at "reversible operations". Everything valuable depends on going
one step further and saying *what an operation is*: a typed delta addressing an object by identity.

```
SetField { object, field, before, after }
AddComponent / RemoveComponent
CreateEntity / DeleteEntity / ReparentEntity
ModifyOverride / ChangeLayerMembership
GraphEdit / TerrainStroke / FoliagePaint
```

Snapshots would be simpler and would foreclose everything: diff, three-way merge, autosave as a
journal, crash recovery, transmission to a collaborator, and application to a running game are all
the same operation stream read by different consumers. **One mechanism, five products** — which is
the argument for specifying it properly once.

Domain-specific payloads matter for the same reason budgets do: a terrain stroke records the tiles
it changed, not a copy of the landscape, and a foliage paint records rule deltas and instance
exceptions. Generic field operations would be correct and unusable at scale.

## 3. Addressing by identity is what makes history survive

An operation addresses `{document, persistent entity, component type, field}` — never a pointer, an
index, or a byte offset.

The payoff is specific: an undo history remains valid across an asset reload, a schema migration,
and a rename. That is only possible because the foundations work made type and field identity stable
and recorded, and it is the clearest demonstration of why that change was worth making.

## 4. Interactive edits produce one operation

Dragging a gizmo generates hundreds of intermediate states, all of which should drive live preview
and none of which belong in history. Interactive transactions therefore have an explicit begin,
update, and commit, with only the commit recorded; typing coalesces the same way.

Nested transactions collapse: a tool that calls another tool produces one entry named for the
outer intent, because "Duplicate Building" is what the user did and "Set Transform" is not.

## 5. Live editing must know what it is allowed to do

Applying an arbitrary authoring change to a running world is unsafe, and refusing to apply anything
makes iteration worthless. So every field and component declares a **live edit policy**: immediate,
reinitialise the component, recreate the entity, reload the asset, restart the world, or
unsupported. The editor reports which applies before the user acts, rather than after something
breaks.

The second half is preservation, and it is where the foundations work pays off again. Field
classification already distinguishes authoring data from runtime state; live editing consumes it
directly. A designer raising `Health.max` while a character is at 53 health updates the maximum and
leaves the current value alone — not because live editing was written carefully, but because the
schema says which is which.

## 6. Tweaking a running game is not editing a project

These are different acts and conflating them is how work is lost in both directions: changes made
while debugging vanish at stop, or worse, silently dirty the scene.

They are therefore distinct, visibly so in the interface, with an explicit path between them.
"Keep changes" at the end of play mode compares the final runtime state against what the authoring
data produced and offers only the differences in **authoring-classified** fields — runtime state is
never offered, because it was never authored.

## 7. Play modes are one protocol, not three implementations

In-process play is the fast path; a separate process is closer to shipping; a remote device is the
only way to see console and mobile behaviour honestly.

The mistake is to build the in-process case around shared memory and pointers and then discover that
the remote case needs a protocol. So the **live bridge** is the interface in all three cases, and
in-process is an optimisation of transport rather than a different architecture. Remote runtime
inspection then costs nothing extra, which is exactly the capability that is hardest to add later
and most valuable when debugging a console.

## 8. Hot reload is staged and honest

Data, asset, and shader reload are achievable and are specified fully: handles stay stable while the
backing resource is replaced at a safe epoch, using the retirement mechanism the memory work already
defines, and a failed shader compile keeps the last working pipeline rather than replacing something
correct with something broken.

Native module reload is harder, and the specification says so rather than promising it everywhere.
It is gated on a property the reflection work already provides: **type ownership**. A module may
unload only when no live instances of the types it owns remain, or when a declared migration exists.
A plugin declares whether it supports reload; one that does not requires a restart, and the editor
says so plainly. Pretending otherwise produces a feature that works until it corrupts something.

## 9. The plugin boundary is the ABI that already exists

The engine already specifies `native-abi`: a versioned, append-only flat C ABI with a generated
Swift overlay, built for exactly this problem. Introducing a second plugin ABI alongside it would be
a duplicated versioning surface and a second thing to keep compatible.

So: **first-party modules built together use native C++**, where ABI stability across versions is
unnecessary; **externally distributed binary plugins cross the existing C ABI**, with no standard
library types, no third-party types, and no engine internals in the boundary. One ABI, two audiences.

## 10. The project graph is the authority

Folder layout stays conventional and stops being load-bearing. Modules declare their dependencies,
public versus private, and the build validates the graph: cycles fail, and **architectural layering
is enforced** — foundation may not depend upward, runtime may not depend on editor.

This is worth enforcing mechanically rather than by review because dependency direction is the
property that decays silently and is most expensive to recover. A layering violation caught in CI is
a five-minute fix; the same violation found two years later is a refactor.

## 11. A build is a graph of derivations, not a script

Every build action — compile, import, cook, shader compile, package — is a node with declared inputs
and outputs and a **derivation key** hashing its inputs, tool versions, parameters, platform, and
profile. If the key is in the cache, the work does not run.

Three rules make that hold, and each rules out a specific, common failure:

- **Undeclared reads are defects.** A cooker that quietly reads a config file it did not declare
  produces stale outputs that no invalidation can catch.
- **Outputs are immutable.** A derived artefact with a given hash never changes; different content
  gets a different hash. This is what makes caching and concurrent builds safe rather than
  racy.
- **Determinism is a requirement, not an aspiration.** Same inputs, byte-identical outputs — which
  is also the precondition for the content-addressed patching below.

## 12. The derived data cache is disposable, and that is a rule

The cache holds only derived data. **Deleting it must never lose project content** — it may make the
next build slow, and that is the entire consequence.

It is also distinct from the asset registry: the registry is authoritative metadata about assets and
their relationships, and belongs in source control; the cache is regenerable output and does not.
Blurring the two produces a project whose correctness depends on a cache, which is the failure this
rule exists to prevent.

## 13. Patching is already half-built

Content-addressed geometry pages, virtual texture pages, world cell payloads, and shader binaries
were each specified as content-addressed for their own reasons. The consequence, which can now be
claimed: a patch transfers the chunks whose hashes changed.

One texture edited in a fifty-gigabyte game moves the pages that changed and a manifest — not
because a delta algorithm was clever, but because the formats were content-addressed from the start.
Binary delta remains available for large changed chunks and is a measurement, not a default.

Patch application stages and switches atomically: download, verify, stage, switch. A failed patch
leaves the previous build playable, which is the only acceptable behaviour on a player's machine.

## 14. Answering "why is this in my build"

Two questions decide whether a team can control build size, and both need the dependency graph that
already exists to answer them: *why is this asset in the build* (a reference chain from a root) and
*who references this asset*.

Specified as first-class rather than as a report, because the alternative — discovering a
twelve-gigabyte texture set after packaging — is where shipping schedules go.

## 15. Order of implementation

The ordering is deliberate and two of its steps are worth stating as rules:

1. Project and module manifests; dependency graph and layering validation
2. Plugin registry and lifecycle
3. Documents; transactions and undo
4. Reflection-driven inspector; workspace, panels, commands
5. Build graph; the daemon; the derived data cache
6. Cook profiles and target cooking
7. Package format; incremental packages and patch manifests
8. In-process play; the live edit compiler
9. Separate-process and remote live bridge
10. Semantic diff and merge; source control
11. Distributed build; collaboration

**Do not build collaboration before transactions are solid**, and **do not build distributed builds
before the dependency graph is correct.** Both are amplifiers: they multiply whatever correctness
the underlying model has, including none.

## 16. Non-goals

- **Real-time collaborative editing.** The transaction log is the right substrate and the identity
  model makes conflict resolution tractable, so the architecture permits it. It is not specified,
  and specifying it before the single-user model has been used in anger would be premature.
- **Sandboxing native plugin code.** A native plugin loaded into the process has the process's
  privileges, and no in-process mechanism honestly changes that. The specification distinguishes
  trusted native extensions from data and script mods rather than making a security promise it
  cannot keep.
