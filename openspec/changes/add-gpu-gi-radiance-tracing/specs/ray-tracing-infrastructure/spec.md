## ADDED Requirements

### Requirement: The service's structures are built on a device from its decisions
The ray tracing service SHALL be able to keep a device's acceleration structures in step with its
own decisions through a mirror it informs of every build, refit, eviction and top level. The device
SHALL build what it is told and nothing else: a refit SHALL be an in-place update of the bottom
level, a moved instance an update of the top level, and a frame in which nothing changed SHALL record
no build. Ray queries over the device structures SHALL agree with the service's own queries.

#### Scenario: Structure maintenance on a device
- **WHEN** an instance moves, a refit geometry is posed, an instance is added and a geometry is
  evicted
- **THEN** the device SHALL update the top level, update the bottom level in place, rebuild the top
  level and evict the structure respectively, and a ray query after each SHALL agree with the
  service's query

#### Scenario: A device without ray queries
- **WHEN** a device does not report ray tracing
- **THEN** creating a device structure SHALL fail as unsupported, and consumers SHALL take their
  non-ray-traced path
