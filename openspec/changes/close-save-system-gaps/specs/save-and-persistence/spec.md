## ADDED Requirements

### Requirement: The world overlay reaches a save through one engine translation
The engine SHALL provide exactly one translation between the world's persistence overlay and the
save overlay, as an engine module above both, and a game or sample SHALL NOT carry its own. The
translation SHALL describe components through the reflected descriptors the ECS component registry
already holds, so that it covers every registered component type without naming any. It SHALL record
a component's persistent fields only, SHALL write a destroyed entity as a tombstone with no field
values, and SHALL refuse — with a structured error rather than silently — world state for which the
save model has no lossless representation. An autosave SHALL capture only the cells recorded against
since the last capture was committed.

#### Scenario: A round trip through a committed save is lossless
- **WHEN** a world overlay is translated, committed as a generation, loaded and translated back
- **THEN** the resulting overlay SHALL equal the original record for record, and a single changed
  field SHALL make the two compare unequal

#### Scenario: Unrepresentable state is refused
- **WHEN** the world overlay holds state the save model cannot carry
- **THEN** the translation SHALL fail with a structured error naming what it could not carry

#### Scenario: An autosave captures what changed
- **WHEN** an autosave runs after changes to a few cells of a large world
- **THEN** only those cells SHALL be translated, and the large-world save benchmark SHALL show the
  autosave's cost independent of the world's size

### Requirement: The save inspector reads a save through the loader and dates fields by generation
The save inspector SHALL read a save through the same manifest decode, chunk verification, migration
and journal replay a game's load uses, and SHALL NOT parse save bytes by any second route, so that
what it reports a save contains is what a load produces. Sizes it attributes to a scope, region,
component or plugin SHALL be measured by encoding with the container's own encoder rather than
estimated. It SHALL report why a save would not be restored under a given policy by naming the
failure's subject — the build, plugin or type — and SHALL still report the save's contents when the
bytes are readable. Because a save records no per-field timestamp, "when a field became dirty" SHALL
be answered as the oldest retained generation, with its simulation point, from which the field has
held its current value, and SHALL say when that answer is bounded by the oldest generation retained.

#### Scenario: Why a field is in a save
- **WHEN** a generation is inspected against the build's schema
- **THEN** each saved field SHALL be reported with its component, field name, persistence trait,
  owning module, entity and the generation it has held its value since, and state of a type the
  schema does not declare SHALL be reported as preserved rather than omitted

#### Scenario: A refused save is still inspectable
- **WHEN** a save is inspected under a policy that would refuse to load it
- **THEN** the inspector SHALL name the reason and its subject, and SHALL still list the save's
  contents

#### Scenario: A diff is semantic
- **WHEN** two generations holding the same logical state in different bytes are compared
- **THEN** the save diff SHALL report no difference

### Requirement: The forbidden save patterns are checked by a runnable checker
The ten forbidden save patterns SHALL each be checked by a tool that can fail: a static check over
save code where the pattern is a shape of code, and a named runtime test of the save path where it is
a property of what a save does. The checker SHALL fail, rather than report the patterns clean, when a
runtime check cannot be run, when a named runtime test no longer exists, or when it finds no save
sources to read, and every static check SHALL be proven to report its pattern when the pattern is
planted.

#### Scenario: A proposal serialising a component by memory copy is flagged
- **WHEN** save code writes a component into a save sink as `sizeof` of its type or through its
  address reinterpreted as bytes
- **THEN** the checker SHALL fail naming the raw-memory pattern, the file and the line

#### Scenario: A failed load invents nothing
- **WHEN** a load fails on a chunk it cannot recover and no older generation loads
- **THEN** the caller's overlay SHALL hold no state from the failed save, and the runtime check for
  invented state SHALL fail if it does
