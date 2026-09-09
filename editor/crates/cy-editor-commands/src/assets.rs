//! What a command needs to import a source asset, declared here so the command can live at layer 2.
//! M8.a task 3.1.
//!
//! --- WHY THERE IS AN `asset.import` AT ALL -----------------------------------------------------
//!
//! Eight milestones in, `tools/import` was reachable only from a command line. Content therefore had
//! to be cooked outside the editor, which meant three things at once: a person could not add a model
//! to a world without leaving the editor, an **agent** could not do it at all —
//! `editor-agent-interface` projects the command registry rather than maintaining a list, so a
//! capability that is not a command is not a tool — and nothing in the editor knew what an import
//! had produced.
//!
//! Making import a command fixes all three in one edit, which is the whole argument for the registry
//! being the single action surface: "an action reachable only through a specific widget SHALL be a
//! defect", and a command line is a widget.
//!
//! --- WHY A TRAIT, AND WHY THIS SHAPE -----------------------------------------------------------
//!
//! The same inversion [`crate::context::ProjectHost`] uses, for the same layering reason: running an
//! importer is I/O against a project, which is a *service* at layer 3, and a command that named one
//! would be the upward dependency `editor-rust-application` requires to be a build error.
//!
//! It is also what makes the command testable without a built engine. The real implementation runs
//! `cy_import_cli`; a test supplies thirty lines that answer from a table, and the command's own
//! behaviour — the refusals, the transaction, the entity it creates — is asserted headlessly. That
//! is `ProjectService`'s `ModuleBuilder` argument applied to the same problem.
//!
//! --- WHY THE OUTCOME IS STRUCTURED RATHER THAN A SENTENCE --------------------------------------
//!
//! Because the caller has to act on it. An agent that imported a model and then wanted its mesh
//! would otherwise have to parse a paragraph, and a tool boundary crossed by prose is a source of
//! bugs rather than an interface. `cy_import_cli --json` exists for exactly this reason.

use std::collections::BTreeMap;

use cy_editor_core::problem::Result;

/// One import, as a caller states it.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct AssetImportRequest {
    /// The project-relative path of the source file — `models/chair.obj`.
    pub source: String,
    /// Import options by name, as the importer's own schema spells them: `lod-count`, `scale`,
    /// `collision-mode`. An option the importer does not declare is a refusal naming it, because
    /// silently ignoring one is a setting a person can change with no effect.
    pub options: BTreeMap<String, String>,
    /// Ignore the cache and re-cook. For diagnosing the cache itself, and for a person who has just
    /// changed something the key cannot see.
    pub force: bool,
}

/// One sub-asset an import produced, and the identity it holds.
///
/// The name carries its own kind by convention — `mesh/Seat`, `material/Oak`,
/// `collision/Seat_collision`, `prefab` — which is `tools/import`'s convention rather than this
/// crate's invention: `kCollisionSubAssetPrefix` is a prefix precisely because a collider IS a mesh
/// and a separate kind would make every consumer that switches on kind grow a case it does not want.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct ImportedSubAsset {
    /// The stable name within the source.
    pub name: String,
    /// The identity, as text. Stable across re-imports — that is what the `.import` sidecar is for.
    pub id: String,
}

/// What an import did.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct AssetImportOutcome {
    /// The source, project-relative and as the importer echoed it back.
    pub source: String,
    /// Which importer ran: `gltf`, `fbx`, `obj`, `texture`.
    pub importer: String,
    /// The identity the source itself holds — its primary sub-asset's.
    pub id: String,
    /// `hit`, `miss`, `invalidated` or `corrupt`.
    pub cache: String,
    /// Everything it produced, in the order the importer produced it.
    pub sub_assets: Vec<ImportedSubAsset>,
    /// How many diagnostics were warnings.
    pub warnings: usize,
    /// How many were errors. A non-zero count is a refusal rather than an outcome — see
    /// [`AssetHost::import_asset`].
    pub errors: usize,
    /// The model-import steps this format does not reach, named — "7 (import skeletons), 8 (import
    /// animations), 10 (produce a prefab of the hierarchy)". Empty when the importer reaches every
    /// step, and empty for an importer the sequence does not apply to at all.
    ///
    /// **This is not a warning and must not be reported as one.** `asset-import-pipeline`: "A step
    /// skipped for that reason is not a warning about the file and SHALL NOT be reported as one."
    /// An OBJ carries no rig; that is what OBJ is.
    pub steps_not_reached: String,
}

impl AssetImportOutcome {
    /// The first sub-asset whose name begins with `prefix`, which is how a caller asks for "the
    /// mesh" or "the prefab" without a second kind field to keep in step with the names.
    #[must_use]
    pub fn first(&self, prefix: &str) -> Option<&ImportedSubAsset> {
        self.sub_assets
            .iter()
            .find(|asset| asset.name.starts_with(prefix))
    }
}

/// Running the project's importers.
///
/// Named operations over named things, in the manner of [`crate::context::ProjectHost`] and for the
/// same layering reason.
pub trait AssetHost {
    /// The source extensions this build can import, lower-case and with their dot.
    ///
    /// Asked of the importer rather than listed here, so that a project's own importer appears in
    /// the refusal message the day it is registered — `asset-import-pipeline` requires a project's
    /// importer to have "the same weight as a built-in", and a hard-coded list in the editor would
    /// be exactly the second-class treatment that forbids.
    ///
    /// Empty when this build cannot reach an importer at all, which the command reports as such
    /// rather than as "that format is unsupported".
    fn importable_extensions(&mut self) -> Vec<String>;

    /// Import one source.
    ///
    /// # Errors
    ///
    /// When the importer cannot be run, when the source is not in the project, or when the import
    /// itself failed — with the importer's own diagnostics as the reason, because "the import
    /// failed" teaches nobody anything.
    fn import_asset(&mut self, request: &AssetImportRequest) -> Result<AssetImportOutcome>;
}
