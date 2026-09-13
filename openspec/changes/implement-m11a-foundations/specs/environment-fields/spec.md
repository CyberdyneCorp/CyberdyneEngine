## MODIFIED Requirements

### Requirement: CPU and GPU access
Fields SHALL be samplable from both CPU code and GPU shaders, through interfaces that produce the
same value for the same position and resolution.

GPU access SHALL be through bindless resources reachable from the GPU scene, so a shader can sample
a field without per-draw binding.

CPU access SHALL be batchable, so a system sampling many positions does not pay per-sample
overhead.

**The GPU half is discharged by a shader module that exists and is measured, not by a buffer layout
that a CPU test exercises.** The engine SHALL ship a shader-side sampler over the declared layout,
and the agreement between the two sides SHALL be measured **on a device** at positions inside written
tiles. A comparison in which both sides return the field's declared default SHALL NOT be reported as
agreement: it is the measurement of an empty field, and it is satisfied by a sampler that reads
nothing.

A quantity a consumer samples per vertex, per pixel or per particle SHALL be samplable where it is
consumed. A renderer-facing consumer that can only reach a field by re-sampling it on the processor
is a field that is not GPU-accessible, whatever the layout supports.

#### Scenario: The same answer on both sides
- **WHEN** CPU foliage placement and a GPU terrain material sample the same field at the same point
- **THEN** they SHALL obtain the same value within the field's declared precision

#### Scenario: Bulk sampling
- **WHEN** a system samples a field at ten thousand positions
- **THEN** it SHALL be able to do so in one batched call

#### Scenario: The device half is measured against a device
- **WHEN** the CPU and GPU samplers are compared
- **THEN** the comparison SHALL run on a device, at positions inside written tiles, and SHALL report
  the sampled values rather than only that a shader compiled

#### Scenario: Agreement by absence is not agreement
- **WHEN** every compared position returns the field's declared default
- **THEN** the comparison SHALL fail as unmeasured rather than pass, and SHALL say which side wrote
  nothing

## ADDED Requirements

### Requirement: A producer's write is verified by reading back through the store
A test that a field is produced SHALL assert on values **read back through the store**, at positions
inside the region the producer reports writing. A producer's own statistics — tiles written, cells
evaluated, the extremes it computed — SHALL NOT be the evidence that a field is readable.

This is a defect this project has now hit twice, and the second time it survived a suite written for
it: `CloudShadowField::update` reports tiles darker than 0.5 while `CloudShadowField::sample` returns
the declared 1.0 at every point inside `radius_metres`, and the case stayed green with the producer's
publish suppressed, because every assertion it carried was satisfied by a field nobody had written
to — a declared default is in range, is ordered against itself, and equals itself.

A declared field SHALL therefore have a read-back assertion that distinguishes a published field from
an empty one: a value that differs from the declared default, and a spread across the written region
where the quantity has one.

#### Scenario: Suppressing the publish turns the suite red
- **WHEN** a producer's publish into the store is suppressed
- **THEN** the suite that claims that field SHALL fail, and the demonstration SHALL be recorded with
  the change that claims it

#### Scenario: A default is not a measurement
- **WHEN** every sample inside a producer's written region equals the field's declared default
- **THEN** the field SHALL be reported as unwritten rather than as sampled

### Requirement: A standard field is declared once, by the capability that owns the quantity
A field in the standard set SHALL be declared by exactly **one** capability, and its declaration —
type, encoding, cadence, residency levels and simulation classification — SHALL be that capability's.
Two modules declaring the same field name with different declarations is a configuration error the
registry already refuses, and it makes a project that registers both producers fail at startup rather
than at a conflicting write.

Ownership SHALL follow the quantity rather than the consumer: a capability that **reads** a field
declares nothing. Where two capabilities need what looks like the same quantity, they SHALL first
establish whether it is one quantity under two names — and where the specification already
distinguishes **potential** from **current state**, those are two fields with two owners, not one
field with two encodings.

#### Scenario: Two declarations of one name are a startup failure
- **WHEN** two modules declare the same standard field with different declarations
- **THEN** registration SHALL fail naming the field and both declarations, in either registration
  order

#### Scenario: A consumer declares nothing
- **WHEN** a capability samples a field another capability's specification gives to it
- **THEN** it SHALL consume the declared field rather than declaring its own copy of the quantity
