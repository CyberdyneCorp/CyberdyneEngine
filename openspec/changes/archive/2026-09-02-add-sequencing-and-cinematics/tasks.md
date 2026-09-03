# Tasks: CyberSequence

Specification-stage change. Sections 1 and 2 are the work of this change and are complete; the
change is archived on that basis. Sections 3 onward are the implementation backlog, sequenced by the
phase table in `design.md`.

## 1. Specification

- [x] 1.1 Record in `design.md`: orchestration rather than ownership, compilation for the seventh
      time and what it buys here, exact time distinct from the simulation tick, the gameplay
      boundary and the sixth command producer, arbitration instead of last-writer-wins, capture and
      restore scoped to touched properties, seeking as the place cinematics systems usually break,
      skipping that applies what it skips, a sequence knowing the future where every other predictor
      extrapolates, replicating intent rather than interpolation, the refusal to become a second
      scripting language, the phase table, and the non-goals
- [x] 1.2 New `sequencing-and-cinematics` (31 requirements, 59 scenarios): orchestration boundary,
      compiled programs, cost proportional to active content, exact time, clock domains, bindings,
      tracks and sections and channels, authority classification, sequences as a command producer,
      batched dispatch, arbitration, capture and restoration, nesting and parameters, events and
      markers, seeking and scrubbing, skipping, preload plans, the streaming source, playback
      control, time scaling and pausing, spawned content, network policies, replay and rollback,
      persistence, stable identity and merging, hot reload, track extension, accessibility
      metadata, diagnostics, performance and testing, and forbidden patterns
- [x] 1.3 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 **Six hooks become connections.** `residency` propagated deadlines for "a cinematic
      declaring a camera cut", `camera-system` specified cinematic overrides and anticipated cuts,
      `temporal-rendering` invalidates history on a cut, `weather-and-wind` required sequencing to
      drive weather state rather than a parallel path, `animation-and-skinning` provides markers, and
      `audio` provides timing. Each was a hook for an orchestrator that did not exist.
- [x] 2.2 `gameplay-framework` — sequences become the sixth producer of the command stream, with
      provenance for diagnostics that does not affect validation. This closes an inconsistency: the
      requirement enumerated five producers and a cinematic changing gameplay state would have been
      a sixth path with no rules.
- [x] 2.3 `camera-system` — sequences named as the principal producer of anticipated cuts, required
      to drive cameras through the stack rather than writing transforms
- [x] 2.4 `residency` — the compiled sequence identified as the strongest predictor the engine has,
      since it knows its shot list rather than extrapolating motion
- [x] 2.5 `weather-and-wind` — the sequencing seam closes; authored and gameplay-triggered weather
      are the same operation
- [x] 2.6 `editor-architecture` — the sequence editor added, and **timeline and curve editors are
      required to share infrastructure** in the same way graph editors already are, so a second
      curve surface and keying model are not built
- [x] 2.7 `thirdparty-dependencies` — sequencing recorded as engine-built, with an external timeline
      runtime evaluated against the duplication it would force
- [x] 2.8 `animation-and-skinning`, `audio`, `vfx-system`, `visual-scripting`,
      `replay-and-rollback`, `simulation-and-determinism`, `world-partition-and-streaming` —
      reviewed; no change needed. Markers, timing and synchronisation, effect handles, asynchronous
      graph waits, the side-effect ledger, determinism classification, and streaming sources already
      accept what sequences produce.
- [x] 2.9 **Non-goal recorded**: sequences do not become a general logic language; deciding whether a
      sequence plays belongs to gameplay graphs and gameplay code

## 3. Phase 1–3 — the core (deferred)

- [ ] 3.1 Stable identities, exact rational time, source schema
- [ ] 3.2 Tracks, sections, typed channels, blending and priority
- [ ] 3.3 Compiler, intermediate representation, interval and event indexing
- [ ] 3.4 Runtime player, instances over shared programs, playback lifecycle
- [ ] 3.5 Binding resolution with required, optional, fallback and constraints
- [ ] 3.6 Batched dispatch with phases and stable ordering

## 4. Phase 4–6 — adapters and prediction (deferred)

- [ ] 4.1 Camera adapter: rigs, cuts, blends, lenses, priority into the camera stack
- [ ] 4.2 Animation adapter: clips, graph parameters, layers, marker synchronisation
- [ ] 4.3 Audio and effects adapters with their seek capabilities declared
- [ ] 4.4 Nested sequences, parameters, parameter mapping
- [ ] 4.5 Preload plan derivation; prepared sequences; preload miss reporting
- [ ] 4.6 Streaming source with future bounds and cut announcements

## 5. Phase 7–9 — gameplay, environment, and time (deferred)

- [ ] 5.1 Gameplay event and command tracks; authority classification and compile-time validation
- [ ] 5.2 Environment, material, light, interface and world layer adapters
- [ ] 5.3 Capture and restore per adapter; completion policies including on interruption
- [ ] 5.4 Seek modes and per-adapter seek capability
- [ ] 5.5 Skip policies with required outcome application
- [ ] 5.6 Time scale tracks with declared domains; pause semantics

## 6. Phase 10–12 — integration and tooling (deferred)

- [ ] 6.1 Network policies; late join by seeking; clock correction
- [ ] 6.2 Replay reconstruction from semantic operations; rollback through the side-effect ledger
- [ ] 6.3 Persistence and compatibility policies
- [ ] 6.4 Sequence editor on the shared timeline and curve infrastructure
- [ ] 6.5 Hot reload with instance policies; semantic diff and three-way merge
- [ ] 6.6 Diagnostics: value provenance, trace emission, profiler views
- [ ] 6.7 Track extension points with declared properties and validation
- [ ] 6.8 Accessibility metadata and attenuation
- [ ] 6.9 Cinematic rendering and export at exact sequence time

## 7. Validation (deferred)

- [ ] 7.1 Evaluation tests: values at given times, headless, with no renderer
- [ ] 7.2 **Seek equivalence**: for every adapter claiming direct seekability, playing to a time and
      seeking to it produce equivalent state
- [ ] 7.3 **Skip correctness**: skipping a gameplay-relevant sequence applies its required outcomes
- [ ] 7.4 Determinism: authoritative output identical across worker counts and chaos scheduling
- [ ] 7.5 Network: late join reconstructs correct state by seeking; clock correction is smooth
- [ ] 7.6 Rollback: a speculatively started sequence does not realise its effects twice
- [ ] 7.7 Restoration: interrupted sequences restore according to declared policies, and spawned
      content is released
- [ ] 7.8 Preload: under artificially slow input and output, misses are reported rather than hidden
- [ ] 7.9 Compile-time validation: an authoritative track in a presentation sequence fails; a
      skippable sequence without declared outcomes fails
- [ ] 7.10 **Sequencing benchmark**: many instances, thousands of active tracks, tens of thousands of
      channels, from assets with on the order of a million authored keys — cost reflects active
      ranges, allocates nothing per frame, and cold seeking does not evaluate linearly from zero
- [ ] 7.11 Merge: independent edits merge, same-element edits conflict, display reordering does not
- [ ] 7.12 Forbidden-pattern checks

---

**Archived 2026-09-02.** Sections 1 and 2 are complete: `sequencing-and-cinematics` is in
`openspec/specs/` with 31 requirements and 59 scenarios, and six capabilities were updated where a
hook became a real connection. The most consequential of those is `gameplay-framework`, whose command
stream enumerated five producers — a cinematic changing gameplay state would have been a sixth path
with no rules, and is now the sixth producer with the same rules. The unchecked items from section 3
onward are the implementation backlog; **phase 2, compilation with interval indexing, is the
milestone that matters**, because it is what makes cost independent of authored size and what makes
the preload plan exist at all.
