//! Searchable settings rows derived from the settings service.

use cy_editor_core::observe::Revision;
use cy_editor_services::settings::Scope;
use cy_editor_services::{Editor, SettingValue};

/// One row in the Settings panel.
#[derive(Clone, PartialEq, Debug)]
pub struct SettingRow {
    /// Stable declared key.
    pub key: String,
    /// Category used for navigation.
    pub category: String,
    /// Searchable human explanation.
    pub summary: String,
    /// Project file or user-preference file.
    pub scope: Scope,
    /// Declared value shape.
    pub shape: &'static str,
    /// Catalogue fallback.
    pub default: SettingValue,
    /// Value effective on the selected platform.
    pub effective: SettingValue,
    /// Explicit value at the active scope, if any.
    pub explicit: Option<SettingValue>,
    /// Whether an explicit value is present.
    pub modified: bool,
    /// Whether platform overrides are permitted.
    pub per_platform: bool,
    /// Whether the selected platform currently has an override.
    pub platform_override: bool,
}

/// Presentation state for category navigation, search, and a selected platform.
#[derive(Debug)]
pub struct SettingsViewModel {
    query: String,
    category: Option<String>,
    platform: String,
    categories: Vec<String>,
    rows: Vec<SettingRow>,
    input: Option<(Revision, String, Option<String>, String)>,
    rebuilds: u64,
}

impl Default for SettingsViewModel {
    fn default() -> Self {
        Self {
            query: String::new(),
            category: None,
            platform: current_platform().to_string(),
            categories: Vec::new(),
            rows: Vec::new(),
            input: None,
            rebuilds: 0,
        }
    }
}

impl SettingsViewModel {
    /// A view over built-in settings on this platform.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Replace the permanent search text.
    pub fn set_query(&mut self, query: impl Into<String>) {
        self.query = query.into();
    }

    /// Select a category, or all categories.
    pub fn select_category(&mut self, category: Option<String>) {
        self.category = category;
    }

    /// Inspect effective values for a platform.
    pub fn set_platform(&mut self, platform: impl Into<String>) {
        self.platform = platform.into();
    }

    /// Current search text.
    #[must_use]
    pub fn query(&self) -> &str {
        &self.query
    }

    /// Selected category.
    #[must_use]
    pub fn selected_category(&self) -> Option<&str> {
        self.category.as_deref()
    }

    /// Platform whose overrides are shown.
    #[must_use]
    pub fn platform(&self) -> &str {
        &self.platform
    }

    /// Categories in stable order.
    #[must_use]
    pub fn categories(&self) -> &[String] {
        &self.categories
    }

    /// Visible settings.
    #[must_use]
    pub fn rows(&self) -> &[SettingRow] {
        &self.rows
    }

    /// Number of derived rebuilds.
    #[must_use]
    pub const fn rebuilds(&self) -> u64 {
        self.rebuilds
    }

    /// Rebuild when service or navigation input changes.
    pub fn refresh(&mut self, editor: &Editor) -> bool {
        let input = (
            editor.settings.revision(),
            self.query.clone(),
            self.category.clone(),
            self.platform.clone(),
        );
        if self.input == Some(input.clone()) {
            return false;
        }

        let catalogue = editor.settings.catalogue();
        self.categories = catalogue
            .categories()
            .into_iter()
            .map(ToOwned::to_owned)
            .collect();
        let declarations = if self.query.trim().is_empty() {
            match &self.category {
                Some(category) => catalogue.in_category(category),
                None => self
                    .categories
                    .iter()
                    .flat_map(|category| catalogue.in_category(category))
                    .collect(),
            }
        } else {
            catalogue.search(&self.query)
        };
        self.rows = declarations
            .into_iter()
            .filter(|declaration| {
                self.category
                    .as_ref()
                    .is_none_or(|category| declaration.category() == category)
            })
            .filter_map(|declaration| {
                let platform_override = declaration.scope == Scope::Project
                    && declaration.per_platform
                    && editor
                        .settings
                        .platform_value(&self.platform, &declaration.key)
                        .is_some();
                let explicit = match declaration.scope {
                    Scope::Project if platform_override => editor
                        .settings
                        .platform_value(&self.platform, &declaration.key),
                    Scope::Project => editor.settings.project_value(&declaration.key),
                    Scope::User => editor.settings.user_value(&declaration.key),
                }
                .cloned();
                Some(SettingRow {
                    key: declaration.key.clone(),
                    category: declaration.category().to_string(),
                    summary: declaration.summary.clone(),
                    scope: declaration.scope,
                    shape: declaration.default.shape(),
                    default: declaration.default.clone(),
                    effective: editor
                        .settings
                        .effective(&declaration.key, &self.platform)?,
                    modified: explicit.is_some(),
                    explicit,
                    per_platform: declaration.per_platform,
                    platform_override,
                })
            })
            .collect();
        self.input = Some(input);
        self.rebuilds += 1;
        true
    }
}

const fn current_platform() -> &'static str {
    if cfg!(target_os = "macos") {
        "macos"
    } else if cfg!(target_os = "windows") {
        "windows"
    } else if cfg!(target_os = "linux") {
        "linux"
    } else {
        "other"
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn categories_search_defaults_overrides_and_preferences_are_derived_on_revision() {
        let mut editor = Editor::default();
        let mut view = SettingsViewModel::new();
        assert!(view.refresh(&editor));
        assert!(!view.refresh(&editor));
        assert!(
            view.categories()
                .iter()
                .any(|category| category == "rendering")
        );

        view.set_query("frame rate");
        assert!(view.refresh(&editor));
        assert_eq!(view.rows().len(), 1);
        assert_eq!(view.rows()[0].key, "rendering.target_frame_rate");
        assert!(!view.rows()[0].modified);

        editor
            .settings
            .set_project("rendering.target_frame_rate", SettingValue::Whole(30))
            .unwrap();
        assert!(view.refresh(&editor));
        assert!(view.rows()[0].modified);
        assert_eq!(view.rows()[0].effective, SettingValue::Whole(30));

        view.set_query("");
        view.select_category(Some("editor".into()));
        assert!(view.refresh(&editor));
        assert!(view.rows().iter().all(|row| row.scope == Scope::User));
    }

    #[test]
    fn a_platform_override_is_visible_only_on_its_platform() {
        let mut editor = Editor::default();
        editor
            .settings
            .set_project("rendering.shadow_resolution", SettingValue::Whole(2048))
            .unwrap();
        editor
            .settings
            .set_platform(
                "ios",
                "rendering.shadow_resolution",
                SettingValue::Whole(512),
            )
            .unwrap();
        let mut view = SettingsViewModel::new();
        view.set_query("shadow");
        view.set_platform("ios");
        view.refresh(&editor);
        assert_eq!(view.rows()[0].effective, SettingValue::Whole(512));
        view.set_platform("macos");
        view.refresh(&editor);
        assert_eq!(view.rows()[0].effective, SettingValue::Whole(2048));
    }
}
