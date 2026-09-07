//! Console, Problems and Profiler — the three diagnostics panels. Task 1.5.
//!
//! --- THE SECOND THING THE REFERENCE IMAGES GET WRONG ------------------------------------------------
//!
//! `editor-visual-language`, on the adventure reference: *"The panel labelled 'World Partition'
//! contains CPU, GPU, memory and VRAM graphs. That is a profiler. World partition is a streaming
//! capability with entirely different concerns."* So the panel that shows frame cost here is called
//! **Profiler**, there is no panel called World Partition in this editor, and the distinction the
//! specification draws is kept: *the ambient overlay answers "is this frame affordable"; the
//! profiler answers "why"*. The overlay is one line in the viewport's corner; the attribution is
//! here.
//!
//! --- WHY THE PROFILER PROFILES THE EDITOR --------------------------------------------------------
//!
//! `editor-ui-ux`: "The editor SHALL be able to profile **itself** ... A slow editor interaction
//! SHALL be attributable to a panel, a service, or an engine call rather than being reported as a
//! whole application stall." `Shell::frame_cost` measures exactly that and names the slowest part in
//! words. A profiler showing the *game's* frame cost is a different panel with a different source,
//! and it needs a runtime that is running one.
//!
//! --- NOTIFICATIONS ARE HISTORY, NOT A LOG ---------------------------------------------------------
//!
//! The console shows `NotificationCentre::history`, which is the same list the toasts are drawn
//! from. One mechanism read twice: a toast that has faded is still readable here, which is what
//! "notifications SHALL NOT be the only record" means in practice.

use cy_editor_commands::Arguments;
use cy_editor_interface::problems::Site;
use cy_editor_services::notifications::Severity;
use cy_editor_visual::colour::Semantic;
use cy_editor_visual::density::TextRole;

use super::{Intent, Panels, heading, nothing_here, numeric, secondary, status};
use crate::theme;

/// The severity's semantic role, so the console, the problems list and the toasts agree.
pub(crate) fn role_of(severity: Severity) -> Semantic {
    match severity {
        Severity::Info => Semantic::Active,
        Severity::Warning => Semantic::Warning,
        Severity::Error => Semantic::Error,
    }
}

/// The console: what the editor has said, and a command line.
pub(super) fn console(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let metrics = panels.metrics();
    let history = panels.shell.notifications.history().to_vec();

    egui::ScrollArea::vertical()
        .auto_shrink([false, false])
        .stick_to_bottom(true)
        .max_height(ui.available_height() - metrics.hit_target() - metrics.gap())
        .show(ui, |ui| {
            if history.is_empty() {
                nothing_here(
                    ui,
                    panels.shell,
                    "The editor has said nothing yet.",
                    "Messages, warnings and failures appear here and stay readable after their \
                     notification has faded.",
                );
            }
            for toast in &history {
                let role = toast.role();
                ui.horizontal_wrapped(|ui| {
                    ui.label(
                        egui::RichText::new(role.glyph().to_string())
                            .color(theme::role(panels.shell.theme, role)),
                    );
                    // `Toast::line` already opens with the severity in words — three encodings,
                    // which is what makes the console readable in a colour-blind-safe palette — so
                    // the panel adds the glyph and not a second copy of the word.
                    ui.label(egui::RichText::new(toast.line()).size(metrics.text(TextRole::Body)));
                });
            }
        });

    ui.separator();
    ui.horizontal(|ui| {
        ui.label(secondary(panels.shell, "Command"));
        let response = ui.add(
            egui::TextEdit::singleline(&mut panels.inputs.console)
                .hint_text(secondary(panels.shell, "a command identifier"))
                .desired_width(f32::INFINITY),
        );
        if response.lost_focus() && ui.input(|input| input.key_pressed(egui::Key::Enter)) {
            let id = panels.inputs.console.trim().to_string();
            if !id.is_empty() {
                // Through the registry, like every other caller. A console that had its own
                // dispatch would be a seventh entry point with a different set of things it can do.
                panels.intents.push(Intent::Invoke(id, Arguments::new()));
                panels.inputs.console.clear();
            }
            response.request_focus();
        }
    });
}

/// Problems: what is wrong, where, and what would fix it.
pub(super) fn problems(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let metrics = panels.metrics();
    ui.label(secondary(panels.shell, panels.shell.problems.summary()));
    ui.add_space(metrics.gap() * 0.5);

    if panels.shell.problems.is_empty() {
        nothing_here(
            ui,
            panels.shell,
            "Nothing is wrong.",
            "Validation failures, import errors and broken references are listed here, and are also \
             marked on the object that has them.",
        );
        return;
    }

    let reports: Vec<_> = panels.shell.problems.all().cloned().collect();
    let mut focus = None;
    egui::ScrollArea::vertical()
        .auto_shrink([false, false])
        .show(ui, |ui| {
            for report in &reports {
                let role = role_of(report.severity);
                ui.horizontal_wrapped(|ui| {
                    ui.label(
                        egui::RichText::new(role.glyph().to_string())
                            .color(theme::role(panels.shell.theme, role)),
                    );
                    ui.label(egui::RichText::new(&report.problem.what));
                    ui.label(secondary(panels.shell, format!("· {}", report.source)));
                });
                ui.indent(("problem", &report.problem.what), |ui| {
                    ui.label(secondary(panels.shell, report.problem.because.clone()));
                    if let Some(remedy) = &report.problem.remedy {
                        // "Each problem SHALL state what would fix it." Never a bare failure.
                        ui.label(
                            egui::RichText::new(remedy)
                                .size(metrics.text(TextRole::Body))
                                .color(theme::role(panels.shell.theme, Semantic::Active)),
                        );
                    }
                    if let Some(node) = report.site.node()
                        && ui
                            .button(secondary(panels.shell, "Select the entity"))
                            .clicked()
                    {
                        focus = Some(node);
                    }
                    if let Site::Asset(path) = &report.site {
                        ui.label(secondary(panels.shell, path.clone()));
                    }
                });
            }
        });
    if let Some(node) = focus {
        panels.hierarchy.select(panels.editor, node);
    }
}

/// The profiler: what the last interface frame cost, and which part of it did.
pub(super) fn profiler(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let cost = panels.shell.frame_cost();
    heading(ui, panels.shell, "Interface frame");
    egui::Grid::new("profiler-frame")
        .num_columns(2)
        .spacing([panels.metrics().gap() * 2.0, panels.metrics().gap() * 0.5])
        .show(ui, |ui| {
            for (name, duration) in [
                ("Notifications", cost.notifications),
                ("Inspector", cost.inspector),
                ("Progress", cost.progress),
                ("Total", cost.total()),
            ] {
                ui.label(secondary(panels.shell, name));
                // Tabular, so the column of numbers lines up. See `crate::theme::NUMERIC`.
                ui.label(numeric(
                    panels.shell,
                    format!("{:>8.3} ms", duration.as_secs_f64() * 1_000.0),
                ));
                ui.end_row();
            }
        });
    ui.label(secondary(
        panels.shell,
        format!(
            "Slowest: {} · inspector rebuilt this frame: {}",
            cost.slowest(),
            if cost.rebuilt { "yes" } else { "no" }
        ),
    ));

    ui.add_space(panels.metrics().gap());
    heading(ui, panels.shell, "Background work");
    let progress = &panels.shell.progress;
    if progress.rows().is_empty() {
        ui.label(secondary(panels.shell, "Nothing is running."));
    } else {
        for row in progress.rows() {
            ui.label(egui::RichText::new(row.line()));
        }
    }

    ui.add_space(panels.metrics().gap());
    heading(ui, panels.shell, "Runtime");
    status(
        ui,
        panels.shell,
        if panels.editor.runtime.is_connected() {
            Semantic::Live
        } else {
            Semantic::SecondaryText
        },
        panels.editor.hosting_mode().name(),
    );
}
