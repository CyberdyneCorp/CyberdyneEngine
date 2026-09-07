//! A click and a gizmo, answered by a runtime in another process. M6 tasks 2.6 and 2.7, end to end.
//!
//! M5.5's ledger recorded this as unreachable: *"Picking has no wire: `cy-editor-protocol` carries
//! no pick message and the SDK has no call, so `pick` produces a `PickRequest` nothing answers."*
//! This file is the wire, driven over a **real Unix domain socket** with a runtime double on the
//! other end — the framing, the message set and the encodings are all exercised, and a disagreement
//! between the two ends is a decode error with a tag number in it rather than a hang.
//!
//! The double is not the engine and does not pretend to be. What it stands for is the contract:
//! given a pixel and the identifier of a frame it rendered, it answers with ordered candidates named
//! by stable identity. `src/servers/render/picking.h` is what will implement it; nothing in the
//! editor changes when it does, which is the property this test is really holding.

#![cfg(unix)]

use std::os::unix::net::UnixListener;
use std::time::Duration;

use cy_editor_core::Actor;
use cy_editor_core::value::ValueKind;
use cy_editor_documents::Document;
use cy_editor_documents::selection::Selection;
use cy_editor_protocol::{FrameId, Message, SessionEvent};
use cy_editor_services::RuntimeSession;
use cy_editor_services::gizmo::{self, Request as GizmoRequest};
use cy_editor_services::picking;
use cy_editor_viewport::gizmo::Handle;
use cy_editor_viewport::layout::{GizmoLayout, HandleSpot};
use cy_editor_viewport::picking::{
    DocumentFilter, Granularity, IdentityMap, PickCandidate, PickIntent, PickRequest,
    PickResolution, PickResponse, SelectionMode,
};
use cy_editor_viewport::transport::{
    FrameImage, Mailbox, MailboxTransport, PresentedFrame, TransportKind,
};
use cy_editor_viewport::viewport::{Viewport, ViewportId};

/// The identity the double reports under the pointer.
const DRAWN: u64 = 4242;
/// The frame the viewport is showing when the click happens.
const SHOWN: u64 = 1016;

/// A runtime that resolves a pick, over a real socket. Returns the path it is listening on.
fn runtime_double(
    directory: &std::path::Path,
) -> (std::path::PathBuf, std::thread::JoinHandle<()>) {
    let path = directory.join("runtime.sock");
    let listener = UnixListener::bind(&path).expect("a listening socket");
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
                    runtime: "picking double".into(),
                }]),
                Message::Pick {
                    request,
                    frame,
                    pick,
                } => {
                    // THE DOUBLE DECODES WHAT THE EDITOR SENT. A double that answered without
                    // reading the request would pass whatever the editor happened to encode, which
                    // is the failure mode of every "end to end" test that is really one end.
                    let decoded = PickRequest::decode(&pick).expect("a request this build knows");
                    assert_eq!(
                        decoded.frame, frame,
                        "the frame on the envelope is the frame in the request"
                    );
                    assert!(
                        matches!(decoded.intent, PickIntent::Click { .. }),
                        "the runtime is told what the pointer did"
                    );
                    Some(vec![Message::Picked {
                        request,
                        candidates: PickResponse {
                            frame,
                            candidates: vec![PickCandidate {
                                identity: DRAWN,
                                distance: 7.5,
                                transparent: false,
                            }],
                        }
                        .encode(),
                    }])
                }
                Message::GizmoIntent {
                    request,
                    viewport,
                    intent,
                } => {
                    // The engine draws the gizmo and says where it drew it. The double asserts that
                    // what arrived is INTENT — a frame, a mode, a pivot and what is selected — and
                    // answers with geometry, which is the division
                    // `editor-viewport-and-gizmos` sets and which nothing carried before M6.
                    let asked = GizmoRequest::decode(&intent).expect("an intent this build knows");
                    assert_eq!(viewport, 1, "the gizmo is for a named viewport");
                    assert_eq!(asked.frame.as_u64(), SHOWN);
                    Some(vec![Message::GizmoGeometry {
                        request,
                        layout: GizmoLayout {
                            frame: asked.frame,
                            mode: asked.mode,
                            centre: (640.0, 360.0),
                            extent: 90.0,
                            spots: vec![HandleSpot {
                                handle: Handle::AxisX,
                                x: 730.0,
                                y: 360.0,
                                radius: 6.0,
                                depth: 4.0,
                            }],
                        }
                        .encode(),
                    }])
                }
                _ => Some(Vec::new()),
            }
        });
    });
    (path, handle)
}

fn scratch(name: &str) -> std::path::PathBuf {
    let directory =
        std::env::temp_dir().join(format!("cy-editor-pick-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&directory);
    std::fs::create_dir_all(&directory).expect("a scratch directory");
    directory
}

/// A viewport that has been told a frame arrived, which is what makes a pick possible at all.
fn showing_a_frame() -> Viewport {
    let mut viewport = Viewport::new(
        ViewportId::from_raw(1),
        "Perspective",
        TransportKind::SharedTexture,
    );
    let mailbox = Mailbox::new();
    mailbox.publish(PresentedFrame::new(
        FrameId::from_raw(SHOWN),
        viewport.state.clone(),
        FrameImage::Surface(0),
        0,
    ));
    let mut transport = MailboxTransport::new(TransportKind::SharedTexture, mailbox);
    viewport.pump(&mut transport, 1_000);
    viewport
}

fn a_document() -> (Document, cy_editor_core::ids::NodeId) {
    let mut document = Document::new("worlds/city.cyworld");
    let marker = document.schema_mut().declare_type("Marker", false);
    document
        .schema_mut()
        .declare_field(marker, "name", ValueKind::Text, "what it is called")
        .unwrap();
    let node = document
        .with_transaction("Build", Actor::human("designer"), |document| {
            document.create_node(None)
        })
        .unwrap();
    (document, node)
}

#[test]
fn a_click_is_resolved_by_the_runtime_and_becomes_the_selection() {
    let directory = scratch("resolved");
    let (path, server) = runtime_double(&directory);
    let runtime = RuntimeSession::connect_hosted(&path).expect("a connected runtime");

    let viewport = showing_a_frame();
    let request = picking::request(
        &runtime,
        &viewport,
        PickIntent::Click { x: 640.0, y: 360.0 },
    )
    .expect("a pick names the frame the user was looking at");

    let reply = runtime
        .block_until(Duration::from_secs(5), |event| match event {
            SessionEvent::Message(Message::Picked {
                request: answered,
                candidates,
            }) if *answered == request => Some(candidates.clone()),
            _ => None,
        })
        .expect("the runtime answers the request it was given");

    let (document, node) = a_document();
    let mut identities = IdentityMap::new();
    identities.insert(DRAWN, node);
    let filter = DocumentFilter::default();
    let resolution = PickResolution {
        identities: &identities,
        document: &document,
        filter: &filter,
        granularity: Granularity::Instance,
    };
    let mut selection = Selection::new();
    let resolved = picking::resolve(
        &reply,
        &resolution,
        &PickIntent::Click { x: 640.0, y: 360.0 },
        0,
        &mut selection,
        SelectionMode::Replace,
    )
    .expect("an answer this build understands");

    assert_eq!(resolved.candidates, 1);
    assert_eq!(resolved.selected, vec![node]);
    assert_eq!(selection.nodes().collect::<Vec<_>>(), vec![node]);

    // The double is left to be reaped with the process. Joining it would mean waiting for a
    // blocking read on a socket the operating system closes when this process exits — a test that
    // hangs rather than fails, which is the worst kind. `serve` returns when the peer goes away.
    drop(runtime);
    drop(server);
    let _ = std::fs::remove_dir_all(&directory);
}

#[test]
fn a_pick_with_no_runtime_is_refused_and_the_editor_keeps_working() {
    let runtime = RuntimeSession::none();
    let viewport = showing_a_frame();
    let problem = picking::request(&runtime, &viewport, PickIntent::Click { x: 1.0, y: 1.0 })
        .expect_err("there is no engine to resolve it");
    assert!(problem.because.contains("no runtime"), "{problem:?}");
    assert!(
        problem.remedy.is_some(),
        "an editor with no runtime is a mode, and the refusal says what to do about it"
    );
}

#[test]
fn the_engine_draws_the_gizmo_and_the_editor_hit_tests_what_it_drew() {
    // M5.5's third recorded gap: "gizmo geometry is the ENGINE's, per editor-viewport-and-gizmos,
    // and nothing publishes it." Here something does, over the same socket, and the editor's only
    // computation is the hit test against what came back.
    let directory = scratch("gizmo");
    let (path, server) = runtime_double(&directory);
    let runtime = RuntimeSession::connect_hosted(&path).expect("a connected runtime");

    let viewport = showing_a_frame();
    let asked = gizmo::request(&runtime, &viewport, vec![DRAWN]).expect("the intent is sent");

    let published = runtime
        .block_until(Duration::from_secs(5), |event| match event {
            SessionEvent::Message(Message::GizmoGeometry {
                request: answered,
                layout,
            }) if *answered == asked.request => Some(layout.clone()),
            _ => None,
        })
        .expect("the runtime publishes the geometry it drew");

    let layout = gizmo::accept(&published, &viewport).expect("it belongs to the frame on screen");
    assert_eq!(layout.frame.as_u64(), SHOWN);
    assert_eq!(
        layout.hit(732.0, 361.0),
        Some(Handle::AxisX),
        "a click near the arrow the engine drew acquires that arrow"
    );
    assert_eq!(
        layout.hit(100.0, 100.0),
        None,
        "and a click nowhere near it acquires nothing"
    );

    drop(runtime);
    drop(server);
    let _ = std::fs::remove_dir_all(&directory);
}
