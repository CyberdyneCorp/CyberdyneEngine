//! Predict locally, reconcile against the runtime's echo. The answer to the milestone's named risk.
//!
//! --- WHICH OF THE TWO THIS CRATE DOES, STATED PLAINLY -------------------------------------------------
//!
//! **Prediction and reconciliation. Not a round trip per frame.**
//!
//! The milestone's control-path spike measured a blocking round trip at p50 9.9 ms locally, p99 20.2
//! ms, with one sample in nine hundred at 29 ms — so a blocking design drops frames on the tail even
//! on the same machine — and 27 ms metro, 85 ms continent, 170 ms intercontinental once the runtime
//! is not local. Its conclusion was unambiguous: "Never block a UI frame on the round trip. Draw the
//! gizmo from locally predicted state and reconcile against the runtime's authoritative echo, keyed
//! by the frame identifier the viewport transport already carries."
//!
//! That is what happens here, and the shape of the rest of this crate is what makes it possible
//! rather than aspirational:
//!
//!   * [`crate::gizmo::Drag`] writes to the **document**, which is in the editor's process. The
//!     gizmo is therefore drawn from a value that is already correct locally, with no wait.
//!   * The same operations go to the runtime as an encoded transaction, asynchronously, through
//!     `cy_editor_protocol::Session`, which has no blocking send.
//!   * The runtime's echo arrives later carrying **the frame identifier the request named**, and
//!     [`Reconciler`] matches them up. The identifier is the viewport transport's
//!     (`cy_editor_protocol::FrameId`), not a second one invented for this — the specification
//!     already requires the transport to carry it, and inventing another would make "the image and
//!     the echo are the same frame" unanswerable.
//!
//! --- WHAT RECONCILIATION IS FOR, WHICH IS NOT WHAT IT SOUNDS LIKE --------------------------------------
//!
//! It is **not** for correcting arithmetic. The editor and the runtime compute the same transform
//! from the same captured start state, so in the ordinary case the echo agrees exactly and
//! reconciliation does nothing but forget a prediction.
//!
//! It is for the cases where the runtime is *entitled to disagree*: a constraint clamped the value, a
//! physics body refused to move there, a script edited the same component, another editor moved the
//! same object. In those the runtime is authoritative and the editor's prediction is wrong — and the
//! failure mode without reconciliation is not a visible glitch, it is an editor that quietly shows a
//! world different from the one that exists.
//!
//! --- THE ONE RULE ------------------------------------------------------------------------------------
//!
//! **An echo for frame N settles every prediction up to and including N.** Predictions are keyed by
//! frame, the runtime applies them in order, and an echo therefore acknowledges the whole prefix. A
//! reconciler that matched only the exact frame would leak a prediction every time a frame's echo was
//! lost, and the leak would be invisible until the pending list was megabytes.

use std::collections::BTreeMap;

use cy_editor_core::ids::NodeId;
use cy_editor_protocol::FrameId;

use crate::gizmo::Transform3;
use crate::math::Vec3;

/// How far the runtime's answer may differ from the prediction before the editor is told.
///
/// A tolerance rather than an equality, because the two sides compute in the same precision but not
/// necessarily in the same order, and a difference of one unit in the last place is not a
/// disagreement about where the object is. A tenth of a millimetre is well below what a user can see
/// and well above float noise at any world coordinate a document uses.
pub const POSITION_TOLERANCE: f32 = 1e-4;

/// The same, for a quaternion's lanes.
pub const ROTATION_TOLERANCE: f32 = 1e-4;

/// What the editor drew, and what it expects the runtime to confirm.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Prediction {
    /// The frame the change was sent on.
    pub frame: FrameId,
    /// What was changed.
    pub node: NodeId,
    /// What the editor computed and is already showing.
    pub transform: Transform3,
}

/// The runtime disagreed, and the runtime is right.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Divergence {
    /// Which object.
    pub node: NodeId,
    /// Which frame's prediction was wrong.
    pub frame: FrameId,
    /// What the editor was showing.
    pub predicted: Transform3,
    /// What the runtime actually has. **This is the value to adopt.**
    pub observed: Transform3,
}

impl Divergence {
    /// How far the prediction was out, in world units. What a diagnostic reports, so that a
    /// constraint clamping by a millimetre and a script teleporting an object by ten metres are
    /// distinguishable without reading two transforms.
    #[must_use]
    pub fn distance(&self) -> f32 {
        (self.observed.translation - self.predicted.translation).length()
    }
}

/// Outstanding predictions, and what to do when an echo arrives.
///
/// Bounded by construction: a prediction is settled by any echo for its frame or a later one, and
/// [`Reconciler::forget_before`] drops everything older than a frame the editor knows the runtime has
/// passed. There is no path that grows this without bound, which matters because the alternative is a
/// leak that only appears in a session long enough for nobody to reproduce it.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct Reconciler {
    /// Keyed by node, then frame: one object may have a prediction outstanding for several frames of
    /// a drag, and the newest is the one being drawn.
    pending: BTreeMap<NodeId, BTreeMap<u64, Transform3>>,
    settled: u64,
    diverged: u64,
}

impl Reconciler {
    /// Nothing outstanding.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Record what the editor is showing for a frame it has just sent.
    pub fn predict(&mut self, prediction: Prediction) {
        self.pending
            .entry(prediction.node)
            .or_default()
            .insert(prediction.frame.as_u64(), prediction.transform);
    }

    /// Take the runtime's authoritative answer for one object at one frame.
    ///
    /// Returns a [`Divergence`] when the runtime's value differs from what the editor predicted for
    /// that frame — the caller adopts it. `None` means the prediction was right, or that there was
    /// no prediction to check, which is the ordinary case for a change the editor did not make.
    ///
    /// Every prediction for that object up to and including `frame` is settled either way. See the
    /// module note for why that is the prefix and not the exact frame.
    pub fn observe(
        &mut self,
        node: NodeId,
        frame: FrameId,
        observed: Transform3,
    ) -> Option<Divergence> {
        let predictions = self.pending.get_mut(&node)?;
        let settled: Vec<u64> = predictions
            .range(..=frame.as_u64())
            .map(|(at, _)| *at)
            .collect();
        let mut newest = None;
        for at in settled {
            if let Some(transform) = predictions.remove(&at) {
                newest = Some(transform);
                self.settled += 1;
            }
        }
        if predictions.is_empty() {
            self.pending.remove(&node);
        }

        let predicted = newest?;
        if agrees(predicted, observed) {
            return None;
        }
        self.diverged += 1;
        Some(Divergence {
            node,
            frame,
            predicted,
            observed,
        })
    }

    /// Drop every prediction older than `frame`, whatever happened to its echo.
    ///
    /// What a reconnection calls. A runtime that restarted will never echo the frames the previous
    /// one was sent, and keeping those predictions would mean the editor waited forever for an
    /// answer that cannot come.
    pub fn forget_before(&mut self, frame: FrameId) {
        self.pending.retain(|_, predictions| {
            predictions.retain(|at, _| *at >= frame.as_u64());
            !predictions.is_empty()
        });
    }

    /// Forget everything. What losing the session calls.
    pub fn clear(&mut self) {
        self.pending.clear();
    }

    /// How many predictions are waiting for an answer.
    #[must_use]
    pub fn outstanding(&self) -> usize {
        self.pending.values().map(BTreeMap::len).sum()
    }

    /// Whether anything is waiting.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.pending.is_empty()
    }

    /// How many predictions have been answered, and how many of those the runtime disagreed with.
    ///
    /// The second number is the one worth watching: a session where it is persistently non-zero is
    /// one where the editor and the runtime disagree about the world, and that is a defect somewhere
    /// rather than a tuning problem.
    #[must_use]
    pub const fn counters(&self) -> (u64, u64) {
        (self.settled, self.diverged)
    }
}

/// Whether two transforms are the same to within the tolerances above.
fn agrees(predicted: Transform3, observed: Transform3) -> bool {
    predicted
        .translation
        .nearly_equals(observed.translation, POSITION_TOLERANCE)
        && predicted
            .scale
            .nearly_equals(observed.scale, POSITION_TOLERANCE)
        && quaternion_agrees(predicted, observed)
}

/// Rotations, compared as the two quaternions they may legitimately be.
///
/// `q` and `−q` are the same rotation, and a runtime that normalised differently can return either.
/// Comparing the lanes without this would report a divergence for every object whose rotation had
/// passed through half a turn — a spurious correction, in the one place a spurious correction is
/// most visible.
fn quaternion_agrees(predicted: Transform3, observed: Transform3) -> bool {
    let lanes = predicted.rotation.to_array();
    let other = observed.rotation.to_array();
    let same = lanes
        .iter()
        .zip(other.iter())
        .all(|(a, b)| (a - b).abs() <= ROTATION_TOLERANCE);
    let negated = lanes
        .iter()
        .zip(other.iter())
        .all(|(a, b)| (a + b).abs() <= ROTATION_TOLERANCE);
    same || negated
}

/// A transform moved by `offset`. A convenience for the caller that adopts a divergence and for the
/// tests below.
#[must_use]
pub fn translated(transform: Transform3, offset: Vec3) -> Transform3 {
    Transform3 {
        translation: transform.translation + offset,
        ..transform
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_core::ids::DocumentId;

    use super::*;
    use crate::math::Quat;

    fn node(ordinal: u64) -> NodeId {
        NodeId::in_document(DocumentId::of_asset("worlds/city.cyworld"), ordinal)
    }

    fn at(x: f32) -> Transform3 {
        Transform3 {
            translation: Vec3::new(x, 0.0, 0.0),
            ..Transform3::default()
        }
    }

    #[test]
    fn an_agreeing_echo_settles_the_prediction_and_says_nothing() {
        let mut reconciler = Reconciler::new();
        let object = node(1);
        reconciler.predict(Prediction {
            frame: FrameId::from_raw(7),
            node: object,
            transform: at(3.0),
        });
        assert_eq!(reconciler.outstanding(), 1);

        assert_eq!(
            reconciler.observe(object, FrameId::from_raw(7), at(3.0)),
            None,
            "the ordinary case: the two sides computed the same thing"
        );
        assert!(reconciler.is_empty());
        assert_eq!(reconciler.counters(), (1, 0));
    }

    #[test]
    fn a_runtime_that_clamped_the_value_is_reported_so_the_editor_can_adopt_it() {
        // The case reconciliation exists for: a constraint, a physics body, a script, or another
        // editor. The runtime is authoritative and the prediction was wrong.
        let mut reconciler = Reconciler::new();
        let object = node(1);
        reconciler.predict(Prediction {
            frame: FrameId::from_raw(3),
            node: object,
            transform: at(10.0),
        });

        let divergence = reconciler
            .observe(object, FrameId::from_raw(3), at(4.0))
            .expect("the runtime disagreed");
        assert_eq!(
            divergence.observed.translation.x.to_bits(),
            4.0_f32.to_bits()
        );
        assert!((divergence.distance() - 6.0).abs() < 1e-5);
        assert_eq!(reconciler.counters(), (1, 1));
    }

    #[test]
    fn an_echo_settles_the_whole_prefix_rather_than_one_frame() {
        // A drag sends a change every frame. If an echo settled only its own frame, a lost or
        // coalesced echo would leak a prediction — invisibly, until the pending list was megabytes.
        let mut reconciler = Reconciler::new();
        let object = node(1);
        for frame in 1_u16..=60 {
            reconciler.predict(Prediction {
                frame: FrameId::from_raw(u64::from(frame)),
                node: object,
                transform: at(f32::from(frame)),
            });
        }
        assert_eq!(reconciler.outstanding(), 60);

        // The runtime coalesced and answered once, for the newest frame it applied.
        let divergence = reconciler.observe(object, FrameId::from_raw(60), at(60.0));
        assert_eq!(divergence, None, "the newest prediction was right");
        assert!(reconciler.is_empty(), "and the whole prefix is settled");
        assert_eq!(reconciler.counters(), (60, 0));
    }

    #[test]
    fn a_later_frame_is_left_outstanding_by_an_earlier_echo() {
        let mut reconciler = Reconciler::new();
        let object = node(1);
        reconciler.predict(Prediction {
            frame: FrameId::from_raw(4),
            node: object,
            transform: at(1.0),
        });
        reconciler.predict(Prediction {
            frame: FrameId::from_raw(9),
            node: object,
            transform: at(2.0),
        });

        assert_eq!(
            reconciler.observe(object, FrameId::from_raw(4), at(1.0)),
            None
        );
        assert_eq!(reconciler.outstanding(), 1, "frame nine is still in flight");
    }

    #[test]
    fn an_echo_for_something_the_editor_did_not_predict_is_not_a_divergence() {
        // A script moved an object the editor is not dragging. That is not the editor being wrong,
        // it is the world changing, and reporting it as a divergence would produce a correction
        // storm out of an ordinary running game.
        let mut reconciler = Reconciler::new();
        assert_eq!(
            reconciler.observe(node(2), FrameId::from_raw(1), at(5.0)),
            None
        );
        assert_eq!(reconciler.counters(), (0, 0));
    }

    #[test]
    fn a_rotation_and_its_negation_are_the_same_rotation_and_not_a_divergence() {
        // q and −q describe the same orientation. Comparing lanes without this reports a
        // divergence for every object whose rotation passed through half a turn, and a spurious
        // correction is worst exactly there.
        let mut reconciler = Reconciler::new();
        let object = node(1);
        let turn = Quat::from_axis_angle(Vec3::Y, 2.0);
        let negated = Quat {
            x: -turn.x,
            y: -turn.y,
            z: -turn.z,
            w: -turn.w,
        };
        reconciler.predict(Prediction {
            frame: FrameId::from_raw(1),
            node: object,
            transform: Transform3 {
                rotation: turn,
                ..Transform3::default()
            },
        });
        assert_eq!(
            reconciler.observe(
                object,
                FrameId::from_raw(1),
                Transform3 {
                    rotation: negated,
                    ..Transform3::default()
                }
            ),
            None
        );
    }

    #[test]
    fn float_noise_is_not_a_disagreement() {
        let mut reconciler = Reconciler::new();
        let object = node(1);
        reconciler.predict(Prediction {
            frame: FrameId::from_raw(1),
            node: object,
            transform: at(1000.0),
        });
        // One unit in the last place at a thousand metres.
        let noisy = at(f32::from_bits(1000.0_f32.to_bits() + 1));
        assert_eq!(
            reconciler.observe(object, FrameId::from_raw(1), noisy),
            None
        );
    }

    #[test]
    fn a_runtime_restart_forgets_the_predictions_that_can_never_be_answered() {
        let mut reconciler = Reconciler::new();
        let object = node(1);
        for frame in 1..=10 {
            reconciler.predict(Prediction {
                frame: FrameId::from_raw(frame),
                node: object,
                transform: at(1.0),
            });
        }
        reconciler.forget_before(FrameId::from_raw(8));
        assert_eq!(
            reconciler.outstanding(),
            3,
            "frames eight, nine and ten remain"
        );

        reconciler.clear();
        assert!(reconciler.is_empty());
    }
}
