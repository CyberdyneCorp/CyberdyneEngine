## ADDED Requirements

### Requirement: The agent can see the editor window
The agent interface SHALL be able to return an image of the desktop editor window **as it was
composited for the person using it**: every panel, overlay, open palette and modal, and the viewport
panel showing the engine image the window displayed. The image SHALL come from the window's own
presentation of that frame. The interface SHALL NOT redraw, simulate or substitute the window to
answer the request.

A request SHALL be able to name one dock panel, and the answer SHALL then contain only that panel's
rectangle as the editor laid it out in that frame. The whole-window answer and each panel answer SHALL
state the window's size, the rectangle returned, and which panel it is. This image is the editor's
presentation and never the shipping frame. It SHALL NOT be reported as one.

Capturing the window SHALL NOT synthesise input, take a keyboard or pointer grab, move focus, or
mutate any document. It SHALL be charged against the connection's render budget and recorded as a
render request, like a viewport observation.

A request SHALL be refused, with its reason and a remedy, when no editor window exists (a headless
session), when the named panel is not visible in the current layout, and when the name is not a panel
kind the editor has.

#### Scenario: An agent confirms the viewport shows the engine
- **WHEN** a desktop session with a hosted runtime has drawn a frame and an agent reads the window cropped to the viewport panel
- **THEN** it SHALL receive a PNG of that panel's pixels as displayed, including the engine image the editor imported

#### Scenario: A capture does not take the machine
- **WHEN** an agent reads the editor window repeatedly while a person uses other applications on the same display
- **THEN** no input SHALL be synthesised or grabbed, and the person's keyboard and pointer SHALL keep going where they send them

#### Scenario: An edit is visible where the person looks
- **WHEN** an agent moves an entity through the transform commands and reads the viewport panel before and after
- **THEN** the two captures SHALL differ, and after `edit.undo` a further capture SHALL match the first

#### Scenario: A panel that is not shown is refused
- **WHEN** an agent names a panel that is closed or hidden behind another tab
- **THEN** the read SHALL be refused, naming the panel and saying how to show it

#### Scenario: A headless session is told why
- **WHEN** an agent connected to a headless editor reads the editor window
- **THEN** the read SHALL be refused because there is no window, and the refusal SHALL suggest a desktop session
