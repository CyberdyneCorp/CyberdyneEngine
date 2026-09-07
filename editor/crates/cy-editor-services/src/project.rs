//! The project around the documents: its source tree, its build, and the runtime that runs it.
//! Tasks 3.6, 3.7 and 3.8.
//!
//! --- WHY A SOURCE EDIT IS A TRANSACTION LIKE ANY OTHER ---------------------------------------------
//!
//! `design.md` §4 decides this, and it decides it against the obvious alternative:
//!
//! > An agent creating a `.swift` file in the project is mutating the project, and it gets no
//! > special path. It is a transaction, it carries an actor and an intent, it is refused outside the
//! > connection's scope, and its effect class is declared.
//!
//! A file is not a node graph, so the document that stands for it holds no nodes. What it holds is
//! [`cy_editor_documents::operation::Operation::Domain`] — the operation the document model already
//! has for "a change this crate does not interpret" — carrying the contents before and after. That
//! one decision buys the whole list `editor-agent-interface` asks for without a second mechanism:
//! undo and redo, the journal and therefore crash recovery, attribution, replay, and a history a
//! human's edits interleave with.
//!
//! The **application** of that operation is this module's, because writing a file is not something
//! `cy-editor-documents` can do — it is layer 1 and owns no I/O. [`rewind`] and [`replay`] are what
//! `edit.undo` and `edit.redo` call, and they are the reason a source edit undoes rather than merely
//! being recorded as though it had.
//!
//! --- WHY THE EFFECT CLASS IS COMPUTED ---------------------------------------------------------------
//!
//! > source creation and edit are **reversible-mutation** where the editor holds the prior contents
//! > in its journal and can restore them, and **irreversible-mutation** where it cannot —
//! > overwriting a file the editor never read, for instance.
//!
//! [`ProjectService::source_is_restorable`] is that test, and it is answered by trying: a file that
//! does not exist is restorable (undo deletes it), a file that reads back as text is restorable
//! (undo writes it back), and a file that exists and cannot be read as text is not. The command
//! declares the worst case and computes the real one — see `crate::authoring`.
//!
//! --- WHY THE BUILD DOES NOT BLOCK -------------------------------------------------------------------
//!
//! Because "the editor stays usable while an agent works" is a requirement and a Swift module takes
//! tens of seconds. [`ProjectService::start_build`] hands the work to
//! [`crate::operations::OperationService`], which is where every other long operation goes, and the
//! caller watches it where every other one is watched. An agent polls the `operations:` resource; a
//! person watches the same progress in the same panel.

use std::path::{Component, Path, PathBuf};
use std::sync::{Arc, Mutex};

use cy_editor_core::problem::{Problem, Result};
use cy_editor_documents::Document;
use cy_editor_documents::operation::Operation;
use cy_editor_documents::transaction::Transaction;

/// The `kind` a source edit's domain operation carries.
///
/// A constant rather than a literal because two places have to agree about it — the command that
/// records the operation and the undo that applies it — and a typo in either is a source edit that
/// silently stops undoing.
pub const SOURCE_DOMAIN: &str = "source";

/// Encode a file's contents for a domain operation's payload.
///
/// One tag byte, because "the file did not exist" and "the file was empty" undo to different things
/// and a bare byte string cannot tell them apart.
#[must_use]
pub fn encode_source(contents: Option<&str>) -> Vec<u8> {
    match contents {
        None => vec![0],
        Some(text) => {
            let mut bytes = Vec::with_capacity(text.len() + 1);
            bytes.push(1);
            bytes.extend_from_slice(text.as_bytes());
            bytes
        }
    }
}

/// Read back what [`encode_source`] wrote.
///
/// A payload this build does not understand answers `None`, which is the safe reading: it means "no
/// file", and restoring to no file is what a failed decode of a *creation* should do anyway.
#[must_use]
pub fn decode_source(bytes: &[u8]) -> Option<String> {
    match bytes.split_first() {
        Some((1, rest)) => String::from_utf8(rest.to_vec()).ok(),
        _ => None,
    }
}

/// How to build the project's script module.
///
/// A trait so that a test does not need a Swift toolchain, and so that a project in another language
/// is a different implementation rather than a branch in this file.
pub trait ModuleBuilder: Send + Sync {
    /// What this builder is, for a message a person reads.
    fn describe(&self) -> String;

    /// Build one generation, returning the library it produced.
    ///
    /// # Errors
    ///
    /// When the toolchain is missing or the sources do not compile — with the compiler's own output
    /// as the reason, because "the build failed" teaches nobody anything.
    fn build(&self, request: &BuildRequest) -> Result<PathBuf>;
}

/// One generation's build.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct BuildRequest {
    /// The project's root.
    pub root: PathBuf,
    /// The directory holding the module's sources.
    pub sources: PathBuf,
    /// Which generation. Part of the library's file name, because `dlopen` of a path already open
    /// returns the image already loaded — M4 measured this and `bindings/swift/tools/`
    /// `cy_swift_module.py` is built around it.
    pub generation: u32,
    /// Where intermediate output goes. Shared across generations so the macro plugin is compiled
    /// once.
    pub work: PathBuf,
}

/// Where a build got to.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum BuildState {
    /// Nothing has been built in this session.
    #[default]
    Never,
    /// A build is running.
    Running,
    /// The last build succeeded.
    Succeeded,
    /// The last build failed.
    Failed,
}

impl BuildState {
    /// The word a caller reads.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            BuildState::Never => "never-built",
            BuildState::Running => "running",
            BuildState::Succeeded => "succeeded",
            BuildState::Failed => "failed",
        }
    }
}

/// What the last build did. Shared with the thread running it.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct BuildRecord {
    /// Where it got to.
    pub state: BuildState,
    /// Which generation it was for.
    pub generation: u32,
    /// The library it produced, when it produced one.
    pub library: Option<PathBuf>,
    /// What it said. The compiler's output on a failure.
    pub message: String,
}

/// The project's source tree, its build, and what has been built.
pub struct ProjectService {
    root: PathBuf,
    sources: String,
    module: String,
    builder: Arc<dyn ModuleBuilder>,
    generation: u32,
    record: Arc<Mutex<BuildRecord>>,
}

impl Default for ProjectService {
    /// A project rooted at the working directory, which is what an editor opened with no project
    /// argument is looking at.
    fn default() -> Self {
        Self::new(std::env::current_dir().unwrap_or_else(|_| PathBuf::from(".")))
    }
}

impl ProjectService {
    /// The file a directory carries to say it is a project. `tools/project/project.py` validates it.
    pub const MANIFEST: &'static str = "project.json";

    /// A project at `root`, with `game/` as its script sources and the Swift module builder.
    ///
    /// The builder is the real one by default rather than a stub, because a capability that is off
    /// until somebody configures it is a capability nobody has.
    #[must_use]
    pub fn new(root: impl Into<PathBuf>) -> Self {
        let root = root.into();
        let builder = Arc::new(SwiftModuleBuilder::found_near(&root));
        Self {
            root,
            sources: "game".to_string(),
            module: "game".to_string(),
            builder,
            generation: 0,
            record: Arc::new(Mutex::new(BuildRecord::default())),
        }
    }

    /// Build with something else — a test's recording double, or another language's toolchain.
    #[must_use]
    pub fn with_builder(mut self, builder: Arc<dyn ModuleBuilder>) -> Self {
        self.builder = builder;
        self
    }

    /// Name the module and the directory its sources live in.
    #[must_use]
    pub fn with_module(mut self, module: impl Into<String>, sources: impl Into<String>) -> Self {
        self.module = module.into();
        self.sources = sources.into();
        self
    }

    /// Where the project is.
    #[must_use]
    pub fn root(&self) -> &Path {
        &self.root
    }

    /// The manifest that makes a directory a project.
    #[must_use]
    pub fn manifest(&self) -> PathBuf {
        self.root.join(Self::MANIFEST)
    }

    /// Whether the root **declares itself** a project, rather than merely being where the editor
    /// happened to be started.
    ///
    /// **One rule, in one place: the editor reads and writes worlds in a declared project and
    /// nowhere else.** `ProjectService::default()` roots at the working directory, so without this
    /// an editor launched from a source tree, a home directory or `/tmp` treats that directory as a
    /// project and saves into it — which is not hypothetical: it put a `worlds/` directory in this
    /// repository the first time `file.save` learned to write one.
    ///
    /// The manifest is `tools/project/project.py`'s, and a project that has one is a project the
    /// engine's own validator recognises. A directory that has not said it is a project is one the
    /// editor may open documents in and may not write to.
    #[must_use]
    pub fn is_declared(&self) -> bool {
        Self::declares(&self.root)
    }

    /// The same question about a path, for a caller that has one and no service.
    #[must_use]
    pub fn declares(root: &Path) -> bool {
        root.join(Self::MANIFEST).is_file()
    }

    /// The module a reload names.
    #[must_use]
    pub fn module(&self) -> &str {
        &self.module
    }

    /// What the last build did.
    #[must_use]
    pub fn record(&self) -> BuildRecord {
        self.locked().clone()
    }

    /// Resolve a project-relative path, refusing one that leaves the project.
    ///
    /// **Refused rather than clamped.** `../../etc/passwd` is either a mistake or an attempt, and
    /// both are better answered with the reason than with a path the caller did not ask for.
    pub fn resolve(&self, path: &str) -> Result<PathBuf> {
        let candidate = Path::new(path);
        let escapes = candidate.is_absolute()
            || candidate
                .components()
                .any(|component| matches!(component, Component::ParentDir | Component::Prefix(_)));
        if escapes || path.trim().is_empty() {
            return Err(Problem::new(
                format!("resolve the project file {path:?}"),
                "a project path is relative to the project and may not leave it",
            )
            .with_remedy(format!(
                "name a path inside {} — for example {}/Player.swift",
                self.root.display(),
                self.sources
            )));
        }
        Ok(self.root.join(candidate))
    }

    /// Whether the project holds this file.
    #[must_use]
    pub fn source_exists(&self, path: &str) -> bool {
        self.resolve(path).is_ok_and(|full| full.is_file())
    }

    /// One source file's contents.
    pub fn read_source(&self, path: &str) -> Result<String> {
        let full = self.resolve(path)?;
        std::fs::read_to_string(&full).map_err(|error| {
            Problem::new(format!("read {path}"), error.to_string())
                .with_remedy("list the project's sources to see what there is")
        })
    }

    /// Whether a write to this file could be undone.
    ///
    /// See the module note. Answered by trying rather than by a rule, because the rule would be a
    /// guess about a file system and this is a fact about one.
    #[must_use]
    pub fn source_is_restorable(&self, path: &str) -> bool {
        match self.resolve(path) {
            Err(_) => false,
            Ok(full) if !full.exists() => true,
            Ok(_) => self.read_source(path).is_ok(),
        }
    }

    /// Put a file into a stated state: `Some` writes it, `None` removes it.
    ///
    /// One function rather than a write and a delete, because undo needs exactly this — "the file
    /// was in this state, put it back" — and two functions would need a caller to decide which,
    /// which is where the "the file did not exist" case gets forgotten.
    pub fn put_source(&mut self, path: &str, contents: Option<&str>) -> Result<()> {
        let full = self.resolve(path)?;
        match contents {
            Some(text) => {
                if let Some(parent) = full.parent() {
                    std::fs::create_dir_all(parent).map_err(|error| {
                        Problem::new(format!("create {}", parent.display()), error.to_string())
                    })?;
                }
                std::fs::write(&full, text).map_err(|error| {
                    Problem::new(format!("write {path}"), error.to_string())
                        .with_remedy("check that the project is writable")
                })
            }
            None if full.exists() => std::fs::remove_file(&full).map_err(|error| {
                Problem::new(format!("remove {path}"), error.to_string())
                    .with_remedy("check that the project is writable")
            }),
            None => Ok(()),
        }
    }

    /// Every source file the project holds, project-relative, in path order.
    #[must_use]
    pub fn source_paths(&self) -> Vec<String> {
        let directory = self.root.join(&self.sources);
        let mut found = Vec::new();
        let mut stack = vec![directory];
        while let Some(next) = stack.pop() {
            let Ok(entries) = std::fs::read_dir(&next) else {
                continue;
            };
            for entry in entries.flatten() {
                let path = entry.path();
                if path.is_dir() {
                    stack.push(path);
                } else if let Ok(relative) = path.strip_prefix(&self.root) {
                    found.push(relative.to_string_lossy().into_owned());
                }
            }
        }
        found.sort_unstable();
        found
    }

    /// What the caller should hand to [`crate::operations::OperationService::start`], and the label
    /// to start it under.
    ///
    /// Split from the starting so that this crate's one place for background work stays
    /// `OperationService` — a service that spawned its own thread would be a second answer to "where
    /// does long work go", and the two would eventually disagree about cancellation.
    pub fn next_build(&mut self) -> Result<(String, impl FnOnce() + Send + 'static)> {
        let sources = self.root.join(&self.sources);
        if !sources.is_dir() {
            return Err(Problem::new(
                format!("build {}", self.module),
                format!("the project has no {} directory", self.sources),
            )
            .with_remedy(format!("write a script into {} first", sources.display())));
        }
        self.generation = self.generation.saturating_add(1);
        let request = BuildRequest {
            root: self.root.clone(),
            sources,
            generation: self.generation,
            work: self.root.join("build").join("script-module"),
        };
        let builder = Arc::clone(&self.builder);
        let record = Arc::clone(&self.record);
        let label = format!("Build {} generation {}", self.module, self.generation);
        {
            let mut held = record
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            held.state = BuildState::Running;
            held.generation = request.generation;
            held.message = format!("building generation {}", request.generation);
        }
        Ok((label, move || {
            let outcome = builder.build(&request);
            let mut held = record
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            match outcome {
                Ok(library) => {
                    held.state = BuildState::Succeeded;
                    held.message = format!("built {}", library.display());
                    held.library = Some(library);
                }
                Err(problem) => {
                    held.state = BuildState::Failed;
                    held.message = problem.to_string();
                }
            }
        }))
    }

    /// The library and generation a reload should load, or why there is not one.
    pub fn built(&self) -> Result<(PathBuf, u32)> {
        let record = self.locked().clone();
        match (record.state, record.library) {
            (BuildState::Succeeded, Some(library)) => Ok((library, record.generation)),
            (BuildState::Running, _) => Err(Problem::new(
                "reload the script module",
                "the build is still running",
            )
            .with_remedy("read the operations resource and wait for it to settle")),
            (BuildState::Failed, _) => Err(Problem::new(
                "reload the script module",
                format!("the last build failed: {}", record.message),
            )
            .with_remedy("fix the sources and build again")),
            _ => Err(
                Problem::new("reload the script module", "nothing has been built")
                    .with_remedy("invoke project.build first"),
            ),
        }
    }

    /// One line saying what the build has done and is doing.
    #[must_use]
    pub fn describe_build(&self) -> String {
        let record = self.locked().clone();
        format!(
            "builder: {}\nstate: {}\ngeneration: {}\nlibrary: {}\nmessage: {}\n",
            self.builder.describe(),
            record.state.name(),
            record.generation,
            record
                .library
                .as_ref()
                .map_or_else(|| "none".to_string(), |path| path.display().to_string()),
            record.message
        )
    }

    /// A poisoned lock is recoverable: the record is four fields a panicking builder cannot leave
    /// half-written in a way that matters, and losing the build's outcome is a worse answer than
    /// reading a stale one.
    fn locked(&self) -> std::sync::MutexGuard<'_, BuildRecord> {
        self.record
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
    }
}

/// Put every source file a transaction changed back to what it was before it.
///
/// What `edit.undo` calls. The path comes from the document's primary asset rather than from the
/// operation, because the document *is* the file: one document per source file is what makes a
/// source edit interleave with a scene edit in one history.
pub fn rewind(project: &mut ProjectService, document: &Document, transaction: &Transaction) {
    apply_domain(project, document, transaction, false);
}

/// Put them back to what the transaction made them. What `edit.redo` calls.
pub fn replay(project: &mut ProjectService, document: &Document, transaction: &Transaction) {
    apply_domain(project, document, transaction, true);
}

/// The half [`rewind`] and [`replay`] share.
///
/// **Failures are swallowed deliberately, and this is the one place in the module where that is
/// right.** Undo has already moved the history; refusing here would leave the editor's history and
/// the file system disagreeing with no way to say so, which is worse than a file that did not move.
/// The caller notices the same way a person does — by reading the file.
fn apply_domain(
    project: &mut ProjectService,
    document: &Document,
    transaction: &Transaction,
    forward: bool,
) {
    let Some(path) = document.assets().first().cloned() else {
        return;
    };
    for operation in &transaction.operations {
        let Operation::Domain {
            kind,
            before,
            after,
            ..
        } = operation
        else {
            continue;
        };
        if kind != SOURCE_DOMAIN {
            continue;
        }
        let wanted = decode_source(if forward { after } else { before });
        let _ = project.put_source(&path, wanted.as_deref());
    }
}

/// M4's Swift module build, driven by the script M4 delivered.
///
/// `bindings/swift/tools/cy_swift_module.py` is the proven model and this does not reimplement any
/// of it: it gives every generation a unique `-module-name` and a unique file name, which the M4
/// spike measured as the difference between a reload that works and one where the new image's type
/// lookup finds the old image's metadata.
pub struct SwiftModuleBuilder {
    driver: Option<PathBuf>,
}

impl SwiftModuleBuilder {
    /// Find the driver by walking up from the project, which is where a sample inside this
    /// repository finds it.
    #[must_use]
    pub fn found_near(root: &Path) -> Self {
        let relative = Path::new("bindings")
            .join("swift")
            .join("tools")
            .join("cy_swift_module.py");
        let mut directory = Some(root);
        while let Some(current) = directory {
            let candidate = current.join(&relative);
            if candidate.is_file() {
                return Self {
                    driver: Some(candidate),
                };
            }
            directory = current.parent();
        }
        Self { driver: None }
    }
}

impl ModuleBuilder for SwiftModuleBuilder {
    fn describe(&self) -> String {
        self.driver.as_ref().map_or_else(
            || "no Swift module driver found near this project".to_string(),
            |driver| format!("Swift module, built by {}", driver.display()),
        )
    }

    fn build(&self, request: &BuildRequest) -> Result<PathBuf> {
        let driver = self.driver.as_ref().ok_or_else(|| {
            Problem::new(
                "build the project's script module",
                "no Swift module driver was found above the project",
            )
            .with_remedy(
                "open a project inside a checkout that has bindings/swift/tools/\
                 cy_swift_module.py, or configure another builder",
            )
        })?;
        let out = request.work.join("out");
        let output = std::process::Command::new("python3")
            .arg(driver)
            .arg("--work")
            .arg(&request.work)
            .arg("--generation")
            .arg(format!(
                "{}={}",
                request.generation,
                request.sources.display()
            ))
            .arg("--out")
            .arg(&out)
            .output()
            .map_err(|error| {
                Problem::new("run the Swift module driver", error.to_string())
                    .with_remedy("check that python3 is on PATH")
            })?;
        if !output.status.success() {
            return Err(Problem::new(
                format!("build generation {}", request.generation),
                String::from_utf8_lossy(&output.stderr).trim().to_string(),
            )
            .with_remedy("fix the reported errors and build again"));
        }
        // The driver names each generation's library for the generation, which is the whole point of
        // it — see `bindings/swift/tools/cy_swift_module.py`.
        let library = out.join(format!("libCyGame_g{}.so", request.generation));
        if !library.is_file() {
            return Err(Problem::new(
                format!("build generation {}", request.generation),
                format!("the driver produced no {}", library.display()),
            )
            .with_remedy("run the driver by hand to see what it wrote"));
        }
        Ok(library)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use cy_editor_core::Actor;

    fn project() -> (ProjectService, tempdir::TempDirectory) {
        let directory = tempdir::TempDirectory::new("cy-project");
        let service = ProjectService::new(directory.path());
        (service, directory)
    }

    #[test]
    fn a_path_that_leaves_the_project_is_refused_with_what_would_have_worked() {
        let (project, _held) = project();
        let refused = project.resolve("../../etc/passwd").unwrap_err();
        assert!(
            refused.remedy.as_deref().unwrap().contains("game/"),
            "{refused}"
        );
        assert!(project.resolve("game/Player.swift").is_ok());
    }

    #[test]
    fn a_file_that_does_not_exist_yet_is_restorable_because_undo_deletes_it() {
        let (mut project, _held) = project();
        assert!(project.source_is_restorable("game/Player.swift"));
        project
            .put_source("game/Player.swift", Some("struct Player {}"))
            .unwrap();
        assert!(project.source_is_restorable("game/Player.swift"));
        assert_eq!(
            project.read_source("game/Player.swift").unwrap(),
            "struct Player {}"
        );
    }

    #[test]
    fn a_file_the_editor_cannot_read_as_text_is_not_restorable() {
        // `design.md` §4's own example, made concrete: the editor cannot hold what it cannot read,
        // so overwriting this is the irreversible case rather than the reversible one.
        let (project, held) = project();
        std::fs::create_dir_all(held.path().join("game")).unwrap();
        std::fs::write(held.path().join("game/blob.bin"), [0xff, 0xfe, 0x00]).unwrap();
        assert!(!project.source_is_restorable("game/blob.bin"));
    }

    #[test]
    fn the_absent_file_and_the_empty_file_encode_differently() {
        // They undo to different things, and a payload that could not tell them apart would make
        // undoing the creation of a file leave an empty one behind.
        assert_eq!(decode_source(&encode_source(None)), None);
        assert_eq!(decode_source(&encode_source(Some(""))), Some(String::new()));
        assert_eq!(
            decode_source(&encode_source(Some("hello"))),
            Some("hello".to_string())
        );
    }

    #[test]
    fn undo_puts_a_source_file_back_and_redo_puts_it_forward_again() {
        let (mut project, held) = project();
        project
            .put_source("game/Player.swift", Some("one"))
            .unwrap();

        let mut document = Document::new("game/Player.swift");
        document
            .with_transaction("Write game/Player.swift", Actor::human("designer"), |doc| {
                doc.record(Operation::Domain {
                    node: None,
                    kind: SOURCE_DOMAIN.to_string(),
                    before: encode_source(Some("one")),
                    after: encode_source(Some("two")),
                })
            })
            .unwrap();
        project
            .put_source("game/Player.swift", Some("two"))
            .unwrap();

        let undone = document.undo().unwrap().unwrap();
        rewind(&mut project, &document, &undone);
        assert_eq!(project.read_source("game/Player.swift").unwrap(), "one");

        let redone = document.redo().unwrap().unwrap();
        replay(&mut project, &document, &redone);
        assert_eq!(project.read_source("game/Player.swift").unwrap(), "two");
        drop(held);
    }

    #[test]
    fn a_build_with_no_sources_is_refused_rather_than_started() {
        let (mut project, _held) = project();
        let Err(refused) = project.next_build() else {
            panic!("a project with no sources cannot build")
        };
        assert!(refused.because.contains("no game directory"), "{refused}");
        assert!(matches!(project.record().state, BuildState::Never));
    }

    #[test]
    fn a_build_records_what_it_produced_and_a_reload_can_then_find_it() {
        struct Fake;
        impl ModuleBuilder for Fake {
            fn describe(&self) -> String {
                "a test double".to_string()
            }
            fn build(&self, request: &BuildRequest) -> Result<PathBuf> {
                Ok(request
                    .work
                    .join(format!("libCyGame_g{}.so", request.generation)))
            }
        }

        let (mut project, held) = project();
        std::fs::create_dir_all(held.path().join("game")).unwrap();
        project = project.with_builder(Arc::new(Fake));

        assert!(project.built().is_err(), "nothing is built yet");
        let Ok((label, work)) = project.next_build() else {
            panic!("a project with sources queues a build")
        };
        assert!(label.contains("generation 1"), "{label}");
        assert_eq!(project.record().state, BuildState::Running);
        work();
        let (library, generation) = project.built().unwrap();
        assert_eq!(generation, 1);
        assert!(library.to_string_lossy().contains("_g1."), "{library:?}");

        // The generation moves, because `dlopen` of a path already open returns the same image.
        let Ok((_label, work)) = project.next_build() else {
            panic!("a project with sources queues a build")
        };
        work();
        assert_eq!(project.built().unwrap().1, 2);
    }

    #[test]
    fn a_failed_build_says_what_the_compiler_said_rather_than_that_it_failed() {
        struct Angry;
        impl ModuleBuilder for Angry {
            fn describe(&self) -> String {
                "a test double that refuses".to_string()
            }
            fn build(&self, _request: &BuildRequest) -> Result<PathBuf> {
                Err(Problem::new(
                    "build",
                    "Player.swift:3:1: error: cannot find 'Vecter' in scope",
                ))
            }
        }
        let (mut project, held) = project();
        std::fs::create_dir_all(held.path().join("game")).unwrap();
        project = project.with_builder(Arc::new(Angry));
        let Ok((_label, work)) = project.next_build() else {
            panic!("a project with sources queues a build")
        };
        work();
        let refused = project.built().unwrap_err();
        assert!(
            refused.because.contains("cannot find 'Vecter'"),
            "{refused}"
        );
    }

    /// A directory that removes itself, so these tests need no fixture crate.
    mod tempdir {
        use std::path::{Path, PathBuf};

        pub(super) struct TempDirectory(PathBuf);

        impl TempDirectory {
            pub(super) fn new(prefix: &str) -> Self {
                let unique = std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .map_or(0, |elapsed| elapsed.as_nanos());
                let path =
                    std::env::temp_dir().join(format!("{prefix}-{}-{unique}", std::process::id()));
                std::fs::create_dir_all(&path).expect("a writable temporary directory");
                Self(path)
            }

            pub(super) fn path(&self) -> &Path {
                &self.0
            }
        }

        impl Drop for TempDirectory {
            fn drop(&mut self) {
                let _ = std::fs::remove_dir_all(&self.0);
            }
        }
    }
}
