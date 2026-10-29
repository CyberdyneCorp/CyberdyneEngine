## ADDED Requirements

### Requirement: Games are played in a window
The project SHALL provide a desktop play host under which a game is PLAYED: a window, the
platform's keyboard, mouse and gamepads delivered through the game's own action maps and assigned
to its player, a real-time loop that runs the simulation at its fixed rate and presents each frame
at the frame's own time between two ticks, and a pause. The host SHALL decide nothing about the
game: what input means, what the interface shows and what a tick does are the game's. Esc, a pad's
Start and the window losing the keyboard's focus SHALL pause the game — no tick runs, presentation
stands still, the mouse is released — and only the player SHALL resume it. The first-person and
third-person demos SHALL be played under it, and a played game's input SHALL be testable by pushing
a person's events through the platform rather than by calling the game.

#### Scenario: A mouse count turns the first-person camera by its one sensitivity
- **WHEN** the first-person demo is played and the mouse moves 200 counts right and 50 away
- **THEN** the camera SHALL turn 200 times the camera's sensitivity right and 50 times up

#### Scenario: A shot fired with the mouse hits a target
- **WHEN** the first-person demo is played, turned onto a target and the trigger pulled
- **THEN** shots SHALL hit the target and the host's own raycast SHALL confirm every shot

#### Scenario: A lost focus pauses, and only the player resumes
- **WHEN** the window loses the focus while a key is held, gets it back, and Esc is pressed later
- **THEN** no tick SHALL run from the focus loss to the Esc, the mouse SHALL turn nothing meanwhile,
  and the game SHALL run again only from the Esc

#### Scenario: The pause menu resumes and quits
- **WHEN** the menu's Resume is clicked, and after a second pause its Quit
- **THEN** the game SHALL resume without the click firing, and the run SHALL end at the second click

#### Scenario: The scripted runs are unchanged
- **WHEN** either demo's scripted session runs after the play host exists
- **THEN** its report and its digest SHALL be the ones it printed before

#### Scenario: A played frame keeps its budget
- **WHEN** a demo is played full screen at 1920x1080 on the device the budget names
- **THEN** a frame's work SHALL take no more than 16.7 ms on average
