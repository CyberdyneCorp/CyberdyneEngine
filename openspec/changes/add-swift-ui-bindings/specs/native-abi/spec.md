# native-abi Spec Delta

## ADDED Requirements

### Requirement: Runtime interface entries
The interface SHALL let a module build and update a runtime interface: obtain the screen's root,
create elements of a declared kind (panel, label, image, progress bar, button) under the root or
under an element the module created, destroy them with their subtrees, and set their layout input
(model, direction, justification, alignment, gap, size, margin, padding, flex factors, anchors and
offsets, grid placement), their style (background, border, corner radius, accent, clipping), a
label's or button's text, an image's atlas page and rectangle, a progress bar's fraction, their
visibility and their opacity. It SHALL let a module read an element's laid-out rectangle, the
module's element under a window point, and the element with keyboard focus, and move focus. Every
such entry SHALL be callable at module initialisation and in frame update and SHALL be refused in a
fixed step; a write the element's kind does not take, a non-finite number, a stale handle, and an
element the module did not create SHALL be refused.

#### Scenario: An element the embedder built
- **WHEN** a module writes to or destroys an interface element the embedder created itself
- **THEN** the call SHALL return `CY_RESULT_NOT_FOUND` and the element SHALL be unchanged

#### Scenario: The interface in a fixed step
- **WHEN** a module calls an interface entry during a fixed update
- **THEN** the call SHALL return `CY_RESULT_PERMISSION_DENIED` and nothing SHALL change

#### Scenario: A point over the world
- **WHEN** a module asks which of its elements is under a window point over no element
- **THEN** the answer SHALL be the null element, not a failure

### Requirement: Interface events reach the owning behaviour
An element SHALL be created for an owning entity, and the behaviour vtable SHALL carry an
interface-event callback. A press and a release of the primary button over the same button element
SHALL be a click, and a change of keyboard focus SHALL be a blur and a focus; each SHALL be
delivered in frame update to every behaviour on the owning entity that implements the callback,
through the vtable of the generation that created it. A module compiled before the callback was
appended SHALL receive none.

#### Scenario: A click on a button
- **WHEN** the pointer presses and releases the primary button over a button a behaviour's entity
  owns
- **THEN** that behaviour SHALL receive exactly one click event naming the button, its owner and
  the pointer's position

#### Scenario: A press dragged off the button
- **WHEN** the pointer presses over a button and releases elsewhere
- **THEN** no click SHALL be delivered
