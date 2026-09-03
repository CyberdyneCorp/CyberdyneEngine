## ADDED Requirements

### Requirement: Typed spatial datasets
Procedural generation SHALL operate on **typed spatial datasets**, not on arbitrary engine objects:
point sets, spatial volumes, surfaces, splines, fields, geometry, attribute tables, entity sets,
regions, and rasters.

A generator SHALL be a transformation from datasets to datasets. It SHALL NOT be a procedure that
directly creates runtime objects.

Dataset elements SHALL carry **typed attributes** addressed by compiled identifiers, using the
identity mechanism in `core-type-system`. Attribute names SHALL NOT be resolved by string lookup at
execution time.

#### Scenario: One infrastructure, many content kinds
- **WHEN** forests, buildings, resource nodes, and spawn regions are generated
- **THEN** each SHALL use the same dataset and transformation infrastructure rather than a
  content-specific tool

#### Scenario: Attributes are compiled
- **WHEN** a generator reads a point's species attribute
- **THEN** it SHALL use a compiled identifier, not a string key

### Requirement: Graphs compile to programs
Generators SHALL be authored as **graphs** and compiled through a typed intermediate representation
into an executable **program**.

The compiler SHALL apply at minimum: constant folding, dead-node elimination, filter fusion, spatial
query fusion, attribute projection, parallelisation analysis, and classification of nodes eligible
for GPU execution.

Execution SHALL run the compiled program. Interpreted graphs of virtual node objects SHALL NOT be
executed in hot paths.

Graphs SHALL support **subgraphs** with typed exposed parameters, so that large generators remain
composable.

#### Scenario: The graph is authoring only
- **WHEN** a generator runs
- **THEN** it SHALL execute a compiled program, and the authoring graph SHALL not be traversed

#### Scenario: Unused work is removed
- **WHEN** a graph computes an attribute nothing consumes
- **THEN** the compiler SHALL eliminate it and be able to report that it did

### Requirement: Execution domains
Every generator SHALL declare the **domains** in which it may execute: editor, cook, runtime,
streaming, or dynamic.

A generator not declared for a domain SHALL NOT run in it. A city generator intended for cooking
SHALL NOT be invocable at runtime by accident.

Runtime and streaming domains SHALL declare their budgets, and execution SHALL be scheduled through
the task system under those budgets.

#### Scenario: Build-time work stays at build time
- **WHEN** an expensive generator is declared editor and cook only
- **THEN** an attempt to run it at runtime SHALL be rejected with a diagnostic

#### Scenario: Runtime generation is budgeted
- **WHEN** a regrowth generator runs during play
- **THEN** it SHALL execute within its declared task and memory budget, incrementally if necessary

### Requirement: Deterministic derivation
Generation SHALL derive its randomness from **stable inputs**: world seed, generator identity,
generator version, region identity, and input dataset hashes — using the random stream mechanism in
`simulation-and-determinism`.

A global sequential generator SHALL NOT be used. Changing one region SHALL NOT alter the content of
another.

Each generator SHALL declare a determinism level consistent with the session's determinism profile,
so that decorative generation need not pay for reproducibility that gameplay-relevant generation
requires.

#### Scenario: Regions are independent
- **WHEN** one region's parameters change and it is regenerated
- **THEN** neighbouring regions' generated content SHALL be unchanged

#### Scenario: Gameplay-relevant generation is reproducible
- **WHEN** resource placement matters competitively
- **THEN** its generator SHALL declare a determinism level and produce identical results for the
  same inputs

### Requirement: Stable generated identity
Every generated output SHALL carry a **stable identity** derived from generator, region, and a stable
per-output key.

Identity SHALL NOT derive from iteration order, array index, insertion order, or worker scheduling.

Regeneration with unchanged inputs SHALL produce the same identities. Regeneration with changed
inputs SHALL preserve the identities of outputs whose determining inputs did not change, where the
generator's structure permits.

Stable identity is what allows author overrides, persistent state, damage, and destruction to remain
attached to the intended object across regeneration.

#### Scenario: A felled tree does not return
- **WHEN** a region is regenerated after a player felled a generated tree
- **THEN** the persistence delta SHALL still refer to that tree and it SHALL remain felled

#### Scenario: Order does not decide identity
- **WHEN** generation runs with different worker counts
- **THEN** generated identities SHALL be unchanged

### Requirement: Generation regions
Generation SHALL be partitioned into **regions**, and world-scale generators SHALL NOT execute as one
operation.

Region size SHALL be a per-generator property. It SHALL NOT be required to match world partition cell
size, since a road network's domain and a grass patch's are not the same scale.

Generators SHALL support a **hierarchy**: macro generation producing regional inputs, regional
producing local, local producing detail — so that scale is expressed by composition rather than by
one enormous graph.

#### Scenario: A road network spans regions
- **WHEN** a road generator operates over a large domain
- **THEN** it SHALL declare a region size appropriate to it, independent of world cell size

#### Scenario: Scale composes
- **WHEN** climate produces biomes, biomes produce forests, and forests produce undergrowth
- **THEN** each SHALL be a generator consuming the previous stage's outputs

### Requirement: Dependencies and spatial invalidation
Generators SHALL declare their **inputs, outputs, and sampling radius**, so that the effect of a
change is computable rather than guessed.

An edit SHALL produce a **dirty set**: the regions of each dependent generator whose inputs the edit
touched, expanded by each consumer's declared sampling radius.

Regeneration SHALL process only the dirty set. Editing one road segment SHALL NOT regenerate
unrelated regions.

The dependency graph SHALL be inspectable, and the reason a region is dirty SHALL be reportable.

#### Scenario: A road edit is scoped
- **WHEN** five hundred metres of road are moved
- **THEN** terrain stamps along it, foliage within the declared clearance radius, and settlement
  placement reading road access SHALL be invalidated, and nothing else

#### Scenario: Why is this dirty
- **WHEN** a region regenerates
- **THEN** the tooling SHALL name the change and the dependency path that reached it

### Requirement: Caching and distribution
Each region's generation SHALL have a **derivation key** hashing: the compiled program, the generator
version, region identity, input dataset hashes, field versions, parameters, and platform where
relevant.

Results SHALL be stored in the derived data cache defined in `build-and-packaging`, so that identical
generation is never repeated and generation distributes across build workers without additional
mechanism.

Generation SHALL therefore be a set of derivations in the build graph, with declared inputs and
immutable outputs, like any other build node.

#### Scenario: Generation is shared
- **WHEN** continuous integration has generated a region
- **THEN** a developer on that branch SHALL fetch the result rather than regenerating it

#### Scenario: Generation distributes
- **WHEN** a large world is generated
- **THEN** independent regions SHALL be executable on remote workers

### Requirement: Output adapters
Generators SHALL produce results through **output adapters**, and the adapter SHALL determine the
representation:

| Output | Representation |
|---|---|
| Foliage | Compact instance populations in clusters (see `foliage`) |
| Terrain | Stamps and layers in the non-destructive modifier stack (see `terrain`) |
| Fields | Field tiles (see `environment-fields`) |
| Entities | Entity template instances, flattened into cells at cook or batch-instantiated at runtime |
| Splines | Spline networks for roads, rivers, and rails |
| Geometry | Assets through the asset pipeline |
| World metadata | Layers, navigation modifiers, region annotations |

**A procedural result SHALL NOT be an entity by default.** A generator producing ten million trees
SHALL produce a foliage population, not ten million spawn operations.

Projects and plugins SHALL be able to register adapters through the extension points in
`project-and-plugins`.

#### Scenario: Ten million trees are a population
- **WHEN** a forest generator produces ten million instances
- **THEN** they SHALL become foliage clusters, and no entities SHALL be created

#### Scenario: The right representation per target
- **WHEN** one generator produces both terrain modification and vegetation
- **THEN** each output SHALL go through its adapter to the representation that subsystem uses

### Requirement: Field integration
Generators SHALL be able to **read** any environment field — height, slope, curvature, biome,
moisture, temperature, water distance, road distance, wind exposure, snow, burn state — as first-class
inputs.

Generators SHALL be able to **write** fields where they are the declared producer of one: a road
generator writing road distance, a fire simulation writing burn state, a terraforming system writing
soil health.

Field reads SHALL participate in dependency tracking, so that a field change invalidates the
generators that sample it, within their declared radius.

#### Scenario: Rules are field expressions
- **WHEN** a forest generator selects locations
- **THEN** it SHALL express its rules over field values rather than over painted masks

#### Scenario: A field change invalidates consumers
- **WHEN** moisture changes in a region
- **THEN** generators sampling moisture there SHALL be invalidated within their declared radius

### Requirement: Overrides and regeneration
Authors SHALL be able to modify generated results — delete, move, replace, add, lock, or change
attributes of a generated instance — and those modifications SHALL be stored as an **override layer**
over the generated base.

```
generated base + author overrides = authored result
```

Regeneration SHALL **merge** overrides by stable identity. An override whose target survives
regeneration SHALL survive with it.

An override whose target no longer exists SHALL become an **orphan**: retained, reported, and
resolvable — never silently discarded. Regenerating a region must not quietly delete a designer's
work.

Locked instances SHALL be preserved through regeneration.

#### Scenario: A moved tree stays moved
- **WHEN** a designer moves a generated tree and the region is regenerated
- **THEN** the override SHALL reattach by identity and the tree SHALL remain where it was placed

#### Scenario: A rule change orphans an override
- **WHEN** a rule change removes the instance an override referred to
- **THEN** the override SHALL be reported as orphaned rather than dropped

### Requirement: Provenance
Development builds SHALL retain **provenance** for generated results: the generator, its version, the
region, the seed, the graph node that produced the output, and the attribute values that determined
it.

The editor SHALL answer **why is this here** for any generated instance, naming the rule and the
values that selected it.

The editor SHALL also answer **why is nothing here** for a location: which filter rejected it, with
the value tested and the threshold applied.

Provenance SHALL be strippable from shipping builds.

#### Scenario: Why is this tree here
- **WHEN** a designer selects a generated tree
- **THEN** the editor SHALL report the generator, region, seed, rule, and the field values that
  selected it

#### Scenario: Why is nothing here
- **WHEN** a designer selects an empty area expecting vegetation
- **THEN** the editor SHALL report which filter rejected it and by how much

### Requirement: CPU and GPU execution
The compiler SHALL classify nodes by execution suitability, and the runtime SHALL be able to execute
generation on the **CPU**, on the **GPU**, or as a hybrid.

Authors SHALL NOT be required to maintain separate graphs for CPU and GPU execution.

GPU-suitable work SHALL include large-scale candidate generation, field sampling, noise, density
evaluation, and filtering. CPU work SHALL include entity template construction, complex constraint
solving, navigation queries, and world persistence integration.

A generator declared deterministic SHALL only use GPU execution where that execution meets its
declared determinism level.

#### Scenario: Millions of candidates on the GPU
- **WHEN** a scatter generates millions of candidate points
- **THEN** candidate generation and filtering SHALL be executable on the GPU

#### Scenario: Determinism constrains the path
- **WHEN** a generator requires cross-platform determinism
- **THEN** it SHALL NOT be scheduled on an execution path that cannot provide it

### Requirement: Spatial queries
Generators SHALL have access to a common **spatial query** service over: terrain surfaces and height,
coarse geometry, world objects, navigation, fields, and other PCG datasets.

Build-time generation SHALL NOT require a running physics world in order to perform geometric
queries; canonical geometric representations SHALL be used.

Queries SHALL be batchable, since generation issues them in bulk.

#### Scenario: Cooking needs no physics world
- **WHEN** a generator tests ground height and slope during cooking
- **THEN** it SHALL query terrain and geometry representations directly

### Requirement: Bounded iteration
Iterative operations SHALL be provided as **bounded, specialised nodes** — relaxation, fixed-count
iteration, growth, flood fill — rather than as general unbounded loops in the graph.

A general scripted node MAY exist and SHALL be marked as an optimisation and parallelisation
barrier, so its cost is visible.

Every iterative node SHALL declare a bound, so that generation cannot fail to terminate.

#### Scenario: Generation terminates
- **WHEN** a relaxation node runs
- **THEN** it SHALL respect its declared iteration bound

#### Scenario: A barrier is visible
- **WHEN** a scripted node prevents fusion or parallelisation
- **THEN** the compiler SHALL report it

### Requirement: Runtime generation
Generation at runtime SHALL be **incremental and budgeted**: scheduled through the task system,
cancellable, and bounded by declared processor, GPU, and memory budgets.

Runtime generation SHALL be requestable with a **deadline** through the residency layer, so that a
region approaching relevance is generated before it is needed rather than when it is.

A runtime generator SHALL NOT block simulation or presentation. Partial results SHALL be publishable
where the output representation permits.

#### Scenario: Regrowth does not hitch
- **WHEN** vegetation regrows across a large area
- **THEN** generation SHALL proceed incrementally within budget rather than in one operation

#### Scenario: Generation has a deadline
- **WHEN** the world predicts arrival at an ungenerated region
- **THEN** its generation SHALL be requested against that deadline

### Requirement: Macro state and materialisation
Regions that are not resident SHALL be representable by **macro state** — a small number of field
values such as vegetation density, forest age, burn fraction, and soil health — that evolves without
detail existing.

When a region becomes relevant, PCG SHALL **materialise detail deterministically consistent with its
macro state**, so that a forest that grew while unloaded appears as a grown forest.

Demotion SHALL update macro state from the detailed representation, so the round trip does not lose
what happened.

#### Scenario: A distant forest grows
- **WHEN** a region is unloaded for a long period
- **THEN** its vegetation state SHALL evolve as a few field values, without instantiating trees

#### Scenario: Materialisation matches state
- **WHEN** the region becomes relevant
- **THEN** the generated detail SHALL be consistent with its macro state rather than regenerated
  from scratch as if nothing had happened

### Requirement: Persistence of generated content
Generated base content SHALL NOT be saved. A save SHALL record the generator version and seed where
needed, plus **persistent exceptions**: instances removed, modified, or added by gameplay or
authoring.

```
generated base + persistent delta = current procedural world
```

Exceptions SHALL be anchored by stable identity and spatially, so they can be re-resolved after a
generator version change, and reported as orphaned when they cannot.

#### Scenario: A cleared forest saves cheaply
- **WHEN** a player fells two hundred trees in a generated forest of a million
- **THEN** the save SHALL record two hundred exceptions

#### Scenario: A generator change is survivable
- **WHEN** a generator version changes
- **THEN** exceptions SHALL be re-resolved where possible and reported where not

### Requirement: Networking of generated content
Where generation is deterministic, peers SHALL be able to generate identical content locally from the
shared seed and version rather than receiving it.

Where it is not, or where the content is authoritative, the authority SHALL replicate the generated
result or the persistent deltas, according to networking policy.

A session SHALL be able to verify that peers agree on generator versions, so that a mismatch is
detected at connection rather than as divergent worlds.

#### Scenario: Bandwidth is not spent on trees
- **WHEN** a deterministic forest generator is used
- **THEN** peers SHALL generate it locally from seed and version

#### Scenario: Version mismatch is caught
- **WHEN** a peer has a different generator version
- **THEN** the mismatch SHALL be detected at connection

### Requirement: PCG diagnostics
The editor SHALL provide: a graph editor with live preview, a region debugger showing generated and
rejected candidates, an attribute inspector, a seed inspector, the dependency graph, the dirty set
and its causes, the override and orphan list, and a generation profiler.

The profiler SHALL report per node: processor and GPU time, candidates in and out, memory, and cache
hit rate — so that a slow generator is attributable to a node rather than to the graph.

Viewport visualisation SHALL include candidate points, accepted and rejected results, density,
region bounds, sampled fields, and dirty regions.

#### Scenario: The slow node is identified
- **WHEN** a generator is slow
- **THEN** the profiler SHALL show which nodes dominate its time

#### Scenario: Rejection is visible
- **WHEN** a designer inspects a region
- **THEN** rejected candidates and the filter that rejected them SHALL be visualisable

### Requirement: PCG performance
Generation SHALL support regions containing **millions of candidate points** without per-point heap
allocation, with major scatter and filter operations executing in parallel.

Region results SHALL be independently cacheable, so a planet-scale world never requires global
detailed output to be resident.

The engine SHALL maintain a **large-world generation benchmark**: a hundred-kilometre-square world
with climate and biome fields, terrain analysis, a road network, settlements, ten million trees, and
ground cover at conceptual scale — where modifying one road segment regenerates only the affected
terrain stamps, nearby vegetation, and dependent settlement placement, unrelated regions come from
cache, and author overrides survive.

#### Scenario: The benchmark is incremental regeneration
- **WHEN** the large-world benchmark modifies one road segment
- **THEN** only dependent regions SHALL regenerate, the rest SHALL be cache hits, and overrides
  SHALL survive

#### Scenario: No per-point allocation
- **WHEN** a region generates millions of candidates
- **THEN** no per-point heap allocation SHALL occur

### Requirement: Forbidden PCG patterns
The following SHALL NOT appear, and each SHALL be checkable:

- Producing one heavyweight runtime object per generated instance where a compact representation
  exists
- Generated identity derived from iteration order, index, or worker scheduling
- Regeneration silently discarding valid author overrides
- Interpreted virtual-node graph traversal in a hot execution path
- Editing a small region requiring regeneration of unrelated regions
- Runtime generation bypassing task scheduling, memory budgets, or streaming budgets
- Saving generated base content rather than exceptions
- A global sequential random generator as the source of procedural variation

#### Scenario: A proposal is checked
- **WHEN** a change would spawn an entity per generated instance
- **THEN** it SHALL be flagged against this requirement
