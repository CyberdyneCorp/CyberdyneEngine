//! Killing the hosted runtime leaves the editor running with its documents intact.
//!
//! This is the decision M5 is built on, tested the only way it can honestly be tested: by starting
//! a runtime as a **separate process**, connecting to it, editing, killing it with a signal, and
//! then asserting that the editor is still there and that everything it knew is still true.
//!
//! `editor-rust-application`: "A runtime failure SHALL NOT terminate the editor: the editor SHALL
//! survive, surface the crash artefact, and offer to restart the runtime or open the reproduction",
//! with the scenario "WHEN the hosted runtime crashes THEN the editor SHALL remain running with its
//! documents and journal intact, and SHALL present the crash artefact."
//!
//! A test that dropped a socket would prove the protocol handles an end of stream. Only killing a
//! process proves the editor's fate is not tied to the runtime's — which is the entire argument for
//! paying the boundary's 60 microseconds.

#![cfg(unix)]

use std::io::{BufRead, BufReader};
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

use cy_editor_app::Application;
use cy_editor_commands::Arguments;
use cy_editor_core::Actor;
use cy_editor_core::ids::DocumentId;
use cy_editor_core::value::Value;
use cy_editor_viewport::play::PlayState;

/// Where Cargo put `cy-runtime-stub`.
fn stub_binary() -> PathBuf {
    let executable = std::env::current_exe().expect("a test binary knows its own path");
    let deps = executable
        .parent()
        .expect("the test binary is in a directory");
    let profile = if deps.ends_with("deps") {
        deps.parent().expect("a profile directory")
    } else {
        deps
    };
    let path = profile.join("cy-runtime-stub");
    assert!(
        path.is_file(),
        "cy-runtime-stub was not built at {}. It is a dev-dependency of this crate so that \
         `cargo test -p cy-editor-app` builds it.",
        path.display()
    );
    path
}

/// Start a runtime, waiting for it to say it is listening.
fn start_runtime(socket: &PathBuf) -> Child {
    let mut child = Command::new(stub_binary())
        .arg(socket)
        .stdout(Stdio::piped())
        .spawn()
        .expect("the runtime stub starts");

    let stdout = child.stdout.take().expect("the stub's stdout was piped");
    let mut lines = BufReader::new(stdout).lines();
    let ready = lines
        .next()
        .expect("the stub prints when it is listening")
        .expect("a readable line");
    assert_eq!(ready, "listening");
    child
}

fn set_all_viewports(application: &mut Application, state: PlayState) {
    for viewport in application.editor.viewports.all_mut().iter_mut() {
        viewport.play = state;
    }
}

fn all_viewports_are(application: &Application, state: PlayState) -> bool {
    application
        .editor
        .viewports
        .all()
        .iter()
        .all(|viewport| viewport.play == state)
}

fn wait_for_connection_state(application: &mut Application, connected: bool) {
    let deadline = Instant::now() + Duration::from_secs(10);
    while application.editor.runtime.is_connected() != connected && Instant::now() < deadline {
        application.pump();
        std::thread::sleep(Duration::from_millis(2));
    }
    application.pump();
    assert_eq!(
        application.editor.runtime.is_connected(),
        connected,
        "the runtime connection reached the requested state"
    );
}

fn assert_dirty_document(
    application: &Application,
    document: DocumentId,
    nodes: usize,
    history: usize,
) {
    let document = application.editor.documents.get(document).unwrap();
    assert_eq!(document.content().node_count(), nodes);
    assert_eq!(document.history().entries().len(), history);
    assert!(document.is_dirty());
}

#[test]
fn the_editor_survives_the_runtime_being_killed_mid_session() {
    let directory = std::env::temp_dir().join(format!("cy-editor-crash-{}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let socket = directory.join("runtime.sock");

    let mut runtime = start_runtime(&socket);

    let mut application = Application::new(Actor::human("designer")).unwrap();
    application
        .editor
        .documents
        .journal_into(directory.join("journal"));
    application
        .attach_hosted_runtime(&socket)
        .expect("the runtime is listening");
    assert!(application.editor.runtime.is_connected());

    let document = application
        .editor
        .open_document("worlds/city.cyworld")
        .unwrap();
    let outcome = application
        .invoke("scene.create-entity", &Arguments::new())
        .unwrap();
    let entity = outcome.values["entity"].as_text().unwrap().to_string();
    application
        .invoke(
            "scene.create-entity",
            &Arguments::new().with("parent", Value::Text(entity.clone())),
        )
        .unwrap();

    let nodes_before = application
        .editor
        .documents
        .get(document)
        .unwrap()
        .content()
        .node_count();
    let history_before = application
        .editor
        .documents
        .get(document)
        .unwrap()
        .history()
        .entries()
        .len();
    assert_eq!(nodes_before, 2);
    assert_eq!(history_before, 2);

    // Model the state the runtime owned before it died. The loss path must not leave the editor
    // claiming that this simulation still exists after the process is gone.
    set_all_viewports(&mut application, PlayState::Playing);

    // The runtime dies. SIGKILL rather than a clean shutdown, because a crash is what is being
    // tested and a crash does not run a shutdown path.
    runtime.kill().expect("the runtime can be killed");
    runtime.wait().expect("and reaped");

    // The editor notices — on its own frame, without blocking on anything.
    wait_for_connection_state(&mut application, false);
    assert!(all_viewports_are(&application, PlayState::Editing));

    // THE POINT OF ALL OF IT: everything the editor knew is still true.
    let after = application.editor.documents.get(document).unwrap();
    assert_eq!(
        after.content().node_count(),
        nodes_before,
        "the document is intact"
    );
    assert_eq!(
        after.history().entries().len(),
        history_before,
        "and so is its history"
    );
    assert!(
        after.is_dirty(),
        "including the fact that it has unsaved work"
    );

    // The editor is still usable, with no runtime — which is a mode, not a broken state.
    application
        .invoke("scene.create-entity", &Arguments::new())
        .unwrap();
    assert_eq!(
        application
            .editor
            .documents
            .get(document)
            .unwrap()
            .content()
            .node_count(),
        nodes_before + 1
    );

    // And the loss was surfaced with something to act on.
    let mut cursor = cy_editor_core::observe::Cursor::default();
    let notifications = application.editor.notifications.drain_from(&mut cursor);
    let crash = notifications
        .iter()
        .find(|notification| notification.message.contains("runtime"))
        .expect("the crash is surfaced rather than silent");
    assert!(crash.problem.as_ref().unwrap().remedy.is_some());

    // Starting the runtime again is enough. The same editor process reconnects at its bounded
    // retry cadence, retains the dirty document, and replays the three unsaved transactions into
    // the fresh runtime before incremental mirroring resumes.
    let mut restarted = start_runtime(&socket);
    wait_for_connection_state(&mut application, true);
    assert_dirty_document(&application, document, nodes_before + 1, history_before + 1);
    assert_eq!(
        application.editor.mirror.forwarded_transactions(),
        3,
        "the fresh runtime received every unsaved transaction"
    );

    restarted
        .kill()
        .expect("the restarted runtime can be killed");
    restarted.wait().expect("and reaped");
    drop(application);

    std::fs::remove_dir_all(&directory).unwrap();
}

#[test]
fn the_journal_survives_the_editor_stopping_and_recovers_what_was_unsaved() {
    // The other half of the same property. `editor-documents-and-transactions`: "After an abnormal
    // termination, the editor SHALL offer recovery from the last saved revision plus its journal,
    // reporting how many transactions are recoverable."
    let directory = std::env::temp_dir().join(format!("cy-editor-journal-{}", std::process::id()));
    let journal = directory.join("journal");
    let _ = std::fs::remove_dir_all(&directory);
    std::fs::create_dir_all(&directory).unwrap();

    {
        let mut application = Application::new(Actor::human("designer")).unwrap();
        application.editor.documents.journal_into(&journal);
        application
            .editor
            .open_document("worlds/city.cyworld")
            .unwrap();
        for _ in 0..3 {
            application
                .invoke("scene.create-entity", &Arguments::new())
                .unwrap();
        }
        // The editor stops without saving. No shutdown, no flush beyond what each commit already did.
    }

    let mut restarted = Application::new(Actor::human("designer")).unwrap();
    restarted.editor.documents.journal_into(&journal);
    let document = restarted
        .editor
        .open_document("worlds/city.cyworld")
        .unwrap();

    let recovery = restarted
        .editor
        .documents
        .get(document)
        .unwrap()
        .recoverable()
        .unwrap()
        .expect("a journal is attached");
    assert_eq!(
        recovery.count(),
        3,
        "and it says how many transactions are recoverable"
    );
    assert!(!recovery.truncated);

    let replayed = restarted
        .editor
        .documents
        .get_mut(document)
        .unwrap()
        .recover(&recovery)
        .unwrap();
    assert_eq!(replayed, 3);
    assert_eq!(
        restarted
            .editor
            .documents
            .get(document)
            .unwrap()
            .content()
            .node_count(),
        3
    );

    // And the attribution survived the round trip through the journal.
    let entry = &restarted
        .editor
        .documents
        .get(document)
        .unwrap()
        .history()
        .entries()[0];
    assert!(!entry.actor.is_agent());

    std::fs::remove_dir_all(&directory).unwrap();
}

/// The stub's "connected" line means the editor's `Hello` arrived, not merely that `accept`
/// returned.
///
/// `samples/05-editor-session/session.py` kills the runtime the moment it reads that line. When the
/// line was printed at `accept`, the kill could land between the editor's `connect` and its `Hello`:
/// the bridge's reader saw the end of stream first, `send(Hello)` returned the loss, and
/// `connect_hosted` failed. The editor then printed `cyberdyne-editor: the hosted runtime: it closed
/// the connection` on stderr before its script started and ran the act with no runtime at all, so
/// the session counted one summary too many and never surfaced "The hosted runtime stopped".
#[test]
fn the_stub_reports_a_connection_only_once_the_editors_hello_arrived() {
    use std::io::Write as _;
    use std::os::unix::net::UnixStream;
    use std::sync::mpsc;

    use cy_editor_protocol::{Message, write_frame};

    let directory = std::env::temp_dir().join(format!("cy-editor-hello-{}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let socket = directory.join("runtime.sock");

    let mut runtime = Command::new(stub_binary())
        .arg(&socket)
        .stdout(Stdio::piped())
        .spawn()
        .expect("the runtime stub starts");
    let stdout = runtime.stdout.take().expect("the stub's stdout was piped");
    let (lines_sender, lines) = mpsc::channel();
    std::thread::spawn(move || {
        for line in BufReader::new(stdout).lines() {
            let Ok(line) = line else { return };
            if lines_sender.send(line).is_err() {
                return;
            }
        }
    });
    assert_eq!(
        lines.recv_timeout(Duration::from_secs(10)).as_deref(),
        Ok("listening")
    );

    let mut stream = UnixStream::connect(&socket).expect("the stub accepts a connection");
    assert!(
        lines.recv_timeout(Duration::from_millis(500)).is_err(),
        "the stub said it was connected before any Hello reached it"
    );

    let hello = Message::Hello {
        abi_major: cy_editor_sdk::abi::MAJOR,
        abi_minor: cy_editor_sdk::abi::MINOR,
        editor: "the hello test".to_string(),
    };
    write_frame(&mut stream, &hello.encode()).expect("the Hello is written");
    stream.flush().unwrap();
    assert_eq!(
        lines.recv_timeout(Duration::from_secs(10)).as_deref(),
        Ok("connected"),
        "the stub reports the connection once the Hello arrived"
    );

    runtime.kill().unwrap();
    runtime.wait().unwrap();
    std::fs::remove_dir_all(&directory).unwrap();
}
