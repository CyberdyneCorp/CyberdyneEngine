## ADDED Requirements

### Requirement: A read waits for the layout transition an earlier read made
When a read changes an image's layout, the transition is a write; a later read in the same layout,
in a pipeline stage the transition was not made visible to, SHALL be ordered after that transition
by the barrier the graph derives for it, as well as after the image's last write.

#### Scenario: Sampled by compute, then by a fragment shader
- **WHEN** an image written as an attachment is sampled by a compute pass, whose barrier transitions
  it, and then sampled by a fragment shader in the same layout
- **THEN** the fragment reader's barrier SHALL name the compute stage in its first scope beside the
  attachment write's, and synchronisation validation SHALL report no hazard
