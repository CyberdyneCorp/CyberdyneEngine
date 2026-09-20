//! Source control, behind a provider interface. `editor-documents-and-transactions`, M11.b task 4.2.
//!
//! --- WHAT THIS REPLACES --------------------------------------------------------------------------
//!
//! Nothing, and that is the finding M10's audit recorded: `grep -niE 'source.control'
//! editor/crates/*/src/` returned seven hits and **every one of them was a comment or a remedy
//! string** — "restore it from source control", "check it out first". The editor told people to use
//! source control and could not talk to any.
//!
//! --- WHY THE NULL PROVIDER IS NOT A PLACEHOLDER --------------------------------------------------
//!
//! The requirement names three implementations — Git, Perforce and a null provider — and the null
//! one is what makes the other two *optional*. A project in no repository at all is a project the
//! editor has to open, and the alternative to a provider that answers "not versioned" for every
//! path is an `Option<Provider>` threaded through every caller, where the unversioned case is a
//! branch each of them gets to forget.
//!
//! --- THE RULE THE SCENARIOS TURN ON --------------------------------------------------------------
//!
//! *"Features a provider does not support SHALL be reported as unavailable rather than emulated
//! incorrectly"*, and its scenario: *"WHEN a provider has no exclusive locking THEN locking SHALL be
//! reported as unavailable rather than silently doing nothing"*. Git is exactly that provider. So
//! [`Capabilities`] is declared per provider and every optional operation checks it **first**, and
//! returns a [`Problem`] naming the provider and the operation. A silent `Ok(())` here would be a
//! lock a person believed they held.
//!
//! --- WHY THE PROVIDERS RUN A COMMAND AND WHY THAT IS TESTABLE ------------------------------------
//!
//! The editor workspace carries no third-party dependencies, so there is no `git2` and no p4 API to
//! bind. Both real providers therefore drive the vendor's own command-line client, which is also
//! what makes them replaceable: [`CommandRunner`] is the seam, a test substitutes a recorded
//! transcript, and `tests::` below drives the whole of both providers without either binary being
//! installed. [`tests::git_reads_a_real_repository`] then runs against a real `git` when one is on
//! the path, because a transcript proves the parser and not the invocation.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::Command as ProcessCommand;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use cy_editor_commands::{Command, EffectClass, Metadata, Outcome, ParameterSpec, Registry};
use cy_editor_core::observe::Revision as ObserveRevision;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};

/// Register provider-neutral source-control commands.
pub fn register_commands(registry: &mut Registry) -> Result<()> {
    registry.register(Command::new(
        Metadata::new(
            "source-control.refresh",
            "Refresh Source Control",
            "Source Control",
            "Starts a background status refresh without blocking the editor interface.",
            EffectClass::Read,
        ),
        |context, _| {
            let summary = context
                .source_control()
                .ok_or_else(|| Problem::not_found("source control in this host"))?
                .refresh()?;
            Ok(Outcome::new(summary))
        },
    ))?;
    registry.register(path_command(
        "source-control.history",
        "Show File History",
        EffectClass::Read,
        "history",
    ))?;
    registry.register(path_command(
        "source-control.checkout",
        "Check Out File",
        EffectClass::IrreversibleMutation,
        "checkout",
    ))?;
    registry.register(path_command(
        "source-control.revert",
        "Revert File",
        EffectClass::IrreversibleMutation,
        "revert",
    ))?;
    registry.register(path_command(
        "source-control.lock",
        "Lock File",
        EffectClass::ExternalEffect,
        "lock",
    ))?;
    registry.register(path_command(
        "source-control.unlock",
        "Unlock File",
        EffectClass::ExternalEffect,
        "unlock",
    ))?;
    registry.register(submit_command())?;
    Ok(())
}

fn path_command(
    id: &'static str,
    label: &'static str,
    effect: EffectClass,
    operation: &'static str,
) -> Command {
    Command::new(
        Metadata::new(
            id,
            label,
            "Source Control",
            format!(
                "Runs provider-neutral {operation} for one project-relative file and reports \
                 unsupported capabilities by provider name."
            ),
            effect,
        )
        .with(ParameterSpec::required(
            "path",
            ValueKind::Text,
            "The project-relative file the provider operation acts on.",
        )),
        move |context, arguments| {
            let path = arguments.text("path").unwrap_or_default();
            let host = context
                .source_control()
                .ok_or_else(|| Problem::not_found("source control in this host"))?;
            let summary = if operation == "history" {
                host.history(path)?
            } else {
                host.operate(operation, path, "")?
            };
            Ok(Outcome::new(summary))
        },
    )
}

fn submit_command() -> Command {
    Command::new(
        Metadata::new(
            "source-control.submit",
            "Submit Files",
            "Source Control",
            "Submits one project-relative file with the supplied change description.",
            EffectClass::ExternalEffect,
        )
        .with(ParameterSpec::required(
            "path",
            ValueKind::Text,
            "The project-relative file to include in the submitted change.",
        ))
        .with(ParameterSpec::required(
            "description",
            ValueKind::Text,
            "The change description recorded by the source-control provider.",
        )),
        |context, arguments| {
            let path = arguments.text("path").unwrap_or_default();
            let description = arguments.text("description").unwrap_or_default();
            let summary = context
                .source_control()
                .ok_or_else(|| Problem::not_found("source control in this host"))?
                .operate("submit", path, description)?;
            Ok(Outcome::new(summary).with("path", Value::Text(path.to_string())))
        },
    )
}

/// One thing a provider can do.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub enum Capability {
    /// Per-file status is available.
    Status,
    /// A file's revisions can be listed.
    History,
    /// A revision's content can be produced, so a diff has two sides.
    Diff,
    /// Files are checked out before editing. Absent where every file is writable.
    CheckOut,
    /// Local changes can be discarded.
    Revert,
    /// Changes can be submitted.
    Submit,
    /// **Exclusive** locking, which is the capability the requirement's scenario is about. A
    /// provider without it must say so rather than accept a lock nobody holds.
    ExclusiveLock,
}

impl Capability {
    /// The word a diagnostic uses for it. The same word the refusal names.
    #[must_use]
    pub const fn spelling(self) -> &'static str {
        match self {
            Capability::Status => "status",
            Capability::History => "history",
            Capability::Diff => "reading a past revision",
            Capability::CheckOut => "check out",
            Capability::Revert => "revert",
            Capability::Submit => "submit",
            Capability::ExclusiveLock => "exclusive locking",
        }
    }

    const fn bit(self) -> u32 {
        1 << (self as u32)
    }
}

/// What a provider can do. **Declared rather than discovered**, so a caller can grey out a control
/// instead of offering one that fails.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct Capabilities {
    bits: u32,
}

impl Capabilities {
    /// The set holding exactly `capabilities`.
    #[must_use]
    pub fn of(capabilities: &[Capability]) -> Self {
        Self {
            bits: capabilities
                .iter()
                .fold(0, |bits, capability| bits | capability.bit()),
        }
    }

    /// Whether the provider declared `capability`.
    #[must_use]
    pub const fn has(self, capability: Capability) -> bool {
        (self.bits & capability.bit()) != 0
    }
}

/// Where a path stands with the provider.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum FileState {
    /// Tracked and unchanged.
    Clean,
    /// Tracked and changed locally.
    Modified,
    /// Added locally and not yet submitted.
    Added,
    /// Deleted locally.
    Deleted,
    /// Known to the provider under a different path.
    Renamed,
    /// Not under source control at all. Every path the null provider is asked about.
    Unversioned,
    /// Changed on both sides; the merge is the caller's problem and this crate will not guess.
    Conflicted,
}

/// One path's state.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct FileStatus {
    /// The path, as the provider spells it: relative to the working root.
    pub path: PathBuf,
    /// Where it stands.
    pub state: FileState,
    /// Who holds an exclusive lock on it, where the provider has locking and one is held.
    pub locked_by: Option<String>,
}

/// One revision of one path.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Revision {
    /// The provider's own identifier: a hash, a changelist number. Opaque to the editor.
    pub id: String,
    /// Who made it.
    pub author: String,
    /// What they said about it.
    pub description: String,
}

/// A provider's answer to `diff`, as the provider produced it.
///
/// **Text, and deliberately not a parsed hunk list.** `editor-documents-and-transactions` requires
/// that "diff and history SHALL use the semantic diff above where the provider supplies revisions"
/// — so the semantic diff is what a caller shows for an authored document, and this carries the
/// revision's own bytes so that the semantic differ has two documents to compare. A hunk parser
/// here would be a third diff representation nobody asked for.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct FileRevisionContent {
    /// The path this content is of.
    pub path: PathBuf,
    /// The revision it was taken at.
    pub revision: String,
    /// The bytes at that revision.
    pub content: Vec<u8>,
}

/// Source control, as the editor sees it.
///
/// **No method here names Git or Perforce**, which is the requirement's "editor code SHALL NOT embed
/// the semantics of any one provider" as a property of the trait rather than a convention. The one
/// place a provider's identity appears is [`SourceControlProvider::name`], which exists so a
/// diagnostic can say which provider refused.
pub trait SourceControlProvider: Send + Sync {
    /// The provider's name, for a diagnostic and for the status bar. Never a version string.
    fn name(&self) -> &str;

    /// What this provider can do.
    fn capabilities(&self) -> Capabilities;

    /// Where each of `paths` stands.
    fn status(&self, paths: &[PathBuf]) -> Result<Vec<FileStatus>>;

    /// The revisions of one path, newest first.
    fn history(&self, path: &Path) -> Result<Vec<Revision>>;

    /// One path's content at one revision.
    fn content_at(&self, path: &Path, revision: &str) -> Result<FileRevisionContent>;

    /// Make `paths` editable. A no-op is not an acceptable implementation; a provider where files
    /// are always writable declares `check_out: false` and this refuses.
    fn check_out(&self, paths: &[PathBuf]) -> Result<()>;

    /// Discard local changes to `paths`.
    fn revert(&self, paths: &[PathBuf]) -> Result<()>;

    /// Submit `paths` with `description`, answering the new revision's identifier.
    fn submit(&self, paths: &[PathBuf], description: &str) -> Result<String>;

    /// Take an exclusive lock on `paths`.
    fn lock(&self, paths: &[PathBuf]) -> Result<()>;

    /// Release an exclusive lock on `paths`.
    fn unlock(&self, paths: &[PathBuf]) -> Result<()>;
}

/// The refusal every unsupported operation returns.
///
/// One function rather than a string per site, so that "reported as unavailable" is one shape a
/// caller can match on and not seven near-identical sentences.
#[must_use]
pub fn unsupported(provider: &str, operation: &str) -> Problem {
    Problem::new(
        format!("{operation} through {provider}"),
        format!("{provider} has no {operation}"),
    )
    .with_remedy(format!(
        "use a provider that supports {operation}, or do it outside the editor"
    ))
}

// --- the null provider ----------------------------------------------------------------------------

/// A project in no repository. Every path is [`FileState::Unversioned`] and every operation refuses.
///
/// It is the default, and it is what makes the other two optional: without it every caller would
/// carry an `Option<Provider>` and the unversioned case would be a branch each of them could forget.
#[derive(Clone, Copy, Debug, Default)]
pub struct NullSourceControl;

impl SourceControlProvider for NullSourceControl {
    fn name(&self) -> &'static str {
        "no source control"
    }

    fn capabilities(&self) -> Capabilities {
        // Status is present because "unversioned" is a real answer; everything else is absent.
        Capabilities::of(&[Capability::Status])
    }

    fn status(&self, paths: &[PathBuf]) -> Result<Vec<FileStatus>> {
        Ok(paths
            .iter()
            .map(|path| FileStatus {
                path: path.clone(),
                state: FileState::Unversioned,
                locked_by: None,
            })
            .collect())
    }

    fn history(&self, _path: &Path) -> Result<Vec<Revision>> {
        Err(unsupported(self.name(), "history"))
    }

    fn content_at(&self, _path: &Path, _revision: &str) -> Result<FileRevisionContent> {
        Err(unsupported(self.name(), "reading a past revision"))
    }

    fn check_out(&self, _paths: &[PathBuf]) -> Result<()> {
        Err(unsupported(self.name(), "check out"))
    }

    fn revert(&self, _paths: &[PathBuf]) -> Result<()> {
        Err(unsupported(self.name(), "revert"))
    }

    fn submit(&self, _paths: &[PathBuf], _description: &str) -> Result<String> {
        Err(unsupported(self.name(), "submit"))
    }

    fn lock(&self, _paths: &[PathBuf]) -> Result<()> {
        Err(unsupported(self.name(), "exclusive locking"))
    }

    fn unlock(&self, _paths: &[PathBuf]) -> Result<()> {
        Err(unsupported(self.name(), "exclusive locking"))
    }
}

// --- the seam the two real providers run through ---------------------------------------------------

/// What a provider ran, and what it got back.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct CommandOutput {
    /// The process's exit status, or `None` when it could not be started at all.
    pub status: Option<i32>,
    /// Its standard output.
    pub stdout: Vec<u8>,
    /// Its standard error, which is what a refusal's `because` is built from.
    pub stderr: String,
}

impl CommandOutput {
    /// Whether the process exited reporting success.
    #[must_use]
    pub fn succeeded(&self) -> bool {
        self.status == Some(0)
    }
}

/// How a provider runs its vendor's client.
///
/// The seam exists so that the providers are testable without the binaries, and it is a trait
/// rather than a function pointer so a test can record what was asked of it — half of what is worth
/// checking about a provider is the argument list it built.
pub trait CommandRunner: Send + Sync {
    /// Run `program` with `arguments` in `working_directory`.
    fn run(&self, program: &str, arguments: &[String], working_directory: &Path) -> CommandOutput;
}

/// Runs the real thing.
#[derive(Clone, Copy, Debug, Default)]
pub struct ProcessRunner;

impl CommandRunner for ProcessRunner {
    fn run(&self, program: &str, arguments: &[String], working_directory: &Path) -> CommandOutput {
        match ProcessCommand::new(program)
            .args(arguments)
            .current_dir(working_directory)
            .output()
        {
            Ok(output) => CommandOutput {
                status: output.status.code(),
                stdout: output.stdout,
                stderr: String::from_utf8_lossy(&output.stderr).into_owned(),
            },
            // A client that is not installed is not an error condition of the operation, it is the
            // provider being unusable — reported as a failed run so the caller sees the name of the
            // program it could not start rather than "unknown error".
            Err(error) => CommandOutput {
                status: None,
                stdout: Vec::new(),
                stderr: format!("{program} could not be started: {error}"),
            },
        }
    }
}

/// The failure a client's non-zero exit becomes.
fn failed(provider: &str, operation: &str, output: &CommandOutput) -> Problem {
    let because = if output.stderr.trim().is_empty() {
        match output.status {
            Some(code) => format!("the client exited with status {code}"),
            None => "the client could not be started".to_string(),
        }
    } else {
        output.stderr.trim().to_string()
    };
    Problem::new(format!("{operation} through {provider}"), because)
        .with_remedy("run the same operation in the client to see the whole message")
}

// --- Git ------------------------------------------------------------------------------------------

/// Git, through `git`.
///
/// **It declares no exclusive locking**, and that is not an omission: Git has none. It is therefore
/// the provider the requirement's second scenario is written about, and [`GitSourceControl::lock`]
/// is the call that has to refuse rather than return `Ok(())`.
pub struct GitSourceControl {
    root: PathBuf,
    runner: Box<dyn CommandRunner>,
}

impl GitSourceControl {
    /// A provider over the working tree at `root`, running the real `git`.
    #[must_use]
    pub fn new(root: impl Into<PathBuf>) -> Self {
        Self::with_runner(root, Box::new(ProcessRunner))
    }

    /// The same, over a substituted runner. What the tests drive.
    #[must_use]
    pub fn with_runner(root: impl Into<PathBuf>, runner: Box<dyn CommandRunner>) -> Self {
        Self {
            root: root.into(),
            runner,
        }
    }

    fn run(&self, operation: &str, arguments: &[&str]) -> Result<CommandOutput> {
        let owned: Vec<String> = arguments.iter().map(|word| (*word).to_string()).collect();
        let output = self.runner.run("git", &owned, &self.root);
        if output.succeeded() {
            Ok(output)
        } else {
            Err(failed(self.name(), operation, &output))
        }
    }
}

/// One `git status --porcelain=v1` code pair, as a state.
///
/// The index column and the working-tree column, read together. `??` is untracked; `U` on either
/// side, and the `AA`/`DD` pairs, are the conflict cases Git spells that way.
fn git_state(index: u8, worktree: u8) -> FileState {
    match (index, worktree) {
        (b'?', b'?') => FileState::Unversioned,
        (b'U', _) | (_, b'U') | (b'A', b'A') | (b'D', b'D') => FileState::Conflicted,
        (b'R', _) => FileState::Renamed,
        (b'A', _) => FileState::Added,
        (b'D', _) | (_, b'D') => FileState::Deleted,
        (b' ', b' ') => FileState::Clean,
        _ => FileState::Modified,
    }
}

/// `git status --porcelain=v1 -z`, parsed.
///
/// `-z` rather than the newline form: a path with a space or a quote in it is quoted and escaped by
/// the newline form, and an authored asset path is exactly where one of those turns up.
fn parse_git_status(stdout: &[u8]) -> BTreeMap<PathBuf, FileState> {
    let mut states = BTreeMap::new();
    let mut fields = stdout.split(|byte| *byte == 0);
    while let Some(record) = fields.next() {
        if record.len() < 4 {
            continue;
        }
        let state = git_state(record[0], record[1]);
        let path = String::from_utf8_lossy(&record[3..]).into_owned();
        if state == FileState::Renamed {
            // A rename is two records: the new path, then the old one. The old path is consumed
            // here so it is not read as a status line of its own.
            let _ = fields.next();
        }
        states.insert(PathBuf::from(path), state);
    }
    states
}

impl SourceControlProvider for GitSourceControl {
    fn name(&self) -> &'static str {
        "Git"
    }

    fn capabilities(&self) -> Capabilities {
        // NEITHER `CheckOut` NOR `ExclusiveLock`, and both absences are Git's rather than this
        // module's. Git files are always writable, so a check out has nothing to do, and Git has no
        // exclusive locking at all. Declared absent rather than implemented as a no-op: a no-op is
        // the "emulated incorrectly" the requirement forbids, and a caller that greys the control
        // out is telling the truth.
        Capabilities::of(&[
            Capability::Status,
            Capability::History,
            Capability::Diff,
            Capability::Revert,
            Capability::Submit,
        ])
    }

    fn status(&self, paths: &[PathBuf]) -> Result<Vec<FileStatus>> {
        let output = self.run("status", &["status", "--porcelain=v1", "-z"])?;
        let states = parse_git_status(&output.stdout);
        Ok(paths
            .iter()
            .map(|path| FileStatus {
                path: path.clone(),
                // A tracked, unmodified path is absent from the porcelain output entirely, so
                // absence is `Clean` — which is why this maps over the paths asked about rather
                // than over what the command printed.
                state: states.get(path).copied().unwrap_or(FileState::Clean),
                locked_by: None,
            })
            .collect())
    }

    fn history(&self, path: &Path) -> Result<Vec<Revision>> {
        let path = path.to_string_lossy().into_owned();
        let output = self.run("history", &["log", "--format=%H%x1f%an%x1f%s", "--", &path])?;
        Ok(String::from_utf8_lossy(&output.stdout)
            .lines()
            .filter_map(|line| {
                let mut fields = line.split('\u{1f}');
                Some(Revision {
                    id: fields.next()?.to_string(),
                    author: fields.next().unwrap_or_default().to_string(),
                    description: fields.next().unwrap_or_default().to_string(),
                })
            })
            .collect())
    }

    fn content_at(&self, path: &Path, revision: &str) -> Result<FileRevisionContent> {
        let spelled = format!("{revision}:{}", path.to_string_lossy());
        let output = self.run("reading a past revision", &["show", &spelled])?;
        Ok(FileRevisionContent {
            path: path.to_path_buf(),
            revision: revision.to_string(),
            content: output.stdout,
        })
    }

    fn check_out(&self, _paths: &[PathBuf]) -> Result<()> {
        Err(unsupported(self.name(), "check out"))
    }

    fn revert(&self, paths: &[PathBuf]) -> Result<()> {
        let mut arguments = vec!["checkout".to_string(), "--".to_string()];
        arguments.extend(paths.iter().map(|path| path.to_string_lossy().into_owned()));
        let borrowed: Vec<&str> = arguments.iter().map(String::as_str).collect();
        self.run("revert", &borrowed).map(|_| ())
    }

    fn submit(&self, paths: &[PathBuf], description: &str) -> Result<String> {
        let mut staged = vec!["add".to_string(), "--".to_string()];
        staged.extend(paths.iter().map(|path| path.to_string_lossy().into_owned()));
        let borrowed: Vec<&str> = staged.iter().map(String::as_str).collect();
        self.run("submit", &borrowed)?;
        self.run("submit", &["commit", "-m", description])?;
        let head = self.run("submit", &["rev-parse", "HEAD"])?;
        Ok(String::from_utf8_lossy(&head.stdout).trim().to_string())
    }

    fn lock(&self, _paths: &[PathBuf]) -> Result<()> {
        // THE SCENARIO, AS ONE LINE. "WHEN a provider has no exclusive locking THEN locking SHALL
        // be reported as unavailable rather than silently doing nothing." Returning `Ok(())` here
        // would be a lock a person believed they held while somebody else edited the file.
        Err(unsupported(self.name(), "exclusive locking"))
    }

    fn unlock(&self, _paths: &[PathBuf]) -> Result<()> {
        Err(unsupported(self.name(), "exclusive locking"))
    }
}

// --- Perforce ---------------------------------------------------------------------------------------

/// Perforce, through `p4`.
///
/// The provider with the capabilities Git has not — check out and exclusive locking — which is why
/// the requirement names both: a provider interface with one implementation behind it is an
/// abstraction nobody has tested, and the two operations that differ are exactly the two the
/// interface has to carry.
pub struct PerforceSourceControl {
    root: PathBuf,
    runner: Box<dyn CommandRunner>,
}

impl PerforceSourceControl {
    /// A provider over the client workspace rooted at `root`, running the real `p4`.
    #[must_use]
    pub fn new(root: impl Into<PathBuf>) -> Self {
        Self::with_runner(root, Box::new(ProcessRunner))
    }

    /// The same, over a substituted runner.
    #[must_use]
    pub fn with_runner(root: impl Into<PathBuf>, runner: Box<dyn CommandRunner>) -> Self {
        Self {
            root: root.into(),
            runner,
        }
    }

    fn run(&self, operation: &str, arguments: &[String]) -> Result<CommandOutput> {
        let output = self.runner.run("p4", arguments, &self.root);
        if output.succeeded() {
            Ok(output)
        } else {
            Err(failed(self.name(), operation, &output))
        }
    }

    fn with_paths(verb: &[&str], paths: &[PathBuf]) -> Vec<String> {
        let mut arguments: Vec<String> = verb.iter().map(|word| (*word).to_string()).collect();
        arguments.extend(paths.iter().map(|path| path.to_string_lossy().into_owned()));
        arguments
    }
}

/// One `p4 -ztag fstat` field block, as a state.
fn perforce_state(action: Option<&str>, head_action: Option<&str>) -> FileState {
    match action {
        Some("add") => FileState::Added,
        Some("delete") => FileState::Deleted,
        Some("move/add") => FileState::Renamed,
        Some("edit" | "integrate") => FileState::Modified,
        // No open action: the file's own head action says whether the depot knows it at all.
        None if head_action.is_some() => FileState::Clean,
        _ => FileState::Unversioned,
    }
}

impl SourceControlProvider for PerforceSourceControl {
    fn name(&self) -> &'static str {
        "Perforce"
    }

    fn capabilities(&self) -> Capabilities {
        Capabilities::of(&[
            Capability::Status,
            Capability::History,
            Capability::Diff,
            Capability::CheckOut,
            Capability::Revert,
            Capability::Submit,
            Capability::ExclusiveLock,
        ])
    }

    fn status(&self, paths: &[PathBuf]) -> Result<Vec<FileStatus>> {
        let mut answers = Vec::with_capacity(paths.len());
        for path in paths {
            let arguments = Self::with_paths(&["-ztag", "fstat"], std::slice::from_ref(path));
            // A path the depot has never heard of makes `fstat` exit non-zero, which is an answer
            // rather than a failure: it is exactly `Unversioned`.
            let Ok(output) = self.run("status", &arguments) else {
                answers.push(FileStatus {
                    path: path.clone(),
                    state: FileState::Unversioned,
                    locked_by: None,
                });
                continue;
            };
            let text = String::from_utf8_lossy(&output.stdout);
            let field = |name: &str| -> Option<String> {
                text.lines()
                    .find_map(|line| line.strip_prefix(&format!("... {name} ")))
                    .map(str::to_string)
            };
            answers.push(FileStatus {
                path: path.clone(),
                state: perforce_state(field("action").as_deref(), field("headAction").as_deref()),
                locked_by: field("ourLock").map(|_| "you".to_string()).or_else(|| {
                    field("otherLock0").or_else(|| field("otherOpen0").filter(|_| false))
                }),
            });
        }
        Ok(answers)
    }

    fn history(&self, path: &Path) -> Result<Vec<Revision>> {
        let arguments = Self::with_paths(&["-ztag", "filelog", "-s"], &[path.to_path_buf()]);
        let output = self.run("history", &arguments)?;
        let text = String::from_utf8_lossy(&output.stdout);
        let mut revisions: Vec<Revision> = Vec::new();
        for line in text.lines() {
            if let Some(change) = line.strip_prefix("... change") {
                let id = change.split_whitespace().next().unwrap_or_default();
                revisions.push(Revision {
                    id: id.to_string(),
                    author: String::new(),
                    description: String::new(),
                });
            } else if let (Some(user), Some(last)) =
                (line.strip_prefix("... user"), revisions.last_mut())
            {
                last.author = user.trim().to_string();
            } else if let (Some(description), Some(last)) =
                (line.strip_prefix("... desc"), revisions.last_mut())
            {
                last.description = description.trim().to_string();
            }
        }
        Ok(revisions)
    }

    fn content_at(&self, path: &Path, revision: &str) -> Result<FileRevisionContent> {
        let spelled = format!("{}@{revision}", path.to_string_lossy());
        let output = self.run(
            "reading a past revision",
            &["print".to_string(), "-q".to_string(), spelled],
        )?;
        Ok(FileRevisionContent {
            path: path.to_path_buf(),
            revision: revision.to_string(),
            content: output.stdout,
        })
    }

    fn check_out(&self, paths: &[PathBuf]) -> Result<()> {
        self.run("check out", &Self::with_paths(&["edit"], paths))
            .map(|_| ())
    }

    fn revert(&self, paths: &[PathBuf]) -> Result<()> {
        self.run("revert", &Self::with_paths(&["revert"], paths))
            .map(|_| ())
    }

    fn submit(&self, paths: &[PathBuf], description: &str) -> Result<String> {
        let mut arguments = vec![
            "submit".to_string(),
            "-d".to_string(),
            description.to_string(),
        ];
        arguments.extend(paths.iter().map(|path| path.to_string_lossy().into_owned()));
        let output = self.run("submit", &arguments)?;
        let text = String::from_utf8_lossy(&output.stdout);
        // "Change 42 submitted." — the number is the revision identifier a caller records.
        let id = text
            .split_whitespace()
            .skip_while(|word| *word != "Change")
            .nth(1)
            .unwrap_or_default();
        Ok(id.to_string())
    }

    fn lock(&self, paths: &[PathBuf]) -> Result<()> {
        self.run("exclusive locking", &Self::with_paths(&["lock"], paths))
            .map(|_| ())
    }

    fn unlock(&self, paths: &[PathBuf]) -> Result<()> {
        self.run("exclusive locking", &Self::with_paths(&["unlock"], paths))
            .map(|_| ())
    }
}

// --- the service ------------------------------------------------------------------------------------

/// The editor's one source-control handle: whichever provider the project is under.
///
/// A service rather than a bare `Box<dyn SourceControlProvider>` so that switching providers is one
/// call and every caller keeps working, which is the requirement's first scenario — *"WHEN a project
/// switches source control systems THEN editor integration SHALL continue to work through the
/// provider interface"*.
pub struct SourceControlService {
    provider: Arc<dyn SourceControlProvider>,
    status: Arc<Mutex<StatusCache>>,
    revision: Arc<AtomicU64>,
}

#[derive(Clone, Debug, Default)]
struct StatusCache {
    requested: Option<Vec<PathBuf>>,
    pending: bool,
    result: Option<Result<Vec<FileStatus>>>,
    generation: u64,
    history: Option<(PathBuf, Result<Vec<Revision>>)>,
}

impl Default for SourceControlService {
    fn default() -> Self {
        Self {
            provider: Arc::new(NullSourceControl),
            status: Arc::default(),
            revision: Arc::default(),
        }
    }
}

impl std::fmt::Debug for SourceControlService {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("SourceControlService")
            .field("provider", &self.provider.name())
            .field(
                "status",
                &self.status.lock().expect("source-control status lock"),
            )
            .field("revision", &self.revision())
            .finish()
    }
}

impl SourceControlService {
    /// A service over `provider`.
    #[must_use]
    pub fn new(provider: Box<dyn SourceControlProvider>) -> Self {
        Self {
            provider: Arc::from(provider),
            status: Arc::default(),
            revision: Arc::default(),
        }
    }

    /// Replace the provider. Every caller keeps working because none of them named the old one.
    pub fn adopt(&mut self, provider: Box<dyn SourceControlProvider>) {
        self.provider = Arc::from(provider);
        *self.status.lock().expect("source-control status lock") = StatusCache::default();
        self.revision.fetch_add(1, Ordering::Release);
    }

    /// The provider in use.
    #[must_use]
    pub fn provider(&self) -> &dyn SourceControlProvider {
        self.provider.as_ref()
    }

    /// Revision of provider identity, pending state, or cached status.
    #[must_use]
    pub fn revision(&self) -> ObserveRevision {
        ObserveRevision::from_u64(self.revision.load(Ordering::Acquire))
    }

    /// Ask for status in the background, once per distinct path set.
    ///
    /// Returns whether a provider process was started. Calling this on every interface frame is
    /// therefore cheap: an unchanged path set starts nothing.
    pub fn request_status(&self, paths: Vec<PathBuf>) -> bool {
        let generation = {
            let mut status = self.status.lock().expect("source-control status lock");
            if status.requested.as_ref() == Some(&paths) {
                return false;
            }
            status.requested = Some(paths.clone());
            status.pending = true;
            status.result = None;
            status.generation += 1;
            status.generation
        };
        self.revision.fetch_add(1, Ordering::Release);
        let provider = Arc::clone(&self.provider);
        let cache = Arc::clone(&self.status);
        let revision = Arc::clone(&self.revision);
        std::thread::Builder::new()
            .name("cy-source-control-status".into())
            .spawn(move || {
                let result = provider.status(&paths);
                let mut status = cache.lock().expect("source-control status lock");
                if status.generation == generation {
                    status.result = Some(result);
                    status.pending = false;
                    revision.fetch_add(1, Ordering::Release);
                }
            })
            .expect("the source-control status worker can be started");
        true
    }

    /// Invalidate the current request so the same paths are checked again.
    pub fn request_refresh(&self, paths: Vec<PathBuf>) {
        self.status
            .lock()
            .expect("source-control status lock")
            .requested = None;
        let _ = self.request_status(paths);
    }

    /// Refresh synchronously for headless callers and deterministic integration tests.
    pub fn refresh_now(&self, paths: Vec<PathBuf>) {
        let result = self.provider.status(&paths);
        let mut status = self.status.lock().expect("source-control status lock");
        status.requested = Some(paths);
        status.pending = false;
        status.result = Some(result);
        status.generation += 1;
        self.revision.fetch_add(1, Ordering::Release);
    }

    /// Cached status, when the background request has completed.
    #[must_use]
    pub fn status_snapshot(&self) -> Option<Result<Vec<FileStatus>>> {
        self.status
            .lock()
            .expect("source-control status lock")
            .result
            .clone()
    }

    /// Whether status is currently being refreshed.
    #[must_use]
    pub fn status_pending(&self) -> bool {
        self.status
            .lock()
            .expect("source-control status lock")
            .pending
    }

    /// Fetch and retain history so the panel can render the command's result.
    pub fn fetch_history(&self, path: &Path) -> Result<Vec<Revision>> {
        let result = self.provider.history(path);
        self.status
            .lock()
            .expect("source-control status lock")
            .history = Some((path.to_path_buf(), result.clone()));
        self.revision.fetch_add(1, Ordering::Release);
        result
    }

    /// The most recently requested file history.
    #[must_use]
    pub fn history_snapshot(&self) -> Option<(PathBuf, Result<Vec<Revision>>)> {
        self.status
            .lock()
            .expect("source-control status lock")
            .history
            .clone()
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Mutex;

    use super::*;

    /// A runner that answers from a table and records what it was asked.
    ///
    /// The record is shared rather than owned, because half of what is worth checking about a
    /// provider is the ARGUMENT LIST it built, and the provider owns the runner once it has it.
    #[derive(Default)]
    struct Transcript {
        answers: Vec<(String, CommandOutput)>,
        asked: std::sync::Arc<Mutex<Vec<String>>>,
    }

    impl Transcript {
        fn answering(answers: &[(&str, &str)]) -> Self {
            Self {
                answers: answers
                    .iter()
                    .map(|(prefix, stdout)| {
                        (
                            (*prefix).to_string(),
                            CommandOutput {
                                status: Some(0),
                                stdout: stdout.as_bytes().to_vec(),
                                stderr: String::new(),
                            },
                        )
                    })
                    .collect(),
                asked: std::sync::Arc::default(),
            }
        }

        /// A handle on the record this runner will write, taken before it is handed away.
        fn log(&self) -> std::sync::Arc<Mutex<Vec<String>>> {
            std::sync::Arc::clone(&self.asked)
        }
    }

    impl CommandRunner for Transcript {
        fn run(&self, program: &str, arguments: &[String], _root: &Path) -> CommandOutput {
            let line = format!("{program} {}", arguments.join(" "));
            self.asked.lock().unwrap().push(line.clone());
            for (prefix, output) in &self.answers {
                if line.starts_with(prefix) {
                    return output.clone();
                }
            }
            CommandOutput {
                status: Some(1),
                stdout: Vec::new(),
                stderr: format!("the transcript has no answer for `{line}`"),
            }
        }
    }

    #[test]
    fn an_idle_status_view_starts_no_provider_processes() {
        let transcript = Transcript::answering(&[("git status", "")]);
        let asked = transcript.log();
        let service = SourceControlService::new(Box::new(GitSourceControl::with_runner(
            ".",
            Box::new(transcript),
        )));
        let paths = vec![PathBuf::from("worlds/city.cyworld")];
        assert!(service.request_status(paths.clone()));
        while service.status_pending() {
            std::thread::yield_now();
        }
        assert!(!service.request_status(paths));
        assert_eq!(
            asked.lock().unwrap().len(),
            1,
            "an unchanged frame queried the provider again"
        );
    }

    #[test]
    fn source_control_commands_declare_their_actual_effects() {
        let mut registry = Registry::new();
        register_commands(&mut registry).unwrap();
        for (id, effect) in [
            ("source-control.refresh", EffectClass::Read),
            ("source-control.history", EffectClass::Read),
            ("source-control.checkout", EffectClass::IrreversibleMutation),
            ("source-control.revert", EffectClass::IrreversibleMutation),
            ("source-control.submit", EffectClass::ExternalEffect),
            ("source-control.lock", EffectClass::ExternalEffect),
            ("source-control.unlock", EffectClass::ExternalEffect),
        ] {
            assert_eq!(registry.metadata(id).unwrap().effect, effect, "{id}");
        }
    }

    #[test]
    fn the_null_provider_answers_unversioned_and_refuses_everything_else() {
        let null = NullSourceControl;
        let paths = vec![PathBuf::from("worlds/city.cyworld")];
        let status = null.status(&paths).unwrap();
        assert_eq!(status[0].state, FileState::Unversioned);

        // Each refusal names the provider and the operation, so a caller can tell a person which
        // of the two is missing rather than "unsupported".
        let refusal = null.submit(&paths, "anything").unwrap_err();
        assert!(refusal.what.contains("submit"), "{refusal}");
        assert!(refusal.what.contains("no source control"), "{refusal}");
        assert!(null.history(Path::new("a")).is_err());
        assert!(null.check_out(&paths).is_err());
        assert!(null.revert(&paths).is_err());
        assert!(null.lock(&paths).is_err());
    }

    #[test]
    fn a_provider_without_exclusive_locking_refuses_rather_than_doing_nothing() {
        // The requirement's second scenario, and the one a silent `Ok(())` would pass.
        let git = GitSourceControl::with_runner(".", Box::new(Transcript::default()));
        assert!(!git.capabilities().has(Capability::ExclusiveLock));
        let refusal = git
            .lock(&[PathBuf::from("worlds/city.cyworld")])
            .unwrap_err();
        assert!(refusal.what.contains("Git"), "{refusal}");
        assert!(refusal.because.contains("exclusive locking"), "{refusal}");
        assert!(refusal.remedy.is_some(), "a refusal says what would work");

        // And the same operation through a provider that HAS it is not a refusal, which is what
        // makes the capability a declaration rather than a blanket "no".
        let transcript = Transcript::answering(&[("p4 lock", "")]);
        let perforce = PerforceSourceControl::with_runner(".", Box::new(transcript));
        assert!(perforce.capabilities().has(Capability::ExclusiveLock));
        assert!(
            perforce
                .lock(&[PathBuf::from("worlds/city.cyworld")])
                .is_ok()
        );
    }

    #[test]
    fn git_check_out_refuses_because_git_has_no_such_concept() {
        let git = GitSourceControl::with_runner(".", Box::new(Transcript::default()));
        assert!(!git.capabilities().has(Capability::CheckOut));
        assert!(git.check_out(&[PathBuf::from("a")]).is_err());

        let perforce = PerforceSourceControl::with_runner(
            ".",
            Box::new(Transcript::answering(&[("p4 edit", "")])),
        );
        assert!(perforce.capabilities().has(Capability::CheckOut));
        assert!(perforce.check_out(&[PathBuf::from("a")]).is_ok());
    }

    #[test]
    fn git_status_reads_the_porcelain_form_including_a_clean_file() {
        let transcript = Transcript::answering(&[(
            "git status",
            " M worlds/city.cyworld\0?? worlds/new.cyworld\0R  worlds/moved.cyworld\0worlds/old.cyworld\0",
        )]);
        let git = GitSourceControl::with_runner(".", Box::new(transcript));
        let paths = vec![
            PathBuf::from("worlds/city.cyworld"),
            PathBuf::from("worlds/new.cyworld"),
            PathBuf::from("worlds/moved.cyworld"),
            PathBuf::from("worlds/untouched.cyworld"),
        ];
        let status = git.status(&paths).unwrap();
        assert_eq!(status[0].state, FileState::Modified);
        assert_eq!(status[1].state, FileState::Unversioned);
        assert_eq!(status[2].state, FileState::Renamed);
        // Absent from the porcelain output means tracked and unchanged, which is the case a parser
        // that only maps what it printed reports as missing rather than as clean.
        assert_eq!(status[3].state, FileState::Clean);
    }

    #[test]
    fn a_failed_client_call_carries_the_clients_own_message() {
        let git = GitSourceControl::with_runner(".", Box::new(Transcript::default()));
        let refusal = git.history(Path::new("worlds/city.cyworld")).unwrap_err();
        assert!(refusal.what.contains("history through Git"), "{refusal}");
        assert!(
            refusal.because.contains("transcript has no answer"),
            "the client's own words reach the caller: {refusal}"
        );
    }

    #[test]
    fn the_editor_never_names_a_provider_and_switching_one_changes_nothing_else() {
        // The requirement's first scenario, written as the thing a caller does.
        fn describe(service: &SourceControlService, paths: &[PathBuf]) -> Vec<FileState> {
            service
                .provider()
                .status(paths)
                .unwrap()
                .into_iter()
                .map(|status| status.state)
                .collect()
        }

        let paths = vec![PathBuf::from("worlds/city.cyworld")];
        let mut service = SourceControlService::default();
        assert_eq!(describe(&service, &paths), [FileState::Unversioned]);

        service.adopt(Box::new(GitSourceControl::with_runner(
            ".",
            Box::new(Transcript::answering(&[(
                "git status",
                " M worlds/city.cyworld\0",
            )])),
        )));
        assert_eq!(describe(&service, &paths), [FileState::Modified]);

        service.adopt(Box::new(PerforceSourceControl::with_runner(
            ".",
            Box::new(Transcript::answering(&[(
                "p4 -ztag fstat",
                "... depotFile //depot/worlds/city.cyworld\n... headAction edit\n... action edit\n",
            )])),
        )));
        assert_eq!(describe(&service, &paths), [FileState::Modified]);
    }

    #[test]
    fn perforce_history_reads_the_tagged_form() {
        let transcript = Transcript::answering(&[(
            "p4 -ztag filelog",
            "... change 42 edit\n... user designer\n... desc raised the lamps\n\
             ... change 41 add\n... user designer\n... desc added the lamps\n",
        )]);
        let perforce = PerforceSourceControl::with_runner(".", Box::new(transcript));
        let history = perforce.history(Path::new("worlds/city.cyworld")).unwrap();
        assert_eq!(history.len(), 2);
        assert_eq!(history[0].id, "42");
        assert_eq!(history[0].author, "designer");
        assert_eq!(history[0].description, "raised the lamps");
    }

    #[test]
    fn git_builds_the_argument_list_the_client_expects() {
        // Half of what is worth checking about a provider is the command it built: a parser tested
        // against a transcript of a command nobody runs is a parser of a format nobody produces.
        let transcript = Transcript::answering(&[
            ("git add", ""),
            ("git commit", ""),
            ("git rev-parse", "9f1c0de\n"),
        ]);
        let asked = transcript.log();
        let git = GitSourceControl::with_runner(".", Box::new(transcript));
        let id = git
            .submit(&[PathBuf::from("worlds/city.cyworld")], "name the nodes")
            .unwrap();
        assert_eq!(id, "9f1c0de");
        assert_eq!(
            *asked.lock().unwrap(),
            vec![
                "git add -- worlds/city.cyworld".to_string(),
                "git commit -m name the nodes".to_string(),
                "git rev-parse HEAD".to_string(),
            ],
            "a `--` before the paths, and the description passed as one argument"
        );
    }

    /// The real client, where one is installed. A transcript proves the parser; this proves the
    /// invocation, which is the half a transcript can agree with forever while being wrong.
    #[test]
    fn git_reads_a_real_repository() {
        let root = std::env::temp_dir().join(format!(
            "cy-source-control-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|since| since.as_nanos())
                .unwrap_or_default()
        ));
        if std::fs::create_dir_all(&root).is_err() {
            return;
        }
        let runner = ProcessRunner;
        let git = |arguments: &[&str]| -> CommandOutput {
            let owned: Vec<String> = arguments.iter().map(|word| (*word).to_string()).collect();
            runner.run("git", &owned, &root)
        };
        if !git(&["init", "-q"]).succeeded() {
            // No `git` on this machine. The transcript cases above still ran; this one reports
            // nothing rather than failing, the way the render suites do without a device.
            let _ = std::fs::remove_dir_all(&root);
            return;
        }
        let _ = git(&["config", "user.email", "designer@example.invalid"]);
        let _ = git(&["config", "user.name", "designer"]);
        std::fs::write(root.join("city.cyworld"), "cyworld 1\n").unwrap();

        let provider = GitSourceControl::new(&root);
        let tracked = vec![PathBuf::from("city.cyworld")];
        assert_eq!(
            provider.status(&tracked).unwrap()[0].state,
            FileState::Unversioned,
            "a file git has never seen"
        );

        let revision = provider.submit(&tracked, "add the world").unwrap();
        assert_eq!(revision.len(), 40, "a git object name: {revision}");
        assert_eq!(
            provider.status(&tracked).unwrap()[0].state,
            FileState::Clean
        );

        let history = provider.history(Path::new("city.cyworld")).unwrap();
        assert_eq!(history.len(), 1);
        assert_eq!(history[0].author, "designer");
        assert_eq!(history[0].description, "add the world");

        std::fs::write(root.join("city.cyworld"), "cyworld 1\nnode 0 - \"set\"\n").unwrap();
        assert_eq!(
            provider.status(&tracked).unwrap()[0].state,
            FileState::Modified
        );
        let past = provider
            .content_at(Path::new("city.cyworld"), &revision)
            .unwrap();
        assert_eq!(past.content, b"cyworld 1\n", "the revision, not the file");

        provider.revert(&tracked).unwrap();
        assert_eq!(
            provider.status(&tracked).unwrap()[0].state,
            FileState::Clean,
            "revert discarded the local change"
        );

        let _ = std::fs::remove_dir_all(&root);
    }
}
