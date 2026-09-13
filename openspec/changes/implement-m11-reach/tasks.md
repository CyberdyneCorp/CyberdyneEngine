# Tasks: M11 — Reach

**THIS WAS A DECISION LIST AND THE DECISIONS ARE TAKEN.** The change was opened by M10's closing gate
(M10 task 8.5, checked by `m10:m11-open`) so the ladder continued as a deliberate act, and section 0
had to be answered before any of the rest could be written honestly. **It is answered: M11 is five
rungs.** Sections 1 to 3 below were the unexpanded scope; each now names the rung that owns it, and
the work is in that rung's own `tasks.md`.

## 0. The decisions that scope this milestone

- [x] 0.1 **Is M11 one milestone or two?** **ANSWERED: five.** The matrix had said since M6 that M11
      was the one milestone that could reasonably be split, "through a change if the work turns out
      to be separable along a real seam rather than an arbitrary one". Its load reached **65 of 76
      capabilities**, seventeen of which arrived at M10's gate alone. The seam is **the artefact**:
      a milestone whose scope cannot be judged by one closing artefact is several milestones sharing
      a number, because a gate that cannot name what it is looking at is not a gate. The rungs are
      `implement-m11a-foundations`, `implement-m11b-authoring`, `implement-m11c-image`,
      `implement-m11d-desktop` and `implement-m11e-ship` — 12, 24, 15, 10 and 4 rows, 233, 420, 232,
      129 and 55 requirements, every one of the 65 rows owned exactly once.
- [x] 0.1.1 **The ladder mechanics, all in one commit, because they cannot be separated.**
      `record.MILESTONES` loses `m11` and gains `m11a` … `m11e` in position; the seven inherited gaps
      are re-pointed at the rung that closes each (`criteria._check_known_gap` refuses a
      `known_gap_closes` the ladder does not carry, so the two halves are one edit); five ledgers;
      five gates at `joins-on-close`; five floors in `selftest.MINIMUM_CRITERIA`; five matrix columns
      and five load rows; five ROADMAP sections; the dependency subgraph; and risks entry 12.
- [x] 0.1.2 **The three readers that drop a split milestone's columns**, which M8's split found the
      hard way, were widened from `.5|.a-.c` to `.5|.a-.e` — and M11.d and M11.e proved the point by
      vanishing from the matrix until they were. `debts.py`'s label renderer had the same defect and
      printed `M11E`. `_check_every_reader_admits_an_insertion` now names **every** suffix on the
      ladder rather than the ones the last split needed.
- [x] 0.1.3 **The handover criterion between rungs opened together.** `m11b-open` … `m11e-open` could
      not be `kind = "path"` over `openspec/changes/**/*m11x*/proposal.md`, because the split opened
      all five changes at once: such a criterion would pass the moment it was written and could never
      go red, and **a criterion that cannot fail is not a criterion**. The glob is kept as the search
      and what is asked of the result is **entry** rather than existence — a checked task in the
      *body* of the next rung's list, not in its section 0, because a split can answer a rung's
      scoping section in advance and two of the five already had one checked when this was written.
      All four fail today. The rule is in this change's
      `delivery-roadmap` delta so the ladder has one handover shape rather than five. The last rung
      has **no** `*-open` criterion and says so in its notes; `m11e:ladder-ends-here` replaces it.
- [x] 0.2 **The milestone's named spike.** M11 had none, because M11 had no scope. **Each rung now
      names its own**, recorded in `docs/roadmap/risks.md` entry 12 and run at the head of its rung
      rather than inside it: M11.a ports one band to a shader and measures it; M11.b asks whether the
      hosted runtime carries three play modes without a second world model; M11.c authors one material
      end to end; M11.d settles the eight RHI interface gaps on Vulkan and null, and asks first
      whether a hosted runner can present a device; M11.e asks whether a hosted runner can produce a
      mobile artefact at all. **None has run**, and nothing downstream of one should be read as
      settled until it has.
- [x] 0.3 **What `save-and-persistence` actually costs.** **ANSWERED: it is M11.a's, and M11.a
      re-scopes it through a change against its own specification before recording it either way** —
      which is what `delivery-roadmap` requires of a capability demoted twice. The AEAD half is a
      dependency adoption and goes through the OpenSpec change flow with its evaluation recorded, as
      `thirdparty-dependencies` requires. M11.a names the row as the one it predicts it will demote.

## 1. The three inherited gaps that are one piece of work → **M11.a**

`m10:fields-sampled-on-a-device`, `m10:world-frame-budget` and a field sampler on the device. Scoped
together, in `implement-m11a-foundations/tasks.md` §1, with `m10:sky-field-round-trip` and
`m10:fields-one-vegetation-potential` beside them. All four gaps now name `m11a`.

- [x] 1.1 Allocated to M11.a — `cy/field.slang` and the shader-side sampler
- [x] 1.2 Allocated to M11.a — the three budget bands, in the order the gap names them
- [x] 1.3 Allocated to M11.a — the sky's write path, the parked assertions, and the consumers
- [x] 1.4 Allocated to M11.a — one `vegetation-potential`, a modelling decision across two rows

## 2. The one CI job that answers three criteria → **M11.a**

- [x] 2.1 Allocated to M11.a — the digest-publishing, digest-comparing job.
      `m9:lockstep-cross-platform` now names `m11a`; `m10:pcg-regeneration-cross-platform` and
      `m10:pcg-gpu-domain-agreement` are answered by the same job and stay reported NOT EVALUATED
      until it exists

## 3. The milestone as planned → **the five rungs**

- [x] 3.1 `rhi-and-render-graph` → **M11.d**
- [x] 3.2 `core-platform-abstraction` → **M11.d**
- [x] 3.3 `build-system-and-platforms` → **M11.e**
- [x] 3.4 `build-and-packaging` → **M11.d**, with downloadable content and distributed execution at
      **M11.e**
- [x] 3.5 `rendering-forward-clustered` → **M11.e** for the cell; MSAA and multi-view at **M11.d**
- [x] 3.6 `m8c:steam-audio-configures` → **M11.a**, with `audio`
- [x] 3.7 `testing-and-quality` → **M11.d**
- [x] 3.8 Every remaining capability → allocated across the five rungs, each row owned exactly once;
      `xr-support` stays deferred at **M11.e**, the only rung permitted to record a deferral
- [x] 3.9 `samples/11-ship` → **M11.d** on desktop, **M11.e** everywhere

## 4. What this change does not do

- [ ] 4.1 **It does not implement anything.** Every row above is scoped in a rung's own change, and
      this one stays as the record of the split. It is not archived into a rung and it is not deleted
- [ ] 4.2 **It does not record a tier.** Recording a tier is a rung's closing gate, and each rung's
      `roadmap-tiers` criterion fails until its own closing change writes them — deliberately, as
      M8.a's and M8.b's ledgers did before it
