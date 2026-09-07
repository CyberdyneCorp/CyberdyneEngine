# Design: M7 — Fidelity

This document is opened with the milestone and is filled in by the spikes before the implementing
work starts. What is written here now is the shape of each decision and the criterion that settles
it, so that a spike is commissioned against a question rather than a subject.

## 1. Spike — the material IR and closure lowering

**The question.** What is the intermediate representation between a material graph and a compiled
program, and what does lowering to closures cost when the same material has to appear in the forward
pass, the visibility-buffer material resolve, a shadow pass, a GI probe and a node preview?

**Why it is expensive to reverse.** Every other system in this milestone consumes the material table.
An IR that cannot express what the visibility buffer needs is discovered when the visibility buffer
is written, which is after virtual geometry has been built on top of it.

**The criterion**, and it is one of M7's own exit criteria so the spike tests the gate rather than a
proxy: the IR round-trips, and a graph and a hand-written material produce **identical programs**.

## 2. Spike — the budget arbiter's control loop

**The question.** Six systems each declare a cost and a set of quality levers; the arbiter allocates
frame time between them every frame. What control law converges without oscillating, and what does a
system have to declare for the arbiter to be able to reason about it?

**Why it is expensive to reverse.** `residency`'s lever schedule at M6 is the same shape a level
lower — declared levers, tightening immediately and relaxing after a dwell — and it was designed that
way so that this arbiter is one mechanism rather than seven. If the arbiter needs something a lever
cannot declare, every system that has already declared one is edited.

**The criterion**: the arbiter's allocations converge without oscillation under a **step load**, and
every paged system degrades along its declared axis — a coarse root, a resident mip tail, a
stale-but-valid shadow page. A frame is never missing, only coarser.

## 3. Virtual geometry's cluster hierarchy and GPU traversal

Third in the roadmap's order and not given a separate spike, because the two above decide what it can
assume. What must be settled before it is built: the cook is deterministic and cache-friendly at
cluster granularity, per `asset-import-pipeline`'s "Virtual geometry cooking" requirement — which is
the requirement that stops `asset-import-pipeline` completing at M6 — and it lands as a **node in
the build graph** rather than beside it.

## 4. What M6 left that must be closed here rather than later

The proposal lists these; the two with a deadline are here.

**One key, one cache.** `tools/build/`'s key refuses an incomplete toolchain fingerprint;
`cy::import::import_derivation_key` contributes no toolchain at all, and it is the one that cooks.
M7 adds material and geometry cooks that are expensive enough that a wrong cache hit is a wrong
shipped artefact rather than a slow build. Merging them is cheapest before those cooks exist.

**One overlay, one persistent identity.** `cy::world::PersistenceOverlay` and `cy::save::Overlay`
are two structures for one requirement. Reconciling them after a save format has shipped is a
migration; before, it is an edit.

## 5. What must not be retrofitted

| Invariant | Why it cannot wait |
|---|---|
| One temporal framework, not one per stochastic system | Five reprojections that disagree about history invalidation is the defect that cannot be found from a screenshot |
| The arbiter reads declared levers, not measured guesses | A system the arbiter cannot reason about is a system that takes the frame |
| Every new cook is a graph node with a two-run determinism gate | M6's spike measured what a non-deterministic step plus a cache costs: the artefact that ships is decided by a race |
| The Metal seed lands with the renderer, not after it | Its whole purpose is to expose Vulkan-specific assumptions while they are still cheap |
