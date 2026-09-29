// SPDX-License-Identifier: MIT
//! The frame every specialised editor panel is drawn in, and the check that keeps it an MCP peer.
//!
//! `cy_editor_interface::specialised` owns the sixteen domains and the shared surfaces a domain
//! edits on. This module is the matching piece on the window's side: a new authoring tool is one
//! type that implements [`SpecialisedTool`], and [`show`] gives it the rest —
//!
//! * the standard header: the tool's title and Undo/Redo over the active document's transaction
//!   history, so every tool undoes the same way the Undo History panel does;
//! * the domain opened through [`cy_editor_interface::SpecialisedEditors::open`], which is what
//!   hands the tool its shared surface and what refuses, by name, a domain this build cannot open;
//! * a diagnostics area between the header and the body, for what the tool refused or the engine
//!   reported;
//! * [`register_tool`], which proves that every command the tool's panel invokes is registered,
//!   is projected as an MCP tool with no exclusion, and is undoable — so a button never has a
//!   mutation an agent lacks, and never has one that bypasses the transaction history.
//!
//! A tool mutates nothing itself. Its body pushes [`Intent::Invoke`] for registered commands, which
//! the window applies after the frame; the command runs inside a document transaction.

use cy_editor_commands::metadata::EffectClass;
use cy_editor_commands::{Arguments, Registry};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_interface::Domain;
use cy_editor_interface::shell::Shell;
use cy_editor_interface::specialised::Session;
use cy_editor_visual::colour::Semantic;

use super::{Inputs, Intent, Panels, nothing_here, status};

/// One line in a tool's diagnostics area.
#[derive(Clone, PartialEq, Debug)]
pub(crate) struct ToolDiagnostic {
    /// How serious it is. Drawn with its glyph and word as well as its colour.
    pub role: Semantic,
    /// What happened, in the words of whoever refused.
    pub message: String,
}

impl ToolDiagnostic {
    /// A refusal the author has to act on.
    pub(crate) fn error(message: impl Into<String>) -> Self {
        Self {
            role: Semantic::Error,
            message: message.into(),
        }
    }
}

/// What a tool's body is handed for one frame, beside the open [`Session`].
pub(crate) struct ToolFrame<'a> {
    /// Theme, density and the rest of the interface's models.
    pub shell: &'a Shell,
    /// The panels' persistent field and gesture state.
    pub inputs: &'a mut Inputs,
    /// Commands to invoke after the frame. The only way a tool changes a document.
    pub intents: Vec<Intent>,
}

/// One specialised authoring tool.
///
/// Implement this, add the kind to the dispatch in `panels/mod.rs`, list the type in
/// [`register_specialised_tools`], and the tool has the header, the session, the diagnostics area
/// and the MCP parity check. See `editor/README.md`, "Adding a specialised editor".
pub(crate) trait SpecialisedTool {
    /// The domain this tool edits. Its panel kind and its shared surfaces follow from it.
    const DOMAIN: Domain;
    /// The header's title.
    const TITLE: &'static str;
    /// Every registered command the panel invokes. [`register_tool`] checks each one.
    const COMMANDS: &'static [&'static str];

    /// What the tool edits this frame, resolved before the domain is opened.
    type Target;

    /// Register the tool's commands. The default registers nothing, for a tool whose commands
    /// the built-in registration already carries.
    fn register(registry: &mut Registry) -> Result<()> {
        let _ = registry;
        Ok(())
    }

    /// Resolve the target, or draw the tool's empty state and answer `None`.
    ///
    /// Runs before the domain is opened, so an empty state leaves the reserved region as it was.
    fn target(panels: &mut Panels<'_>, ui: &mut egui::Ui) -> Option<Self::Target>;

    /// Diagnostics the tool is holding from earlier frames.
    fn diagnostics(inputs: &Inputs) -> Vec<ToolDiagnostic> {
        let _ = inputs;
        Vec::new()
    }

    /// Draw the body on the open session.
    fn body(
        frame: &mut ToolFrame<'_>,
        session: Session<'_>,
        target: Self::Target,
        ui: &mut egui::Ui,
    );
}

/// Draw one specialised tool: header, target, session, diagnostics, body.
pub(super) fn show<T: SpecialisedTool>(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    header(panels, ui, T::TITLE);
    let Some(target) = T::target(panels, ui) else {
        return;
    };
    let shell = &*panels.shell;
    let inputs = &mut *panels.inputs;
    diagnostics_area(ui, shell, &T::diagnostics(inputs));
    let session = match panels.specialised.open(T::DOMAIN) {
        Ok(session) => session,
        Err(problem) => {
            nothing_here(
                ui,
                shell,
                &format!("The {} editor could not be opened.", T::DOMAIN.spec_term()),
                &problem.to_string(),
            );
            return;
        }
    };
    let mut frame = ToolFrame {
        shell,
        inputs,
        intents: Vec::new(),
    };
    T::body(&mut frame, session, target, ui);
    panels.intents.extend(frame.intents);
}

/// The title, and Undo/Redo over the active document's history.
///
/// Shared with the authoring panels that are not one of the sixteen domains (the physics panel),
/// so every authoring surface undoes the same way.
pub(super) fn header(panels: &mut Panels<'_>, ui: &mut egui::Ui, title: &str) {
    ui.horizontal(|ui| {
        ui.heading(title);
        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
            if ui
                .add_enabled(panels.history.can_redo(), egui::Button::new("Redo"))
                .on_disabled_hover_text("Nothing in this document can be redone")
                .clicked()
            {
                panels
                    .intents
                    .push(Intent::Invoke("edit.redo".into(), Arguments::new()));
            }
            if ui
                .add_enabled(panels.history.can_undo(), egui::Button::new("Undo"))
                .on_disabled_hover_text("Nothing in this document can be undone")
                .clicked()
            {
                panels
                    .intents
                    .push(Intent::Invoke("edit.undo".into(), Arguments::new()));
            }
        });
    });
}

/// Every diagnostic, glyph and word beside colour. Draws nothing when there are none.
pub(super) fn diagnostics_area(ui: &mut egui::Ui, shell: &Shell, diagnostics: &[ToolDiagnostic]) {
    for diagnostic in diagnostics {
        status(ui, shell, diagnostic.role, &diagnostic.message);
    }
}

/// Register one tool's commands and prove its MCP and undo parity.
///
/// Refuses, naming the command, when a command the panel invokes is not registered, is excluded
/// from the agent projection, or is not an undoable mutation or a read.
pub(crate) fn register_tool<T: SpecialisedTool>(registry: &mut Registry) -> Result<()> {
    T::register(registry)?;
    parity(registry, T::DOMAIN, T::COMMANDS)
}

/// Every scaffolded tool, registered and checked. Called wherever the command registry is built.
pub fn register_specialised_tools(registry: &mut Registry) -> Result<()> {
    register_tool::<super::terrain::TerrainTool>(registry)?;
    command_parity(registry, super::physics::PANEL, super::physics::COMMANDS)
}

fn parity(registry: &Registry, domain: Domain, commands: &[&str]) -> Result<()> {
    command_parity(
        registry,
        &format!("the {} editor", domain.spec_term()),
        commands,
    )
}

/// The same MCP and undo parity check, for an authoring panel that is not one of the sixteen
/// specialised domains. Refuses exactly what [`register_tool`] refuses.
pub(crate) fn command_parity(registry: &Registry, panel: &str, commands: &[&str]) -> Result<()> {
    let action = format!("register {panel}");
    for command in commands {
        let Some(tool) = cy_editor_agent::tool::project_one(registry, command) else {
            return Err(Problem::new(
                action,
                format!("its panel invokes `{command}`, which no registration declares"),
            )
            .with_remedy("register the command before the tool, or drop it from COMMANDS"));
        };
        if let Some(reason) = tool.exclusion {
            return Err(Problem::new(
                action,
                format!("`{command}` is excluded from the agent interface: {reason}"),
            )
            .with_remedy("every authoring action needs an MCP tool that does the same thing"));
        }
        if !matches!(
            tool.effect,
            EffectClass::Read | EffectClass::ReversibleMutation
        ) {
            return Err(Problem::new(
                action,
                format!(
                    "`{command}` is {}, so the panel could change a document outside undo",
                    tool.effect.name()
                ),
            )
            .with_remedy("author through a reversible command recorded as one transaction"));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use cy_editor_commands::metadata::{Metadata, ParameterSpec};
    use cy_editor_commands::registry::Command;

    use super::*;

    fn builtin() -> Registry {
        let mut registry = Registry::new();
        cy_editor_services::builtin::register(&mut registry).unwrap();
        registry
    }

    #[test]
    fn every_scaffolded_tool_is_an_undoable_mcp_peer_of_its_panel() {
        let mut registry = builtin();
        register_specialised_tools(&mut registry).expect("parity holds for the shipped tools");
        for command in <super::super::terrain::TerrainTool as SpecialisedTool>::COMMANDS {
            let tool = cy_editor_agent::tool::project_one(&registry, command).unwrap();
            assert!(tool.exclusion.is_none(), "{command}");
            assert_eq!(tool.effect, EffectClass::ReversibleMutation, "{command}");
        }
    }

    #[test]
    fn a_panel_command_nobody_registered_is_refused_by_name() {
        let problem = parity(&Registry::new(), Domain::Terrain, &["terrain.create"])
            .expect_err("an empty registry has no terrain commands");
        assert!(
            problem.to_string().contains("`terrain.create`"),
            "{problem}"
        );
    }

    fn single(effect: EffectClass, exclusion: Option<&str>) -> Registry {
        let mut metadata = Metadata::new(
            "probe.author",
            "Author Probe",
            "Probe",
            "Authors one probe value in the open document as one undoable transaction.",
            effect,
        )
        .with(ParameterSpec::required(
            "value",
            cy_editor_core::value::ValueKind::Text,
            "The probe value to author into the open document.",
        ));
        if let Some(reason) = exclusion {
            metadata = metadata.not_for_agents(reason);
        }
        let mut registry = Registry::new();
        registry
            .register(Command::new(metadata, |_, _| {
                Ok(cy_editor_commands::Outcome::new("Authored the probe"))
            }))
            .unwrap();
        registry
    }

    #[test]
    fn an_excluded_or_irreversible_panel_command_is_refused() {
        let excluded = single(
            EffectClass::ReversibleMutation,
            Some("it opens a native chooser only a person can answer"),
        );
        let problem = parity(&excluded, Domain::Terrain, &["probe.author"]).unwrap_err();
        assert!(problem.to_string().contains("excluded"), "{problem}");

        let irreversible = single(EffectClass::IrreversibleMutation, None);
        let problem = parity(&irreversible, Domain::Terrain, &["probe.author"]).unwrap_err();
        assert!(problem.to_string().contains("outside undo"), "{problem}");

        let reversible = single(EffectClass::ReversibleMutation, None);
        parity(&reversible, Domain::Terrain, &["probe.author"]).expect("an undoable MCP peer");
    }
}
