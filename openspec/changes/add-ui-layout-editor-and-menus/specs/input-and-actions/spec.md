## ADDED Requirements

### Requirement: Rebinding from game code
A game SHALL be able to rebind an action through the native ABI by the input system's own flow:
it SHALL begin a rebind for an action, a control scheme and a binding slot under a conflict policy,
the input system SHALL take the next eligible control the player presses on that scheme, and the
game SHALL be able to poll whether the rebind is listening, applied, in conflict or cancelled. A
cancel control (Escape, a gamepad's Back) SHALL cancel it. A conflict SHALL be another action of a
context that also binds the rebound action; a control bound in another context SHALL not be a
conflict, so an interface's navigation bindings and a game's bindings may share a control. An
applied rebind SHALL be an override in the user's profile, leaving the authored bindings unchanged.
The game SHALL be able to read the control an action's slot has in effect, and to save the profile
as text keyed by the actions' stable identifiers, load it back, and reset it.

#### Scenario: A gamepad button rebound by swap
- **WHEN** a game rebinds Jump on the gamepad to the north button, which Inventory holds, by swap
- **THEN** Jump SHALL fire on the north button and Inventory on Jump's previous button

#### Scenario: The interface's bindings are not a conflict
- **WHEN** a game action is rebound to a control the interface's own context binds
- **THEN** the rebind SHALL apply without changing the interface's binding

#### Scenario: Cancelled by the player
- **WHEN** the player presses Escape while a rebind listens
- **THEN** the rebind SHALL be cancelled and no binding SHALL change

#### Scenario: The profile persists
- **WHEN** a game saves the profile after a rebind and a later run loads it
- **THEN** the rebound control SHALL fire the action in the later run
