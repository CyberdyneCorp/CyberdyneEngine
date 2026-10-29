## ADDED Requirements

### Requirement: Atlas pages updated inside a frame
The interface renderer SHALL be able to replace an uploaded atlas page's texels inside the next
frame's own graph — without creating a texture, submitting work of its own or waiting for the
device — staging the texels in the frame slot's buffer and copying them in a pass every interface
pass of that frame reads after. Texels of a size other than the page's, or a page never uploaded,
SHALL be refused.

#### Scenario: The next frame shows it
- **WHEN** a page is staged with new texels and the next frame is drawn
- **THEN** that frame SHALL show the new texels, and the device SHALL have begun exactly one frame for it
