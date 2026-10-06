# swift-scripting Spec Delta

## ADDED Requirements

### Requirement: Runtime interface from Swift
CyberdyneKit SHALL wrap the interface entries in a Swift API with no SwiftUI dependency: element
handles with typed layout, style, text, image, progress, visibility and opacity writes, hit testing
and focus, and a declarative layer that describes a tree of panels, labels, images, progress bars
and buttons, mounts it once for a behaviour, and finds its elements by identifier for later updates.
A button described with an action SHALL run that action on the behaviour that mounted it when the
engine delivers its click, and a behaviour SHALL be able to override an interface-event callback
that receives every event on elements its entity owns.

#### Scenario: A HUD written in Swift
- **WHEN** a Swift behaviour mounts a resource bar, a selection panel with health bars and a minimap
  and writes them from game state each frame
- **THEN** the store SHALL hold the texts, the visibility and the fill the game's state implies

#### Scenario: A button's action
- **WHEN** the engine delivers a click on a button a behaviour mounted with an action
- **THEN** the action SHALL run on that behaviour, followed by its interface-event callback if it
  overrides one
