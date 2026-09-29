// SPDX-License-Identifier: MIT
//! The lighting and lightmap baking editor: a form over the engine's bake and its density view.
//!
//! `editor-architecture` names "lighting and lightmap baking" among the specialised editors and
//! builds it on [`super::Surface::Form`] — named settings and a button that starts a bake. What the
//! form holds is the INPUT to two commands, never a second copy of their state:
//!
//! - `lighting.bake-lightmaps` runs the engine's bake (`cy_build lightmap`) as an operation, so its
//!   progress and its cancel are the unified progress surface's, and an agent reaches the same bake
//!   through the same command;
//! - `viewport.view-mode.lightmap-density` asks the ENGINE to draw the density view
//!   (`editor-viewport-and-gizmos`: "an engine debug view requested by the editor, not editor-side
//!   drawing").
//!
//! The running bake is read off the operation service by its request identity, so the form cannot
//! disagree with the footer about how far it has got.

use cy_editor_commands::Arguments;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::progress::OperationState;
use cy_editor_core::value::Value;
use cy_editor_services::Editor;
use cy_editor_viewport::viewmode::ViewMode;

/// The command a bake is started with.
pub const BAKE_COMMAND: &str = "lighting.bake-lightmaps";
/// The command a running bake is stopped with.
pub const CANCEL_COMMAND: &str = "lighting.cancel-lightmap-bake";

/// What the form shows about the bake the editor most recently started.
#[derive(Clone, PartialEq, Debug)]
pub struct BakeStatus {
    /// The operation's request identity.
    pub request: u64,
    /// Its state, as the operation service holds it.
    pub state: OperationState,
    /// How far, when running.
    pub fraction: Option<f32>,
    /// What it is doing, when running.
    pub step: String,
}

impl BakeStatus {
    /// Whether cancelling it is offered.
    #[must_use]
    pub const fn cancellable(&self) -> bool {
        !self.state.is_settled()
    }
}

/// The form's inputs.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct LightingForm {
    /// The level's project-relative `.cylightmap` description.
    pub description: String,
    /// Where the cooked lightmap goes, project-relative; empty for the service's default.
    pub output: String,
}

impl LightingForm {
    /// The invocation the Bake button makes.
    ///
    /// # Errors
    ///
    /// When no description is named, so the button can say why it did nothing.
    pub fn bake(&self) -> Result<(&'static str, Arguments)> {
        let description = self.description.trim();
        if description.is_empty() {
            return Err(
                Problem::new("bake lightmaps", "no level description is named")
                    .with_remedy("name the level's .cylightmap description, project-relative"),
            );
        }
        let mut arguments =
            Arguments::new().with("description", Value::Text(description.to_string()));
        if !self.output.trim().is_empty() {
            arguments = arguments.with("output", Value::Text(self.output.trim().to_string()));
        }
        Ok((BAKE_COMMAND, arguments))
    }

    /// The invocation the Cancel button makes: the bake `status` describes.
    #[must_use]
    pub fn cancel(status: &BakeStatus) -> (&'static str, Arguments) {
        (
            CANCEL_COMMAND,
            Arguments::new().with(
                "request",
                Value::Int(i64::try_from(status.request).unwrap_or(i64::MAX)),
            ),
        )
    }

    /// The command the density view button invokes: the engine's view mode.
    #[must_use]
    pub fn density_view() -> String {
        ViewMode::LightmapDensity.command_id()
    }

    /// The bake the editor most recently started, read off the operation service.
    #[must_use]
    pub fn status(editor: &Editor) -> Option<BakeStatus> {
        let request = editor.lightmaps.latest()?;
        let operation = editor
            .operations
            .all()
            .iter()
            .find(|operation| operation.id() == request)?;
        let state = operation.state();
        let (fraction, step) = match &state {
            OperationState::Running { fraction, step } => (*fraction, step.clone()),
            _ => (None, String::new()),
        };
        Some(BakeStatus {
            request,
            state,
            fraction,
            step,
        })
    }
}

#[cfg(test)]
mod tests {
    use std::path::Path;
    use std::sync::Arc;
    use std::time::Duration;

    use cy_editor_commands::{Registry, Scope};
    use cy_editor_core::progress::Cancellation;
    use cy_editor_services::lightmaps::{
        BakeStep, LightmapBakeOutcome, LightmapBakeService, LightmapBaker,
    };

    use super::*;

    /// A bake that reports its trace in steps until cancelled, held at a gate until the test lets
    /// it start.
    struct Slow(Arc<std::sync::Barrier>);

    impl LightmapBaker for Slow {
        fn describe(&self) -> String {
            "slow".into()
        }

        fn bake(
            &self,
            _: &Path,
            _: &str,
            output: &str,
            step: &mut dyn FnMut(BakeStep),
            cancellation: &Cancellation,
        ) -> Result<Option<LightmapBakeOutcome>> {
            step(BakeStep {
                stage: "trace".into(),
                done: 1024,
                total: 65536,
            });
            self.0.wait();
            while !cancellation.is_cancelled() {
                std::thread::sleep(Duration::from_millis(1));
            }
            let _ = output;
            Ok(None)
        }
    }

    #[test]
    fn the_form_bakes_through_the_command_and_cancels_what_it_started() {
        let root = std::env::temp_dir().join(format!("cy-lighting-form-{}", std::process::id()));
        std::fs::create_dir_all(root.join("levels")).unwrap();
        std::fs::write(root.join("levels/corner.cylightmap"), "cylightmap 1\n").unwrap();
        let gate = Arc::new(std::sync::Barrier::new(2));
        let mut editor = Editor::default().with_lightmap_baker(LightmapBakeService::with_baker(
            &root,
            Arc::new(Slow(Arc::clone(&gate))),
        ));
        let mut registry = Registry::new();
        cy_editor_services::builtin::register(&mut registry).unwrap();

        let mut form = LightingForm::default();
        assert!(form.bake().is_err(), "no description, no bake");
        assert!(LightingForm::status(&editor).is_none());
        form.description = "levels/corner.cylightmap".into();
        let (command, arguments) = form.bake().unwrap();
        editor
            .invoke(&registry, command, &Scope::unrestricted(), &arguments)
            .unwrap();
        gate.wait();

        // The trace has reported: the form shows the operation's own progress and offers cancel.
        let status = LightingForm::status(&editor).expect("a bake is running");
        assert!(status.cancellable());
        assert!(status.fraction.is_some_and(|fraction| fraction > 0.05));
        assert!(status.step.contains("1024 of 65536"), "{status:?}");

        let (command, arguments) = LightingForm::cancel(&status);
        editor
            .invoke(&registry, command, &Scope::unrestricted(), &arguments)
            .unwrap();
        let operation = editor
            .operations
            .all()
            .iter()
            .find(|operation| operation.id() == status.request)
            .cloned()
            .unwrap();
        assert_eq!(
            operation.block_until_settled(Duration::from_secs(10)),
            OperationState::Cancelled
        );
        assert!(!LightingForm::status(&editor).unwrap().cancellable());
        std::fs::remove_dir_all(&root).ok();
    }

    #[test]
    fn the_density_view_is_the_engines_view_mode_command() {
        let mut registry = Registry::new();
        cy_editor_services::builtin::register(&mut registry).unwrap();
        let command = LightingForm::density_view();
        assert_eq!(command, "viewport.view-mode.lightmap-density");
        assert!(
            registry.metadata(&command).is_some(),
            "the density view is a registered view mode"
        );
    }
}
