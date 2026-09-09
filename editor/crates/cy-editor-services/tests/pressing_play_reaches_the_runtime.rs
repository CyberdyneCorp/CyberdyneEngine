//! Pressing play, over a real socket, to a runtime that answers. M8.a task 5.1.
//!
//! --- WHAT WAS BROKEN, IN design.md §4's WORDS -----------------------------------------------------
//!
//! > Pressing play must simulate the world the editor authored. Today it reports `hosting:
//! > NoRuntime`.
//!
//! Concretely: `play.enter` called `ProjectHost::set_play`, which set
//! `cy_editor_viewport::play::PlayState` on every viewport and **told the runtime nothing**. The
//! badge said PLAYING and the world did not move. There was no `Message::Play` in the protocol, so
//! there was nowhere for the intent to go.
//!
//! This file drives the whole path over a **real Unix domain socket** with a runtime double on the
//! far end: the command, the registry, the session, the framing, the message set. The double is not
//! the engine — `cy::gameplay::PlaySession` is, and `src/gameplay/play/tests/test_play.cpp` holds it
//! to what it must do — but the CONTRACT is what this exercises, and nothing in the editor changes
//! when the real runtime is on the other end. `samples/05b-editor-window/runtime/main.cpp` already
//! is.
//!
//! --- THE CASE WORTH READING TWICE ------------------------------------------------------------------
//!
//! `an_editor_with_no_runtime_still_switches_its_badge_and_says_nothing_is_simulating`. An editor
//! with no engine attached is a first-class mode, so pressing play there must not be an error — but
//! it must not claim to be simulating either. Both halves are asserted, because getting one of them
//! is easy and getting the pair right is the requirement.

#![cfg(unix)]

use std::os::unix::net::UnixListener;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use cy_editor_commands::registry::{Arguments, Registry};
use cy_editor_commands::scope::Scope;
use cy_editor_core::Actor;
use cy_editor_protocol::{Message, SessionEvent};
use cy_editor_services::builtin;
use cy_editor_services::editor::Editor;
use cy_editor_services::{HostingMode, RuntimeSession};

/// What the double was asked for, in order, so a case can assert on the sequence rather than only
/// on the last one. Play is a state machine and the order is the thing that goes wrong.
type Asked = Arc<Mutex<Vec<String>>>;

/// A runtime that answers a play the way `cy_editor_window_runtime` does: with the state now in
/// force and a line for a person.
fn runtime_double(
    directory: &std::path::Path,
) -> (std::path::PathBuf, Asked, std::thread::JoinHandle<()>) {
    let path = directory.join("runtime.sock");
    let listener = UnixListener::bind(&path).expect("a listening socket");
    let asked: Asked = Arc::new(Mutex::new(Vec::new()));
    let recorded = Arc::clone(&asked);
    let handle = std::thread::spawn(move || {
        let Ok((stream, _)) = listener.accept() else {
            return;
        };
        let mut reader = stream.try_clone().expect("a reader");
        let mut writer = stream;
        let _ = cy_editor_protocol::server::serve(&mut reader, &mut writer, |message| {
            match message {
                Message::Hello { .. } => Some(vec![Message::Welcome {
                    abi_major: 1,
                    abi_minor: 1,
                    runtime: "play double".into(),
                }]),
                Message::Play { request, state } => {
                    // THE DOUBLE READS WHAT THE EDITOR SENT rather than answering blind, which is
                    // the difference between an end-to-end test and one end of one.
                    recorded.lock().expect("the record").push(state.clone());
                    let detail = match state.as_str() {
                        "playing" => "2 entities, 2 bodies, 2 colliders",
                        "editing" => "180 ticks, 2 placements restored, document identical",
                        "paused" => "paused at tick 180",
                        _ => "play: the states are editing, playing and paused",
                    }
                    .to_string();
                    // A state this build does not know leaves the session where it was, which is
                    // what `cy::gameplay::play_state_of` refusing it produces.
                    let now = if ["playing", "editing", "paused"].contains(&state.as_str()) {
                        state
                    } else {
                        "editing".to_string()
                    };
                    Some(vec![Message::Playing {
                        request,
                        state: now,
                        detail,
                    }])
                }
                _ => Some(Vec::new()),
            }
        });
    });
    (path, asked, handle)
}

fn scratch(name: &str) -> std::path::PathBuf {
    let directory =
        std::env::temp_dir().join(format!("cy-editor-play-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&directory);
    std::fs::create_dir_all(&directory).expect("a scratch directory");
    directory
}

fn registry() -> Registry {
    let mut registry = Registry::new();
    builtin::register(&mut registry).expect("the built-in commands satisfy their own metadata");
    registry
}

/// The runtime's answer to the request the editor just sent, or nothing.
fn answer(editor: &Editor) -> Option<(String, String)> {
    editor.runtime.block_until(Duration::from_secs(2), |event| {
        if let SessionEvent::Message(Message::Playing { state, detail, .. }) = event {
            Some((state.clone(), detail.clone()))
        } else {
            None
        }
    })
}

fn press(editor: &mut Editor, registry: &Registry, command: &str) -> String {
    editor
        .invoke(registry, command, &Scope::unrestricted(), &Arguments::new())
        .unwrap_or_else(|problem| panic!("{command} was refused: {problem:?}"))
        .summary
}

#[test]
fn pressing_play_asks_the_runtime_to_simulate_and_stopping_asks_it_to_restore() {
    let directory = scratch("simulate");
    let (path, asked, server) = runtime_double(&directory);
    let runtime = RuntimeSession::connect_hosted(&path).expect("a connected runtime");

    let mut editor = Editor::new(Actor::human("designer"));
    editor.runtime = runtime;
    assert_eq!(editor.hosting_mode(), HostingMode::Hosted);
    let registry = registry();

    // PLAY.
    let summary = press(&mut editor, &registry, "play.enter");
    assert!(summary.contains("PLAYING"), "{summary}");
    assert!(summary.contains("asked the runtime"), "{summary}");
    let (state, detail) = answer(&editor).expect("the runtime answered the play");
    assert_eq!(state, "playing");
    assert!(detail.contains("bodies"), "{detail}");

    // PAUSE.
    let summary = press(&mut editor, &registry, "play.pause");
    assert!(summary.contains("PAUSED"), "{summary}");
    let (state, _) = answer(&editor).expect("the runtime answered the pause");
    assert_eq!(state, "paused");

    // STOP, and the sentence a designer reads is the one that says the document is intact.
    let summary = press(&mut editor, &registry, "play.leave");
    assert!(summary.contains("EDIT"), "{summary}");
    let (state, detail) = answer(&editor).expect("the runtime answered the stop");
    assert_eq!(state, "editing");
    assert!(detail.contains("identical"), "{detail}");

    // THE ORDER IS WHAT WENT OVER THE WIRE, not what the editor believes it sent.
    assert_eq!(
        *asked.lock().expect("the record"),
        vec![
            "playing".to_string(),
            "paused".to_string(),
            "editing".to_string()
        ]
    );

    // DROPPED, NOT JOINED. The double's `serve` loop runs until its peer disconnects, and joining
    // it would make the test wait for a thread that is waiting for the test — which is exactly what
    // the first draft of this file did, and it hung. `picking_reaches_the_engine` drops for the
    // same reason.
    drop(editor);
    drop(server);
    let _ = std::fs::remove_dir_all(&directory);
}

#[test]
fn the_viewports_badge_and_the_runtime_are_switched_together() {
    let directory = scratch("badge");
    let (path, _asked, server) = runtime_double(&directory);
    let runtime = RuntimeSession::connect_hosted(&path).expect("a connected runtime");

    let mut editor = Editor::new(Actor::human("designer"));
    editor.runtime = runtime;
    let registry = registry();

    assert_eq!(
        editor.viewports.focused().play,
        cy_editor_viewport::play::PlayState::Editing
    );
    press(&mut editor, &registry, "play.enter");
    assert_eq!(
        editor.viewports.focused().play,
        cy_editor_viewport::play::PlayState::Playing
    );
    assert!(answer(&editor).is_some());

    // EVERY viewport, because play is a property of the runtime rather than of a panel: two
    // viewports showing different play states would be two runtimes.
    for viewport in editor.viewports.all().iter() {
        assert_eq!(viewport.play, cy_editor_viewport::play::PlayState::Playing);
    }

    // DROPPED, NOT JOINED. The double's `serve` loop runs until its peer disconnects, and joining
    // it would make the test wait for a thread that is waiting for the test — which is exactly what
    // the first draft of this file did, and it hung. `picking_reaches_the_engine` drops for the
    // same reason.
    drop(editor);
    drop(server);
    let _ = std::fs::remove_dir_all(&directory);
}

#[test]
fn an_editor_with_no_runtime_still_switches_its_badge_and_says_nothing_is_simulating() {
    // BOTH HALVES. `NoRuntime` is a first-class mode — `cy_editor_services::runtime`'s header
    // argues it at length — so pressing play with no engine attached must not be an error. But it
    // must not claim anything is simulating either, and a designer who saw PLAYING over a still
    // world with no explanation would reasonably think the engine had hung.
    let mut editor = Editor::new(Actor::human("designer"));
    assert_eq!(editor.hosting_mode(), HostingMode::NoRuntime);
    let registry = registry();

    let summary = press(&mut editor, &registry, "play.enter");
    assert!(summary.contains("PLAYING"), "{summary}");
    assert!(summary.contains("nothing is simulating"), "{summary}");
    assert_eq!(
        editor.viewports.focused().play,
        cy_editor_viewport::play::PlayState::Playing
    );

    // And it is said where a person will see it, not only in the return value a script reads.
    let mut cursor = cy_editor_core::observe::Cursor::default();
    let posted: Vec<String> = editor
        .notifications
        .drain_from(&mut cursor)
        .iter()
        .map(|notification| notification.message.clone())
        .collect();
    assert!(
        posted.iter().any(|message| message.contains("no runtime")),
        "{posted:?}"
    );
}

#[test]
fn a_state_the_runtime_does_not_know_leaves_it_where_it_was() {
    // The protocol carries the state as a WORD so that a fourth one added on one side is refused by
    // name rather than falling through a match to the closest number. This is that, from the
    // editor's end: the runtime answers with the state actually in force, which is not the one that
    // was asked for.
    let directory = scratch("unknown");
    let (path, asked, server) = runtime_double(&directory);
    let session = RuntimeSession::connect_hosted(&path).expect("a connected runtime");
    let request = session.play("rewinding").expect("the ask was sent");
    assert!(request.as_u64() > 0);

    let answered = session
        .block_until(Duration::from_secs(2), |event| {
            if let SessionEvent::Message(Message::Playing { state, detail, .. }) = event {
                Some((state.clone(), detail.clone()))
            } else {
                None
            }
        })
        .expect("the runtime answered");
    assert_eq!(answered.0, "editing");
    assert!(
        answered.1.contains("editing, playing and paused"),
        "{answered:?}"
    );
    assert_eq!(*asked.lock().expect("the record"), vec!["rewinding"]);

    drop(session);
    drop(server);
    let _ = std::fs::remove_dir_all(&directory);
}
