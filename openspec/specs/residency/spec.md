# residency Specification

## Purpose

Defines the shared policy layer beneath every paged subsystem: virtual geometry, virtual textures,
virtual shadows, and the illumination caches.

Each of those independently invented the same vocabulary — request, priority, residency, budget,
eviction, prefetch — which is a strong signal that the policy is one thing. It is equally a trap:
their storage genuinely differs. A geometry page is immutable and content-addressed; a texture page
may be produced at runtime; a shadow page is *rendered*, is invalidated by unrelated objects moving,
and is worth keeping precisely because regenerating it is expensive.

So this capability owns **policy and observability** and explicitly does not own storage. It scores
requests comparably across subsystems, coordinates reduction under memory pressure instead of
letting each evict independently, controls churn, and answers *why is this not resident*.

Two of its requirements reach furthest. **Render importance becomes one value** — published per
instance and consumed by geometry detail, texture priority, shadow resolution, animation rate and
illumination quality — so a hero unit is important once rather than five times. And **deadline
propagation** turns one prediction into many preparations: when the world predicts arrival at a
region, that becomes geometry, texture, shadow, illumination and audio deadlines, rather than five
subsystems discovering the same future separately and disagreeing about how soon.

## Requirements

### Requirement: Shared policy, separate storage
The engine SHALL provide a **residency layer** owning the policy shared by every paged subsystem:
importance, priority scoring, budgets, eviction rules, hysteresis, prefetch and deadline
propagation, pressure response, and diagnostics.

Each paged subsystem SHALL retain its **own storage and update mechanism**: virtual geometry pages,
virtual texture tiles, virtual shadow pages, and illumination caches differ in lifetime, mutability,
and cost to regenerate, and a single physical cache serving all of them would serve none well.

The residency layer SHALL NOT own page storage, page formats, or page production. A change that
moves storage into it SHALL be treated as violating this requirement.

#### Scenario: One policy, four caches
- **WHEN** geometry, texture, shadow, and illumination pages compete for memory
- **THEN** they SHALL be scored and evicted by one policy, while each keeps its own cache

#### Scenario: A shadow page is not a geometry page
- **WHEN** eviction is considered
- **THEN** the policy SHALL account for the cost of regenerating a page, so an expensive rendered
  shadow page and a cheap streamed geometry page are not treated identically

### Requirement: Unified render importance
Renderable instances SHALL carry a single **render importance** value, published in the GPU scene
and consumed by every quality decision: geometry detail, texture page priority, shadow page
resolution and refresh rate, animation update rate, and illumination quality.

Importance SHALL derive from screen coverage combined with a gameplay-supplied component, so that
systems can mark what matters without knowing how each consumer will use it.

Subsystems SHALL NOT maintain independent notions of importance for the same instance. Where a
subsystem needs a different weighting, it SHALL apply a declared transform of the shared value
rather than compute its own.

#### Scenario: A hero unit is important once
- **WHEN** gameplay marks a unit as important
- **THEN** its geometry, textures, shadows, animation, and illumination SHALL all receive higher
  quality, from one declaration

#### Scenario: Weighting, not redefinition
- **WHEN** shadows weight distance more strongly than textures do
- **THEN** they SHALL apply a declared transform of the shared importance, not a separate importance
  model

### Requirement: Request priority
Every residency request SHALL be scored from: importance, screen coverage, the detail deficit
between what is wanted and what is resident, prediction confidence, time until needed, age, and the
cost of producing or fetching the page.

Scoring SHALL be defined once and applied by every subsystem, so that a texture page and a shadow
page competing for the same budget are comparable.

Requests SHALL be **deduplicated and compacted before scheduling**, and where a subsystem generates
requests on the GPU that compaction SHALL happen there.

#### Scenario: Comparable across subsystems
- **WHEN** memory pressure forces a choice between a texture page and a shadow page
- **THEN** both SHALL have been scored by the same policy and the decision SHALL be explicable

#### Scenario: Requests are compacted
- **WHEN** a million pixels request the same page
- **THEN** one request SHALL reach the scheduler

### Requirement: Deadline propagation
When a subsystem predicts a future need — the world predicting arrival at a region, a cinematic
declaring a camera cut, a teleport being initiated — that prediction SHALL be expressed **once** as
a deadline and propagated to every consumer: geometry pages, texture pages, shadow warm-up,
illumination prefetch, audio preload, and world cell preparation.

Subsystems SHALL NOT each derive the same prediction independently, since independent derivations
disagree about how soon.

A deadline SHALL be a scheduling input, consistent with the task system's rule that deadlines
influence when work runs and never what the simulation computes.

#### Scenario: One prediction, many preparations
- **WHEN** the world predicts the camera reaching a region in a known time
- **THEN** geometry, texture, shadow, illumination, and audio work for that region SHALL be
  scheduled against that deadline, from one prediction

#### Scenario: A camera cut is announced
- **WHEN** a cinematic declares a cut ahead of time
- **THEN** the destination's content SHALL be requested with an urgent deadline rather than
  discovered after the cut

### Requirement: Budgets and pressure response
Each paged subsystem SHALL hold a **memory budget** from the memory budget tree (see
`core-memory-and-containers`), and the residency layer SHALL coordinate them.

On rising memory pressure the layer SHALL apply a **coordinated reduction** across subsystems
weighted by importance and by visible impact, rather than each subsystem independently evicting —
which produces one subsystem freeing memory another immediately consumes.

Reduction levers SHALL be declared per subsystem: texture mip bias and prefetch radius, geometry
error threshold, shadow page resolution and refresh rate, illumination cache density.

Adjustment SHALL be hysteretic, and pinned mode SHALL disable coordinated adjustment together with
the renderer's budget arbiter.

#### Scenario: Coordinated reduction
- **WHEN** GPU memory pressure becomes elevated
- **THEN** the layer SHALL reduce across subsystems in a declared order, rather than each evicting
  independently

#### Scenario: Gameplay-critical content is protected
- **WHEN** reduction is applied
- **THEN** high-importance instances SHALL retain quality while background content degrades first

### Requirement: Eviction and churn control
Eviction SHALL consider recency, importance, screen contribution, regeneration cost, and whether a
page is pinned or guaranteed resident.

A page SHALL have a **minimum residency age** before becoming evictable, and eviction SHALL apply
hysteresis, so that a page evicted this frame is not requested again the next.

**Churn** — pages evicted and re-requested within a short window — SHALL be measured and reported
per subsystem, since sustained churn indicates a budget too small or a policy misconfigured, and is
invisible in hit-rate statistics alone.

#### Scenario: No oscillation
- **WHEN** demand slightly exceeds the budget
- **THEN** minimum residency age and hysteresis SHALL prevent pages cycling in and out each frame

#### Scenario: Churn is visible
- **WHEN** pages are repeatedly evicted and re-fetched
- **THEN** the churn rate SHALL be reported per subsystem

### Requirement: No residency system blocks another
A residency system SHALL NOT block, within a frame, on another residency system.

Where one subsystem's work requires another's data — shadow rasterisation requiring an opacity
texture, illumination requiring geometry — the dependency SHALL be satisfied by a **guaranteed
resident coarse representation**, and the fine representation SHALL be requested for a later frame.

Circular residency dependencies SHALL be detected and reported at configuration time.

#### Scenario: Shadows do not wait for textures
- **WHEN** a shadow page must rasterise masked geometry whose opacity texture's fine pages are not
  resident
- **THEN** the guaranteed coarse representation SHALL be used and the page marked for refresh, and
  the frame SHALL NOT stall

#### Scenario: A cycle is a configuration error
- **WHEN** two subsystems declare hard residency dependencies on each other
- **THEN** the cycle SHALL be reported rather than discovered as a stall

### Requirement: Residency diagnostics
The layer SHALL report, per subsystem and in aggregate: resident and requested page counts, cache
hit rate, fetch and production rate, bytes in use against budget, churn, evictions with cause,
outstanding deadlines and misses, and prediction accuracy.

For any page or any screen pixel, the system SHALL be able to answer **why it is at its current
quality**: whether the cause is that it was never requested, was outscored, is budget-blocked, is
awaiting production, missed a deadline, or is deliberately capped by pressure.

#### Scenario: Why is this blurry
- **WHEN** a surface renders at a lower detail than expected
- **THEN** the diagnostics SHALL name the cause and the desired and resident levels

#### Scenario: Prediction quality is measurable
- **WHEN** predictive prefetching is evaluated
- **THEN** the fraction of prefetched pages actually sampled SHALL be reported, so prediction can be
  tuned against evidence
