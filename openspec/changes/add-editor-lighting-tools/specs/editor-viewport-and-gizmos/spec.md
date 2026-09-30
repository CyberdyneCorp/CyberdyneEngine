# editor-viewport-and-gizmos Spec Delta

## ADDED Requirements

### Requirement: A GI probe view mode
The viewport SHALL offer GI probes as a view mode named in the engine's own debug view enumeration,
with a palette command that states what it shows and how to read it.

#### Scenario: The probe view mode is the engine's
- **WHEN** the editor lists its view modes
- **THEN** GI probes SHALL appear with the engine's name and in the engine's order
