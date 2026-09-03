## MODIFIED Requirements

### Requirement: Validation and debugging
Development builds SHALL enable backend validation layers, name every resource for debugging
tools, emit debug labels per render graph pass, and support RenderDoc, PIX, and Xcode GPU capture.

The graph SHALL be able to dump its structure — passes, resources, lifetimes, barriers, aliasing
decisions — as text or a Graphviz diagram.

**Breadcrumb markers SHALL be written per pass** where the backend supports it, using the breadcrumb
mechanism in `diagnostics-profiling-and-crash`, so that they survive into a crash artefact when the
trace tail does not.

On **device loss or a graphics fault**, the engine SHALL contribute to the crash artefact: the render
graph as built for the frame, the last submitted and last completed passes, pipeline and shader
identities, resource identities and states, barrier history where available, queue state, the frame
identity, and any driver-reported reason. A driver message alone SHALL NOT be considered the
diagnosis.

GPU pass timings, queue occupancy, barriers, and transient memory SHALL be emitted into the shared
trace, so that GPU behaviour correlates with task, memory, and streaming activity on one timeline.

#### Scenario: GPU crash diagnosis
- **WHEN** the device is lost
- **THEN** the engine SHALL report the last breadcrumb reached, naming the render graph pass, and
  contribute the surrounding graph and pipeline state to the crash artefact

#### Scenario: Validation error fails loudly
- **WHEN** a validation layer reports an error in a development build
- **THEN** the engine SHALL log it with the pass name and, by configuration, break into the
  debugger

#### Scenario: GPU and CPU on one timeline
- **WHEN** a frame is investigated
- **THEN** GPU pass events and CPU task events SHALL appear on the same timeline
