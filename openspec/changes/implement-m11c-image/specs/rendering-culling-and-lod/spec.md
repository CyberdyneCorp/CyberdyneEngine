## ADDED Requirements

### Requirement: An occlusion-culling claim is made on a device or is not made
"Occlusion culling" is currently refused by name on the device — `GpuCullPass::upload` rejects the
occlusion flag rather than dispatching a pass that would report "nothing was occluded" and be
indistinguishable from a working one. That refusal is right, and it creates the risk this requirement
addresses: a **CPU model** of a hierarchical depth buffer exists and is tested, and a model passing
its own tests is the most available way to record a capability that no frame performs.

The engine SHALL distinguish the model from the pass. A claim that occlusion culling works SHALL be
evaluated on a device, and the device pass SHALL be checked against the CPU model's answer for the
same scene rather than against a screenshot or a counter that reads non-zero.

Where no device is available, the claim SHALL be reported as **not evaluated** rather than satisfied
by the model, through the same mechanism the project uses for a question a host cannot ask.

One hierarchical depth buffer SHALL serve both this capability's occlusion culling and
`virtual-geometry`'s cluster-granular occlusion; two implementations of the same structure would let
one piece of work be recorded as two satisfied requirements.

#### Scenario: The model stands in for the pass
- **WHEN** the CPU occlusion model passes and no device pass exists
- **THEN** the capability's occlusion claim SHALL be reported as not evaluated, and SHALL NOT be
  recorded as satisfied

#### Scenario: The device pass disagrees with the model
- **WHEN** the device occlusion pass and the CPU model are run over the same scene and disagree
- **THEN** the disagreement SHALL be reported as a failure, naming the counts, rather than resolved
  in favour of the device

#### Scenario: One buffer, two consumers
- **WHEN** cluster-granular occlusion for virtual geometry is implemented
- **THEN** it SHALL consume the same hierarchical depth buffer this capability's culling uses, and
  the criteria for the two SHALL NOT be the same command reported twice
