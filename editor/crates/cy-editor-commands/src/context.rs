//! What a command handler is given, expressed as a trait so that this crate stays below services.
//!
//! A command needs documents, selection, the acting actor and a way to notify — all of which are
//! *services*, and services are layer 3. Depending on them from layer 2 would be the upward
//! dependency `editor-rust-application` requires to be a build error, so the dependency is inverted:
//! this crate declares the interface a handler needs, and `cy-editor-services` implements it.
//!
//! The second payoff is testability. A test double implementing [`CommandContext`] is thirty lines
//! and needs no engine, no window and no service graph, which is how this crate's own tests invoke
//! commands — and how `editor-rust-application`'s "Services, models, view models, and commands SHALL
//! be testable **headlessly**" is satisfied by construction rather than by effort.

use std::collections::BTreeMap;

use cy_editor_core::Actor;
use cy_editor_core::ids::DocumentId;
use cy_editor_core::problem::Result;
use cy_editor_core::value::Value;
use cy_editor_documents::Document;
use cy_editor_documents::selection::Selection;

/// What a command invocation produced.
///
/// `summary` is for a person and a log; `values` is for a machine. Both, because the same invocation
/// serves a menu item and an agent, and an agent that has to parse a sentence to learn which entity
/// was created is an agent that will get it wrong.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct Outcome {
    /// One line a person reads: "Created 3 entities".
    pub summary: String,
    /// Structured results, keyed by name.
    pub values: BTreeMap<String, Value>,
}

impl Outcome {
    /// An outcome with a summary and no values.
    pub fn new(summary: impl Into<String>) -> Self {
        Self {
            summary: summary.into(),
            values: BTreeMap::new(),
        }
    }

    /// Attach a structured result.
    #[must_use]
    pub fn with(mut self, name: impl Into<String>, value: Value) -> Self {
        self.values.insert(name.into(), value);
        self
    }
}

/// What a command handler may reach.
///
/// Deliberately small. Everything a command does to persistent state goes through a document, and a
/// document's only write path is a transaction — so a handler cannot mutate project state any other
/// way however much it would like to, and this trait is where that stays true.
pub trait CommandContext {
    /// The document a command with no explicit target acts on.
    fn active_document(&self) -> Option<DocumentId>;

    /// A document, for reading.
    fn document(&self, id: DocumentId) -> Option<&Document>;

    /// A document, for writing — which still means through its transaction system.
    fn document_mut(&mut self, id: DocumentId) -> Option<&mut Document>;

    /// Who is acting. Attribution, not authorisation; see the crate note.
    fn actor(&self) -> Actor;

    /// What is selected.
    fn selection(&self) -> &Selection;

    /// Replace the selection.
    fn set_selection(&mut self, selection: Selection);

    /// Report something to the user. A notification, not a log line.
    fn notify(&mut self, message: &str);

    /// The viewport's own controls, when this context has a viewport.
    ///
    /// **Why this is here and why it is stringly typed.** A command that switches the transform mode
    /// changes no document, so it has nothing to reach through the six methods above — and yet it has
    /// to be a *command*, because `editor-rust-application` requires one action to have one
    /// implementation reachable from the menu, the palette, a key, a script and an agent alike. A
    /// viewport control that the window handled privately would be one an agent cannot use.
    ///
    /// It cannot name a viewport type: `cy-editor-viewport` is layer 2, the same layer as this
    /// crate, and a dependency between two crates at the same level is a layering failure. So the
    /// interface is named controls and named values, and the crate that knows what they mean —
    /// `cy-editor-services` — implements it. The set of names is small, closed and listed by
    /// [`ViewportControls::controls`], so a caller that cannot see the interface can still discover
    /// it.
    ///
    /// The default is `None`, so a test double and a headless host stay thirty lines.
    fn viewport(&mut self) -> Option<&mut dyn ViewportControls> {
        None
    }

    /// The project around the documents: its source tree, its build, and the runtime that runs it.
    ///
    /// **Why this is separate from a document.** A `.swift` file is not a node graph, so nothing in
    /// [`Document`] can hold it; a build is not state at all; and the runtime belongs to the editor
    /// rather than to the project. Those are the three things a command needs that a document cannot
    /// give it, and they arrive together because they are one loop — write a script, build it,
    /// reload it, play it — rather than three unrelated capabilities.
    ///
    /// A source *edit* is still a transaction. See [`ProjectHost::write_source`]: the host performs
    /// the write and reports what it replaced, and the command records that as an operation in the
    /// document that stands for the file, so undo, the journal, attribution and replay all work
    /// without a second mechanism.
    ///
    /// `None` for a host with no project — a test double, or the editor before a project is open —
    /// so a command refuses with a remedy rather than panicking.
    fn project(&mut self) -> Option<&mut dyn ProjectHost> {
        None
    }

    /// The project's importers, for a command that has to cook a source asset. M8.a task 3.1.
    ///
    /// **Why this is not on [`ProjectHost`].** Importing is not a source edit and not a build: it
    /// reads a file the project already holds, runs an importer over it, and writes cooked bytes and
    /// two sidecars — none of which is a transaction and none of which `ProjectHost`'s five source
    /// methods can express. Bolting it on would give that trait a second subject, and a test double
    /// for a source edit would then have to implement an importer to compile.
    ///
    /// `None` for a host with no importer — a test double, or an editor with no project — so a
    /// command refuses with a remedy rather than panicking.
    fn assets(&mut self) -> Option<&mut dyn crate::assets::AssetHost> {
        None
    }

    /// Open the document that stands for an asset, returning the one already open when there is
    /// one.
    ///
    /// A source file's transaction needs a document to live in, and a command cannot make one:
    /// documents belong to a service at layer 3. The default refuses with a remedy rather than
    /// panicking, so a test double stays thirty lines.
    ///
    /// # Errors
    ///
    /// When this host cannot open documents, or the asset cannot be opened.
    fn open_document(&mut self, asset: &str) -> Result<DocumentId> {
        Err(cy_editor_core::problem::Problem::new(
            format!("open {asset}"),
            "this host cannot open documents",
        )
        .with_remedy("invoke this through the editor rather than a test double"))
    }

    /// Where the runtime is — `editing`, `playing` or `paused` — for a command that has to know
    /// before it runs.
    ///
    /// Separate from [`ProjectHost::play_state`] because an availability predicate is handed
    /// `&dyn CommandContext` and cannot reach a `&mut` accessor. `None` for a host with no runtime.
    fn play_state(&self) -> Option<String> {
        None
    }

    /// The directories this invocation may touch, and the name of the scope that says so.
    ///
    /// `None` means unrestricted, which is what a human at the interface is and what a test double
    /// should be. `Some` carries the connection's grant — see `crate::scope::Scope::directories` —
    /// and a command that touches a path checks it, because the registry cannot: only the command
    /// knows which of its arguments is a path.
    fn permitted_paths(&self) -> Option<(String, Vec<String>)> {
        None
    }

    /// Translate, rotate or scale the selection through the editor's own manipulation path.
    ///
    /// **Why a hook rather than a command doing the work.** `editor-agent-interface` requires that a
    /// translate "execute through the same manipulation implementation a gizmo drag uses", with the
    /// same start-state capture, the same one transaction, and the same treatment of pivot, space,
    /// snapping and constraints. That implementation lives in the viewport model, which is at this
    /// crate's own layer and so cannot be named here; and the settings it needs — which pivot, which
    /// space, which increments — live in a service above. So the request is plain data, and the
    /// crate that owns both ends performs it.
    ///
    /// It is not an agent path. A numeric field, a nudge key and a script all reach the same place.
    ///
    /// # Errors
    ///
    /// When there is nothing selected that carries a transform, when no document is active, or when
    /// this host has no viewport to manipulate through.
    fn manipulate(&mut self, request: &Manipulation) -> Result<String> {
        let _ = request;
        Err(cy_editor_core::problem::Problem::new(
            "manipulate the selection",
            "this host has no viewport to manipulate through",
        )
        .with_remedy("invoke this through the editor rather than a test double"))
    }
}

/// A manipulation stated rather than dragged.
///
/// Plain data on purpose: it names no viewport type, so it can live at this layer, and it says the
/// same things a person's hands say — which quantity, how much, along which axes, relative or
/// absolute, and whether the increments apply.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Manipulation {
    /// `translate`, `rotate` or `scale`.
    pub kind: ManipulationKind,
    /// How much, per axis. Metres, degrees, or a factor — the units a person types, not the
    /// engine's.
    pub amount: [f32; 3],
    /// Whether `amount` is the value to end at rather than the change to make.
    pub absolute: bool,
    /// Whether the viewport's snapping increments apply.
    pub snap: bool,
}

/// Which quantity a manipulation changes.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ManipulationKind {
    /// Where it is.
    Translate,
    /// Which way it faces.
    Rotate,
    /// How big it is.
    Scale,
}

impl ManipulationKind {
    /// The name a caller uses.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            ManipulationKind::Translate => "translate",
            ManipulationKind::Rotate => "rotate",
            ManipulationKind::Scale => "scale",
        }
    }

    /// The unit `amount` is in, for a description and a report.
    #[must_use]
    pub const fn unit(self) -> &'static str {
        match self {
            ManipulationKind::Translate => "metres",
            ManipulationKind::Rotate => "degrees",
            ManipulationKind::Scale => "a factor",
        }
    }

    /// What `amount` means when nothing is stated for an axis.
    ///
    /// Zero for a translation and a rotation, one for a scale: a scale is multiplicative and "no
    /// change" is a factor of one, which is the arithmetic that gets forgotten.
    #[must_use]
    pub const fn identity(self) -> f32 {
        match self {
            ManipulationKind::Translate | ManipulationKind::Rotate => 0.0,
            ManipulationKind::Scale => 1.0,
        }
    }
}

/// The project's source tree, its build, and the runtime that runs it.
///
/// Named operations over named things, in the manner of [`ViewportControls`] and for the same
/// layering reason: this crate is layer 2 and cannot see the services that hold a project. What it
/// can do is declare what a command needs.
///
/// **Nothing here writes history.** The host performs the file operation and reports what it
/// replaced; the *command* records that in a document as a transaction. Keeping the two apart is
/// what stops a second write path appearing beside the transaction system — see
/// `cy_editor_services::project` for the whole argument.
pub trait ProjectHost {
    /// Where the project is, for a message a person reads.
    fn project_root(&self) -> String;

    /// Every source file the project holds, project-relative, in path order.
    fn source_paths(&self) -> Vec<String>;

    /// Whether the project holds this file.
    fn source_exists(&self, path: &str) -> bool;

    /// One source file's contents.
    ///
    /// # Errors
    ///
    /// When there is no such file, or the editor cannot read it as text.
    fn read_source(&self, path: &str) -> Result<String>;

    /// Whether the editor could capture this file's prior contents, and so reverse a write to it.
    ///
    /// True for a file that does not exist yet — creating one undoes to deleting it — and for one
    /// the editor can read back as text. False for a file that exists and cannot be read, which is
    /// the case `design.md` §4 calls "overwriting a file the editor never read".
    fn source_is_restorable(&self, path: &str) -> bool;

    /// Put a source file into a stated state: `Some` writes it, `None` removes it.
    ///
    /// One operation rather than a write and a delete, because undo needs exactly this — "the file
    /// was in this state, put it back" — and two would leave a caller to decide which, which is
    /// where the "it did not exist" case gets forgotten.
    ///
    /// # Errors
    ///
    /// When the path leaves the project, or the write or the delete fails.
    fn put_source(&mut self, path: &str, contents: Option<&str>) -> Result<()>;

    /// Start a build of the project's script module, returning what a caller should watch.
    ///
    /// **Does not block.** A build takes tens of seconds and the editor stays usable while an agent
    /// works, so this queues the work and returns; progress is read where every other long
    /// operation's is.
    ///
    /// # Errors
    ///
    /// When there is nothing to build, or no toolchain to build it with.
    fn build(&mut self) -> Result<String>;

    /// What the last build did, and what is running now.
    fn build_state(&self) -> String;

    /// Ask the attached runtime to load the newest built generation of a module.
    ///
    /// An empty name means the project's own module.
    ///
    /// # Errors
    ///
    /// When no runtime is attached, or nothing has been built.
    fn reload(&mut self, module: &str) -> Result<String>;

    /// Enter, pause or leave play. The values are `playing`, `paused` and `editing`.
    ///
    /// # Errors
    ///
    /// When the value is not one of the three.
    fn set_play(&mut self, state: &str) -> Result<String>;

    /// Where the runtime is: `editing`, `playing` or `paused`.
    fn play_state(&self) -> String;
}

/// A viewport's own controls, addressed by name.
///
/// Everything here changes what the *editor* is showing and nothing in the project, which is why no
/// method returns a `Result` that could mean "the document refused": the only failure is a name this
/// implementation does not have, and that is a caller's mistake rather than a document's state.
pub trait ViewportControls {
    /// Set a control to a value, and describe what happened for the invocation's outcome.
    ///
    /// # Errors
    ///
    /// When the control or the value is not one this viewport has, with the list of what it does
    /// have — an agent that guessed "move" for a mode called "translate" gets told the difference
    /// rather than a silent no-op.
    fn set(&mut self, control: &str, value: &str) -> Result<String>;

    /// What a control currently holds, or `None` when there is no such control.
    fn get(&self, control: &str) -> Option<String>;

    /// Perform a control that takes no value — focusing the selection, cycling the view.
    ///
    /// # Errors
    ///
    /// When the action is not one this viewport has.
    fn perform(&mut self, action: &str) -> Result<String>;

    /// Point the camera at a sphere, which is what framing a selection comes to.
    ///
    /// Typed rather than named, because the caller that knows *where* the selection is has to read a
    /// document, and a control addressed by name has no way to carry three numbers honestly.
    fn focus(&mut self, centre: [f32; 3], radius: f32);

    /// Every control name and what it accepts, for a description a caller can read.
    fn controls(&self) -> Vec<(&'static str, Vec<String>)>;
}
