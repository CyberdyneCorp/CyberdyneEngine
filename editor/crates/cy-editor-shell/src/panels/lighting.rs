// SPDX-License-Identifier: MIT
//! The lighting and lightmap baking editor: the bake form, its progress and cancel, and the
//! texel-density view.
//!
//! A [`SpecialisedTool`] like every other specialised panel: the scaffold draws the header, opens
//! the domain and checks the commands. Drawing only. What the buttons do is
//! `cy_editor_interface::specialised::lighting::LightingForm`'s invocations, through the registry
//! like every other caller; what the progress bar shows is the operation service's own state for
//! the bake's request, the same row the footer and the diagnostics panel show. The bake is the
//! scaffold's one kind of [`SpecialisedTool::OPERATIONS`]: it writes a cooked lightmap, not a
//! document, so there is no transaction for undo to restore.

use cy_editor_commands::Arguments;
use cy_editor_core::progress::OperationState;
use cy_editor_interface::Domain;
use cy_editor_interface::specialised::Session;
use cy_editor_interface::specialised::lighting::{
    BAKE_COMMAND, BakeStatus, CANCEL_COMMAND, LightingForm,
};

use super::specialised::{SpecialisedTool, ToolFrame};
use super::{Intent, Panels, heading, secondary};

/// The lighting and lightmap baking editor.
pub(crate) struct LightingTool;

impl SpecialisedTool for LightingTool {
    const DOMAIN: Domain = Domain::LightingAndLightmapBaking;
    const TITLE: &'static str = "Lighting & Lightmaps";
    // `LightingForm::density_view()` is this mode's command id; a test holds the two together.
    const COMMANDS: &'static [&'static str] =
        &[CANCEL_COMMAND, "viewport.view-mode.lightmap-density"];
    const OPERATIONS: &'static [&'static str] = &[BAKE_COMMAND];

    /// The bake the editor most recently started, if any.
    type Target = Option<BakeStatus>;

    fn target(panels: &mut Panels<'_>, _ui: &mut egui::Ui) -> Option<Self::Target> {
        Some(LightingForm::status(panels.editor))
    }

    fn body(
        frame: &mut ToolFrame<'_>,
        session: Session<'_>,
        status: Self::Target,
        ui: &mut egui::Ui,
    ) {
        let form = session
            .lighting
            .expect("lighting and lightmap baking opens onto its form");
        bake_form(frame, form, status.as_ref(), ui);
        density_view(frame, ui);
    }
}

fn bake_form(
    frame: &mut ToolFrame<'_>,
    form: &mut LightingForm,
    status: Option<&BakeStatus>,
    ui: &mut egui::Ui,
) {
    let shell = frame.shell;
    let intents = &mut frame.intents;
    heading(ui, shell, "Bake lightmaps");
    ui.label(secondary(
        shell,
        "Bakes the level's static lighting with the engine's path tracer, off the interface \
         thread. A cancelled bake writes nothing.",
    ));
    egui::Grid::new("lighting-bake-form")
        .num_columns(2)
        .show(ui, |ui| {
            ui.label("Level description");
            ui.text_edit_singleline(&mut form.description)
                .on_hover_text("The level's project-relative .cylightmap description.");
            ui.end_row();
            ui.label("Output");
            ui.text_edit_singleline(&mut form.output)
                .on_hover_text("Project-relative; .cy/cooked/lightmaps/<level>.lightmap if empty.");
            ui.end_row();
        });
    let running = status.is_some_and(BakeStatus::cancellable);
    ui.horizontal(|ui| {
        let bake = form.bake();
        let clicked = ui
            .add_enabled(
                !running && bake.is_ok(),
                egui::Button::new("Bake lightmaps"),
            )
            .clicked();
        if clicked && let Ok((command, arguments)) = bake {
            intents.push(Intent::Invoke(command.into(), arguments));
        }
        if let Some(status) = status.filter(|status| status.cancellable())
            && ui.button("Cancel bake").clicked()
        {
            let (command, arguments) = LightingForm::cancel(status);
            intents.push(Intent::Invoke(command.into(), arguments));
        }
    });
    if let Some(status) = status {
        let line = match &status.state {
            OperationState::Pending => "Queued".to_string(),
            OperationState::Running { .. } => status.step.clone(),
            OperationState::Completed => "Baked".to_string(),
            OperationState::Cancelled => "Cancelled: nothing was written".to_string(),
            OperationState::Failed(problem) => format!("Failed: {}", problem.because),
        };
        if let Some(fraction) = status.fraction {
            ui.add(egui::ProgressBar::new(fraction).text(line));
        } else {
            ui.label(secondary(shell, line));
        }
    }
}

fn density_view(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui) {
    let shell = frame.shell;
    let intents = &mut frame.intents;
    heading(ui, shell, "Texel density");
    ui.label(secondary(
        shell,
        "Draws every lightmapped surface as a checker of its lightmap texels: green on the level's \
         density, blue below, red above; grey has no lightmap.",
    ));
    if ui.button("Show lightmap density").clicked() {
        intents.push(Intent::Invoke(
            LightingForm::density_view(),
            Arguments::new(),
        ));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_tool_lists_every_command_its_form_invokes() {
        let form = LightingForm {
            description: "levels/corner.cylightmap".into(),
            ..LightingForm::default()
        };
        let (bake, _) = form.bake().expect("a named description bakes");
        assert!(LightingTool::OPERATIONS.contains(&bake));
        assert!(LightingTool::COMMANDS.contains(&LightingForm::density_view().as_str()));
        assert!(LightingTool::COMMANDS.contains(&CANCEL_COMMAND));
    }
}
