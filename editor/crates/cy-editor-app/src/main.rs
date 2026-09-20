//! `cyberdyne-editor` — the editor binary.
//!
//! **It opens a window.** That is the default and it needs no flag, because a delivered capability
//! that has to be asked for is a capability nothing exercises: M5's editor was headless by design
//! and the milestone closed on a scripted session that could not show docking, workspaces, the
//! palette or keyboard-first operation. M5.5 is the correction, and the window being the default is
//! the part of it that keeps working.
//!
//! The headless driver is still here and is still what a test, a script and an agent use — the same
//! `Registry::invoke`, so the window is one more caller rather than a second path. It is selected by
//! asking for something that has no window in it: `--script`, `--list-commands`, `--version`, or
//! `--headless` when you want the loop without a display.
//!
//! ```text
//! cyberdyne-editor                                     # opens the window
//! cyberdyne-editor --open worlds/city.cyworld          # opens the window on a world
//! cyberdyne-editor --version
//! cyberdyne-editor --list-commands
//! cyberdyne-editor --headless --open worlds/city.cyworld
//! cyberdyne-editor --smoke                            # open, draw three frames, close
//! cyberdyne-editor --open worlds/city.cyworld --script session.cyscript
//! cyberdyne-editor --open worlds/city.cyworld --host /run/cyberdyne.sock --script session.cyscript
//! cyberdyne-editor --open worlds/city.cyworld --mcp --agent-scope author
//! ```
//!
//! `--mcp` is the agent interface, over the Model Context Protocol on standard input and output.
//! It is compiled only when the `agent-interface` feature is on, which it is by default; a build
//! without it has no transport at all and `tests/gating.rs` is what says so.

#![forbid(unsafe_code)]

use std::process::ExitCode;

use cy_editor_app::{Application, run_script};
use cy_editor_commands::{AssetHost, ImportFormat};
use cy_editor_core::Actor;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_services::{Notification, WorkspaceStore};

fn main() -> ExitCode {
    let arguments: Vec<String> = std::env::args().skip(1).collect();
    match run(&arguments) {
        Ok(()) => ExitCode::SUCCESS,
        Err(problem) => {
            eprintln!("cyberdyne-editor: {problem}");
            ExitCode::FAILURE
        }
    }
}

/// What the command line asked for.
///
/// A flat record of flags rather than an enumeration of modes, because that is what a command line
/// is: `--headless --open x --journal y` is four independent answers, and folding them into a mode
/// enumeration would need one variant per combination.
#[allow(
    clippy::struct_excessive_bools,
    reason = "each is one command-line flag, independent of the others; grouping them into \
              sub-structures would make the parser longer and say nothing new"
)]
struct Options {
    open: Vec<String>,
    script: Option<String>,
    host: Option<String>,
    list_commands: bool,
    list_importers: bool,
    version: bool,
    journal: Option<String>,
    headless: bool,
    smoke: bool,
    mcp: bool,
    agent_scope: String,
    agent_intent: String,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            open: Vec::new(),
            script: None,
            host: None,
            list_commands: false,
            list_importers: false,
            version: false,
            journal: None,
            headless: false,
            smoke: false,
            mcp: false,
            // The narrowest useful setting, which is what `editor-agent-interface` requires a scope
            // to default to: an agent may look at everything and change nothing until somebody says
            // otherwise. `--agent-scope author` is that somebody.
            agent_scope: "read".to_string(),
            agent_intent: "an agent session started from the command line".to_string(),
        }
    }
}

fn run(arguments: &[String]) -> Result<()> {
    let options = parse(arguments)?;

    if options.version {
        println!("cyberdyne-editor {}", env!("CARGO_PKG_VERSION"));
        println!(
            "ABI {}.{}",
            cy_editor_sdk::abi::MAJOR,
            cy_editor_sdk::abi::MINOR
        );
        return Ok(());
    }

    let mut application = Application::new(Actor::human(whoami()))?;

    if options.list_commands {
        // The projection an agent reads, printed. Every line states the command's parameters and its
        // effect class, which is what `editor-agent-interface` requires a tool listing to carry.
        for line in application.registry.projection() {
            println!("{line}");
        }
        return Ok(());
    }

    if options.list_importers {
        let formats = application.editor.imports.import_formats();
        if formats.is_empty() {
            return Err(Problem::new(
                "list the editor's importers",
                format!(
                    "the import service reported none ({})",
                    application.editor.imports.describe()
                ),
            )
            .with_remedy("build cy_import_cli or set CY_IMPORT_CLI to that executable"));
        }
        print!("{}", format_importers(&formats));
        return Ok(());
    }

    if let Some(directory) = &options.journal {
        application.editor.documents.journal_into(directory);
    }

    #[cfg(unix)]
    if let Some(endpoint) = &options.host {
        // Reported and not fatal: an editor with no runtime is a mode, not a broken state.
        if let Err(problem) = application.attach_hosted_runtime(endpoint) {
            eprintln!("cyberdyne-editor: {problem}");
        }
    }
    #[cfg(not(unix))]
    if options.host.is_some() {
        eprintln!(
            "cyberdyne-editor: hosted runtimes are reached over a Unix domain socket, which \
                   this platform build does not have"
        );
    }

    let opens_window = opens_window(&options);
    let workspace_store = if opens_window && !options.smoke {
        restore_workspace(&mut application)
    } else {
        None
    };

    for asset in &options.open {
        application.editor.open_document(asset)?;
    }

    #[cfg(feature = "agent-interface")]
    if options.mcp && options.headless {
        return serve_agent(&mut application, &options);
    }
    #[cfg(not(feature = "agent-interface"))]
    if options.mcp {
        return serve_agent(&mut application, &options);
    }

    // The window, unless something was asked for that has no window in it. `--script` implies
    // headless because a script is a session that ends, and a window is a session that does not.
    if opens_window {
        return run_window(application, &options, workspace_store);
    }

    if let Some(path) = &options.script {
        let script = std::fs::read_to_string(path)
            .map_err(|error| Problem::new(format!("read the script {path}"), error.to_string()))?;
        for summary in run_script(&mut application, &script)?.summaries {
            println!("{summary}");
        }
    }

    application.pump();
    report(&mut application);
    Ok(())
}

fn restore_workspace(application: &mut Application) -> Option<WorkspaceStore> {
    let store = match WorkspaceStore::for_user(application.editor.project.root()) {
        Ok(store) => store,
        Err(problem) => {
            eprintln!("cyberdyne-editor: {problem}");
            return None;
        }
    };

    match store.restore(&mut application.editor) {
        Ok(report) if !report.missing.is_empty() => {
            application
                .editor
                .notifications
                .post(Notification::warning(format!(
                    "Skipped {} missing document(s) from the previous workspace: {}",
                    report.missing.len(),
                    report.missing.join(", ")
                )));
        }
        Ok(_) => {}
        Err(problem) => application
            .editor
            .notifications
            .post(Notification::error(problem.what.clone(), problem)),
    }
    Some(store)
}

fn opens_window(options: &Options) -> bool {
    !options.headless && options.script.is_none()
}

fn run_window(
    application: Application,
    options: &Options,
    workspace_store: Option<WorkspaceStore>,
) -> Result<()> {
    let Application {
        editor,
        registry,
        scope,
    } = application;
    let mut window = cy_editor_shell::EditorWindow::new(editor, registry, scope)?;
    if options.smoke {
        window = window.with_smoke_frames(3);
    }
    if let Some(store) = workspace_store {
        window = window.with_workspace_store(store);
    }
    #[cfg(not(feature = "agent-interface"))]
    let _ = options;
    #[cfg(feature = "agent-interface")]
    if options.mcp {
        window = window.with_agent_host(spawn_desktop_agent(options)?);
    }
    cy_editor_shell::run(window).map_err(|error| {
        Problem::new("open the editor window", error.to_string()).with_remedy(
            "run with --headless to use the editor without a display, or check that a display is \
             available",
        )
    })
}

/// What the editor has to say, and one line about where the session ended up.
///
/// The notifications are printed, not merely counted, because one of them is the offer of recovery:
/// `editor-documents-and-transactions` requires the editor to "**offer** recovery from the last
/// saved revision plus its journal, reporting how many transactions are recoverable", and an offer
/// nobody is shown is not one. A window will render these; a command line prints them.
fn report(application: &mut Application) {
    let mut cursor = cy_editor_core::observe::Cursor::default();
    for notification in application.editor.notifications.drain_from(&mut cursor) {
        println!("{:?}: {}", notification.severity, notification.message);
        if let Some(remedy) = notification
            .problem
            .as_ref()
            .and_then(|problem| problem.remedy.as_ref())
        {
            println!("    {remedy}");
        }
    }
    println!(
        "{} document(s) open, {} unsaved, engine: {}",
        application.editor.documents.len(),
        usize::from(application.editor.documents.any_dirty()),
        application.hosting_mode().name()
    );
}

fn parse(arguments: &[String]) -> Result<Options> {
    let mut options = Options::default();
    let mut index = 0;
    while index < arguments.len() {
        let argument = arguments[index].as_str();
        match argument {
            "--version" | "-V" => options.version = true,
            "--list-commands" => options.list_commands = true,
            "--list-importers" => options.list_importers = true,
            "--open" => options.open.push(value(arguments, &mut index, "--open")?),
            "--script" => options.script = Some(value(arguments, &mut index, "--script")?),
            "--host" => options.host = Some(value(arguments, &mut index, "--host")?),
            "--journal" => options.journal = Some(value(arguments, &mut index, "--journal")?),
            "--headless" => options.headless = true,
            "--smoke" => options.smoke = true,
            "--mcp" => options.mcp = true,
            "--agent-scope" => {
                options.agent_scope = value(arguments, &mut index, "--agent-scope")?;
            }
            "--agent-intent" => {
                options.agent_intent = value(arguments, &mut index, "--agent-intent")?;
            }
            "--help" | "-h" => {
                println!("{USAGE}");
                std::process::exit(0);
            }
            other => {
                return Err(Problem::new(
                    format!("the option {other:?}"),
                    "it is not one this editor knows",
                )
                .with_remedy("run with --help to see what there is"));
            }
        }
        index += 1;
    }
    Ok(options)
}

fn value(arguments: &[String], index: &mut usize, option: &str) -> Result<String> {
    *index += 1;
    arguments.get(*index).cloned().ok_or_else(|| {
        Problem::new(
            format!("read {option}"),
            "it takes a value and none followed it",
        )
    })
}

/// Canonical editor-side projection of the importer tool's dynamic catalogue.
///
/// A line is `name<TAB>extensions<TAB>setting:type,...`. Keeping this deliberately simple lets the
/// cross-language criterion parse the tool's human listing independently and compare exact rows.
fn format_importers(formats: &[ImportFormat]) -> String {
    let mut output = String::new();
    for format in formats {
        let settings = format
            .settings
            .iter()
            .map(|setting| format!("{}:{}", setting.name, setting.kind))
            .collect::<Vec<_>>()
            .join(",");
        output.push_str(&format.importer);
        output.push('\t');
        output.push_str(&format.extensions.join(","));
        output.push('\t');
        output.push_str(&settings);
        output.push('\n');
    }
    output
}

/// Serve one agent over standard input and output.
///
/// **Standard output is the wire.** Nothing else may print to it while this runs, which is why the
/// editor's own notifications go to standard error here and nowhere near `println!`.
///
/// The confirmer is `RefuseEverything`, and that is the honest answer rather than a limitation: an
/// irreversible operation "SHALL require explicit human confirmation ... unless the connection has
/// been granted that effect class deliberately", and there is no human on this connection — the
/// only thing at the other end of standard input is the agent asking. A person who wants to grant
/// an effect class does it by starting the editor differently, which is a decision they make once,
/// deliberately, rather than a prompt they answer while reading something else.
#[cfg(feature = "agent-interface")]
fn serve_agent(application: &mut Application, options: &Options) -> Result<()> {
    use cy_editor_agent::session::RefuseEverything;
    let session = agent_session(options)?;

    eprintln!(
        "cyberdyne-editor: serving the agent interface on standard input; scope {:?}, {} command(s)",
        options.agent_scope,
        application.registry.len()
    );
    let mut server = cy_editor_mcp::McpServer::new(std::io::stdout().lock(), session);
    cy_editor_mcp::serve(
        std::io::stdin().lock(),
        &mut server,
        &mut application.editor,
        &application.registry,
        &mut RefuseEverything,
    )
}

#[cfg(feature = "agent-interface")]
fn agent_session(options: &Options) -> Result<cy_editor_agent::AgentSession> {
    use cy_editor_commands::EffectClass;
    use cy_editor_commands::scope::{DocumentScope, Scope};

    let scope = match options.agent_scope.as_str() {
        "read" => Scope::new("read", DocumentScope::All, [EffectClass::Read]),
        "author" => Scope::new(
            "author",
            DocumentScope::All,
            [EffectClass::Read, EffectClass::ReversibleMutation],
        )
        .with_directory("game/"),
        "operator" => Scope::new("operator", DocumentScope::All, EffectClass::ALL)
            .with_directory(String::new()),
        other => {
            return Err(Problem::new(
                format!("read --agent-scope {other:?}"),
                "there is no such scope",
            )
            .with_remedy(
                "the scopes are: read (look only), author (undoable changes and game/ source), and \
                 operator (all effects and project paths, with desktop confirmation for \
                 irreversible or external work)",
            ));
        }
    };
    Ok(cy_editor_agent::AgentSession::new(
        cy_editor_agent::AgentIdentity {
            agent: std::env::var("CY_AGENT").unwrap_or_else(|_| "agent".into()),
            session: format!("desktop-{}", std::process::id()),
        },
        options.agent_intent.clone(),
        scope,
        cy_editor_agent::Budget::default(),
        "unversioned",
        0,
    ))
}

#[cfg(feature = "agent-interface")]
fn spawn_desktop_agent(options: &Options) -> Result<cy_editor_agent::DesktopAgentHost> {
    let (host, endpoint) = cy_editor_agent::DesktopAgentHost::new(agent_session(options)?, 64);
    std::thread::Builder::new()
        .name("cy-editor-mcp".into())
        .spawn(move || {
            if let Err(problem) = cy_editor_mcp::serve_desktop(
                std::io::BufReader::new(std::io::stdin()),
                std::io::stdout(),
                &endpoint,
            ) {
                eprintln!("cyberdyne-editor: {problem}");
            }
        })
        .map_err(|error| Problem::new("start the desktop MCP transport", error.to_string()))?;
    Ok(host)
}

/// The refusal a build without the agent interface gives.
///
/// "WHEN the editor is built without the agent interface THEN no agent transport SHALL be compiled,
/// linked, or listening." This function is what is left in its place: the option is still *named*,
/// so a person who asks for it is told the build does not have it rather than that the option does
/// not exist — which is the difference between a diagnosable answer and a puzzling one.
#[cfg(not(feature = "agent-interface"))]
fn serve_agent(_application: &mut Application, _options: &Options) -> Result<()> {
    Err(Problem::new(
        "serve the agent interface",
        "this build does not contain one",
    )
    .with_remedy("rebuild with the agent-interface feature, which is on by default"))
}

/// The name to attribute this session's transactions to.
///
/// Attribution, not authorisation: `editor-documents-and-transactions` is explicit that it "is not a
/// security control and SHALL NOT be treated as one". Reading it from the environment is exactly
/// right for a question whose answer is "who should the history say did this".
fn whoami() -> String {
    std::env::var("USER")
        .or_else(|_| std::env::var("USERNAME"))
        .unwrap_or_else(|_| "user".into())
}

const USAGE: &str = "\
cyberdyne-editor — CyberEngine, a client of the engine over its stable C ABI

    --open <asset>        open a document (repeatable)
    --headless            run without a window; the default is to open one
    --smoke               open the real window, draw three frames, then close successfully
    --mcp                 host MCP alongside the window; combine with --headless for stdio-only
    --agent-scope <name>  what it may do: read (default), author, or confirmed operator work
    --agent-intent <text> what it says it is trying to do; recorded on every change it makes
    --script <path>       run a script of commands, one per line
    --host <socket>       attach a hosted runtime over a Unix domain socket
    --journal <directory> write transaction journals here, for crash recovery
    --list-commands       print the command registry, with parameters and effect classes
    --list-importers      print the importers and option schemas discovered from cy_import_cli
    --version             print the editor and ABI versions
    --help                this";

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mcp_runs_with_the_window_unless_headless_is_explicit() {
        let desktop = parse(&["--mcp".into()]).unwrap();
        assert!(desktop.mcp);
        assert!(opens_window(&desktop));

        let headless = parse(&["--mcp".into(), "--headless".into()]).unwrap();
        assert!(headless.mcp);
        assert!(!opens_window(&headless));
    }

    #[test]
    fn smoke_uses_the_window_and_is_bounded() {
        let smoke = parse(&["--smoke".into()]).unwrap();
        assert!(smoke.smoke);
        assert!(opens_window(&smoke));
    }

    #[test]
    fn importer_projection_preserves_names_extensions_and_option_types() {
        let listing = format_importers(&[ImportFormat {
            importer: "mesh".into(),
            extensions: vec![".gltf".into(), ".glb".into()],
            settings: vec![cy_editor_commands::ImportSetting {
                name: "scale".into(),
                kind: "float".into(),
                description: "not part of the parity key".into(),
            }],
        }]);
        assert_eq!(listing, "mesh\t.gltf,.glb\tscale:float\n");
    }
}
