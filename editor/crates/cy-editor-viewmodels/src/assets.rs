//! Content Browser navigation and combined name/type filtering.

use std::collections::BTreeSet;

use cy_editor_commands::{Arguments, AssetHost};
use cy_editor_core::observe::Watch;
use cy_editor_core::value::Value;
use cy_editor_services::AssetCatalogueService;
use cy_editor_services::assets::AssetImportService;

/// A command invocation raised by an asset drag/drop gesture.
#[derive(Clone, PartialEq, Debug)]
pub struct AssetDropIntent {
    /// Registered command identifier.
    pub command: &'static str,
    /// Typed command arguments.
    pub arguments: Arguments,
}

/// One importer-declared setting row.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct ImportSettingRow {
    /// Stable importer-owned setting name.
    pub name: String,
    /// Value kind accepted by the importer.
    pub kind: String,
    /// Human-facing explanation supplied by the importer.
    pub description: String,
}

/// Import settings presentation for one selected asset.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct ImportSettingsViewModel {
    asset: String,
    importer: String,
    rows: Vec<ImportSettingRow>,
}

impl ImportSettingsViewModel {
    /// Derive the panel from importer-owned schemas, refusing unsupported formats by name.
    pub fn refresh(&mut self, imports: &mut AssetImportService, asset: &str) -> bool {
        let extension = std::path::Path::new(asset)
            .extension()
            .and_then(std::ffi::OsStr::to_str)
            .map(|extension| format!(".{}", extension.to_ascii_lowercase()))
            .unwrap_or_default();
        let format = imports
            .import_formats()
            .into_iter()
            .find(|format| format.extensions.contains(&extension));
        let next = format.map_or_else(
            || Self {
                asset: asset.to_string(),
                ..Self::default()
            },
            |format| Self {
                asset: asset.to_string(),
                importer: format.importer,
                rows: format
                    .settings
                    .into_iter()
                    .map(|setting| ImportSettingRow {
                        name: setting.name,
                        kind: setting.kind,
                        description: setting.description,
                    })
                    .collect(),
            },
        );
        if *self == next {
            return false;
        }
        *self = next;
        true
    }

    #[must_use]
    /// Whether an importer advertises settings for the selected format.
    pub fn supported(&self) -> bool {
        !self.importer.is_empty()
    }

    #[must_use]
    /// Name of the importer owning these settings.
    pub fn importer(&self) -> &str {
        &self.importer
    }

    #[must_use]
    /// Importer-declared settings in stable order.
    pub fn rows(&self) -> &[ImportSettingRow] {
        &self.rows
    }

    /// Produce the registered command intent; the view never edits a sidecar directly.
    #[must_use]
    pub fn set(&self, setting: &str, value: &str) -> AssetDropIntent {
        AssetDropIntent {
            command: "asset.import-setting.set",
            arguments: Arguments::new()
                .with("path", Value::Text(self.asset.clone()))
                .with("setting", Value::Text(setting.to_string()))
                .with("value", Value::Text(value.to_string())),
        }
    }
}

/// A folder or asset row in deterministic display order.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct AssetRow {
    /// Project-relative identity. Folders have no trailing slash.
    pub path: String,
    /// Last path component.
    pub name: String,
    /// Asset kind, or `folder`.
    pub kind: String,
    /// Whether activating this row navigates rather than opens an asset.
    pub folder: bool,
}

/// Presentation state for the project asset catalogue.
#[derive(Debug, Default)]
pub struct AssetBrowserViewModel {
    folder: String,
    name_filter: String,
    kind_filter: Option<String>,
    rows: Vec<AssetRow>,
    watch: Watch,
    rebuilds: u64,
}

impl AssetBrowserViewModel {
    /// An asset browser at the project root.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Navigate to a project-relative folder without accepting traversal components.
    pub fn navigate(&mut self, folder: &str) -> bool {
        let folder = folder.trim_matches('/');
        if folder.split('/').any(|part| part == ".." || part == ".") {
            return false;
        }
        self.folder = folder.to_string();
        self.watch = Watch::new();
        true
    }

    /// Navigate to the parent folder.
    pub fn up(&mut self) {
        self.folder = self
            .folder
            .rsplit_once('/')
            .map_or_else(String::new, |(parent, _)| parent.to_string());
        self.watch = Watch::new();
    }

    /// Set the case-insensitive name filter.
    pub fn set_name_filter(&mut self, filter: impl Into<String>) {
        self.name_filter = filter.into();
        self.watch = Watch::new();
    }

    /// Limit rows to one kind; `None` shows every type.
    pub fn set_kind_filter(&mut self, kind: Option<String>) {
        self.kind_filter = kind.filter(|kind| !kind.is_empty());
        self.watch = Watch::new();
    }

    /// Rebuild only when discovery or presentation inputs changed.
    pub fn refresh(&mut self, catalogue: &AssetCatalogueService) -> bool {
        if !self.watch.changed(catalogue.revision()) {
            return false;
        }
        self.rows = self.build(catalogue);
        self.watch.accept(catalogue.revision());
        self.rebuilds += 1;
        true
    }

    fn build(&self, catalogue: &AssetCatalogueService) -> Vec<AssetRow> {
        let prefix = if self.folder.is_empty() {
            String::new()
        } else {
            format!("{}/", self.folder)
        };
        let filter = self.name_filter.to_lowercase();
        let mut folders = BTreeSet::new();
        let mut assets = Vec::new();
        for entry in catalogue.entries() {
            let Some(rest) = entry.path.strip_prefix(&prefix) else {
                continue;
            };
            if let Some((child, _)) = rest.split_once('/') {
                folders.insert(child.to_string());
                continue;
            }
            if !filter.is_empty() && !rest.to_lowercase().contains(&filter) {
                continue;
            }
            if self
                .kind_filter
                .as_ref()
                .is_some_and(|kind| kind != &entry.kind)
            {
                continue;
            }
            assets.push(AssetRow {
                path: entry.path.clone(),
                name: rest.to_string(),
                kind: entry.kind.clone(),
                folder: false,
            });
        }
        let mut rows: Vec<_> = folders
            .into_iter()
            .filter(|name| filter.is_empty() || name.to_lowercase().contains(&filter))
            .map(|name| AssetRow {
                path: format!("{prefix}{name}"),
                name,
                kind: "folder".into(),
                folder: true,
            })
            .collect();
        rows.extend(assets);
        rows
    }

    /// Current project-relative folder.
    #[must_use]
    pub fn folder(&self) -> &str {
        &self.folder
    }

    /// Visible deterministic rows.
    #[must_use]
    pub fn rows(&self) -> &[AssetRow] {
        &self.rows
    }

    /// Number of derived-list rebuilds.
    #[must_use]
    pub const fn rebuilds(&self) -> u64 {
        self.rebuilds
    }

    /// Drag an asset into the scene through the registered placement command.
    #[must_use]
    pub fn drop_in_scene(&self, path: &str, at: [f32; 3]) -> AssetDropIntent {
        AssetDropIntent {
            command: "asset.place",
            arguments: Arguments::new()
                .with("path", Value::Text(path.to_string()))
                .with("at", Value::Vec3(at)),
        }
    }

    /// Drag an asset onto an inspector field through the registered assignment command.
    #[must_use]
    pub fn drop_on_entity(&self, path: &str, entity: &str) -> AssetDropIntent {
        AssetDropIntent {
            command: "asset.assign",
            arguments: Arguments::new()
                .with("path", Value::Text(path.to_string()))
                .with("entity", Value::Text(entity.to_string())),
        }
    }
}

#[cfg(test)]
mod tests {
    use std::path::{Path, PathBuf};
    use std::sync::Arc;

    use cy_editor_commands::{AssetImportOutcome, AssetImportRequest, ImportFormat, ImportSetting};
    use cy_editor_services::assets::ImportRunner;

    use super::*;

    #[test]
    fn navigation_and_combined_filters_are_deterministic_and_revision_driven() {
        let root = std::env::temp_dir().join(format!("cy-asset-vm-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(root.join("art/props")).unwrap();
        std::fs::write(root.join("art/props/crate.obj"), []).unwrap();
        std::fs::write(root.join("art/props/crate.png"), []).unwrap();
        std::fs::write(root.join("art/sky.png"), []).unwrap();
        let mut catalogue = AssetCatalogueService::new(PathBuf::from(&root));
        catalogue.refresh().unwrap();
        let mut browser = AssetBrowserViewModel::new();
        assert!(browser.refresh(&catalogue));
        assert_eq!(browser.rows()[0].path, "art");
        assert!(!browser.refresh(&catalogue), "an idle refresh does no work");
        assert!(browser.navigate("art/props"));
        browser.set_name_filter("crate");
        browser.set_kind_filter(Some("mesh".into()));
        browser.refresh(&catalogue);
        assert_eq!(browser.rows().len(), 1);
        assert_eq!(browser.rows()[0].path, "art/props/crate.obj");
        assert!(!browser.navigate("../outside"));
        std::fs::remove_dir_all(root).unwrap();
    }

    struct SettingsRunner;

    impl ImportRunner for SettingsRunner {
        fn describe(&self) -> String {
            "settings test".into()
        }

        fn extensions(&self) -> Vec<String> {
            vec![".obj".into()]
        }

        fn formats(&self) -> Vec<ImportFormat> {
            vec![ImportFormat {
                importer: "obj".into(),
                extensions: vec![".obj".into()],
                settings: vec![ImportSetting {
                    name: "scale".into(),
                    kind: "float".into(),
                    description: "Scale source positions.".into(),
                }],
            }]
        }

        fn run(
            &self,
            _root: &Path,
            _request: &AssetImportRequest,
        ) -> cy_editor_core::problem::Result<AssetImportOutcome> {
            unreachable!("presentation does not run the importer")
        }
    }

    #[test]
    fn drops_and_import_settings_are_command_intents_not_direct_mutations() {
        let browser = AssetBrowserViewModel::new();
        let scene = browser.drop_in_scene("models/chair.obj", [1.0, 2.0, 3.0]);
        assert_eq!(scene.command, "asset.place");
        assert_eq!(
            scene.arguments.get("path").and_then(Value::as_text),
            Some("models/chair.obj")
        );
        let inspector = browser.drop_on_entity("models/chair.obj", "entity-id");
        assert_eq!(inspector.command, "asset.assign");

        let root = std::env::temp_dir();
        let mut imports = AssetImportService::new(root).with_runner(Arc::new(SettingsRunner));
        let mut settings = ImportSettingsViewModel::default();
        assert!(settings.refresh(&mut imports, "models/chair.obj"));
        assert!(settings.supported());
        assert_eq!(settings.importer(), "obj");
        assert_eq!(settings.rows()[0].name, "scale");
        let set = settings.set("scale", "0.01");
        assert_eq!(set.command, "asset.import-setting.set");
        assert_eq!(
            set.arguments.get("value").and_then(Value::as_text),
            Some("0.01")
        );

        assert!(settings.refresh(&mut imports, "models/chair.blend"));
        assert!(!settings.supported());
        assert!(settings.rows().is_empty());
    }
}
