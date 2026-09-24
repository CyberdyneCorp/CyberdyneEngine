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
//! `an_editor_with_no_runtime_keeps_its_editing_state_and_says_nothing_is_simulating`. An editor
//! with no engine attached is a first-class authoring mode, so pressing play there does not end the
//! session — and it does not display a play state no engine accepted.

#![cfg(unix)]

use std::os::unix::net::UnixListener;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use cy_editor_commands::registry::{Arguments, Registry};
use cy_editor_commands::scope::Scope;
use cy_editor_core::Actor;
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::Document;
use cy_editor_protocol::{Message, SessionEvent};
use cy_editor_services::builtin;
use cy_editor_services::editor::Editor;
use cy_editor_services::mirror::RuntimeMirror;
use cy_editor_services::{HostingMode, RuntimeSession};
use cy_editor_viewport::play::{PlayMode, PlayState};
use cy_editor_viewport::transport::TransportKind;
use cy_editor_viewport::viewport::{Viewport, ViewportId};

/// What the double was asked for, in order, so a case can assert on the sequence rather than only
/// on the last one. Play is a state machine and the order is the thing that goes wrong.
type Asked = Arc<Mutex<Vec<String>>>;

/// A runtime that answers a play the way `cy_editor_window_runtime` does: with the state now in
/// force and a line for a person.
fn runtime_double(
    directory: &std::path::Path,
) -> (
    std::path::PathBuf,
    Asked,
    Asked,
    Asked,
    std::thread::JoinHandle<()>,
) {
    let path = directory.join("runtime.sock");
    let listener = UnixListener::bind(&path).expect("a listening socket");
    let asked: Asked = Arc::new(Mutex::new(Vec::new()));
    let recorded = Arc::clone(&asked);
    // And the MODE each ask carried. M11.b task 3.1: the mode is a second word on the wire, and a
    // double that recorded only the state could not tell an editor that sends the mode from one
    // that does not.
    let asked_modes: Asked = Arc::new(Mutex::new(Vec::new()));
    let modes = Arc::clone(&asked_modes);
    let asked_when: Asked = Arc::new(Mutex::new(Vec::new()));
    let scheduling = Arc::clone(&asked_when);
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
                Message::Play {
                    request,
                    state,
                    mode,
                } => {
                    // THE DOUBLE READS WHAT THE EDITOR SENT rather than answering blind, which is
                    // the difference between an end-to-end test and one end of one.
                    recorded.lock().expect("the record").push(state.clone());
                    modes.lock().expect("the record").push(mode.clone());
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
                        // ANSWERED WITH THE MODE THAT WAS ASKED FOR, never another one. A runtime
                        // that could not honour the mode refuses the request; answering with a
                        // different mode would be the silent fallback `specs/live-editing/` forbids,
                        // and the editor would have no way to tell it from success.
                        mode,
                        detail,
                    }])
                }
                // M11.b task 3.2: WHEN an edit was scheduled for. Recorded on the wire rather than
                // read back out of the mirror, because the mirror deciding and the mirror reporting
                // are the same object and a test of both would be a test of neither.
                Message::Apply { request, when, .. } => {
                    scheduling
                        .lock()
                        .expect("the record")
                        .push(format!("{when:?}"));
                    Some(vec![Message::Applied {
                        request,
                        frame: cy_editor_protocol::FrameId::from_raw(1),
                        observed: Vec::new(),
                    }])
                }
                Message::SyncWorld { request, world } => {
                    let snapshot = String::from_utf8(world).expect("the authored world is text");
                    assert!(snapshot.contains("type 1 runtime \"Transform\""));
                    assert!(snapshot.contains("node 0 -"));
                    scheduling
                        .lock()
                        .expect("the record")
                        .push("SyncWorld".into());
                    Some(vec![Message::Applied {
                        request,
                        frame: cy_editor_protocol::FrameId::from_raw(1),
                        observed: Vec::new(),
                    }])
                }
                _ => Some(Vec::new()),
            }
        });
    });
    (path, asked, asked_modes, asked_when, handle)
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
    let (path, asked, modes, _when, server) = runtime_double(&directory);
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
    // AND THE MODE RODE WITH EVERY ONE OF THEM. M11.b task 3.1: the mode is carried on every play
    // message rather than only on the one that enters play, because a pause or a stop that omitted
    // it would leave the two ends disagreeing about which machine is simulating. `in-editor` is the
    // default these presses did not override.
    assert_eq!(
        *modes.lock().expect("the record"),
        vec![
            "in-editor".to_string(),
            "in-editor".to_string(),
            "in-editor".to_string()
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
    let (path, _asked, _modes, _when, server) = runtime_double(&directory);
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
fn an_editor_with_no_runtime_keeps_its_editing_state_and_says_nothing_is_simulating() {
    // `NoRuntime` is a first-class authoring mode, but PLAYING is not: it says an engine accepted
    // the request and is simulating. A disconnected editor stays in Editing and gives the person
    // the remedy instead of presenting a contradictory badge.
    let mut editor = Editor::new(Actor::human("designer"));
    assert_eq!(editor.hosting_mode(), HostingMode::NoRuntime);
    let registry = registry();

    let summary = press(&mut editor, &registry, "play.enter");
    assert!(summary.contains("unchanged (editing)"), "{summary}");
    assert!(summary.contains("nothing is simulating"), "{summary}");
    assert_eq!(
        editor.viewports.focused().play,
        cy_editor_viewport::play::PlayState::Editing
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
    let (path, asked, modes, _when, server) = runtime_double(&directory);
    let session = RuntimeSession::connect_hosted(&path).expect("a connected runtime");
    let request = session
        .play("rewinding", PlayMode::InEditor)
        .expect("the ask was sent");
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
    assert_eq!(*modes.lock().expect("the record"), vec!["in-editor"]);

    drop(session);
    drop(server);
    let _ = std::fs::remove_dir_all(&directory);
}

// --- M11.b: the mode is on the wire, and an edit reaches a PLAYING world ---------------------------

#[test]
fn the_mode_is_carried_on_every_play_message_and_an_unknown_one_is_refused() {
    // `specs/live-editing/` (M11.b), first scenario: *"WHEN a play mode that has no implementation
    // on this configuration is selected THEN the request SHALL fail naming the mode and the reason,
    // and no other mode SHALL start."* Both halves are checked, and the second is the one that
    // catches a silent fallback: the double records what crossed the socket, so an editor that
    // "refused" and sent the default anyway fails here rather than passing on its own message.
    let directory = scratch("modes");
    let (path, asked, modes, _when, server) = runtime_double(&directory);
    let runtime = RuntimeSession::connect_hosted(&path).expect("a connected runtime");

    let mut editor = Editor::new(Actor::human("designer"));
    editor.runtime = runtime;
    let registry = registry();

    // A mode this build does not know: refused, and NOTHING crosses the wire.
    let arguments = Arguments::new().with("mode", Value::Text("console-over-the-road".to_string()));
    let refusal = editor
        .invoke(&registry, "play.enter", &Scope::unrestricted(), &arguments)
        .expect_err("a mode this build does not know is refused");
    let said = format!("{refusal:?}");
    assert!(said.contains("console-over-the-road"), "{said}");
    assert!(said.contains("in-editor"), "{said}");
    assert!(
        asked.lock().expect("the record").is_empty(),
        "a refused mode must not reach the runtime: {:?}",
        asked.lock().expect("the record")
    );

    // And a mode it does know: the word crosses the socket beside the state.
    let arguments = Arguments::new().with("mode", Value::Text("separate-process".to_string()));
    let summary = editor
        .invoke(&registry, "play.enter", &Scope::unrestricted(), &arguments)
        .expect("separate-process is available")
        .summary;
    assert!(summary.contains("SEPARATE PROCESS"), "{summary}");
    // Waited for rather than read straight away: the double is a thread, and reading its record
    // before it has answered would be a race that passes when the machine is slow.
    let (state, _) = answer(&editor).expect("the runtime answered the play");
    assert_eq!(state, "playing");
    assert_eq!(*asked.lock().expect("the record"), vec!["playing"]);
    assert_eq!(*modes.lock().expect("the record"), vec!["separate-process"]);

    drop(editor);
    drop(server);
    let _ = std::fs::remove_dir_all(&directory);
}

#[test]
fn an_edit_made_while_the_world_is_playing_is_scheduled_for_a_tick_boundary() {
    // M11.b task 3.2, and the defect it closes is larger than a scheduling tag.
    //
    // `RuntimeMirror::send` hard-coded `ApplyWhen::OnArrival` with a comment beside it saying *"a
    // playing world would want `AtTickBoundary`, and the editor is what knows which"*. The
    // consequence: `AtTickBoundary` was constructed in exactly one place in the entire tree — a
    // protocol round-trip unit test — so **no edit had ever reached a playing world**. There was no
    // live editing to give a policy to.
    //
    // The scheduling is read off the WIRE, from the double, rather than from the mirror's own
    // counters: a mirror that decided and reported would agree with itself by construction.
    let directory = scratch("tickboundary");
    let (path, _asked, _modes, when, server) = runtime_double(&directory);
    let runtime = RuntimeSession::connect_hosted(&path).expect("a connected runtime");

    let mut document = Document::new("worlds/city.cyworld");
    let component = document.schema_mut().declare_type("Transform", false);
    let translation = document
        .schema_mut()
        .declare_field(component, "translation", ValueKind::Vec3, "where it is")
        .expect("a fresh schema");

    let mut viewport = Viewport::new(
        ViewportId::from_raw(1),
        "Perspective",
        TransportKind::LocalSurface,
    );
    let mut mirror = RuntimeMirror::new();

    // A FIRST LOOK AT A DOCUMENT ONLY BASELINES IT. `forward`'s own comment: forwarding entry three
    // of a document the runtime has never seen would move an object that is not there. So the first
    // sync sends nothing, and the edits below are what the mirror actually forwards.
    viewport.play = PlayState::Editing;
    mirror.sync(&runtime, Some(&document), &viewport, Vec::new());
    assert_eq!(mirror.forwarded_transactions(), 0);

    // AUTHORING: the world is not simulating, so the change applies on arrival.
    let place = |document: &mut Document, label: &'static str, y: f32| {
        document
            .with_transaction(label, Actor::human("designer"), |document| {
                let node = document.create_node(None)?;
                document.add_component(
                    node,
                    component,
                    vec![(translation, Value::Vec3([2.0, y, 0.0]))],
                )?;
                Ok(())
            })
            .expect("the transaction commits");
    };
    place(&mut document, "Place a lamp", 0.0);
    mirror.sync(&runtime, Some(&document), &viewport, Vec::new());
    assert_eq!(mirror.forwarded_transactions(), 1);
    assert_eq!(mirror.forwarded_at_a_tick_boundary(), 0);

    // AND THEN PLAYING: the same mirror, the same document, one more edit — and it is scheduled for
    // a tick boundary instead, because a change landing halfway through a simulation step is a
    // world that was never in either state.
    viewport.play = PlayState::Playing;
    place(&mut document, "Place a crate", 1.0);
    mirror.sync(&runtime, Some(&document), &viewport, Vec::new());
    assert_eq!(mirror.forwarded_transactions(), 2);
    assert_eq!(mirror.forwarded_at_a_tick_boundary(), 1);

    // AND THE WIRE AGREES. Two applies, scheduled differently, in that order. Waited for, because
    // the double is a thread.
    let deadline = std::time::Instant::now() + Duration::from_secs(2);
    loop {
        if when.lock().expect("the record").len() >= 2 || std::time::Instant::now() > deadline {
            break;
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    let scheduling = when.lock().expect("the record").clone();
    assert_eq!(
        scheduling,
        vec!["OnArrival".to_string(), "AtTickBoundary".to_string()],
        "the scheduling that crossed the socket"
    );

    drop(mirror);
    drop(runtime);
    drop(server);
    let _ = std::fs::remove_dir_all(&directory);
}

#[test]
fn a_new_schema_is_sent_again_after_the_runtime_reconnects_to_an_empty_file() {
    let directory = scratch("schema-reconnect");
    let (path, _asked, _modes, messages, server) = runtime_double(&directory);
    let runtime = RuntimeSession::connect_hosted(&path).expect("a connected runtime");
    let mut document = Document::new("worlds/city.cyworld");
    let viewport = Viewport::new(
        ViewportId::from_raw(1),
        "Perspective",
        TransportKind::LocalSurface,
    );
    let mut mirror = RuntimeMirror::new();
    mirror.sync(&runtime, Some(&document), &viewport, Vec::new());

    let transform = document.schema_mut().declare_type("Transform", false);
    let translation = document
        .schema_mut()
        .declare_field(transform, "translation", ValueKind::Vec3, "position")
        .unwrap();
    document
        .with_transaction("Place a tree", Actor::human("designer"), |document| {
            let tree = document.create_node(None)?;
            document.add_component(
                tree,
                transform,
                vec![(translation, Value::Vec3([1.0, 0.0, 0.0]))],
            )?;
            Ok(())
        })
        .unwrap();
    mirror.sync(&runtime, Some(&document), &viewport, Vec::new());
    mirror.runtime_restarted();
    mirror.sync(&runtime, Some(&document), &viewport, Vec::new());

    let deadline = std::time::Instant::now() + Duration::from_secs(2);
    while messages.lock().unwrap().len() < 2 && std::time::Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(5));
    }
    assert_eq!(
        *messages.lock().unwrap(),
        vec!["SyncWorld".to_string(), "SyncWorld".to_string()]
    );
    assert_eq!(mirror.forwarded_transactions(), 0);
    drop(runtime);
    drop(server);
    std::fs::remove_dir_all(directory).unwrap();
}
