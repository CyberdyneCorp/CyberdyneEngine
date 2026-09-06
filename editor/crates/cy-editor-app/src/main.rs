//! `cyberdyne-editor` — the editor binary.
//!
//! Headless today, by choice rather than by omission: `editor-rust-application` requires the
//! interface toolkit to be "an implementation choice behind editor abstractions, **selected on
//! measurement**", and this milestone delivers the layers that must be finished before that
//! measurement is worth making. What the binary does now is what a test, a script and an agent do —
//! invoke commands against the same registry — so the day a window arrives it is one more caller
//! rather than a rewrite.
//!
//! ```text
//! cyberdyne-editor --version
//! cyberdyne-editor --list-commands
//! cyberdyne-editor --open worlds/city.cyworld --script session.cyscript
//! cyberdyne-editor --open worlds/city.cyworld --host /run/cyberdyne.sock --script session.cyscript
//! ```

#![forbid(unsafe_code)]

use std::process::ExitCode;

use cy_editor_app::{Application, run_script};
use cy_editor_core::Actor;
use cy_editor_core::problem::{Problem, Result};

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
#[derive(Default)]
struct Options {
    open: Vec<String>,
    script: Option<String>,
    host: Option<String>,
    list_commands: bool,
    version: bool,
    journal: Option<String>,
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

    for asset in &options.open {
        application.editor.open_document(asset)?;
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
            "--open" => options.open.push(value(arguments, &mut index, "--open")?),
            "--script" => options.script = Some(value(arguments, &mut index, "--script")?),
            "--host" => options.host = Some(value(arguments, &mut index, "--host")?),
            "--journal" => options.journal = Some(value(arguments, &mut index, "--journal")?),
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
cyberdyne-editor — the Cyberdyne editor, a client of the engine over its stable C ABI

    --open <asset>        open a document (repeatable)
    --script <path>       run a script of commands, one per line
    --host <socket>       attach a hosted runtime over a Unix domain socket
    --journal <directory> write transaction journals here, for crash recovery
    --list-commands       print the command registry, with parameters and effect classes
    --version             print the editor and ABI versions
    --help                this";
