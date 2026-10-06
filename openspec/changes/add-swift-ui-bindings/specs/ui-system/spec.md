# ui-system Spec Delta

## ADDED Requirements

### Requirement: An interface built through the ABI draws as one built in engine code
An interface a scripting module builds through the native interface SHALL live in the same element
store, be laid out, flattened and drawn by the same code, and mark the same dirty states for the
same changes as one built in engine code. A module's elements and the embedder's SHALL coexist in
one store, and a module SHALL reach only the elements it created.

#### Scenario: The same HUD two ways
- **WHEN** a HUD is built once in C++ game code and once by a Swift module through the ABI, showing
  the same state
- **THEN** the two stores SHALL flatten to the same primitives and draw the same pixels
