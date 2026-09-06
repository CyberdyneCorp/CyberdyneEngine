//! Long operations, off the interface thread and observable from it. Task 2.6.
//!
//! "The interface thread SHALL NEVER block on asset import, cooking, shader compilation, world
//! loading, builds, network operations, project scans, or engine calls of unbounded duration."
//!
//! [`OperationService::start`] takes the work and a thread runs it. What comes back is a handle to
//! *state*, which the interface reads once per frame and renders — never a future it holds, never a
//! join it waits on. There is no call on this type that blocks, which is how the rule is kept rather
//! than remembered.

use std::sync::Arc;

use cy_editor_core::observe::{Revision, Versioned};
use cy_editor_core::problem::Problem;
use cy_editor_core::progress::{Cancellation, Operation};

/// Every long operation the editor has started, running or settled.
#[derive(Default)]
pub struct OperationService {
    operations: Versioned<Vec<Arc<Operation>>>,
}

impl OperationService {
    /// A service with nothing running.
    #[must_use]
    pub fn new() -> Self {
        Self {
            operations: Versioned::new(Vec::new()),
        }
    }

    /// Start a long operation on its own thread.
    ///
    /// The closure is given the [`Operation`] so that it can report progress and check for
    /// cancellation, and its result settles the operation. A panic inside it settles the operation
    /// as failed rather than taking the editor with it — which is the same isolation argument as the
    /// hosted runtime's, applied to the editor's own background work.
    pub fn start(
        &mut self,
        label: impl Into<String>,
        work: impl FnOnce(&Operation) -> Result<(), Problem> + Send + 'static,
    ) -> Arc<Operation> {
        let operation = Operation::new(label);
        self.operations
            .update(|running| running.push(Arc::clone(&operation)));

        let handle = Arc::clone(&operation);
        std::thread::Builder::new()
            .name("cy-editor-operation".into())
            .spawn(move || {
                let outcome =
                    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| work(&handle)));
                match outcome {
                    Ok(Ok(())) if handle.cancellation().is_cancelled() => handle.cancelled(),
                    Ok(Ok(())) => handle.complete(),
                    Ok(Err(problem)) => handle.fail(problem),
                    Err(_) => handle.fail(
                        Problem::new("a background operation", "it stopped unexpectedly")
                            .with_remedy("see the editor's log for what it was doing"),
                    ),
                }
            })
            .expect("spawning an operation thread");

        operation
    }

    /// Every operation, settled or not.
    #[must_use]
    pub fn all(&self) -> &[Arc<Operation>] {
        self.operations.get()
    }

    /// The operations that are still running.
    pub fn running(&self) -> impl Iterator<Item = &Arc<Operation>> {
        self.operations
            .get()
            .iter()
            .filter(|operation| !operation.state().is_settled())
    }

    /// The revision of the operation *set* — it moves when one starts, not while one runs.
    ///
    /// A progress bar watches an operation's own state; a list of operations watches this. Making
    /// this move on every progress report would rebuild the whole list sixty times a second for one
    /// bar that moved, which is exactly the polling the specification forbids.
    #[must_use]
    pub fn revision(&self) -> Revision {
        self.operations.revision()
    }

    /// Forget operations that have settled, so the list does not grow for a session.
    ///
    /// Checks before it mutates. `Versioned::update` bumps the revision whether or not the closure
    /// changed anything — deliberately, because asking a closure is unreliable — so a `retain` that
    /// removed nothing would still move the revision, and this is called once per interface frame.
    /// The first version of this method did exactly that and a test caught it: an idle editor
    /// reported a hundred changes in a hundred frames, which is precisely the polling
    /// `editor-rust-application` forbids.
    pub fn retain_running(&mut self) {
        let settled = self
            .operations
            .get()
            .iter()
            .any(|operation| operation.state().is_settled());
        if !settled {
            return;
        }
        self.operations
            .update(|running| running.retain(|operation| !operation.state().is_settled()));
    }

    /// Ask every running operation to stop. What quitting does.
    pub fn cancel_all(&self) {
        for operation in self.running() {
            operation.cancel();
        }
    }
}

/// A cancellation token a caller can hand to work that is not on this service's thread.
#[must_use]
pub fn cancellation_of(operation: &Operation) -> Cancellation {
    operation.cancellation()
}

#[cfg(test)]
mod tests {
    use std::time::Duration;

    use cy_editor_core::progress::OperationState;

    use super::*;

    #[test]
    fn the_interface_thread_never_waits_for_the_work() {
        // Held by the test, so the work CANNOT finish until the test lets it. That makes the
        // property structural rather than a race against a clock: if `start` waited for the work,
        // this test would deadlock rather than fail intermittently on a loaded machine.
        //
        // An earlier version asserted `start()` returned within thirty milliseconds and did fail
        // once, on a machine that was running a formatter at the same time. A timing threshold is
        // the wrong instrument for a question about ordering.
        let gate = Arc::new(std::sync::Barrier::new(2));
        let held = Arc::clone(&gate);

        let mut service = OperationService::new();
        let operation = service.start("Import city.gltf", move |operation| {
            operation.report(Some(0.5), "reading meshes");
            held.wait();
            Ok(())
        });

        // Reached only because `start` returned while the work was still blocked.
        assert!(
            !operation.state().is_settled(),
            "the work is still waiting on the barrier"
        );
        gate.wait();

        assert_eq!(
            operation.block_until_settled(Duration::from_secs(5)),
            OperationState::Completed
        );
    }

    #[test]
    fn cancellation_is_offered_and_leaves_completed_work_valid() {
        let mut service = OperationService::new();
        let done = Arc::new(std::sync::atomic::AtomicU32::new(0));
        let counter = Arc::clone(&done);
        let operation = service.start("Cook shaders", move |operation| {
            for _ in 0..1000 {
                if operation.cancellation().is_cancelled() {
                    return Ok(());
                }
                counter.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                std::thread::sleep(Duration::from_millis(1));
            }
            Ok(())
        });

        std::thread::sleep(Duration::from_millis(20));
        operation.cancel();
        assert_eq!(
            operation.block_until_settled(Duration::from_secs(5)),
            OperationState::Cancelled
        );
        assert!(
            done.load(std::sync::atomic::Ordering::Relaxed) > 0,
            "the work it did is still done"
        );
    }

    #[test]
    fn a_failure_carries_its_remedy_to_the_interface() {
        let mut service = OperationService::new();
        let operation = service.start("Connect", |_| {
            Err(
                Problem::new("connect to the runtime", "nothing is listening")
                    .with_remedy("start the runtime with `just run-editor --host`"),
            )
        });
        match operation.block_until_settled(Duration::from_secs(5)) {
            OperationState::Failed(problem) => assert!(problem.remedy.is_some()),
            other => panic!("expected a failure, got {other:?}"),
        }
    }

    #[test]
    fn work_that_stops_unexpectedly_does_not_take_the_editor_with_it() {
        let mut service = OperationService::new();
        let operation = service.start("Import a broken file", |_| {
            panic!("a defect in an importer")
        });
        match operation.block_until_settled(Duration::from_secs(5)) {
            OperationState::Failed(problem) => assert!(problem.remedy.is_some()),
            other => panic!("expected a failure, got {other:?}"),
        }
        // And the service is still usable.
        assert_eq!(service.all().len(), 1);
    }

    #[test]
    fn a_progress_report_does_not_rebuild_the_operation_list() {
        let mut service = OperationService::new();
        let operation = service.start("Cook", |operation| {
            operation.report(Some(0.5), "half way");
            Ok(())
        });
        let revision = service.revision();
        operation.block_until_settled(Duration::from_secs(5));
        assert_eq!(
            service.revision(),
            revision,
            "progress is watched per operation, not per list"
        );
    }
}
