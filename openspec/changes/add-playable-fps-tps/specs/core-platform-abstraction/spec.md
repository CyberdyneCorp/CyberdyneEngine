## ADDED Requirements

### Requirement: Relative mouse mode
A display server SHALL be able to hold the pointer in a window — the cursor hidden and the mouse's
motion delivered as raw counts with no pointer acceleration — and SHALL answer whether it can
through `has_feature(RelativeMouse)` before it is asked. A display server that cannot SHALL refuse
by name rather than pretend.

#### Scenario: A played game holds the mouse while it runs
- **WHEN** a game is played in a window on a desktop
- **THEN** the pointer SHALL be held while the game runs and released while it is paused

#### Scenario: A server with no pointer says so
- **WHEN** the stub display server is asked about relative mouse mode
- **THEN** it SHALL answer false

### Requirement: Synthetic platform input
The desktop platform SHALL be able to push synthetic input — a key, mouse motion and buttons, a
window gaining or losing the focus, and a gamepad's axes and buttons — through its own event queue,
so that a test or a capture drives a game by exactly the path a person's input takes: the platform's
translation, the input server, the game's actions.

#### Scenario: An injected key is the key a person presses
- **WHEN** a key is injected
- **THEN** the input server SHALL receive that key, by the same scancode a person's press carries

#### Scenario: A virtual gamepad plays as a pad on a cable
- **WHEN** a virtual gamepad is attached and its stick and trigger moved
- **THEN** the input server SHALL receive them as a gamepad's, the stick's forward as +Y
