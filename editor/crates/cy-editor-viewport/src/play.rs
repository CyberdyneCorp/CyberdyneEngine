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
}
