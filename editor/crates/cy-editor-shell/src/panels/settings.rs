//! Searchable project settings and user preferences.

use cy_editor_commands::Arguments;
use cy_editor_core::value::Value;
use cy_editor_services::settings::Scope;
use cy_editor_viewmodels::SettingRow;

use super::{Intent, Panels, heading, secondary};

pub(super) fn show(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    if super::search_field(
        ui,
        panels.shell,
        "Search settings",
        &mut panels.inputs.settings_filter,
    ) {
        panels
            .settings
            .set_query(panels.inputs.settings_filter.clone());
    }
    ui.horizontal_wrapped(|ui| {
        if ui
            .selectable_label(panels.settings.selected_category().is_none(), "All")
            .clicked()
        {
            panels.settings.select_category(None);
        }
        for category in panels.settings.categories().to_vec() {
            let selected = panels.settings.selected_category() == Some(category.as_str());
            if ui.selectable_label(selected, &category).clicked() {
                panels.settings.select_category(Some(category));
            }
        }
    });
    ui.horizontal(|ui| {
        ui.label(secondary(panels.shell, "Platform"));
        if ui
            .text_edit_singleline(&mut panels.inputs.settings_platform)
            .changed()
        {
            panels
                .settings
                .set_platform(panels.inputs.settings_platform.clone());
        }
    });

    let rows = panels.settings.rows().to_vec();
    egui::ScrollArea::vertical()
        .id_salt("settings")
        .auto_shrink([false, false])
        .show(ui, |ui| {
            for row in &rows {
                setting(ui, panels, row);
                ui.separator();
            }
        });
}

fn setting(ui: &mut egui::Ui, panels: &mut Panels<'_>, row: &SettingRow) {
    ui.horizontal_wrapped(|ui| {
        heading(ui, panels.shell, &row.key);
        if row.modified {
            ui.label(secondary(panels.shell, "Modified"));
        }
        ui.label(secondary(
            panels.shell,
            match row.scope {
                Scope::Project => "Project",
                Scope::User => "User preference",
            },
        ));
    });
    ui.label(secondary(panels.shell, &row.summary));
    ui.label(secondary(
        panels.shell,
        format!("Default: {}", row.default.display()),
    ));

    let use_platform = row.per_platform
        && (row.platform_override || panels.inputs.settings_platform_overrides.contains(&row.key));
    if row.per_platform {
        let mut selected = use_platform;
        if ui
            .checkbox(
                &mut selected,
                format!("Override for {}", panels.settings.platform()),
            )
            .changed()
        {
            if selected {
                panels
                    .inputs
                    .settings_platform_overrides
                    .insert(row.key.clone());
            } else {
                panels.inputs.settings_platform_overrides.remove(&row.key);
            }
        }
    }

    if let cy_editor_services::SettingValue::Flag(mut value) = row.effective {
        if ui.checkbox(&mut value, "Enabled").changed() {
            panels.intents.push(Intent::Invoke(
                "settings.set-flag".into(),
                setting_arguments(
                    row,
                    panels.settings.platform(),
                    use_platform,
                    Value::Bool(value),
                ),
            ));
        }
    } else {
        let entry = panels
            .inputs
            .settings_entries
            .entry(row.key.clone())
            .or_insert_with(|| row.effective.entry_text());
        let response = ui.text_edit_singleline(entry);
        let submit = ui.button("Apply").clicked()
            || (response.lost_focus() && ui.input(|input| input.key_pressed(egui::Key::Enter)));
        if submit {
            match setting_intent(row, panels.settings.platform(), use_platform, entry) {
                Ok(intent) => {
                    panels.inputs.settings_errors.remove(&row.key);
                    panels.intents.push(intent);
                }
                Err(error) => {
                    panels.inputs.settings_errors.insert(row.key.clone(), error);
                }
            }
        }
    }

    if let Some(error) = panels.inputs.settings_errors.get(&row.key) {
        super::status(
            ui,
            panels.shell,
            cy_editor_visual::colour::Semantic::Error,
            error,
        );
    }
    if row.modified && ui.button("Reset").clicked() {
        let mut arguments = Arguments::new().with("key", Value::Text(row.key.clone()));
        if use_platform {
            arguments = arguments.with(
                "platform",
                Value::Text(panels.settings.platform().to_string()),
            );
        }
        panels
            .intents
            .push(Intent::Invoke("settings.reset".into(), arguments));
        panels.inputs.settings_entries.remove(&row.key);
    }
}

fn setting_intent(
    row: &SettingRow,
    platform: &str,
    use_platform: bool,
    entry: &str,
) -> Result<Intent, String> {
    let (command, value) = match row.shape {
        "whole number" => (
            "settings.set-whole",
            Value::Int(
                entry
                    .trim()
                    .parse()
                    .map_err(|_| "Enter a whole number.".to_string())?,
            ),
        ),
        "real number" => (
            "settings.set-real",
            Value::Double(
                entry
                    .trim()
                    .parse()
                    .map_err(|_| "Enter a real number.".to_string())?,
            ),
        ),
        "text" => ("settings.set-text", Value::Text(entry.to_string())),
        "list" => ("settings.set-list", Value::Text(entry.to_string())),
        _ => return Err(format!("{} is not editable as text.", row.shape)),
    };
    Ok(Intent::Invoke(
        command.into(),
        setting_arguments(row, platform, use_platform, value),
    ))
}

fn setting_arguments(
    row: &SettingRow,
    platform: &str,
    use_platform: bool,
    value: Value,
) -> Arguments {
    let scope = match row.scope {
        Scope::Project => "project",
        Scope::User => "user",
    };
    let mut arguments = Arguments::new()
        .with("key", Value::Text(row.key.clone()))
        .with("value", value)
        .with("scope", Value::Text(scope.into()));
    if use_platform {
        arguments = arguments.with("platform", Value::Text(platform.to_string()));
    }
    arguments
}

#[cfg(test)]
mod tests {
    use super::*;
    use cy_editor_services::SettingValue;

    #[test]
    fn invalid_numeric_input_is_reported_before_it_becomes_an_intent() {
        let row = SettingRow {
            key: "rendering.target_frame_rate".into(),
            category: "rendering".into(),
            summary: String::new(),
            scope: Scope::Project,
            shape: "whole number",
            default: SettingValue::Whole(60),
            effective: SettingValue::Whole(60),
            explicit: None,
            modified: false,
            per_platform: true,
            platform_override: false,
        };
        assert!(setting_intent(&row, "macos", false, "sixty").is_err());
        assert!(setting_intent(&row, "macos", false, "30").is_ok());
    }
}
