## ADDED Requirements

### Requirement: The platform boundary
Input processing SHALL be layered: the platform layer produces normalised, **timestamped** device
events; this capability turns them into semantic actions; and `gameplay-framework` turns actions
into commands.

Platform device events SHALL carry a high-resolution timestamp, a stable device identifier, and a
control identifier, so that latency analysis, fixed-tick resolution, and replay are possible.

Gameplay systems SHALL NOT consume raw device events, key codes, or button indices. Raw access
SHALL remain available to tooling, diagnostics, and editor code.

#### Scenario: Gameplay sees no devices
- **WHEN** a gameplay system needs player intent
- **THEN** it SHALL read an action or receive a command, and SHALL NOT query a keyboard, mouse, or
  gamepad

#### Scenario: Events carry time
- **WHEN** a device event is delivered
- **THEN** it SHALL carry the timestamp at which the platform observed it, not the time it was
  processed

### Requirement: Input users and device ownership
Input SHALL be organised around **input users**, each owning: a set of assigned devices, a context
stack, a rebinding profile, a preferred device, and a focus state.

A process SHALL support several input users, so local multiplayer is structural rather than
retrofitted.

Device assignment SHALL be **explicit**, and keyboard and mouse ownership SHALL follow a declared
policy — exclusive to one user, shared, or split — rather than an assumption that they belong to the
first player.

An input user SHALL NOT be identified by a device index; a user without a device is a user awaiting
one, not a lost player.

#### Scenario: Two players, one machine
- **WHEN** two people play locally
- **THEN** each SHALL be an input user with its own devices, contexts, and bindings

#### Scenario: Identity is not the device
- **WHEN** a controller is unassigned from a user
- **THEN** the user SHALL persist with no devices, and its gameplay participant SHALL be unaffected

### Requirement: Device lifecycle
The system SHALL handle device connection, disconnection, reconnection, capability change, and
low-battery notification, and SHALL surface them as events.

Reassignment on disconnection SHALL follow a declared **policy** — hold the user and await
reconnection, reassign to another device, or pause — rather than being decided implicitly.

Reconnection SHALL restore a device to the user that held it where the policy permits, so a
controller running out of battery does not shuffle players.

#### Scenario: A controller disconnects mid-match
- **WHEN** a device is lost
- **THEN** the declared policy SHALL apply, the user's identity SHALL persist, and the game SHALL be
  able to present a reconnection prompt

#### Scenario: Reconnection restores the pairing
- **WHEN** the device returns
- **THEN** it SHALL be reassigned to the same user under the default policy

### Requirement: Actions and value types
An **action** SHALL be a semantic, device-independent input with a declared value type: digital,
scalar, two-dimensional axis, three-dimensional axis, or pose.

Actions SHALL be identified by **stable identifiers** cooked from authored names, using the identity
mechanism in `core-type-system`. Strings SHALL NOT be the runtime identity of an action, and control
paths SHALL NOT be parsed at runtime.

The pose value type SHALL exist from the outset so that tracked devices can be added without
changing the action model.

#### Scenario: One action, many devices
- **WHEN** movement is bound to keys, a stick, and a virtual touch stick
- **THEN** consumers SHALL read one two-dimensional action, unaware of which device produced it

#### Scenario: Identity is stable
- **WHEN** an action is renamed
- **THEN** its identifier SHALL be unchanged and existing bindings and profiles SHALL still resolve

### Requirement: Mapping contexts
Bindings SHALL be organised into **mapping contexts**, and each input user SHALL hold an ordered
**context stack** with priorities.

A context SHALL be able to **consume** an action so lower contexts do not observe it, **override** a
binding, or **augment** lower contexts.

Contexts SHALL be pushed and popped by **handle**, not by assuming stack positions, so that
overlapping activations unwind correctly.

Gameplay features SHALL be able to contribute contexts, so that entering a vehicle or a photo mode
installs and removes its bindings without a global toggle.

#### Scenario: A modal over an inventory over a vehicle
- **WHEN** three contexts are active
- **THEN** each action SHALL resolve to the highest-priority context that binds it, and consumption
  SHALL prevent lower contexts from also acting

#### Scenario: Unwinding is order-independent
- **WHEN** contexts are removed in a different order than they were added
- **THEN** each SHALL be removed by handle and the remaining stack SHALL be correct

### Requirement: Bindings and composites
A **binding** SHALL associate a device control with an action, and SHALL be resolved at cook or load
time to compact device and control identifiers.

**Composite bindings** SHALL be supported: one-dimensional and two-dimensional axes assembled from
discrete controls, radial composites, chords, and sequences — so that four keys produce one
two-dimensional action without gameplay knowing.

A binding SHALL declare its **interpretation**: delta (mouse motion), absolute (a position), or rate
(a stick deflection). Consumers SHALL integrate accordingly, and a delta SHALL NOT be scaled by
frame time.

Bindings SHALL declare their **device scheme** membership, so the interface can present the correct
prompts.

#### Scenario: Keys become an axis
- **WHEN** four keys are bound as a two-dimensional composite
- **THEN** the action SHALL produce a normalised vector

#### Scenario: Look is not frame-rate dependent
- **WHEN** a mouse delta drives look
- **THEN** it SHALL NOT be multiplied by frame time, while a stick's rate SHALL be

### Requirement: Processors
**Processors** SHALL transform values numerically: dead zone (including radial), normalisation,
scaling, inversion, sensitivity, response curve, clamping, swizzling, and smoothing.

Processors SHALL be composable into a chain evaluated per binding.

Processors SHALL be stateless where possible; those requiring state SHALL store it in per-binding
storage rather than as allocated objects.

Processor evaluation SHALL NOT allocate per frame.

#### Scenario: A stick is shaped
- **WHEN** a stick drives look
- **THEN** its value SHALL pass through radial dead zone, response curve, and sensitivity before
  reaching the action

#### Scenario: No allocation
- **WHEN** input is evaluated for a frame
- **THEN** no heap allocation SHALL occur in the evaluation path

### Requirement: Modifiers
**Modifiers** SHALL alter an action's meaning contextually, distinct from numerical processors:
relative to a reference frame, scaled by a player setting, negated, or active only in a declared
state.

A **reference frame** modifier SHALL receive a semantic frame — forward, right, and up — supplied by
a provider such as the camera or the controlled entity, and SHALL NOT depend on renderer internals.

Keeping processors numerical and modifiers contextual SHALL be maintained, so that a processor chain
can be evaluated with no knowledge of the world.

#### Scenario: Camera-relative movement
- **WHEN** a stick drives movement in a third-person game
- **THEN** a reference-frame modifier SHALL take the camera's frame, and the input system SHALL not
  reach into the renderer

#### Scenario: Sensitivity is a setting, not a binding
- **WHEN** a player changes look sensitivity
- **THEN** a modifier SHALL apply the setting, without rewriting bindings

### Requirement: Triggers and the action lifecycle
**Triggers** SHALL determine when an action becomes active: pressed, released, held for a duration,
tap, double tap, threshold crossing, chord, sequence, and pulse.

Every action SHALL expose a lifecycle: **started**, **ongoing**, **triggered**, **completed**, and
**cancelled**, so that a hold that is abandoned is distinguishable from one that completes.

Chords and sequences SHALL be expressed as triggers rather than reconstructed in gameplay code from
individual button states.

#### Scenario: A hold that is abandoned
- **WHEN** a player begins a hold and releases before the threshold
- **THEN** the action SHALL report started then cancelled, and SHALL NOT report triggered

#### Scenario: A chord is one action
- **WHEN** a modifier and a button form a chord
- **THEN** it SHALL be one binding with a chord trigger, not two states combined in gameplay code

### Requirement: Action state and edge queries
Action state SHALL be maintained as compact per-user records holding current value, previous value,
transition flags, trigger phase, and the time of the last transition.

Consumers SHALL be able to query value, pressed, just pressed, just released, and held duration
without querying devices.

**Continuous actions SHALL NOT generate an event or an allocation per frame.** Edge events SHALL be
available for discrete transitions; sampling SHALL be the mechanism for continuous values.

#### Scenario: An axis does not spam
- **WHEN** a stick is held for a second
- **THEN** its action SHALL be sampled, and no per-frame event object SHALL be produced

#### Scenario: One query answers several questions
- **WHEN** a system needs both the value and whether the action began this frame
- **THEN** both SHALL come from the action's state record

### Requirement: Control schemes and device detection
The engine SHALL define **control schemes** — keyboard and mouse, gamepad, touch, wheel, and
project-defined — each declaring required and optional devices.

The active scheme SHALL be detected from recent meaningful input, with **hysteresis and a
significance threshold**, so that a noisy device or an incidental mouse movement does not flip
prompts back and forth.

The active scheme SHALL be observable so the interface can present matching glyphs (see
`ui-system`).

#### Scenario: Prompts follow the player
- **WHEN** a player puts down the controller and uses the mouse
- **THEN** the active scheme SHALL change and prompts SHALL update

#### Scenario: Noise does not flip prompts
- **WHEN** an idle controller reports drift below the significance threshold
- **THEN** the active scheme SHALL NOT change

### Requirement: Fixed-tick sampling
Input consumed by fixed-step simulation SHALL be resolved from **timestamped events accumulated
between ticks**, not from whichever value happened to be current when the tick began.

A press and release occurring within one frame SHALL still be observed by the tick.

For continuous control, the system SHALL produce a **command frame** per simulation tick — a compact
record of the tick number, continuous axis values, and button state — suitable for prediction and
replay.

Command frames SHALL be the input side of the gameplay command stream defined in
`gameplay-framework`, and discrete complex intents SHALL be typed commands rather than packed bits.

#### Scenario: A fast press is not lost
- **WHEN** a button is pressed and released between two ticks
- **THEN** the tick SHALL observe both transitions

#### Scenario: One frame per tick
- **WHEN** a networked character is controlled
- **THEN** each simulation tick SHALL produce one command frame carrying its continuous inputs and
  button state

### Requirement: Input buffering
Actions SHALL support **buffering**: a triggered action may remain valid for a declared window, so
that an input arriving slightly early is not discarded.

The buffer window SHALL be configurable per action, and buffered intent SHALL be consumable exactly
once.

Buffering SHALL provide the mechanism; gameplay semantics that use it — grace periods, cancel
windows, queued attacks — SHALL remain gameplay concerns.

#### Scenario: An early press still counts
- **WHEN** a jump is pressed shortly before landing
- **THEN** the buffered action SHALL remain available within its window and be consumed once

### Requirement: Rebinding and profiles
Runtime rebinding SHALL be supported through an explicit flow: begin a rebind for an action, listen
for an eligible control, apply the conflict policy, and store the result.

**Authored binding assets SHALL be immutable at runtime.** Player changes SHALL be stored as
**overrides in a profile**, applied over the defaults, so that content updates do not conflict with
customisation.

Conflict policy SHALL be declared: reject duplicates, swap, allow, unbind the previous, or ask —
per project and per context.

Overrides SHALL be per device scheme, so a player may rebind gamepad and keyboard independently.

#### Scenario: An update does not lose customisation
- **WHEN** a game ships new default bindings
- **THEN** player overrides SHALL still apply over them, and the shipped asset SHALL be unmodified

#### Scenario: Conflicts are handled by policy
- **WHEN** a rebind duplicates an existing binding
- **THEN** the declared conflict policy SHALL apply rather than silently producing two bindings

### Requirement: Accessibility
The following SHALL be first-class capabilities of the input model, not per-game implementations:
hold-to-toggle conversion, repeated-press assistance, dead-zone adjustment, single-stick modes,
sticky modifiers, input scaling and sensitivity limits, and full remapping including composite
elements.

Accessibility settings SHALL apply as processors and modifiers within the standard pipeline, so that
they work for every action without gameplay changes.

Gameplay SHALL NOT be able to bypass accessibility transformations by reading device state directly,
which is a further reason raw device access is not the gameplay path.

#### Scenario: Hold becomes toggle everywhere
- **WHEN** a player enables hold-to-toggle
- **THEN** every hold action SHALL convert, with no per-action implementation

#### Scenario: Assistance cannot be bypassed
- **WHEN** a system reads player intent
- **THEN** it SHALL read the action, and accessibility transformations SHALL already have applied

### Requirement: Interface routing and focus
Input focus SHALL be routed through declared layers — operating system focus, editor focus,
interface focus, gameplay focus — with a declared policy at each boundary.

The interface SHALL consume actions through the same action model (see `ui-system`), not through
raw key checks, so that keyboard, gamepad, touch, and accessibility devices work uniformly.

When the interface holds focus for text entry, gameplay actions bound to the same controls SHALL be
suppressed unless the context explicitly permits pass-through.

#### Scenario: Typing does not move the player
- **WHEN** a text field has focus
- **THEN** movement actions bound to letter keys SHALL be suppressed

#### Scenario: The interface is device-agnostic
- **WHEN** a menu is navigated
- **THEN** it SHALL respond to the interface actions regardless of device

### Requirement: Text entry is separate
Text entry SHALL use the platform's text input services — composition, input method editors,
clipboard, and platform keyboards — and SHALL NOT be reconstructed by interpreting key actions or
scan codes.

Key actions and text input SHALL be distinct streams, and a text field SHALL consume the text
stream while navigation uses actions.

#### Scenario: Composed input works
- **WHEN** a player uses an input method editor to enter text
- **THEN** the composed text SHALL be delivered through the text stream, not inferred from key
  presses

### Requirement: Synthetic and remote input
The system SHALL support **synthetic input**: injecting action values and events for a user
programmatically, without fabricating operating-system events.

It SHALL support **remote input**: a device on another machine feeding a user, for testing touch
input from a desktop, driving a device under test, or automation.

Synthetic and remote sources SHALL be **marked as such**, so diagnostics and anti-cheat policies can
distinguish them, and a shipping build SHALL be able to disable them.

#### Scenario: A test drives the game
- **WHEN** an automated test exercises player input
- **THEN** it SHALL inject actions directly, with no operating-system event simulation

#### Scenario: Synthetic input is identifiable
- **WHEN** input is injected
- **THEN** its source SHALL be recorded as synthetic

### Requirement: Input assets and cooking
Actions, contexts, bindings, processors, triggers, and schemes SHALL be authored as assets and
**cooked**: names resolved to stable identifiers, control paths resolved to device and control
identifiers, and processor chains flattened into compact tables.

Runtime SHALL perform no string lookup, path parsing, or asset search in the evaluation path.

Input assets SHALL participate in the derived data cache and the identity manifest like other
content.

#### Scenario: No parsing at runtime
- **WHEN** input is evaluated
- **THEN** all identifiers SHALL be pre-resolved, and no control path SHALL be parsed

### Requirement: Headless operation
Input SHALL function with **no devices present**: a dedicated server, a test harness, or a replay
has no keyboard, and the action and command path SHALL still operate from synthetic, network,
replay, and artificial-intelligence sources.

Absence of a device backend SHALL NOT prevent the system from initialising.

#### Scenario: A server has no input devices
- **WHEN** a dedicated server runs
- **THEN** the input system SHALL initialise without devices and commands SHALL arrive from the
  network

### Requirement: Input diagnostics
The engine SHALL provide an input inspector showing, per user: assigned devices, active scheme,
context stack with priorities, and for each action its raw value, its value after each processor,
its final value, its trigger phase, and **which binding produced it**.

It SHALL be able to answer **why an action did not trigger**: no binding in an active context, the
value below a threshold, consumed by a higher context, the trigger's conditions unmet, suppressed by
focus, or the device unassigned.

An **event trace** SHALL record device events with timestamps, and a **latency view** SHALL show the
path from device event through action evaluation to simulation tick and rendered frame.

#### Scenario: Why did nothing happen
- **WHEN** a player presses a key and nothing occurs
- **THEN** the inspector SHALL state the reason, naming the context, binding, or trigger responsible

#### Scenario: Latency is measurable
- **WHEN** input responsiveness is investigated
- **THEN** the latency view SHALL show each stage from platform event to presented frame

### Requirement: Input performance
Input evaluation SHALL support at minimum: 8 local users, 64 connected devices, a thousand declared
actions, and hundreds of active bindings per user, without becoming a measurable frame cost.

Evaluation SHALL: allocate nothing per frame, take no global lock, and process only controls that
changed or bindings that are active.

#### Scenario: Scale is not a special case
- **WHEN** eight users with full binding sets are active
- **THEN** input evaluation SHALL remain a negligible fraction of frame time

### Requirement: Forbidden input patterns
The following SHALL NOT appear, and each SHALL be checkable in review:

- Gameplay logic depending on platform key codes or controller button indices
- Parsing string control paths in a runtime evaluation path
- Raw device events mutating gameplay state directly
- Local player identity inferred from device index
- A heap-allocated event object per frame for a continuous action
- Rebinding that mutates shipped authored assets
- Text entry reconstructed from physical key presses
- Accessibility transformations bypassed by reading device state directly

#### Scenario: A proposal is checked
- **WHEN** a change would read a key code in a gameplay system
- **THEN** it SHALL be flagged against this requirement
