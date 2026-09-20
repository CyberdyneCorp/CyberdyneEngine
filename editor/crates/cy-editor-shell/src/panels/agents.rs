//! Desktop agent status, controls, confirmations, and privacy-labelled audit records.

use cy_editor_visual::colour::Semantic;

use super::{Intent, Panels, heading, nothing_here, secondary, status};

pub(super) fn show(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let Some(agent) = panels.agent.as_deref_mut() else {
        nothing_here(
            ui,
            panels.shell,
            "No agent is connected.",
            "Start the editor with --mcp to host MCP alongside this window.",
        );
        return;
    };
    let snapshot = agent.status();
    summary(ui, panels.shell, &snapshot);
    controls(ui, panels.intents, &snapshot);
    confirmation(
        ui,
        panels.shell,
        panels.intents,
        agent.pending_confirmation(),
    );
    activity(ui, panels.shell, agent.audit_records());
}

fn summary(
    ui: &mut egui::Ui,
    shell: &cy_editor_interface::Shell,
    snapshot: &cy_editor_agent::AgentSessionStatus,
) {
    let (role, label) = state(snapshot.connected, snapshot.paused, snapshot.revoked);
    status(ui, shell, role, label);
    ui.label(format!(
        "{} · session {}",
        snapshot.identity.agent, snapshot.identity.session
    ));
    ui.label(secondary(shell, format!("Intent: {}", snapshot.intent)));
    ui.label(secondary(shell, format!("Scope: {}", snapshot.scope)));
    ui.label(secondary(
        shell,
        format!("Budget: {}", snapshot.budget.describe()),
    ));
    ui.label(secondary(
        shell,
        format!(
            "Current operation: {} · {} request(s) outstanding",
            snapshot.current_operation.as_deref().unwrap_or("Idle"),
            snapshot.outstanding
        ),
    ));
}

fn controls(
    ui: &mut egui::Ui,
    intents: &mut Vec<Intent>,
    snapshot: &cy_editor_agent::AgentSessionStatus,
) {
    ui.horizontal(|ui| {
        if snapshot.paused {
            if ui.button("Resume agent").clicked() {
                intents.push(Intent::ResumeAgent);
            }
        } else if ui
            .add_enabled(
                snapshot.connected && !snapshot.revoked,
                egui::Button::new("Pause agent"),
            )
            .clicked()
        {
            intents.push(Intent::PauseAgent);
        }
        if ui
            .add_enabled(
                snapshot.connected && !snapshot.revoked,
                egui::Button::new("Revoke access"),
            )
            .clicked()
        {
            intents.push(Intent::RevokeAgent);
        }
    });
}

fn confirmation(
    ui: &mut egui::Ui,
    shell: &cy_editor_interface::Shell,
    intents: &mut Vec<Intent>,
    pending: Option<cy_editor_agent::PendingAgentConfirmation>,
) {
    let Some(pending) = pending else { return };
    ui.separator();
    heading(ui, shell, "Confirmation required");
    ui.label(format!(
        "{} requests {} ({})",
        pending.confirmation.agent,
        pending.confirmation.command,
        pending.confirmation.effect.name()
    ));
    ui.label(pending.confirmation.what_happens);
    ui.label(format!(
        "What will be lost: {}",
        pending.confirmation.what_is_lost
    ));
    ui.horizontal_wrapped(|ui| {
        for (label, allow, grant_millis) in [
            ("Allow once", true, None),
            ("Grant for 5 minutes", true, Some(5 * 60 * 1000)),
            ("Refuse request", false, None),
        ] {
            if ui.button(label).clicked() {
                intents.push(Intent::DecideAgent {
                    ticket: pending.ticket,
                    allow,
                    grant_millis,
                });
            }
        }
    });
}

fn activity(
    ui: &mut egui::Ui,
    shell: &cy_editor_interface::Shell,
    records: &[cy_editor_agent::AgentAuditRecord],
) {
    ui.separator();
    heading(ui, shell, "Activity and privacy");
    egui::ScrollArea::vertical().show(ui, |ui| {
        for record in records.iter().rev().take(100) {
            ui.label(format!(
                "#{} {} · {} · {}",
                record.sequence,
                record.kind.name(),
                record.privacy.name(),
                record.summary
            ));
        }
    });
}

const fn state(connected: bool, paused: bool, revoked: bool) -> (Semantic, &'static str) {
    if revoked {
        (Semantic::Error, "Revoked")
    } else if !connected {
        (Semantic::SecondaryText, "Disconnected")
    } else if paused {
        (Semantic::Warning, "Paused")
    } else {
        (Semantic::Live, "Connected")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_agent_state_has_a_distinct_non_colour_label() {
        assert_eq!(state(true, false, false).1, "Connected");
        assert_eq!(state(true, true, false).1, "Paused");
        assert_eq!(state(true, false, true).1, "Revoked");
        assert_eq!(state(false, false, false).1, "Disconnected");
    }
}
