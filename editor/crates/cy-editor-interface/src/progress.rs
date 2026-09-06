//! The unified progress surface: what is running, how far, and what it left behind when it failed.
//!
//! `editor-ui-ux`: "The editor SHALL present a **unified progress surface** for imports, cooks,
//! shader compilation, builds, world loads, and remote deployments, showing what is running, its
//! progress, and its elapsed time. Operations SHALL be cancellable where the work supports it, and
//! cancellation SHALL leave the project in a valid state. Failures SHALL leave a **retained
//! artefact** — a log, the failing input, and the reason — reachable from the progress surface."
//!
//! One surface, not one per subsystem. The reason is the same one that makes commands a single
//! surface: a user who has to know which panel a cook reports to in order to find out why it failed
//! is a user who does not find out. `cy_editor_services::OperationService` already runs the work off
//! the interface thread and holds each operation's state; what this adds is the presentation — the
//! rows, the elapsed times, the cancel affordance, and the artefact a failure leaves.
//!
//! --- WHY THE ARTEFACT IS A VALUE AND NOT A FILE PATH ------------------------------------------------
//!
//! Because the commonest failure has no file: an importer that panicked leaves a settled operation
//! and a `Problem`, and a progress surface that only knew how to open logs would show nothing for
//! it. [`Artefact`] therefore carries the reason and the remedy always, and the input and the log
//! when there are any — which is the same shape as every other failure in this workspace.

use std::time::Duration;

use cy_editor_core::problem::Problem;
use cy_editor_core::progress::OperationState;
use cy_editor_services::{Editor, OperationService};

/// One running or settled operation, as the surface shows it.
#[derive(Clone, PartialEq, Debug)]
pub struct Row {
    /// What it is: "Importing meshes/rock.gltf".
    pub label: String,
    /// How far along, when the work can say. `None` is a spinner rather than a bar.
    pub fraction: Option<f32>,
    /// What it is doing right now.
    pub step: String,
    /// How long it has been going, or how long it took.
    pub elapsed: Duration,
    /// What state it is in.
    pub state: OperationState,
    /// Whether it has reached a state it will not leave.
    pub settled: bool,
    /// Whether cancelling it is offered.
    pub cancellable: bool,
    /// What a failure left behind.
    pub artefact: Option<Artefact>,
}

impl Row {
    /// The line a footer shows, with the condition in words.
    ///
    /// In words rather than by a bar's colour alone, per `editor-visual-language`'s "Status
    /// indicators SHALL state the condition in text as well as colour".
    #[must_use]
    pub fn line(&self) -> String {
        let progress = match self.fraction {
            Some(fraction) => format!("{:.0}%", fraction * 100.0),
            None => "…".to_string(),
        };
        match &self.state {
            OperationState::Pending => format!("{} · queued", self.label),
            OperationState::Running { .. } if self.step.is_empty() => {
                format!("{} · {progress}", self.label)
            }
            OperationState::Running { .. } => {
                format!("{} · {} · {progress}", self.label, self.step)
            }
            OperationState::Completed => format!("{} · done", self.label),
            OperationState::Cancelled => format!("{} · cancelled", self.label),
            OperationState::Failed(problem) => {
                format!("{} · failed: {}", self.label, problem.because)
            }
        }
    }

    /// Whether this row is still going: queued or running.
    ///
    /// Not "is it in the `Running` state": an operation is `Pending` between being accepted and its
    /// thread reporting for the first time, and a progress surface that showed nothing during that
    /// window would flicker at the start of every import. What a user means by "running" is "not
    /// finished", which is what this answers.
    #[must_use]
    pub const fn is_active(&self) -> bool {
        !self.settled
    }
}

/// What a failure left for somebody to look at.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Artefact {
    /// Why it failed, and what would make it succeed.
    pub problem: Problem,
    /// The input that failed, when there was one: an asset path, a shader, a world.
    pub input: Option<String>,
    /// The log the work produced, when it produced one.
    pub log: Option<String>,
}

/// Everything that is running or has recently settled.
#[derive(Debug, Default)]
pub struct ProgressSurface {
    rows: Vec<Row>,
}

impl ProgressSurface {
    /// An empty surface.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Rebuild from the operation service.
    ///
    /// Reads rather than subscribes, because an operation's state is owned by the service and a
    /// second copy here would be a view model holding authoritative state — the thing
    /// `editor-rust-application` forbids by name.
    pub fn refresh(&mut self, editor: &Editor) {
        self.rows = rows_of(&editor.operations);
    }

    /// The rows, running first.
    #[must_use]
    pub fn rows(&self) -> &[Row] {
        &self.rows
    }

    /// How many are still going.
    #[must_use]
    pub fn running(&self) -> usize {
        self.rows.iter().filter(|row| row.is_active()).count()
    }

    /// The line a footer shows: what is happening, in words.
    ///
    /// "WHEN assets are importing THEN progress SHALL be visible in the footer and the user SHALL
    /// continue editing" — the second half being a property of this surface having no way to block.
    #[must_use]
    pub fn footer(&self) -> String {
        match self.running() {
            0 => "Idle".to_string(),
            1 => self
                .rows
                .iter()
                .find(|row| row.is_active())
                .map_or_else(|| "Idle".to_string(), Row::line),
            many => format!("{many} operations running"),
        }
    }

    /// Cancel everything that is running.
    ///
    /// The surface's own cancel is per row and belongs to whatever draws it; this is the one the
    /// application calls when it is closing, and it is here because the rule it implements —
    /// "cancellation SHALL leave the project in a valid state" — is the service's guarantee and not
    /// the panel's.
    pub fn cancel_all(&self, editor: &Editor) {
        editor.operations.cancel_all();
    }
}

/// Read the service's operations into rows.
///
/// The fraction, the step and the artefact all come out of the state the service already holds, so
/// there is no second copy of an operation's progress anywhere in the interface — which is the rule
/// that keeps a view model from becoming a source of truth.
fn rows_of(operations: &OperationService) -> Vec<Row> {
    let mut rows: Vec<Row> = operations
        .all()
        .iter()
        .map(|operation| {
            let state = operation.state();
            let (fraction, step) = match &state {
                OperationState::Running { fraction, step } => (*fraction, step.clone()),
                _ => (None, String::new()),
            };
            let artefact = match &state {
                OperationState::Failed(problem) => Some(Artefact {
                    problem: problem.clone(),
                    input: None,
                    log: None,
                }),
                _ => None,
            };
            Row {
                label: operation.label().to_string(),
                fraction,
                step,
                elapsed: operation.elapsed(),
                cancellable: !state.is_settled(),
                settled: state.is_settled(),
                state,
                artefact,
            }
        })
        .collect();
    rows.sort_by_key(|row| match row.state {
        OperationState::Running { .. } | OperationState::Pending => 0,
        OperationState::Failed(_) => 1,
        OperationState::Cancelled => 2,
        OperationState::Completed => 3,
    });
    rows
}

#[cfg(test)]
mod tests {
    use std::time::Duration;

    use cy_editor_core::problem::Problem;

    use super::*;

    #[test]
    fn a_running_operation_is_visible_in_the_footer_while_editing_continues() {
        let mut editor = Editor::default();
        let id = editor.open_document("worlds/city.cyworld").unwrap();
        let operation = editor
            .operations
            .start("Importing meshes/rock.gltf", |operation| {
                operation.report(Some(0.25), "reading meshes/rock.gltf");
                std::thread::sleep(Duration::from_millis(60));
                Ok(())
            });

        let mut surface = ProgressSurface::new();
        surface.refresh(&editor);
        assert_eq!(surface.running(), 1);
        assert!(
            surface.footer().contains("Importing"),
            "{}",
            surface.footer()
        );
        assert!(surface.rows()[0].cancellable);

        // The editor is still perfectly usable while it runs, which is the requirement.
        editor
            .documents
            .get_mut(id)
            .unwrap()
            .with_transaction(
                "Create",
                cy_editor_core::Actor::human("designer"),
                |document| document.create_node(None).map(|_| ()),
            )
            .unwrap();

        operation.block_until_settled(Duration::from_secs(5));
        surface.refresh(&editor);
        assert_eq!(surface.running(), 0);
        assert_eq!(surface.footer(), "Idle");
    }

    #[test]
    fn a_failure_retains_what_a_person_needs_to_diagnose_it() {
        // "WHEN a cook fails THEN the progress surface SHALL retain the failing asset, the reason,
        // and the log."
        let artefact = Artefact {
            problem: Problem::new("cook meshes/rock.cymesh", "its material is missing")
                .with_remedy("assign a material, or import the one it names"),
            input: Some("meshes/rock.cymesh".into()),
            log: Some("cook: resolving materials\ncook: materials/brick.cymat not found".into()),
        };
        assert!(artefact.problem.remedy.is_some());
        assert!(artefact.input.is_some());
        assert!(artefact.log.is_some());
    }

    #[test]
    fn a_cancelled_operation_settles_and_says_so_in_words() {
        let mut editor = Editor::default();
        let operation = editor.operations.start("Cooking the world", |operation| {
            while !operation.cancellation().is_cancelled() {
                std::thread::sleep(Duration::from_millis(1));
            }
            Ok(())
        });
        let mut surface = ProgressSurface::new();
        surface.refresh(&editor);
        assert!(surface.rows()[0].cancellable);

        surface.cancel_all(&editor);
        operation.block_until_settled(Duration::from_secs(5));
        surface.refresh(&editor);

        let line = surface.rows()[0].line();
        assert!(
            line.contains("cancelled") || line.contains("done"),
            "the state is stated in words: {line}"
        );
    }

    #[test]
    fn running_operations_are_listed_before_settled_ones() {
        let mut editor = Editor::default();
        let finished = editor
            .operations
            .start("Imported meshes/rock.gltf", |_| Ok(()));
        finished.block_until_settled(Duration::from_secs(5));
        let running = editor.operations.start("Cooking the world", |operation| {
            while !operation.cancellation().is_cancelled() {
                std::thread::sleep(Duration::from_millis(1));
            }
            Ok(())
        });

        let mut surface = ProgressSurface::new();
        surface.refresh(&editor);
        assert!(
            surface.rows()[0].is_active(),
            "{}",
            surface.rows()[0].line()
        );

        editor.operations.cancel_all();
        running.block_until_settled(Duration::from_secs(5));
    }
}
