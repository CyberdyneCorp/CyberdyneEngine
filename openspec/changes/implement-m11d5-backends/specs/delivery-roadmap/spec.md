## ADDED Requirements

### Requirement: A rung is scoped to what the machines it is worked on can judge
A milestone SHALL NOT carry work that no machine available to the project can build or exercise
alongside work that machine can. Where a spike establishes that it does, the unbuildable scope SHALL
move to a rung of its own — with its own gate, its own closing artefact and its own ledger — rather
than being half-built in a rung whose remaining criteria are judgeable.

This is the sibling of the rule that a milestone blocked by its own spike contains two, and of the
rule that a spike may resize a milestone. Those two are about **risk** and **size**; this one is
about **evidence**. A rung that carries work nobody can compile acquires criteria that cannot fail,
which is the defect the whole milestone-gate mechanism exists to prevent, and it acquires them
silently: the ledger still runs, and every one of those criteria reports NOT EVALUATED, which reads
like caution rather than like a rung that cannot be judged.

The receiving rung SHALL carry **the criterion no single machine or continuous-integration leg can
answer alone**, where one exists. A rung that receives unbuildable scope without it can close on
"it compiles somewhere", which is a weaker claim than the scope was moved to preserve.

M11.d.5 was created on exactly this basis: M11.d was written carrying Metal and D3D12, this project
works on a Linux host with one GPU vendor and no Apple toolchain, and the three-backend golden-image
comparison moved with the two backends so the new rung could not close on a compile.

#### Scenario: A rung carries work no available machine can build
- **WHEN** a spike establishes that part of a milestone's scope cannot be compiled or exercised on
  any machine the project can use, and the rest of the scope can
- **THEN** that part SHALL move to an inserted rung with its own gate and closing artefact, and it
  SHALL NOT be half-built in place behind criteria that can only report NOT EVALUATED

#### Scenario: The cross-cutting criterion moves with the scope
- **WHEN** scope moves to a rung of its own and one of the milestone's exit criteria can be answered
  only by comparing several machines or legs
- **THEN** that criterion SHALL move to the receiving rung, so the receiving rung cannot close on a
  claim that one leg compiled

#### Scenario: A claim answered by a device that is not the claimed device
- **WHEN** a criterion is answered on a leg whose device is paravirtual, software or otherwise not
  the hardware the claim is about
- **THEN** the result SHALL name the device that answered, and the shortfall SHALL be recorded as a
  deferral with a re-entry point rather than reported as a pass
