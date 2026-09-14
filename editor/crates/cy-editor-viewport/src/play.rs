//! Editing while playing: what is unmistakable, what persists, and what inspection may not touch.
//!
//! `editor-viewport-and-gizmos` — "Editing while playing":
//!
//! > When the runtime is in a play mode ... the viewport SHALL make the distinction between **edit
//! > state and play state visually unmistakable**, and SHALL state what will persist when play ends.
//! > Manipulation during play SHALL follow the live-editing rules for propagation and persistence
//! > rather than being **silently discarded or silently persisted**. The editor SHALL support
//! > detaching a viewport camera from the game camera during play to inspect the running world
//! > without altering it.
//!
//! --- "SILENTLY" IS THE WORD THE REQUIREMENT TURNS ON --------------------------------------------------
//!
//! Both failure modes are silent ones. An edit discarded when play ends is work a designer did and
//! lost; an edit persisted when play ends is a change nobody made deliberately, sitting in a file.
//! Neither is prevented by choosing one of them — a project needs both, at different times — so what
//! [`PlayState`] provides is that the answer is always *stated*: [`PlayState::persistence`] returns a
//! [`Persistence`] and [`Persistence::statement`] is a sentence the viewport shows. There is no
//! constructor that leaves it unset.
//!
//! --- DETACHING IS A READ, AND THE TYPE SAYS SO -------------------------------------------------------
//!
//! > **WHEN** the viewport camera is detached during play **THEN** the game camera and gameplay SHALL
//! > be unaffected.
//!
//! [`CameraAttachment::detach`] returns the view state the editor will drive from now on, and takes
//! nothing it could write to. Attaching again adopts the game camera's state; it does not push the
//! editor's camera into the game, and there is no method here that could.

use crate::state::ViewState;

/// Where the runtime is.
///
/// The same three the `live-editing` capability names, because a fourth state here would be one the
/// runtime could not be in.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum PlayState {
    /// Authoring. Nothing is simulating, and a command applies on arrival — which the milestone's
    /// spike measured at 0.059 ms against 9.938 ms for the alternative.
    #[default]
    Editing,
    /// Simulating.
    Playing,
    /// Simulating, but stopped. Still play state: what is in the world is the simulation's, not the
    /// document's, and the persistence question is the same one.
    Paused,
}

impl PlayState {
    /// Whether the runtime is in a play mode at all.
    #[must_use]
    pub const fn is_play_mode(self) -> bool {
        matches!(self, PlayState::Playing | PlayState::Paused)
    }

    /// Whether a change should be applied on arrival rather than at a tick boundary.
    ///
    /// The editor knows this and the runtime does not, which is why
    /// `cy_editor_protocol::ApplyWhen` is carried on the message rather than guessed at the far
    /// end. A runtime that guessed would guess wrong exactly when a designer was dragging a gizmo
    /// in a paused world.
    #[must_use]
    pub const fn applies_on_arrival(self) -> bool {
        matches!(self, PlayState::Editing)
    }

    /// What the viewport shows so the state is unmistakable.
    ///
    /// A word, not a colour: `editor-visual-language` owns the colour, and a viewport that
    /// distinguished the two states by hue alone would be indistinguishable to a colour-blind user
    /// — which is a failure of "unmistakable" rather than of taste.
    #[must_use]
    pub const fn badge(self) -> &'static str {
        match self {
            PlayState::Editing => "EDIT",
            PlayState::Playing => "PLAYING",
            PlayState::Paused => "PAUSED",
        }
    }

    /// What happens to edits made in this state when play ends.
    #[must_use]
    pub const fn persistence(self, policy: Persistence) -> Persistence {
        match self {
            PlayState::Editing => Persistence::Authoring,
            PlayState::Playing | PlayState::Paused => policy,
        }
    }
}

/// WHERE the runtime runs a play session. M11.b task 3.1.
///
/// --- WHY THIS IS NOT `HostingMode` ---------------------------------------------------------------
///
/// `cy_editor_sdk::HostingMode` names `NoRuntime`, `Embedded` and `Hosted`, and its own
/// documentation says `Hosted` is *"the engine in a separate process **or** on a remote device"* —
/// so the editor's only locality axis deliberately collapses the two modes `editor-architecture` and
/// `live-editing` deliberately separate. This is that second axis, and the two are orthogonal: a
/// `Hosted` editor can be running any of the three modes below, and `NoRuntime` is running none.
///
/// --- AND WHY THERE IS NO `Fallback` ---------------------------------------------------------------
///
/// `specs/live-editing/` (M11.b): *"Selecting a mode that is not available SHALL refuse, naming the
/// mode and the reason. It SHALL NOT fall back to another mode."* There is therefore no method here
/// that answers "the nearest available mode", and [`PlayMode::from_name`] returns `None` for a word
/// it does not know rather than a default — a `RemoteDevice` request that quietly ran `InEditor`
/// would be a green result over a feature that does not exist.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum PlayMode {
    /// A runtime world in the editor's hosted runtime process. Fast iteration.
    ///
    /// **Not in the editor's own process.** `live-editing` is explicit that no play mode runs there,
    /// because the editor is a separate Rust application; `InEditor` denotes iteration speed and
    /// shared runtime state.
    #[default]
    InEditor,
    /// A second runtime process, so that editor-only state cannot mask a defect.
    SeparateProcess,
    /// A runtime on another machine: a console, a phone, a tablet.
    RemoteDevice,
}

impl PlayMode {
    /// Every mode the editor can ask for, in the order the specification lists them.
    pub const ALL: [PlayMode; 3] = [
        PlayMode::InEditor,
        PlayMode::SeparateProcess,
        PlayMode::RemoteDevice,
    ];

    /// The word the protocol carries. **The same spelling `cy::gameplay::play_mode_name` writes**,
    /// and the two are pinned to each other by a test on each side — the arrangement `.cyprim` and
    /// the body type names already use across this boundary.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            PlayMode::InEditor => "in-editor",
            PlayMode::SeparateProcess => "separate-process",
            PlayMode::RemoteDevice => "remote-device",
        }
    }

    /// The mode a word names, or `None`. See the type's own documentation for why there is no
    /// nearest-match.
    #[must_use]
    pub fn from_name(name: &str) -> Option<PlayMode> {
        PlayMode::ALL.into_iter().find(|mode| mode.name() == name)
    }

    /// What the viewport shows beside the play badge, so that a designer looking at a frame knows
    /// which machine produced it.
    #[must_use]
    pub const fn badge(self) -> &'static str {
        match self {
            PlayMode::InEditor => "IN EDITOR",
            PlayMode::SeparateProcess => "SEPARATE PROCESS",
            PlayMode::RemoteDevice => "REMOTE DEVICE",
        }
    }

    /// Whether this mode can be asked to advance exactly one frame.
    ///
    /// The one capability that differs between the modes, and it differs by TRANSPORT rather than by
    /// architecture: a remote runtime's frames arrive encoded and are not individually addressable.
    /// `live-editing` allows exactly that — *"locality SHALL be an optimisation of transport"* —
    /// and M11.b's delta requires the difference be **queried** rather than discovered by trying.
    /// The engine answers the same question in `cy::gameplay::capabilities_of`.
    #[must_use]
    pub const fn can_step_frame(self) -> bool {
        !matches!(self, PlayMode::RemoteDevice)
    }

    /// Whether this mode can be asked to advance exactly one simulation tick. A tick is a message
    /// rather than a picture, so every mode can.
    #[must_use]
    pub const fn can_step_tick(self) -> bool {
        true
    }

    /// Whether editor-only state is absent from the runtime, so editor-specific behaviour cannot
    /// mask a defect. `live-editing`'s "Standalone behaviour is honest".
    #[must_use]
    pub const fn isolates_editor_state(self) -> bool {
        !matches!(self, PlayMode::InEditor)
    }
}

/// What becomes of an edit made while the runtime is playing.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum Persistence {
    /// The edit is in the authoring document. It is what a save writes and it survives play ending.
    Authoring,
    /// The edit is in the running world only, and play ending discards it. The default during play,
    /// because it is what a designer tuning a value expects and it is the reversible one.
    #[default]
    RuntimeOnly,
    /// The edit is in the running world and will be written back to the document when play ends.
    PromoteOnExit,
}

impl Persistence {
    /// The sentence the viewport shows. **There is no state in which nothing is said.**
    #[must_use]
    pub const fn statement(self) -> &'static str {
        match self {
            Persistence::Authoring => "Edits are saved to the document.",
            Persistence::RuntimeOnly => {
                "Edits apply to the running world only and are discarded when play ends."
            }
            Persistence::PromoteOnExit => {
                "Edits apply to the running world and will be written to the document when play \
                 ends."
            }
        }
    }

    /// Whether an edit made under this policy reaches the document.
    #[must_use]
    pub const fn reaches_the_document(self) -> bool {
        matches!(self, Persistence::Authoring | Persistence::PromoteOnExit)
    }
}

/// Whether the viewport is looking through the game's camera or its own.
#[derive(Clone, PartialEq, Debug)]
pub enum CameraAttachment {
    /// Looking through a game camera, identified by the engine's stable identity for it. The view
    /// state follows what the game does.
    Attached {
        /// The game camera's stable identity.
        camera: u64,
    },
    /// Looking through the editor's own camera, which the user drives.
    Detached {
        /// The editor's camera, driven by [`crate::navigation`].
        state: Box<ViewState>,
    },
}

impl CameraAttachment {
    /// Look through a game camera, so composition can be judged with the shipping camera's
    /// settings.
    #[must_use]
    pub const fn attached(camera: u64) -> Self {
        Self::Attached { camera }
    }

    /// Take the editor's own camera, starting from where the game's is now.
    ///
    /// **Takes nothing it could write to.** Inspecting a running world is a read, and the way to
    /// make that true is for the function that begins the inspection to have no way of being
    /// anything else.
    #[must_use]
    pub fn detach(&self, from: &ViewState) -> Self {
        Self::Detached {
            state: Box::new(from.clone()),
        }
    }

    /// Whether the user is driving the camera.
    #[must_use]
    pub const fn is_detached(&self) -> bool {
        matches!(self, CameraAttachment::Detached { .. })
    }

    /// The game camera being looked through, if one is.
    #[must_use]
    pub const fn game_camera(&self) -> Option<u64> {
        match self {
            CameraAttachment::Attached { camera } => Some(*camera),
            CameraAttachment::Detached { .. } => None,
        }
    }
}

/// What the viewport says about the runtime's state, in one place so that two panels cannot say
/// different things.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct PlayIndication {
    /// The short word: `EDIT`, `PLAYING`, `PAUSED`.
    pub badge: &'static str,
    /// What happens to edits, always present.
    pub persistence: &'static str,
    /// Whether the viewport camera is the game's.
    pub through_game_camera: bool,
}

/// What the viewport shows about the runtime's state.
#[must_use]
pub fn indication(
    state: PlayState,
    policy: Persistence,
    attachment: &CameraAttachment,
) -> PlayIndication {
    PlayIndication {
        badge: state.badge(),
        persistence: state.persistence(policy).statement(),
        through_game_camera: attachment.game_camera().is_some(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::math::Vec3;

    #[test]
    fn play_state_is_unmistakable_and_never_silent_about_persistence() {
        // "WHEN the runtime is playing THEN the viewport SHALL indicate it clearly, and the editor
        // SHALL state which edits persist." Both halves, for every state and every policy: there is
        // no combination that produces nothing to say.
        for state in [PlayState::Editing, PlayState::Playing, PlayState::Paused] {
            for policy in [
                Persistence::Authoring,
                Persistence::RuntimeOnly,
                Persistence::PromoteOnExit,
            ] {
                let shown = indication(state, policy, &CameraAttachment::attached(1));
                assert!(!shown.badge.is_empty());
                assert!(
                    shown.persistence.len() > 20,
                    "{state:?}/{policy:?} says nothing useful"
                );
            }
        }
        assert_ne!(PlayState::Editing.badge(), PlayState::Playing.badge());
    }

    #[test]
    fn editing_persists_to_the_document_whatever_the_play_policy_says() {
        // The policy is about edits made DURING play. An edit made while not playing goes to the
        // document, and a policy that could change that would make authoring conditional on a
        // setting nobody would think to check.
        for policy in [
            Persistence::RuntimeOnly,
            Persistence::PromoteOnExit,
            Persistence::Authoring,
        ] {
            assert_eq!(
                PlayState::Editing.persistence(policy),
                Persistence::Authoring
            );
        }
        assert_eq!(
            PlayState::Playing.persistence(Persistence::RuntimeOnly),
            Persistence::RuntimeOnly
        );
    }

    #[test]
    fn a_paused_world_is_still_a_playing_world_for_persistence_and_for_scheduling() {
        assert!(PlayState::Paused.is_play_mode());
        assert!(!PlayState::Paused.applies_on_arrival());
        assert!(PlayState::Editing.applies_on_arrival());
        assert_eq!(
            PlayState::Paused.persistence(Persistence::RuntimeOnly),
            Persistence::RuntimeOnly
        );
    }

    #[test]
    fn detaching_a_camera_takes_a_copy_and_can_touch_nothing_else() {
        let mut game_view = ViewState::new();
        game_view.camera.position = Vec3::new(5.0, 2.0, 9.0);

        let attached = CameraAttachment::attached(77);
        assert_eq!(attached.game_camera(), Some(77));

        let detached = attached.detach(&game_view);
        assert!(detached.is_detached());
        assert_eq!(detached.game_camera(), None);
        match &detached {
            CameraAttachment::Detached { state } => {
                assert_eq!(state.camera.position, game_view.camera.position);
            }
            CameraAttachment::Attached { .. } => panic!("it was detached"),
        }

        // Driving the editor's camera cannot reach the game's: the two are different values, and
        // there is no method here that writes one into the other.
        game_view.camera.position = Vec3::new(0.0, 0.0, 0.0);
        match &detached {
            CameraAttachment::Detached { state } => {
                assert_ne!(state.camera.position, game_view.camera.position);
            }
            CameraAttachment::Attached { .. } => panic!("it was detached"),
        }
    }

    #[test]
    fn what_reaches_the_document_is_a_question_with_an_answer() {
        assert!(Persistence::Authoring.reaches_the_document());
        assert!(Persistence::PromoteOnExit.reaches_the_document());
        assert!(!Persistence::RuntimeOnly.reaches_the_document());
    }

    #[test]
    fn every_play_mode_round_trips_its_own_word_and_an_unknown_one_is_refused() {
        // M11.b task 3.1. The words are the wire's, and the far end refuses a word it does not know
        // rather than reading it as the closest mode. `cy::gameplay::play_mode_of` is that far end,
        // and `src/gameplay/play/tests/test_editor_play.cpp` holds the same three strings — so a
        // spelling that drifted on one side fails a test on both.
        for mode in PlayMode::ALL {
            assert_eq!(PlayMode::from_name(mode.name()), Some(mode));
            assert!(!mode.badge().is_empty());
        }
        assert_eq!(PlayMode::from_name("console"), None);
        assert_eq!(PlayMode::from_name(""), None);
        assert_eq!(PlayMode::from_name("Hosted"), None);
    }

    #[test]
    fn the_capability_that_differs_by_mode_differs_by_transport() {
        // Queried rather than discovered by trying, and the same answers the engine gives.
        assert!(PlayMode::InEditor.can_step_frame());
        assert!(PlayMode::SeparateProcess.can_step_frame());
        assert!(!PlayMode::RemoteDevice.can_step_frame());
        for mode in PlayMode::ALL {
            assert!(mode.can_step_tick(), "{mode:?}");
        }
        // And the point of each of the two non-default modes.
        assert!(!PlayMode::InEditor.isolates_editor_state());
        assert!(PlayMode::SeparateProcess.isolates_editor_state());
        assert!(PlayMode::RemoteDevice.isolates_editor_state());
    }
}
