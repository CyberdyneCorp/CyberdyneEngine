## ADDED Requirements

### Requirement: Jitter is a fraction of a rendered pixel
When the scene is rendered below the view's output extent, the jitter offset SHALL be applied as a
fraction of a RENDERED pixel, and a change of the render extent alone SHALL NOT be an invalidation
event: histories declared against the output extent survive it. The framework SHALL supply the
camera-relative to clip transform of the current and previous view, so a consumer reprojects through
the same matrices the prepass derived motion with.

#### Scenario: A jittered projection at the render extent
- **WHEN** a view's render extent is half its output extent
- **THEN** `jittered_view_projection()` SHALL offset the projection by the jitter over the render
  extent, not the output extent

#### Scenario: Dynamic resolution is not a cut
- **WHEN** the render extent changes between two frames and the output extent does not
- **THEN** no history SHALL be invalidated and no invalidation cause SHALL be reported
