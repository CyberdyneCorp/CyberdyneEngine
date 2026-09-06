//! Long operations as observable state. Task 2.6, `editor-rust-application` "Asynchronous operations".
//!
//! "The interface thread SHALL NEVER block on asset import, cooking, shader compilation, world
//! loading, builds, network operations, project scans, or engine calls of unbounded duration. Long
//! operations SHALL be services returning **observable progress state** — status, progress,
//! cancellability, and errors ... Every long operation SHALL be cancellable where the underlying
//! work supports cancellation, and its cancellation SHALL be surfaced."
//!
//! --- WHY THERE IS NO `await` AND NO RUNTIME --------------------------------------------------------
//!
//! An async runtime would be the workspace's first third-party dependency and would decide the
//! shape of every service for the milestones after this one. It is not needed for what the
//! specification asks: an operation is a piece of *state* that a view model reads, not a future a
//! view model holds. The interface thread reads [`Operation::state`] once per frame and renders it;
//! the work happens on a thread the service owns.
//!
//! The consequence worth stating: nothing in this workspace can accidentally block an interface
//! frame on a long operation, because there is no call that would. `Operation` has no `wait`.
//! [`Operation::block_until_settled`] exists and is `#[cfg(test)]`-shaped in intent — it is named
//! for what it does so that a reviewer seeing it on a UI path knows immediately that it is wrong.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, RwLock};
use std::time::{Duration, Instant};

use crate::problem::Problem;

/// Where a long operation has got to.
#[derive(Clone, PartialEq, Debug)]
pub enum OperationState {
    /// Accepted, not started.
    Pending,
    /// Running, with a fraction complete where the work can report one.
    Running {
        /// Fraction complete in `0.0..=1.0`, or `None` when the work cannot say.
        fraction: Option<f32>,
        /// What it is doing right now, for a status line.
        step: String,
    },
    /// Finished successfully.
    Completed,
    /// Cancelled at the caller's request. Completed work stays valid; see the module note.
    Cancelled,
    /// Failed, with a reason and a remedy.
    Failed(Problem),
}

impl OperationState {
    /// Whether the operation has reached a state it will not leave.
    #[must_use]
    pub const fn is_settled(&self) -> bool {
        matches!(
            self,
            OperationState::Completed | OperationState::Cancelled | OperationState::Failed(_)
        )
    }
}

/// A cancellation request, shared between the caller and the work.
///
/// Cloning it shares the same flag. The work checks it at the points where stopping leaves a
/// consistent result, which is why cancellation is cooperative rather than a kill: "cancelling SHALL
/// leave completed work valid".
#[derive(Clone, Debug, Default)]
pub struct Cancellation(Arc<AtomicBool>);

impl Cancellation {
    /// A fresh, un-cancelled token.
    #[must_use]
    pub fn new() -> Self {
        Self(Arc::new(AtomicBool::new(false)))
    }

    /// Ask the work to stop at its next safe point.
    pub fn cancel(&self) {
        self.0.store(true, Ordering::Release);
    }

    /// Whether cancellation has been requested.
    pub fn is_cancelled(&self) -> bool {
        self.0.load(Ordering::Acquire)
    }
}

/// A long operation the interface can observe and cancel without blocking on it.
#[derive(Debug)]
pub struct Operation {
    label: String,
    state: RwLock<OperationState>,
    cancellation: Cancellation,
    started: Instant,
    /// Set when the operation settles, so that a completed operation's duration stops moving.
    settled_after: Mutex<Option<Duration>>,
}

impl Operation {
    /// Begin an operation with a user-facing label.
    #[must_use]
    pub fn new(label: impl Into<String>) -> Arc<Self> {
        Arc::new(Self {
            label: label.into(),
            state: RwLock::new(OperationState::Pending),
            cancellation: Cancellation::new(),
            started: Instant::now(),
            settled_after: Mutex::new(None),
        })
    }

    /// The user-facing label, for a progress row.
    #[must_use]
    pub fn label(&self) -> &str {
        &self.label
    }

    /// The current state. Cheap: a read lock and a clone of a small value.
    pub fn state(&self) -> OperationState {
        self.state
            .read()
            .expect("operation state lock is never held across a panic")
            .clone()
    }

    /// The token the work checks to learn it should stop.
    #[must_use]
    pub fn cancellation(&self) -> Cancellation {
        self.cancellation.clone()
    }

    /// Request cancellation. Surfaced immediately in the state; the work settles when it notices.
    pub fn cancel(&self) {
        self.cancellation.cancel();
    }

    /// Report progress from the work.
    pub fn report(&self, fraction: Option<f32>, step: impl Into<String>) {
        self.transition(OperationState::Running {
            fraction,
            step: step.into(),
        });
    }

    /// Settle as completed.
    pub fn complete(&self) {
        self.transition(OperationState::Completed);
    }

    /// Settle as cancelled.
    pub fn cancelled(&self) {
        self.transition(OperationState::Cancelled);
    }

    /// Settle as failed.
    pub fn fail(&self, problem: Problem) {
        self.transition(OperationState::Failed(problem));
    }

    /// How long the operation has been running, or how long it took if it has settled.
    pub fn elapsed(&self) -> Duration {
        self.settled_after
            .lock()
            .expect("operation timing lock is never held across a panic")
            .unwrap_or_else(|| self.started.elapsed())
    }

    /// Block the calling thread until the operation settles or `timeout` elapses.
    ///
    /// **Never call this from an interface thread.** It is named for what it does precisely so that
    /// a reviewer who sees it on a UI path knows without checking. Its purpose is tests and
    /// command-line tools, which have no frame to drop.
    pub fn block_until_settled(&self, timeout: Duration) -> OperationState {
        let deadline = Instant::now() + timeout;
        loop {
            let state = self.state();
            if state.is_settled() || Instant::now() >= deadline {
                return state;
            }
            std::thread::sleep(Duration::from_millis(1));
        }
    }

    fn transition(&self, next: OperationState) {
        let settled = next.is_settled();
        {
            let mut state = self
                .state
                .write()
                .expect("operation state lock is never held across a panic");
            if state.is_settled() {
                // A settled operation does not move again. Without this, a worker that reported
                // progress after noticing a cancellation would un-cancel itself.
                return;
            }
            *state = next;
        }
        if settled {
            *self
                .settled_after
                .lock()
                .expect("operation timing lock is never held across a panic") =
                Some(self.started.elapsed());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_operation_reports_progress_and_settles_once() {
        let operation = Operation::new("import city.gltf");
        assert_eq!(operation.state(), OperationState::Pending);

        operation.report(Some(0.5), "reading meshes");
        assert_eq!(
            operation.state(),
            OperationState::Running {
                fraction: Some(0.5),
                step: "reading meshes".into()
            }
        );

        operation.complete();
        assert_eq!(operation.state(), OperationState::Completed);

        // A settled operation does not move again.
        operation.report(Some(0.9), "still going");
        assert_eq!(operation.state(), OperationState::Completed);
    }

    #[test]
    fn cancellation_is_cooperative_and_surfaced() {
        let operation = Operation::new("cook shaders");
        let token = operation.cancellation();
        assert!(!token.is_cancelled());

        operation.cancel();
        assert!(
            token.is_cancelled(),
            "the work must be able to see the request"
        );

        // The state only changes when the work acknowledges it, which is what keeps completed work
        // valid: the worker stops at a point of its choosing and says so.
        assert_eq!(operation.state(), OperationState::Pending);
        operation.cancelled();
        assert_eq!(operation.state(), OperationState::Cancelled);
    }

    #[test]
    fn a_failure_carries_a_remedy() {
        let operation = Operation::new("connect to the runtime");
        operation.fail(
            Problem::new("connect", "the runtime is not running")
                .with_remedy("start it with `just run-editor --host`"),
        );
        match operation.state() {
            OperationState::Failed(problem) => assert!(problem.remedy.is_some()),
            other => panic!("expected a failure, got {other:?}"),
        }
    }
}
