//! Driving the editor through commands alone, with no interface at all.
//!
//! `editor-rust-application`: "Tests SHALL be able to drive the editor through commands and assert
//! on model and view model state — selection, property values, transaction history, filtering, and
//! workspace state." This is the driver those tests use, and it is the same one the command line
//! uses, which is what stops it from being a test-only path that drifts.
//!
//! A script is one command per line: an identifier, then `name=value` arguments. Deliberately
//! austere — it is not a language and must not become one. When a script needs control flow, the
//! thing it needs is a real host binding to the command registry, and the registry is already the
//! projection that makes writing one straightforward.

use cy_editor_commands::Arguments;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::Value;
use cy_editor_protocol::{Message, SessionEvent};
use std::fmt::Write as _;

use crate::application::Application;

/// What running a script produced.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct ScriptOutcome {
    /// One summary line per command that ran.
    pub summaries: Vec<String>,
}

/// Run a script against an application, stopping at the first command that fails.
///
/// Stopping rather than continuing, because a script is a sequence in which each step assumes the
/// last one worked; carrying on after a failure produces a cascade of consequential errors and hides
/// the one that mattered.
pub fn run_script(application: &mut Application, script: &str) -> Result<ScriptOutcome> {
    let mut outcome = ScriptOutcome::default();
    for (number, line) in script.lines().enumerate() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let (id, arguments) = parse_line(line, number + 1)?;
        let mut result = application.invoke(id, &arguments).map_err(|problem| {
            Problem::new(
                format!("line {}: {}", number + 1, problem.what),
                problem.because.clone(),
            )
            .with_remedy(
                problem
                    .remedy
                    .unwrap_or_else(|| "see the command's metadata".into()),
            )
        })?;
        settle_runtime_play(application, id, number + 1, &mut result.summary)?;
        outcome.summaries.push(result.summary);
    }
    Ok(outcome)
}

/// Wait for the engine's answer when a command-line script changes play state.
///
/// The desktop remains fully asynchronous. A script has no next interface frame, though, and
/// dropping its application immediately after queuing a play message can stop the writer thread
/// before the runtime sees it. Sequential scripts also need the previous state to be authoritative
/// before issuing the next one.
fn settle_runtime_play(
    application: &Application,
    command: &str,
    line: usize,
    summary: &mut String,
) -> Result<()> {
    let expected = match command {
        "play.enter" => "playing",
        "play.pause" => "paused",
        "play.leave" => "editing",
        _ => return Ok(()),
    };
    if !summary.contains("asked the runtime") {
        return Ok(());
    }

    let answer =
        application
            .editor
            .runtime
            .block_until(std::time::Duration::from_secs(5), |event| match event {
                SessionEvent::Message(Message::Playing { state, detail, .. }) => {
                    Some(Ok((state.clone(), detail.clone())))
                }
                SessionEvent::Message(Message::Rejected { reason, remedy, .. }) => {
                    Some(Err(Problem::new(
                        format!("line {line}: the runtime refused {command}"),
                        reason.clone(),
                    )
                    .with_remedy(remedy.clone())))
                }
                SessionEvent::Lost(problem) => Some(Err(Problem::new(
                    format!("line {line}: wait for {command}"),
                    problem.because.clone(),
                )
                .with_remedy(
                    problem
                        .remedy
                        .clone()
                        .unwrap_or_else(|| "restart the runtime and run the script again".into()),
                ))),
                SessionEvent::Message(_) => None,
            });
    let (state, detail) = answer.ok_or_else(|| {
        Problem::new(
            format!("line {line}: wait for {command}"),
            "the runtime did not answer within 5 seconds",
        )
        .with_remedy("check the runtime log and connection, then run the script again")
    })??;
    if state != expected {
        return Err(Problem::new(
            format!("line {line}: confirm {command}"),
            format!("the runtime answered with {state:?}, expected {expected:?}"),
        )
        .with_remedy("inspect the runtime play-session log; it accepted a different state"));
    }
    let _ = write!(summary, " — runtime confirmed {state}: {detail}");
    Ok(())
}

/// `scene.create-entity parent=<identity>` into an identifier and its arguments.
///
/// Every value is text. The registry validates types against the declared parameters and reports a
/// mismatch with the parameter's own meaning, so a parser that guessed types here would be a second
/// place that decision is made — and the two would disagree about `1` on the day somebody added an
/// integer parameter.
fn parse_line(line: &str, number: usize) -> Result<(&str, Arguments)> {
    let mut parts = line.split_whitespace();
    let id = parts
        .next()
        .ok_or_else(|| Problem::new(format!("line {number}"), "it is empty"))?;
    let mut arguments = Arguments::new();
    for part in parts {
        let (name, value) = part.split_once('=').ok_or_else(|| {
            Problem::new(
                format!("line {number}: the argument {part:?}"),
                "it has no `=`",
            )
            .with_remedy("write arguments as name=value")
        })?;
        arguments = arguments.with(name, Value::Text(value.to_string()));
    }
    Ok((id, arguments))
}

#[cfg(test)]
mod tests {
    use cy_editor_core::Actor;

    use super::*;

    fn application() -> Application {
        let mut application = Application::new(Actor::human("designer")).unwrap();
        application
            .editor
            .open_document("worlds/city.cyworld")
            .unwrap();
        application
    }

    #[cfg(unix)]
    fn play_runtime(
        path: &std::path::Path,
    ) -> (
        std::sync::Arc<std::sync::Mutex<Vec<String>>>,
        std::thread::JoinHandle<()>,
    ) {
        use std::os::unix::net::UnixListener;

        let listener = UnixListener::bind(path).expect("a listening runtime socket");
        let received = std::sync::Arc::new(std::sync::Mutex::new(Vec::new()));
        let record = std::sync::Arc::clone(&received);
        let runtime =
            std::thread::spawn(move || {
                let (stream, _) = listener.accept().expect("the editor connects");
                let mut reader = stream.try_clone().expect("a runtime reader");
                let mut writer = stream;
                let _ = cy_editor_protocol::server::serve(&mut reader, &mut writer, |message| {
                    match message {
                        Message::Hello { .. } => Some(vec![Message::Welcome {
                            abi_major: 1,
                            abi_minor: 1,
                            runtime: "script play double".into(),
                        }]),
                        Message::Play {
                            request,
                            state,
                            mode,
                        } => {
                            record.lock().expect("the play record").push(state.clone());
                            Some(vec![Message::Playing {
                                request,
                                detail: format!("{state} accepted"),
                                state,
                                mode,
                            }])
                        }
                        _ => Some(Vec::new()),
                    }
                });
            });
        (received, runtime)
    }

    #[test]
    fn a_session_runs_with_no_window_and_no_graphics_device() {
        let mut application = application();
        let outcome = run_script(
            &mut application,
            "# create three entities and undo one\n\
             scene.create-entity\n\
             scene.create-entity\n\
             scene.create-entity\n\
             edit.undo\n",
        )
        .unwrap();

        assert_eq!(outcome.summaries.len(), 4);
        let id = application.editor.workspace.active().unwrap();
        assert_eq!(
            application
                .editor
                .documents
                .get(id)
                .unwrap()
                .content()
                .node_count(),
            2
        );
    }

    #[cfg(unix)]
    #[test]
    fn a_script_waits_until_the_runtime_confirms_each_play_state() {
        let directory = std::env::temp_dir().join(format!(
            "cy-editor-script-play-{}-{}",
            std::process::id(),
            line!()
        ));
        std::fs::create_dir_all(&directory).unwrap();
        let socket = directory.join("runtime.sock");
        let (received, runtime) = play_runtime(&socket);
        let mut application = application();
        application.attach_hosted_runtime(&socket).unwrap();

        let outcome = run_script(&mut application, "play.enter\nplay.pause\nplay.leave\n").unwrap();

        assert!(
            outcome
                .summaries
                .iter()
                .all(|summary| summary.contains("runtime confirmed")),
            "{outcome:?}"
        );
        assert_eq!(*received.lock().unwrap(), ["playing", "paused", "editing"]);

        // The protocol server owns a blocking read until the process exits. Dropping this handle
        // is deliberate, as in the end-to-end service tests that use the same server loop.
        drop(runtime);
        drop(application);
        let _ = std::fs::remove_dir_all(directory);
    }

    #[test]
    fn a_failing_line_names_itself_and_stops_the_script() {
        let mut application = application();
        let problem = run_script(
            &mut application,
            "scene.create-entity\n\
             edit.select entity=not-an-identity\n\
             scene.create-entity\n",
        )
        .unwrap_err();

        assert!(problem.what.starts_with("line 2:"), "{problem}");
        let id = application.editor.workspace.active().unwrap();
        assert_eq!(
            application
                .editor
                .documents
                .get(id)
                .unwrap()
                .content()
                .node_count(),
            1,
            "the third line did not run"
        );
    }

    #[test]
    fn an_argument_without_an_equals_sign_says_what_the_shape_is() {
        let mut application = application();
        let problem = run_script(&mut application, "edit.select entity").unwrap_err();
        assert_eq!(
            problem.remedy.as_deref(),
            Some("write arguments as name=value")
        );
    }
}
