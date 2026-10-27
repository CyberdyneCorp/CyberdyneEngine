## ADDED Requirements

### Requirement: Screen probe placement is reproducible
Where screen-space final gather places adaptive probes and a frame's adaptive capacity can be spent,
the probes kept SHALL be chosen in a fixed order — the screen's cell order — on every backend, so
that the device places what the host reference places and a frame is the same on every run. A
device SHALL NOT keep whichever candidates its threads happened to reach first.

#### Scenario: A spent capacity
- **WHEN** more cells ask for an adaptive probe than a frame keeps
- **THEN** the device SHALL keep the same candidates the host reference keeps and refuse the same
  ones, and two runs SHALL produce the same probes and the same image
