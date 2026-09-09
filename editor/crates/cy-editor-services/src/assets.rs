//! Importing a source asset from inside the editor, and landing it in the open world.
//! M8.a tasks 3.1 and 3.5.
//!
//! --- THE GAP THIS CLOSES, MEASURED RATHER THAN ASSUMED -----------------------------------------
//!
//! `tools/import` has existed since M5 and was reachable only from a command line. So content was
//! cooked outside the editor; a person adding a model to a world had to leave; and an **agent**
//! could not import at all, because `editor-agent-interface` projects the command registry rather
//! than maintaining a list — a capability that is not a command is not a tool. One command fixes all
//! three, which is the argument for the registry being the single action surface.
//!
//! --- WHY THE IMPORTER IS A SUBPROCESS AND NOT A LINKED LIBRARY ---------------------------------
//!
//! `tools/import` is **layer 7** and the layering rule is that nothing links layer 7. That is not a
//! convention: `thirdparty-dependencies` requires that "WHEN a shipped runtime loads content THEN it
//! SHALL read the cooked format directly, with no glTF or FBX parser present", and the layer is what
//! makes it true rather than intended. An editor that linked `cy_import` would put ufbx and xatlas
//! inside a process the runtime also links.
//!
//! So the editor runs `cy_import_cli`, exactly as [`crate::project::SwiftModuleBuilder`] runs
//! `cy_swift_module.py` and for the same reason. `--json` exists on that tool because of this
//! caller: an agent that had to parse the human report to learn which mesh an import produced is an
//! agent that will get it wrong.
//!
//! --- WHY IT IS A TRAIT ---------------------------------------------------------------------------
//!
//! [`AssetHost`] is what the command sees, and [`AssetImportService`] is what the editor gives it. A
//! test supplies a double and asserts the command's own behaviour — the refusals, the one
//! transaction, the entity it creates — with no engine built. `ModuleBuilder` made the same trade
//! for the same reason, and `editor-rust-application`'s "services, models, view models, and commands
//! SHALL be testable headlessly" is satisfied by construction rather than by effort.
//!
//! --- WHY AN IMPORT CREATES AN ENTITY BY CALLING THE PRIMITIVE PATH -----------------------------
//!
//! Task 3.5 asks that an imported mesh land in the open world as an entity, through a transaction.
//! Task 2.3 asks that nothing downstream be able to tell a generated primitive from an imported
//! mesh. Both are satisfied by there being ONE constructor —
//! [`crate::primitives::create_mesh_instance`] — that both callers use. A difference between an
//! imported entity and a created one would have to be written on purpose, into a function that has
//! no parameter for it.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use cy_editor_commands::{
    Arguments, AssetHost, AssetImportOutcome, AssetImportRequest, Command, CommandContext,
    EffectClass, ImportedSubAsset, Metadata, Outcome, ParameterSpec, Registry,
};
use cy_editor_core::ids::{DocumentId, NodeId};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::selection::Selection;
use cy_editor_viewport::gizmo::Transform3;
use cy_editor_viewport::math::Vec3;

use crate::primitives::create_mesh_instance;

/// Register `asset.import`.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(import())?;
    Ok(())
}

// --- The service ---------------------------------------------------------------------------------

/// Where an import's cooked output lives, relative to the project root.
///
/// Inside the project rather than beside it, because a cook that wrote outside the project would be
/// a cook a person cannot find and continuous integration cannot cache. `tools/project/project.py`
/// already treats a leading dot as the project's own working area.
pub const COOKED_DIRECTORY: &str = ".cy/cooked";

/// Where the derived-data cache lives, relative to the project root. See [`COOKED_DIRECTORY`].
pub const CACHE_DIRECTORY: &str = ".cy/cache";

/// How an import is actually run.
///
/// A trait so that a test does not need a built engine, and so that another way of reaching the
/// importers — in process one day, over a service another — is a different implementation rather
/// than a branch in this file. `ProjectService`'s [`crate::project::ModuleBuilder`] made the same
/// trade for the same reason.
pub trait ImportRunner: Send + Sync {
    /// What this runner is, for a message a person reads.
    fn describe(&self) -> String;

    /// The source extensions it can import, lower-case and with their dot. Empty when it cannot
    /// answer, which is a different thing from "none".
    fn extensions(&self) -> Vec<String>;

    /// Import one source out of `root`.
    ///
    /// # Errors
    ///
    /// When the importer cannot be run, or when the import failed — with the importer's own
    /// diagnostics as the reason.
    fn run(&self, root: &Path, request: &AssetImportRequest) -> Result<AssetImportOutcome>;
}

/// The importers, run out of process.
pub struct AssetImportService {
    root: PathBuf,
    runner: Arc<dyn ImportRunner>,
    /// What the runner reported, read once and kept: asking costs a process launch and the answer
    /// cannot change while this binary is running.
    extensions: Option<Vec<String>>,
}

impl AssetImportService {
    /// The importer for a project, with the command-line runner found the way the Swift driver is.
    ///
    /// The real runner by default rather than a stub, for the reason `ProjectService::new` states:
    /// a capability that is off until somebody configures it is a capability nobody has.
    #[must_use]
    pub fn new(root: impl Into<PathBuf>) -> Self {
        let root = root.into();
        let runner = Arc::new(CliImportRunner::found_near(&root));
        Self {
            root,
            runner,
            extensions: None,
        }
    }

    /// Import through something else — a test's recording double, or another way of reaching the
    /// importers.
    #[must_use]
    pub fn with_runner(mut self, runner: Arc<dyn ImportRunner>) -> Self {
        self.runner = runner;
        self.extensions = None;
        self
    }

    /// Point the service at another project, and find that project's importer.
    pub fn rooted_at(&mut self, root: impl Into<PathBuf>) {
        self.root = root.into();
        self.runner = Arc::new(CliImportRunner::found_near(&self.root));
        self.extensions = None;
    }

    /// Where the project is.
    #[must_use]
    pub fn root(&self) -> &Path {
        &self.root
    }

    /// What is running the importers, for a message a person reads.
    #[must_use]
    pub fn describe(&self) -> String {
        self.runner.describe()
    }
}

impl AssetHost for AssetImportService {
    fn importable_extensions(&mut self) -> Vec<String> {
        if let Some(known) = &self.extensions {
            return known.clone();
        }
        let listed = self.runner.extensions();
        self.extensions = Some(listed.clone());
        listed
    }

    fn import_asset(&mut self, request: &AssetImportRequest) -> Result<AssetImportOutcome> {
        self.runner.run(&self.root, request)
    }
}

/// `cy_import_cli`, as a subprocess.
pub struct CliImportRunner {
    tool: Option<PathBuf>,
}

impl CliImportRunner {
    /// Find `cy_import_cli`: the environment first, then the build directories a checkout has.
    ///
    /// `CY_IMPORT_CLI` first and deliberately, because that is the one answer that is always right —
    /// a developer with two build trees, a packaged editor, and continuous integration all have a
    /// path they know and the editor does not. The search after it is the convenience for a
    /// developer who has not set it, and it walks up from the project exactly as
    /// `SwiftModuleBuilder::found_near` does, for the same reason: a sample inside this repository
    /// finds the tools built beside it.
    #[must_use]
    pub fn found_near(root: &Path) -> Self {
        if let Some(named) = std::env::var_os("CY_IMPORT_CLI") {
            let path = PathBuf::from(named);
            if path.is_file() {
                return Self { tool: Some(path) };
            }
        }
        let relative = Path::new("tools").join("import").join("cy_import_cli");
        let mut directory = Some(root);
        while let Some(current) = directory {
            for profile in ["dev", "profile", "release", "debug"] {
                let candidate = current.join("build").join(profile).join(&relative);
                if candidate.is_file() {
                    return Self {
                        tool: Some(candidate),
                    };
                }
            }
            directory = current.parent();
        }
        Self { tool: None }
    }

    fn tool(&self) -> Result<&Path> {
        self.tool.as_deref().ok_or_else(|| {
            Problem::new("run the asset importer", "no cy_import_cli could be found").with_remedy(
                "build the engine's tools (just build-tools), or set CY_IMPORT_CLI to the importer \
                 binary",
            )
        })
    }
}

impl ImportRunner for CliImportRunner {
    fn describe(&self) -> String {
        self.tool.as_ref().map_or_else(
            || "no cy_import_cli found: set CY_IMPORT_CLI, or build the engine's tools".to_string(),
            |tool| tool.display().to_string(),
        )
    }

    fn extensions(&self) -> Vec<String> {
        let Ok(tool) = self.tool() else {
            return Vec::new();
        };
        let Ok(output) = std::process::Command::new(tool)
            .arg("--list-importers")
            .output()
        else {
            return Vec::new();
        };
        parse_extensions(&String::from_utf8_lossy(&output.stdout))
    }

    fn run(&self, root: &Path, request: &AssetImportRequest) -> Result<AssetImportOutcome> {
        let tool = self.tool()?.to_path_buf();
        let mut command = std::process::Command::new(&tool);
        command
            .arg("--project")
            .arg(root)
            .arg("--out")
            .arg(root.join(COOKED_DIRECTORY))
            .arg("--cache")
            .arg(root.join(CACHE_DIRECTORY))
            .arg("--json");
        if request.force {
            command.arg("--no-cache");
        }
        for (name, value) in &request.options {
            command.arg("--set").arg(format!("{name}={value}"));
        }
        command.arg(&request.source);

        let output = command.output().map_err(|error| {
            Problem::new(format!("run {}", tool.display()), error.to_string())
                .with_remedy("check that the importer binary is executable")
        })?;
        let text = String::from_utf8_lossy(&output.stdout).to_string();
        let complaint = String::from_utf8_lossy(&output.stderr).trim().to_string();
        if !output.status.success() && text.trim().is_empty() {
            // The tool refused before it produced a report — an unknown option, a path outside the
            // project. Its own message is the reason, because "the import failed" teaches nobody.
            return Err(Problem::new(
                format!("import {}", request.source),
                if complaint.is_empty() {
                    "the importer produced no output".to_string()
                } else {
                    complaint
                },
            )
            .with_remedy("check the path and the options against the importer's own schema"));
        }
        let outcome = parse_json_outcome(&text).ok_or_else(|| {
            Problem::new(
                format!("import {}", request.source),
                "the importer produced output this editor could not read",
            )
            .with_remedy("run the importer by hand to see what it wrote")
        })?;
        if outcome.errors != 0 {
            return Err(Problem::new(
                format!("import {}", request.source),
                if complaint.is_empty() {
                    format!("the importer reported {} error(s)", outcome.errors)
                } else {
                    complaint
                },
            )
            .with_remedy("fix the reported problem in the source and import again"));
        }
        Ok(outcome)
    }
}

/// The extensions out of `--list-importers`.
///
/// The tool prints `  extensions: .gltf .glb` under each importer. Parsed rather than duplicated,
/// so an importer a project registers appears here the day it is registered.
fn parse_extensions(listing: &str) -> Vec<String> {
    let mut found: Vec<String> = Vec::new();
    for line in listing.lines() {
        let Some(rest) = line.trim().strip_prefix("extensions:") else {
            continue;
        };
        for extension in rest.split_whitespace() {
            let lowered = extension.to_ascii_lowercase();
            if !found.contains(&lowered) {
                found.push(lowered);
            }
        }
    }
    found
}

// --- Reading what the tool said ------------------------------------------------------------------
//
// A hand-written reader for one document shape, rather than a JSON dependency in this crate. The
// document is `cy_import_cli --json`'s and both halves are in this repository, so the alternative —
// pulling `serde_json` into the editor for six fields — buys nothing a test does not already give.
// `a_report_the_importer_actually_wrote` is that test: it parses the exact bytes the tool produced.

/// The string value of `"<key>": "<value>"`, from `at` onwards.
fn string_field(text: &str, key: &str, at: usize) -> Option<String> {
    let needle = format!("\"{key}\":");
    let start = text.get(at..)?.find(&needle)? + at + needle.len();
    let rest = text.get(start..)?.trim_start();
    let body = rest.strip_prefix('"')?;
    let mut value = String::new();
    let mut characters = body.chars();
    while let Some(character) = characters.next() {
        match character {
            '"' => return Some(value),
            '\\' => value.push(characters.next()?),
            other => value.push(other),
        }
    }
    None
}

/// The number value of `"<key>": <value>`, from `at` onwards.
fn number_field(text: &str, key: &str, at: usize) -> Option<usize> {
    let needle = format!("\"{key}\":");
    let start = text.get(at..)?.find(&needle)? + at + needle.len();
    let rest = text.get(start..)?.trim_start();
    let digits: String = rest.chars().take_while(char::is_ascii_digit).collect();
    digits.parse().ok()
}

/// The first source's row, which is the only one a single-source import produces.
fn parse_json_outcome(text: &str) -> Option<AssetImportOutcome> {
    let mut outcome = AssetImportOutcome {
        source: string_field(text, "source", 0)?,
        importer: string_field(text, "importer", 0).unwrap_or_default(),
        id: string_field(text, "id", 0).unwrap_or_default(),
        cache: string_field(text, "cache", 0).unwrap_or_default(),
        sub_assets: Vec::new(),
        warnings: number_field(text, "warnings", 0).unwrap_or_default(),
        errors: number_field(text, "errors", 0).unwrap_or_default(),
        steps_not_reached: string_field(text, "steps_not_reached", 0).unwrap_or_default(),
    };
    // The sub-asset table lives after `"assets": [`, and each entry is a name and an id. Walking
    // from that offset is what keeps the row's own `"id"` from being read as the first entry's.
    if let Some(table) = text.find("\"assets\":") {
        let mut at = table;
        while let Some(name) = string_field(text, "name", at) {
            let name_at = text[at..].find("\"name\":")? + at;
            let id = string_field(text, "id", name_at).unwrap_or_default();
            outcome.sub_assets.push(ImportedSubAsset { name, id });
            at = name_at + "\"name\":".len();
        }
    }
    Some(outcome)
}

// --- The command ---------------------------------------------------------------------------------

/// The document a command with no explicit target acts on, or a refusal that says what to do.
fn active(context: &dyn CommandContext) -> Result<DocumentId> {
    context.active_document().ok_or_else(|| {
        Problem::new("import into a world", "no document is open").with_remedy(
            "open a world first — the imported asset still cooks without one, but \
                          there is nowhere to put the entity",
        )
    })
}

/// An entity identity as this command's own results print it.
fn parse_node(text: &str) -> Result<Option<NodeId>> {
    let trimmed = text.trim();
    if trimmed.is_empty() {
        return Ok(None);
    }
    u128::from_str_radix(trimmed, 16)
        .map(NodeId::from_u128)
        .map(Some)
        .map_err(|_| {
            Problem::new(
                format!("read the entity {trimmed:?}"),
                "it is not an entity identity",
            )
            .with_remedy("use the identity a command's result printed, which is 32 hex digits")
        })
}

/// `name=value,name=value`, as one argument, because a command's parameters are typed and a map is
/// not one of the types.
///
/// Refuses an entry with no `=` naming it, rather than dropping it: an option a person wrote and the
/// editor ignored is a setting that appears to work and does nothing.
fn parse_options(text: &str) -> Result<BTreeMap<String, String>> {
    let mut options = BTreeMap::new();
    for entry in text.split(',') {
        let trimmed = entry.trim();
        if trimmed.is_empty() {
            continue;
        }
        let (name, value) = trimmed.split_once('=').ok_or_else(|| {
            Problem::new(
                "read the import options",
                format!("{trimmed:?} is not name=value"),
            )
            .with_remedy("write them as lod-count=3,collision-mode=convex")
        })?;
        options.insert(name.trim().to_string(), value.trim().to_string());
    }
    Ok(options)
}

/// Refuse a path the current invocation's scope does not cover.
///
/// The same rule `crate::authoring` applies to a source write, and it applies here for the same
/// reason: an import writes cooked bytes and two sidecars into the project, which is a mutation
/// whatever else it is.
fn within_scope(context: &dyn CommandContext, path: &str) -> Result<()> {
    let Some((scope, directories)) = context.permitted_paths() else {
        return Ok(());
    };
    if directories
        .iter()
        .any(|prefix| path.starts_with(prefix.as_str()))
    {
        return Ok(());
    }
    Err(Problem::new(
        format!("import {path}"),
        format!("the scope {scope:?} does not include it"),
    )
    .with_remedy(if directories.is_empty() {
        "this connection was granted no directories; grant one deliberately".to_string()
    } else {
        format!(
            "the directories it may touch are: {}",
            directories.join(", ")
        )
    }))
}

/// The importers, or a refusal that says what to do about their absence.
fn host(context: &mut dyn CommandContext) -> Result<&mut dyn AssetHost> {
    context.assets().ok_or_else(|| {
        Problem::new("reach the asset importer", "this editor has no importer")
            .with_remedy("open a project first")
    })
}

/// Refuse a source whose extension no importer in this build claims.
///
/// Named against the list the TOOL reported rather than one written here, so a project's own
/// importer is admitted the day it is registered.
fn claimed(context: &mut dyn CommandContext, path: &str) -> Result<()> {
    let extensions = host(context)?.importable_extensions();
    if extensions.is_empty() {
        // Not "unsupported format": the editor could not ask. Saying the first when the second
        // happened is how somebody spends an afternoon on the wrong problem.
        return Ok(());
    }
    let lowered = path.to_ascii_lowercase();
    if extensions
        .iter()
        .any(|extension| lowered.ends_with(extension.as_str()))
    {
        return Ok(());
    }
    Err(Problem::new(
        format!("import {path}"),
        "no importer in this build claims that extension",
    )
    .with_remedy(format!("this build imports: {}", extensions.join(", "))))
}

fn import() -> Command {
    Command::new(import_metadata(), |context, arguments| {
        run_import(context, arguments)
    })
}

/// What `asset.import` says about itself.
///
/// Split from the handler because both are long and neither is easier to read beside the other; the
/// metadata is what a menu, a palette and an agent's tool listing all read, and the handler is what
/// runs.
fn import_metadata() -> Metadata {
    Metadata::new(
        "asset.import",
        "Import Asset",
        "Asset",
        "Imports a source file the project already holds — a glTF, an FBX, an OBJ with its .mtl, \
         or a texture — into cooked assets, and places what it produced in the active world as an \
         entity, as one undoable transaction. The import itself is cached: a second import of an \
         unchanged file costs a file open. Undo removes the entity; the cooked assets stay, \
         because they are the file's and not the world's. The result names any step of the import \
         sequence the format cannot express — an OBJ carries no rig — which is a fact about the \
         format and not a warning about the file.",
        EffectClass::ReversibleMutation,
    )
    .with(ParameterSpec::required(
        "path",
        ValueKind::Text,
        "The project-relative path of the source file, such as models/chair.obj.",
    ))
    .with(ParameterSpec::optional(
        "at",
        ValueKind::Vec3,
        "Where to put the entity, in metres, in the world's own space. The origin when omitted.",
        Value::Vec3([0.0, 0.0, 0.0]),
    ))
    .with(ParameterSpec::optional(
        "parent",
        ValueKind::Text,
        "The identity of the entity to create it under; a root when omitted.",
        Value::Text(String::new()),
    ))
    .with(ParameterSpec::optional(
        "options",
        ValueKind::Text,
        "Import options as name=value, comma separated — lod-count=3,collision-mode=convex. The \
         names are the importer's own, and `cy_import_cli --list-importers` prints every one with \
         its meaning. An option no importer declares is refused rather than ignored.",
        Value::Text(String::new()),
    ))
    .with(ParameterSpec::optional(
        "place",
        ValueKind::Bool,
        "Whether to create an entity for what was imported; true when omitted. False cooks the \
         asset and leaves the world alone, which is what a batch import wants.",
        Value::Bool(true),
    ))
    .with(ParameterSpec::optional(
        "force",
        ValueKind::Bool,
        "Ignore the cache and cook again. For diagnosing the cache itself; off when omitted, \
         because a cache that is bypassed by habit is a cache nobody has.",
        Value::Bool(false),
    ))
}

/// Cook the source, then put what it produced in the world.
fn run_import(context: &mut dyn CommandContext, arguments: &Arguments) -> Result<Outcome> {
    let path = arguments
        .text("path")
        .unwrap_or_default()
        .trim()
        .to_string();
    if path.is_empty() {
        return Err(Problem::new("import an asset", "no path was given")
            .with_remedy("name a project-relative path, such as models/chair.obj"));
    }
    within_scope(context, &path)?;
    claimed(context, &path)?;
    let request = AssetImportRequest {
        source: path,
        options: parse_options(arguments.text("options").unwrap_or_default())?,
        force: matches!(arguments.get("force"), Some(Value::Bool(true))),
    };
    let place = !matches!(arguments.get("place"), Some(Value::Bool(false)));

    // The cook first and the entity second: an entity referencing an asset that never cooked would
    // be a world that does not load, and the failure would arrive later and somewhere else. A
    // failed import leaves no transaction behind, because none has been opened yet.
    let imported = host(context)?.import_asset(&request)?;

    let outcome = Outcome::new(summarise(&imported, place))
        .with("asset", Value::Text(imported.source.clone()))
        .with("importer", Value::Text(imported.importer.clone()))
        .with("id", Value::Text(imported.id.clone()))
        .with("cache", Value::Text(imported.cache.clone()))
        .with(
            "sub-assets",
            Value::Int(count_of(imported.sub_assets.len())),
        )
        .with("warnings", Value::Int(count_of(imported.warnings)))
        .with(
            "steps-not-reached",
            Value::Text(imported.steps_not_reached.clone()),
        );
    if !place {
        return Ok(outcome);
    }
    let node = place_imported(context, arguments, &imported.source)?;
    Ok(outcome.with("entity", Value::Text(node.to_string())))
}

/// Task 3.5: the imported asset as an entity in the open world, as one transaction.
///
/// The entity is built by the SAME function a primitive is built by, so nothing downstream can tell
/// an imported mesh from a generated one — see the module note and [`crate::primitives`].
fn place_imported(
    context: &mut dyn CommandContext,
    arguments: &Arguments,
    source: &str,
) -> Result<NodeId> {
    let document_id = active(context)?;
    let parent = parse_node(arguments.text("parent").unwrap_or_default())?;
    let at = match arguments.get("at") {
        Some(Value::Vec3(lanes)) => *lanes,
        _ => [0.0, 0.0, 0.0],
    };
    let actor = context.actor();
    let document = context
        .document_mut(document_id)
        .ok_or_else(|| Problem::not_found("the active document"))?;
    let placement = Transform3 {
        translation: Vec3::new(at[0], at[1], at[2]),
        ..Transform3::default()
    };
    let node = document.with_transaction(format!("Import {source}"), actor, |document| {
        create_mesh_instance(document, parent, source, placement)
    })?;

    let mut selection = Selection::new();
    selection.add_node(node);
    context.set_selection(selection);
    Ok(node)
}

/// The sentence an invocation reports.
///
/// It says what was produced and what the cache did, and it names the absent steps in the words the
/// requirement asks for — "does not carry" rather than "is missing", because the second is a
/// complaint about the file.
fn summarise(imported: &AssetImportOutcome, placed: bool) -> String {
    let mut said = format!(
        "Imported {} with the {} importer: {} sub-asset(s), cache {}",
        imported.source,
        imported.importer,
        imported.sub_assets.len(),
        imported.cache
    );
    if placed {
        said.push_str(", placed in the world");
    }
    if !imported.steps_not_reached.is_empty() {
        let _ = write!(
            said,
            ". This format does not carry: {}",
            imported.steps_not_reached
        );
    }
    said
}

/// A count as the value vocabulary carries it.
///
/// Saturating rather than wrapping: a count that overflowed an `i64` would be a report about
/// something that cannot have happened, and a negative one in a result an agent reads is worse than
/// a large one.
fn count_of(value: usize) -> i64 {
    i64::try_from(value).unwrap_or(i64::MAX)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The exact bytes `cy_import_cli --json` wrote for an OBJ, pasted from a real run. A reader
    /// asserted against a document its author also wrote is a reader that agrees with itself.
    const REAL_REPORT: &str = r#"{
  "sources": [
    {
      "source": "models/chair.obj",
      "importer": "obj",
      "id": "5e014c7f8f1666b05d2b42948d633399",
      "cache": "miss",
      "cache_reason": "",
      "sub_assets": 3,
      "warnings": 0,
      "errors": 0,
      "cooked_bytes": 704,
      "minted_ids": 3,
      "duration_micros": 11486,
      "steps_not_reached": "7 (import skeletons), 8 (import animations), 10 (produce a prefab of the hierarchy)",
    "assets": [
      {"name": "mesh/Seat", "id": "5e014c7f8f1666b05d2b42948d633399"},
      {"name": "collision/Seat_collision", "id": "9b2c162afaaa36bdc52451c6971adbe4"},
      {"name": "material/Oak", "id": "d658eeb594dfd3c1b877fef741678723"}
    ]
    }
  ],
  "warnings": 0,
  "errors": 0,
  "cache_hits": 0,
  "cache_misses": 1
}
"#;

    #[test]
    fn a_report_the_importer_actually_wrote() {
        let outcome = parse_json_outcome(REAL_REPORT).expect("a readable report");
        assert_eq!(outcome.source, "models/chair.obj");
        assert_eq!(outcome.importer, "obj");
        assert_eq!(outcome.id, "5e014c7f8f1666b05d2b42948d633399");
        assert_eq!(outcome.cache, "miss");
        assert_eq!(outcome.errors, 0);
        assert_eq!(outcome.sub_assets.len(), 3);
        assert_eq!(outcome.sub_assets[0].name, "mesh/Seat");
        assert_eq!(outcome.sub_assets[1].id, "9b2c162afaaa36bdc52451c6971adbe4");
        assert_eq!(outcome.sub_assets[2].name, "material/Oak");
        // The row's own `"id"` must not be read as the first sub-asset's, which is what walking
        // from the `"assets":` offset prevents.
        assert_eq!(
            outcome.first("mesh/").map(|asset| asset.name.as_str()),
            Some("mesh/Seat")
        );
        assert!(outcome.steps_not_reached.contains("7 (import skeletons)"));
    }

    #[test]
    fn the_absent_steps_are_named_and_are_not_counted_as_warnings() {
        // Task 3.3's rule reaching the editor: a clean OBJ import reports three steps it did not
        // reach and ZERO warnings, and the sentence a person reads says "does not carry" rather
        // than complaining about the file.
        let outcome = parse_json_outcome(REAL_REPORT).expect("a readable report");
        assert_eq!(outcome.warnings, 0);
        let said = summarise(&outcome, true);
        assert!(said.contains("does not carry"));
        assert!(!said.to_lowercase().contains("warning"));
        assert!(!said.to_lowercase().contains("missing"));
    }

    #[test]
    fn the_extensions_come_from_the_tools_own_listing() {
        let listing = "gltf (version 2)\n  Imports a glTF.\n  extensions: .gltf .glb\n\n\
                       obj (version 1)\n  Imports an OBJ.\n  extensions: .OBJ\n\n";
        assert_eq!(
            parse_extensions(listing),
            vec![".gltf".to_string(), ".glb".to_string(), ".obj".to_string()]
        );
    }

    #[test]
    fn an_option_without_a_value_is_refused_rather_than_dropped() {
        assert!(parse_options("lod-count=3,collision-mode=convex").is_ok());
        let refused = parse_options("lod-count").expect_err("no equals sign");
        assert!(refused.to_string().contains("lod-count"));
    }

    #[test]
    fn options_survive_the_round_trip_in_order() {
        let options = parse_options(" lod-count = 3 , scale=0.01 ").expect("well formed");
        assert_eq!(options.get("lod-count").map(String::as_str), Some("3"));
        assert_eq!(options.get("scale").map(String::as_str), Some("0.01"));
    }
}
