//! Project settings, user preferences, and project creation from templates. Task 3.6.
//!
//! > Settings SHALL be stored in **text form suitable for version control**, with user-specific
//! > preferences stored **separately** from project settings.
//! >
//! > **WHEN** a project setting changes **THEN** the diff SHALL show only that setting, and user
//! > preferences SHALL not appear in the project file.
//!
//! That scenario is the whole design, and it is stricter than it reads. "The diff shows only that
//! setting" is false of every settings file that is serialised from a hash map, re-indented by a
//! writer, or that records when it was written — and each of those is a locally reasonable thing to
//! do. So the text form here is CANONICAL:
//!
//!   * one setting per line, `category.name = value`, in name order over the whole file;
//!   * per-platform overrides after the defaults, in `[platform] category.name = value` form,
//!     ordered by platform and then by name;
//!   * a real number written with `{:?}`, which round-trips an `f64` exactly, so reading and
//!     writing a file nobody changed produces the same bytes;
//!   * nothing about the machine, the user, or the time.
//!
//! [`SettingsService::project_diff`] is that property made a function a test can call, and
//! [`tests::changing_one_setting_changes_one_line_of_the_project_file`] is the scenario.
//!
//! --- WHY PREFERENCES ARE A DIFFERENT TYPE AND NOT A FLAG ON A SETTING -------------------------------
//!
//! "Stored separately" as a `scope` field on one value, written by one writer, is separation that
//! survives exactly until somebody adds a setting and forgets the field. Here a declaration says
//! which file a setting belongs in ([`Scope`]), [`ProjectSettings`] refuses to hold a `User`
//! setting and [`UserPreferences`] refuses to hold a `Project` one — by name, at the call — and the
//! two have different writers. A preference cannot reach the project file because there is no code
//! path that would put it there.
//!
//! --- WHAT THIS DOES NOT CLAIM ----------------------------------------------------------------------
//!
//! `editor-architecture`'s **Project management and settings** requirement also asks for package and
//! dependency management for engine modules and Swift packages. That is `ProjectService`'s module
//! list and `SwiftModuleBuilder`, neither of which is joined to this, so the requirement is NOT
//! recorded as answered in `tools/roadmap/requirements-coverage.toml`. What is here is project
//! creation from templates, settings by category with search and per-platform overrides, and the
//! version-control scenario above.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;
use std::path::{Path, PathBuf};

use cy_editor_core::problem::{Problem, Result};

/// Which file a setting lives in.
///
/// A property of the DECLARATION rather than of a value, so that where a setting is written is
/// decided once, by whoever declared it, rather than at each call that happens to write one.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum Scope {
    /// Shared with the team, in the project file, in version control.
    Project,
    /// This person's, on this machine, and never in the project file.
    User,
}

/// What a setting holds.
///
/// Five shapes rather than a general value type: every one of them has an unambiguous canonical
/// text form, which is what the version-control scenario rests on.
#[derive(Clone, PartialEq, Debug)]
pub enum SettingValue {
    /// On or off.
    Flag(bool),
    /// A whole number.
    Whole(i64),
    /// A real number, written so that it round-trips exactly.
    Real(f64),
    /// A line of text.
    Text(String),
    /// An ordered list — layers, tags, the platforms a target builds for.
    List(Vec<String>),
}

impl SettingValue {
    /// The canonical text form. `{:?}` on the real, because `{}` drops the precision that makes a
    /// written-then-read file produce the same bytes.
    fn write(&self) -> String {
        match self {
            SettingValue::Flag(value) => value.to_string(),
            SettingValue::Whole(value) => value.to_string(),
            SettingValue::Real(value) => format!("{value:?}"),
            SettingValue::Text(value) => format!("{value:?}"),
            SettingValue::List(values) => values
                .iter()
                .map(|value| format!("{value:?}"))
                .collect::<Vec<_>>()
                .join(", "),
        }
    }

    /// Which shape this is, for the type check a write makes against the declaration.
    fn shape(&self) -> &'static str {
        match self {
            SettingValue::Flag(_) => "flag",
            SettingValue::Whole(_) => "whole number",
            SettingValue::Real(_) => "real number",
            SettingValue::Text(_) => "text",
            SettingValue::List(_) => "list",
        }
    }
}

/// One setting the editor knows about: where it lives, what shape it is, and what it is for.
#[derive(Clone, PartialEq, Debug)]
pub struct Declaration {
    /// `rendering.shadow_resolution` — the category, a dot, and the name.
    pub key: String,
    /// What a person reads when they are looking for it. Searched, so it is prose rather than a
    /// restatement of the key.
    pub summary: String,
    /// Which file it lives in.
    pub scope: Scope,
    /// What it holds when nobody has set it.
    pub default: SettingValue,
    /// Whether a platform may override it. Not every setting may: a layer list that differed
    /// between platforms would make a world mean different things on two machines.
    pub per_platform: bool,
}

impl Declaration {
    /// The part before the dot.
    pub fn category(&self) -> &str {
        self.key
            .split_once('.')
            .map_or(self.key.as_str(), |(category, _)| category)
    }
}

/// Everything the editor can be configured with, by key.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct Catalogue {
    declarations: BTreeMap<String, Declaration>,
}

impl Catalogue {
    /// The editor's own settings: the categories `editor-architecture` names by name — layer and
    /// tag configuration, input action mapping, quality and rendering settings — plus the
    /// preferences that are this person's and not the project's.
    pub fn builtin() -> Self {
        let declare = |key: &str, summary: &str, scope, default, per_platform| Declaration {
            key: key.to_owned(),
            summary: summary.to_owned(),
            scope,
            default,
            per_platform,
        };
        let mut catalogue = Self::default();
        for declaration in [
            declare(
                "layers.names",
                "the world layers a scene may put a node in",
                Scope::Project,
                SettingValue::List(vec!["default".into()]),
                false,
            ),
            declare(
                "tags.names",
                "the tags gameplay code may query nodes by",
                Scope::Project,
                SettingValue::List(Vec::new()),
                false,
            ),
            declare(
                "input.actions",
                "the named actions a device binding maps onto",
                Scope::Project,
                SettingValue::List(Vec::new()),
                false,
            ),
            declare(
                "quality.level",
                "the quality preset a build ships at",
                Scope::Project,
                SettingValue::Text("high".into()),
                true,
            ),
            declare(
                "rendering.shadow_resolution",
                "the edge of a shadow map, in texels",
                Scope::Project,
                SettingValue::Whole(2048),
                true,
            ),
            declare(
                "rendering.target_frame_rate",
                "the frame rate the project is budgeted against",
                Scope::Project,
                SettingValue::Whole(60),
                true,
            ),
            declare(
                "rendering.virtual_texturing",
                "whether textures stream through the virtual texture system",
                Scope::Project,
                SettingValue::Flag(true),
                true,
            ),
            declare(
                "project.name",
                "what this project is called",
                Scope::Project,
                SettingValue::Text("untitled".into()),
                false,
            ),
            declare(
                "editor.theme",
                "which theme this person's editor draws in",
                Scope::User,
                SettingValue::Text("dark".into()),
                false,
            ),
            declare(
                "editor.autosave_seconds",
                "how often this person's editor saves a draft",
                Scope::User,
                SettingValue::Whole(120),
                false,
            ),
            declare(
                "editor.recent_projects",
                "the projects this person opened last",
                Scope::User,
                SettingValue::List(Vec::new()),
                false,
            ),
        ] {
            catalogue
                .declarations
                .insert(declaration.key.clone(), declaration);
        }
        catalogue
    }

    /// Declare a setting. Refuses a key that is already declared, and one with no category.
    pub fn declare(&mut self, declaration: Declaration) -> Result<()> {
        if !declaration.key.contains('.') {
            return Err(Problem::new(
                format!("declare a setting called {}", declaration.key),
                "a setting is `category.name`, and one with no category cannot be organised by \
                 category or found by searching for its category",
            )
            .with_remedy("name it `<category>.<name>`"));
        }
        if self.declarations.contains_key(&declaration.key) {
            return Err(Problem::new(
                format!("declare {} twice", declaration.key),
                "two declarations of one key would make a setting's scope and shape depend on \
                 which plugin loaded last",
            )
            .with_remedy("give the second one its own key"));
        }
        self.declarations
            .insert(declaration.key.clone(), declaration);
        Ok(())
    }

    /// The declaration of this key.
    pub fn get(&self, key: &str) -> Option<&Declaration> {
        self.declarations.get(key)
    }

    /// Every category, in order.
    pub fn categories(&self) -> Vec<&str> {
        self.declarations
            .values()
            .map(Declaration::category)
            .collect::<BTreeSet<_>>()
            .into_iter()
            .collect()
    }

    /// Every setting of a category, in key order.
    pub fn in_category(&self, category: &str) -> Vec<&Declaration> {
        self.declarations
            .values()
            .filter(|declaration| declaration.category() == category)
            .collect()
    }

    /// Settings whose key or summary contains the query, case-insensitively, in key order.
    ///
    /// Over the summary as well as the key, because a person looking for shadows types "shadow"
    /// and a person looking for the frame budget types "frame rate" — and a search that only
    /// matched keys would find the first and not the second.
    pub fn search(&self, query: &str) -> Vec<&Declaration> {
        let needle = query.trim().to_lowercase();
        if needle.is_empty() {
            return Vec::new();
        }
        self.declarations
            .values()
            .filter(|declaration| {
                declaration.key.to_lowercase().contains(&needle)
                    || declaration.summary.to_lowercase().contains(&needle)
            })
            .collect()
    }
}

/// The settings that go in the project file, shared with the team.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct ProjectSettings {
    values: BTreeMap<String, SettingValue>,
    overrides: BTreeMap<(String, String), SettingValue>,
}

/// The settings that stay on this machine.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct UserPreferences {
    values: BTreeMap<String, SettingValue>,
}

/// Project settings, user preferences and the catalogue that says which is which.
#[derive(Clone, PartialEq, Debug)]
pub struct SettingsService {
    catalogue: Catalogue,
    project: ProjectSettings,
    user: UserPreferences,
}

impl Default for SettingsService {
    fn default() -> Self {
        Self::new(Catalogue::builtin())
    }
}

impl SettingsService {
    /// The file the project's settings live in, beside `project.json`.
    pub const PROJECT_FILE: &'static str = "settings.cyset";
    /// The file this person's preferences live in. **Never inside the project directory**: a
    /// preferences file under the project root is a preferences file somebody commits.
    pub const USER_FILE: &'static str = "preferences.cyset";

    /// A service over this catalogue, with nothing set.
    pub fn new(catalogue: Catalogue) -> Self {
        Self {
            catalogue,
            project: ProjectSettings::default(),
            user: UserPreferences::default(),
        }
    }

    /// What settings exist.
    pub fn catalogue(&self) -> &Catalogue {
        &self.catalogue
    }

    /// Set a project setting. Refuses a key that is not declared, a value of the wrong shape, and
    /// a setting the catalogue says is this person's rather than the project's.
    pub fn set_project(&mut self, key: &str, value: SettingValue) -> Result<()> {
        self.declared(key, &value, Scope::Project)?;
        self.project.values.insert(key.to_owned(), value);
        Ok(())
    }

    /// Set a project setting for one platform only.
    pub fn set_platform(&mut self, platform: &str, key: &str, value: SettingValue) -> Result<()> {
        let declaration = self.declared(key, &value, Scope::Project)?;
        if !declaration.per_platform {
            return Err(Problem::new(
                format!("override {key} for {platform}"),
                format!(
                    "{key} is declared the same on every platform; an override would make the \
                     project mean two different things depending on which machine opened it"
                ),
            )
            .with_remedy("set it for the whole project, or declare it per_platform"));
        }
        self.project
            .overrides
            .insert((platform.to_owned(), key.to_owned()), value);
        Ok(())
    }

    /// Set one of this person's preferences.
    pub fn set_user(&mut self, key: &str, value: SettingValue) -> Result<()> {
        self.declared(key, &value, Scope::User)?;
        self.user.values.insert(key.to_owned(), value);
        Ok(())
    }

    /// What a setting is, on this platform, falling back to the project value and then the default.
    pub fn effective(&self, key: &str, platform: &str) -> Option<SettingValue> {
        let declaration = self.catalogue.get(key)?;
        self.project
            .overrides
            .get(&(platform.to_owned(), key.to_owned()))
            .or_else(|| self.project.values.get(key))
            .or_else(|| self.user.values.get(key))
            .cloned()
            .or_else(|| Some(declaration.default.clone()))
    }

    /// The project file's bytes, canonically.
    pub fn write_project(&self) -> String {
        let mut text = String::from(
            "# CyberEngine project settings. Canonical: one setting a \
                                     line, in key order.\n",
        );
        for (key, value) in &self.project.values {
            let _ = writeln!(text, "{key} = {}", value.write());
        }
        for ((platform, key), value) in &self.project.overrides {
            let _ = writeln!(text, "[{platform}] {key} = {}", value.write());
        }
        text
    }

    /// This person's file's bytes, canonically, in a directory that is not the project's.
    pub fn write_user(&self) -> String {
        let mut text = String::from(
            "# This machine's editor preferences. Not the project's; do \
                                     not commit.\n",
        );
        for (key, value) in &self.user.values {
            let _ = writeln!(text, "{key} = {}", value.write());
        }
        text
    }

    /// The lines that differ between the project file this service would write now and `before`.
    ///
    /// The scenario made a function: "the diff SHALL show only that setting". A line diff rather
    /// than a byte one, because a line is what a reviewer reads and what `git diff` reports.
    pub fn project_diff(before: &str, after: &str) -> Vec<String> {
        let earlier: BTreeSet<&str> = before.lines().collect();
        let later: BTreeSet<&str> = after.lines().collect();
        let mut changed: Vec<String> = later
            .difference(&earlier)
            .map(|line| format!("+{line}"))
            .collect();
        changed.extend(earlier.difference(&later).map(|line| format!("-{line}")));
        changed.sort();
        changed
    }

    /// The declaration for a write, or why the write is refused.
    fn declared(&self, key: &str, value: &SettingValue, scope: Scope) -> Result<&Declaration> {
        let declaration = self.catalogue.get(key).ok_or_else(|| {
            Problem::new(
                format!("set {key}"),
                "no setting of that key is declared, and an undeclared setting written into a \
                 project file is one nothing reads",
            )
            .with_remedy("declare it first, or search the catalogue for the key you meant")
        })?;
        if declaration.scope != scope {
            let (is, asked) = match scope {
                Scope::Project => ("this person's preference", "the project file"),
                Scope::User => ("a project setting", "this person's preferences"),
            };
            return Err(Problem::new(
                format!("write {key} into {asked}"),
                format!(
                    "{key} is declared as {is}, and writing it to the other file is how a \
                     preference reaches version control"
                ),
            )
            .with_remedy("set it through the other call, or change the declaration's scope"));
        }
        if declaration.default.shape() != value.shape() {
            return Err(Problem::new(
                format!("set {key} to a {}", value.shape()),
                format!("{key} is declared as a {}", declaration.default.shape()),
            )
            .with_remedy("give it a value of the declared shape"));
        }
        Ok(declaration)
    }
}

/// A project template: what a new project starts as.
#[derive(Clone, PartialEq, Debug)]
pub struct Template {
    /// What it is called in the new-project dialogue.
    pub name: String,
    /// What a person reads to choose between templates.
    pub summary: String,
    /// The files it writes, relative to the new project's root, in path order.
    pub files: Vec<(String, String)>,
}

impl Template {
    /// The templates the editor ships. Two, because one is a choice nobody makes.
    pub fn builtin() -> Vec<Template> {
        vec![
            Template {
                name: "empty".into(),
                summary: "a declared project with one world and nothing in it".into(),
                files: vec![
                    (
                        "project.json".into(),
                        "{\n  \"name\": \"untitled\",\n  \"modules\": []\n}\n".into(),
                    ),
                    ("worlds/main.cyworld".into(), "cyworld 1\n".into()),
                ],
            },
            Template {
                name: "swift-gameplay".into(),
                summary: "an empty project with a Swift gameplay module declared and built".into(),
                files: vec![
                    (
                        "project.json".into(),
                        "{\n  \"name\": \"untitled\",\n  \"modules\": [\"gameplay\"]\n}\n".into(),
                    ),
                    ("gameplay/Gameplay.swift".into(), "// gameplay\n".into()),
                    ("worlds/main.cyworld".into(), "cyworld 1\n".into()),
                ],
            },
        ]
    }

    /// Create a project from this template at `root`.
    ///
    /// Refuses a root that already holds a project manifest, by name. Creating a project on top of
    /// one overwrites its manifest, and the person who did it finds out from source control.
    pub fn create(&self, root: &Path) -> Result<Vec<PathBuf>> {
        if root.join("project.json").is_file() {
            return Err(Problem::new(
                format!("create a project at {}", root.display()),
                "there is already a project there, and creating one over it would overwrite its \
                 manifest",
            )
            .with_remedy("choose an empty directory, or open the project that is there"));
        }
        let mut written = Vec::new();
        for (relative, contents) in &self.files {
            let path = root.join(relative);
            if let Some(parent) = path.parent() {
                std::fs::create_dir_all(parent).map_err(|error| Self::failed(&path, &error))?;
            }
            std::fs::write(&path, contents).map_err(|error| Self::failed(&path, &error))?;
            written.push(path);
        }
        Ok(written)
    }

    fn failed(path: &Path, error: &std::io::Error) -> Problem {
        Problem::new(
            format!("write {}", path.display()),
            format!("the filesystem refused: {error}"),
        )
        .with_remedy("choose a directory this account can write to")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn service() -> SettingsService {
        SettingsService::default()
    }

    #[test]
    fn changing_one_setting_changes_one_line_of_the_project_file() {
        let mut settings = service();
        settings
            .set_project("rendering.shadow_resolution", SettingValue::Whole(2048))
            .expect("a declared project setting");
        settings
            .set_project("project.name", SettingValue::Text("Pillar".into()))
            .expect("a declared project setting");
        let before = settings.write_project();

        settings
            .set_project("rendering.shadow_resolution", SettingValue::Whole(4096))
            .expect("the same setting again");
        let after = settings.write_project();

        let changed = SettingsService::project_diff(&before, &after);
        assert_eq!(
            changed,
            vec![
                "+rendering.shadow_resolution = 4096".to_owned(),
                "-rendering.shadow_resolution = 2048".to_owned(),
            ],
            "the diff shows more than the setting that changed"
        );
    }

    #[test]
    fn a_preference_never_appears_in_the_project_file() {
        let mut settings = service();
        settings
            .set_user("editor.theme", SettingValue::Text("light".into()))
            .expect("a declared preference");
        settings
            .set_user(
                "editor.recent_projects",
                SettingValue::List(vec!["/tmp/a".into()]),
            )
            .expect("a declared preference");
        let project = settings.write_project();
        assert!(
            !project.contains("editor.theme") && !project.contains("recent_projects"),
            "a preference reached the project file:\n{project}"
        );
        assert!(settings.write_user().contains("editor.theme"));
    }

    #[test]
    fn a_preference_written_as_a_project_setting_is_refused_by_name() {
        let mut settings = service();
        let refused = settings
            .set_project("editor.theme", SettingValue::Text("light".into()))
            .expect_err("a preference is not a project setting");
        assert!(
            refused.because.contains("preference"),
            "the refusal does not say what is wrong: {refused:?}"
        );
        assert_eq!(
            settings.write_project().lines().count(),
            1,
            "it was written anyway"
        );
    }

    #[test]
    fn writing_a_file_nobody_changed_produces_the_same_bytes() {
        let mut settings = service();
        settings
            .set_project("rendering.target_frame_rate", SettingValue::Whole(30))
            .expect("declared");
        settings
            .set_platform(
                "ios",
                "rendering.shadow_resolution",
                SettingValue::Whole(1024),
            )
            .expect("declared per platform");
        settings
            .set_project(
                "layers.names",
                SettingValue::List(vec!["default".into(), "sky".into()]),
            )
            .expect("declared");
        assert_eq!(settings.write_project(), settings.write_project());
        assert!(
            SettingsService::project_diff(&settings.write_project(), &settings.write_project())
                .is_empty()
        );

        // AND THE ORDER, WHICH THE DIFF ABOVE CANNOT SEE. `project_diff` compares line SETS, so a
        // writer that emitted the same settings in a different order every run would satisfy it
        // and produce a whole-file `git diff` on every save. The canonical order is what stops
        // that, so it is asserted where it is decided: defaults first, in key order, then the
        // per-platform overrides, in platform and key order.
        let written = settings.write_project();
        let lines: Vec<&str> = written
            .lines()
            .filter(|line| !line.starts_with('#'))
            .collect();
        let defaults: Vec<&str> = lines
            .iter()
            .copied()
            .filter(|line| !line.starts_with('['))
            .collect();
        let overrides: Vec<&str> = lines
            .iter()
            .copied()
            .filter(|line| line.starts_with('['))
            .collect();
        assert_eq!(
            lines,
            [defaults.clone(), overrides.clone()].concat(),
            "the overrides are interleaved with the defaults rather than following them"
        );
        let mut sorted = defaults.clone();
        sorted.sort_unstable();
        assert_eq!(
            defaults, sorted,
            "the settings are not written in key order:\n{written}"
        );
        let mut sorted_overrides = overrides.clone();
        sorted_overrides.sort_unstable();
        assert_eq!(
            overrides, sorted_overrides,
            "the overrides are not written in order"
        );
    }

    #[test]
    fn a_platform_override_wins_on_that_platform_and_nowhere_else() {
        let mut settings = service();
        settings
            .set_project("rendering.shadow_resolution", SettingValue::Whole(2048))
            .expect("declared");
        settings
            .set_platform(
                "ios",
                "rendering.shadow_resolution",
                SettingValue::Whole(512),
            )
            .expect("declared per platform");
        assert_eq!(
            settings.effective("rendering.shadow_resolution", "ios"),
            Some(SettingValue::Whole(512))
        );
        assert_eq!(
            settings.effective("rendering.shadow_resolution", "windows"),
            Some(SettingValue::Whole(2048))
        );
        let refused = settings
            .set_platform("ios", "layers.names", SettingValue::List(Vec::new()))
            .expect_err("a layer list may not differ between platforms");
        assert!(
            refused.because.contains("two different things"),
            "{refused:?}"
        );
    }

    #[test]
    fn settings_are_organised_by_category_and_found_by_searching_their_words() {
        let settings = service();
        let categories = settings.catalogue().categories();
        for expected in ["layers", "tags", "input", "quality", "rendering"] {
            assert!(
                categories.contains(&expected),
                "no {expected} category in {categories:?}"
            );
        }
        let found: Vec<&str> = settings
            .catalogue()
            .search("frame rate")
            .iter()
            .map(|declaration| declaration.key.as_str())
            .collect();
        assert_eq!(
            found,
            vec!["rendering.target_frame_rate"],
            "a search over summaries did not find a setting nobody would guess the key of"
        );
        assert!(
            settings.catalogue().search("").is_empty(),
            "an empty query matched everything"
        );
    }

    #[test]
    fn a_project_is_created_from_a_template_and_not_over_one() {
        let directory =
            std::env::temp_dir().join(format!("cy-settings-{}-{}", std::process::id(), line!()));
        let _ = std::fs::remove_dir_all(&directory);
        std::fs::create_dir_all(&directory).expect("a temporary directory");

        let templates = Template::builtin();
        assert!(templates.len() >= 2, "one template is not a choice");
        let empty = templates
            .iter()
            .find(|template| template.name == "empty")
            .expect("the empty template");
        let written = empty.create(&directory).expect("a new project");
        assert!(written.iter().any(|path| path.ends_with("project.json")));
        assert!(directory.join("worlds/main.cyworld").is_file());

        let refused = empty
            .create(&directory)
            .expect_err("a project already exists there");
        assert!(refused.because.contains("already a project"), "{refused:?}");
        let _ = std::fs::remove_dir_all(&directory);
    }

    #[test]
    fn a_setting_of_the_wrong_shape_is_refused_rather_than_coerced() {
        let mut settings = service();
        let refused = settings
            .set_project(
                "rendering.shadow_resolution",
                SettingValue::Text("big".into()),
            )
            .expect_err("a texel count is a whole number");
        assert!(refused.because.contains("whole number"), "{refused:?}");
        assert!(
            settings
                .set_project("nothing.declared", SettingValue::Flag(true))
                .is_err(),
            "an undeclared setting was written"
        );
    }
}
