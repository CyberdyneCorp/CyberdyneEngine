// SPDX-License-Identifier: MIT
//! The lighting and lightmap baking editor: the bake form, its progress and cancel, and the
//! texel-density view.
//!
//! Drawing only. What the buttons do is `cy_editor_interface::specialised::lighting::LightingForm`'s
//! invocations, through the registry like every other caller; what the progress bar shows is the
//! operation service's own state for the bake's request, the same row the footer and the
//! diagnostics panel show.

use cy_editor_commands::Arguments;
use cy_editor_core::progress::OperationState;
use cy_editor_interface::Domain;
use cy_editor_interface::specialised::lighting::{BakeStatus, LightingForm};

use super::{Intent, Panels, heading, nothing_here, secondary};

pub(super) fn show(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let status = LightingForm::status(panels.editor);
    let shell = &*panels.shell;
    let session = match panels.specialised.open(Domain::LightingAndLightmapBaking) {
        Ok(session) => session,
        Err(problem) => {
            nothing_here(
                ui,
                shell,
                "The lighting editor could not be opened.",
                &problem.to_string(),
            );
            return;
        }
    };
    let form = session
        .lighting
        .expect("lighting and lightmap baking opens onto its form");
    let mut intents = Vec::new();

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
    let running = status.as_ref().is_some_and(BakeStatus::cancellable);
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
        if let Some(status) = status.as_ref().filter(|status| status.cancellable())
            && ui.button("Cancel bake").clicked()
        {
            let (command, arguments) = LightingForm::cancel(status);
            intents.push(Intent::Invoke(command.into(), arguments));
        }
    });
    if let Some(status) = &status {
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
    panels.intents.extend(intents);
}
