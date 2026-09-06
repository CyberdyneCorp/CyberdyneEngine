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
use cy_editor_core::value::Value;

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

    // The runtime dies. SIGKILL rather than a clean shutdown, because a crash is what is being
    // tested and a crash does not run a shutdown path.
    runtime.kill().expect("the runtime can be killed");
    runtime.wait().expect("and reaped");

    // The editor notices — on its own frame, without blocking on anything.
    let deadline = Instant::now() + Duration::from_secs(10);
    while application.editor.runtime.is_connected() && Instant::now() < deadline {
        application.pump();
        std::thread::sleep(Duration::from_millis(2));
    }
    application.pump();

    assert!(
        !application.editor.runtime.is_connected(),
        "the editor learned the runtime is gone"
    );

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
