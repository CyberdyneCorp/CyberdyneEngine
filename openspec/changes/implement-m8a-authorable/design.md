# Design: M8.a — Authorable

## 1. The seam that decides the milestone

**The editor's document and the engine's scene are two worlds that have never been the same world.**
M7's gate recorded the shape precisely: the viewport shows the engine's rendered frame and a gizmo
drag moves the engine's object, but the entities the editor creates change nothing in that frame,
`.cyworld` is read by nothing under `src/` or `tools/`, and the two are associated in first-seen order
by a file whose own header calls itself a stand-in.

Everything else in this milestone is downstream of fixing that. A primitive nobody can see is not
worth creating; a body on an object the runtime does not have cannot simulate.

**The rule that decides the design: there is one world, and the editor edits it.** Not a document
that mirrors a scene, not a scene rebuilt from a document on save — one authoritative representation
with the editor holding a transactional view of it. The alternative is two structures that agree
until they do not, which is exactly the shape `serialization-and-prefabs` has been trying to close
since M6 and the shape M7 left in place.

Do this first and measure it, because getting it wrong is a migration once primitives, bodies and
play mode all depend on it.

## 2. A primitive is not a special case

The temptation is a `PrimitiveNode` with a shape enum. Refuse it. A created box is a **mesh instance
whose mesh the engine generated rather than imported** — the same components, the same asset
handles, the same cooking path. The requirement says every later system must be unable to tell the
difference, and the way to guarantee that is to give it nothing to tell apart.

Generation belongs beside the importers, not in the editor: the editor issues a command, the engine
produces a mesh asset, and the result is indistinguishable from an import. That also means primitives
go through the single derivation key M7 unified, and a generated mesh is cacheable and deterministic
like any other derived artefact.

## 3. The physics bridge, and what M4 already proved

`physics` has specified components since M4 and has had no bridge. `samples/04-character` does that
work in its own host, in C++, which is why its `game.cpp` is larger than its Swift. That sample is
the specification of what the bridge must do, written out longhand — read it before designing the
module, because the behaviour is already settled and only its home is not.

The bridge is layer 4: it registers the components, creates bodies from them, drives
`PhysicsStepper` in the `Physics` stage, and writes `cy::scene::LocalTransform` back. It does not
own the backend and does not decide policy.

**Teardown under load, from the first commit.** M5.5's gate found Jolt destroying its job free list
underneath a worker still releasing a job — one run in forty, as a fault with no physics call on the
stack. Play mode creates and destroys worlds constantly, which is exactly the shape that found it.

## 4. Play mode is a runtime, not a mode flag

Pressing play must simulate the world the editor authored. Today it reports `hosting: NoRuntime`.
The editor already survives its runtime being killed and already speaks to it over the ABI, so what
is missing is the world crossing that boundary — which is §1 again, from the other side.

Stopping must restore exactly. A play session that leaves residue in the document, the scene or the
persistence overlay makes undo a lie, and the closing artefact tests precisely that by undoing back
to an empty world after a simulation has run.

## 5. What must not be retrofitted

| Invariant | Why it cannot wait |
|---|---|
| One world, edited transactionally | Two representations that agree until they do not is the defect this milestone exists to end |
| A generated primitive is an ordinary mesh instance | A special node type spreads into every system that inspects a scene |
| Generation goes through the single derivation key | A second cache key is the correctness bug M7 spent its first section repairing |
| Play mode restores exactly on stop | Residue makes undo unreliable, and unreliable undo is worse than none |
| The physics bridge is torn down under load in a test | M5.5's engine defect was exactly this shape |
