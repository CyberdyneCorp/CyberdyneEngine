## ADDED Requirements

### Requirement: Atlas uploads leave the frame ring to the game
Making an atlas page SHALL begin no device frame and submit no work of its own: the page's texture
and view SHALL be made at once and its texels copied in the next frame's own graph, before every
interface pass of that frame reads them, as a staged page's are. The game's frames SHALL therefore
take the device's frame slots in order however often pages are made between them. A page re-made at
another size SHALL be copied in whole from staging of its own size.

#### Scenario: Making a page begins no frame
- **WHEN** a renderer is created and pages are made between frames
- **THEN** the device SHALL have begun no frame and taken no submission for them

#### Scenario: The frames keep the ring's order
- **WHEN** a page is re-made between some of the game's frames
- **THEN** the game's frame n SHALL run in slot n modulo the frames in flight, every frame

#### Scenario: The next frame copies the page in
- **WHEN** a page is made, or re-made at another size, and the next frame declares the interface
- **THEN** that frame's graph SHALL copy the page's texels in once, and the frame after SHALL copy nothing
