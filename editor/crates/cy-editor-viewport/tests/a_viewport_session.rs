//! One editor interaction, end to end, against a runtime that behaves like a real one.
//!
//! The unit tests in each module check one property each. This checks that they compose, in the
//! order a person actually does them:
//!
//! 1. a frame arrives over the transport, carrying the view state it was rendered with;
//! 2. the user clicks, and the request names **that** frame;
//! 3. the runtime resolves the pick against what it drew and answers with stable identities;
//! 4. the editor maps them to nodes, applies its own filters, and selects;
//! 5. the user drags a gizmo — one transaction, computed from captured state, drawn locally with no
//!    round trip;
//! 6. the runtime's echo disagrees, because a constraint clamped the value, and the editor adopts
//!    the runtime's answer;
//! 7. undo puts everything back.
//!
//! The "runtime" here is a few lines below rather than a process, deliberately: what this test is
//! about is the editor's half of the protocol and the order the pieces run in.
//! `tests/frame_age.rs` is the one that needs a real boundary, because it measures one.

use cy_editor_core::Actor;
use cy_editor_core::ids::NodeId;
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::Document;
use cy_editor_documents::selection::Selection;
use cy_editor_protocol::FrameId;
use cy_editor_viewport::gizmo::{
    Drag, DragInput, DragRequest, GizmoRegistry, GizmoSpace, Handle, Pivot, Transform3,
    TransformBinding,
};
use cy_editor_viewport::math::{Quat, Vec3};
use cy_editor_viewport::picking::{
    DocumentFilter, Granularity, IdentityMap, PickCandidate, PickIntent, PickResolution,
    PickResponse, SelectionMode, apply,
};
use cy_editor_viewport::reconcile::{Prediction, Reconciler};
use cy_editor_viewport::snapping::SnapSettings;
use cy_editor_viewport::state::ViewState;
use cy_editor_viewport::transport::{
    FrameImage, Mailbox, MailboxTransport, PresentedFrame, SharedImage, TransportKind,
};
use cy_editor_viewport::viewport::{Viewport, ViewportId};

/// The document the session edits: one lamp, with a transform.
struct Project {
    document: Document,
    lamp: NodeId,
    binding: TransformBinding,
}

fn project() -> Project {
    let mut document = Document::new("worlds/city.cyworld");
    let component = document.schema_mut().declare_type("Transform", false);
    let translation = document
        .schema_mut()
        .declare_field(component, "translation", ValueKind::Vec3, "where it is")
        .expect("a fresh schema");
    let rotation = document
        .schema_mut()
        .declare_field(component, "rotation", ValueKind::Quat, "which way it faces")
        .expect("a fresh schema");
    let scale = document
        .schema_mut()
        .declare_field(component, "scale", ValueKind::Vec3, "how big it is")
        .expect("a fresh schema");
    let binding = TransformBinding {
        component,
        translation,
        rotation,
        scale,
    };
    let lamp = document
        .with_transaction("Place a lamp", Actor::human("designer"), |document| {
            let lamp = document.create_node(None)?;
            document.add_component(
                lamp,
                component,
                vec![
                    (translation, Value::Vec3([2.0, 0.0, 0.0])),
                    (rotation, Value::Quat(Quat::IDENTITY.to_array())),
                    (scale, Value::Vec3([1.0, 1.0, 1.0])),
                ],
            )?;
            Ok(lamp)
        })
        .expect("a transaction that creates one node");
    Project {
        document,
        lamp,
        binding,
    }
}

fn translation_of(project: &Project) -> [f32; 3] {
    match project
        .document
        .content()
        .field(
            project.lamp,
            project.binding.component,
            project.binding.translation,
        )
        .expect("the lamp has a translation")
    {
        Value::Vec3(lanes) => *lanes,
        other => panic!("expected a Vec3, got {other:?}"),
    }
}

/// The stable identity the runtime issued for the lamp.
const LAMP_IDENTITY: u64 = 0x1A_09;

#[test]
fn a_click_a_drag_and_a_disagreement() {
    let mut project = project();
    let registry = GizmoRegistry::with_builtins();
    let mut selection = Selection::new();
    let mut reconciler = Reconciler::new();

    let mut identities = IdentityMap::new();
    identities.insert(LAMP_IDENTITY, project.lamp);

    // --- 1. A frame arrives ---------------------------------------------------------------------
    //
    // Rendered from a camera twenty metres back. The editor's own camera moves afterwards, which is
    // what actually happens: the interface thread does not wait for the runtime.
    let mailbox = Mailbox::new();
    let mut transport = MailboxTransport::new(TransportKind::SharedTexture, mailbox.clone());
    let mut viewport = Viewport::new(
        ViewportId::from_raw(1),
        "Perspective",
        TransportKind::SharedTexture,
    );

    let mut rendered = ViewState::new();
    rendered.camera.position = Vec3::new(0.0, 0.0, 20.0);
    mailbox.publish(PresentedFrame::new(
        FrameId::from_raw(41),
        rendered.clone(),
        FrameImage::SharedTexture {
            handle: 7,
            image: SharedImage::unpadded(1920, 1080, 1, 3),
        },
        1_000,
    ));
    viewport.pump(&mut transport, 5_000);
    viewport.state.camera.position = Vec3::new(400.0, 0.0, 20.0);

    // --- 2. The user clicks, and the request names the frame that was on screen -------------------
    let cycle = viewport.click(960.0, 540.0);
    let request = viewport
        .pick(PickIntent::Click { x: 960.0, y: 540.0 })
        .expect("a frame is on screen");
    assert_eq!(
        request.frame,
        FrameId::from_raw(41),
        "the click was resolved against a newer frame than the one it happened on"
    );
    assert_eq!(cycle, 0, "the first click on a spot takes the nearest");

    // --- 3. The runtime answers, from what it drew ------------------------------------------------
    //
    // Over the wire and back, because the encoding is part of what is being checked.
    let decoded = cy_editor_viewport::picking::PickRequest::decode(&request.encode())
        .expect("our own encoding");
    let answer = runtime_pick(&decoded);
    let response = PickResponse::decode(&answer.encode()).expect("our own encoding");
    assert_eq!(response.frame, request.frame);

    // --- 4. The editor resolves and selects --------------------------------------------------------
    let filter = DocumentFilter::default();
    let resolution = PickResolution {
        identities: &identities,
        document: &project.document,
        filter: &filter,
        granularity: Granularity::Root,
    };
    let picked = resolution.resolve(&response, &decoded.intent, cycle);
    assert_eq!(picked, vec![project.lamp]);
    apply(&mut selection, SelectionMode::Replace, &picked);
    assert_eq!(selection.node_count(), 1);

    // --- 5. The drag: one transaction, no round trip ------------------------------------------------
    let before = translation_of(&project);
    let entries = project.document.history().entries().len();
    let nodes: Vec<NodeId> = selection.nodes().collect();
    let interaction_view = viewport.interaction_view().clone();
    assert!(
        interaction_view
            .camera
            .position
            .nearly_equals(Vec3::new(0.0, 0.0, 20.0), 1e-5),
        "the drag must use the frame's camera, not the editor's newer one"
    );

    let sent_on = drag_along_x(
        &mut project,
        &registry,
        &nodes,
        &interaction_view,
        &mut reconciler,
    );
    assert_eq!(
        project.document.history().entries().len(),
        entries + 1,
        "one drag, one entry"
    );
    let predicted = translation_of(&project);
    assert!(predicted[0] > before[0], "the lamp moved along X");

    // --- 6 and 7. The runtime disagrees, the editor adopts, and undo puts it all back --------------
    adopt_and_undo(&mut project, &mut reconciler, sent_on, before, predicted);
}

/// The runtime clamped the value; the editor adopts it, and then undoes the whole session.
///
/// The adoption goes through `with_transaction` like every other mutation, which is
/// `editor-documents-and-transactions`' invariant holding for a change the *runtime* originated:
/// there is no second write path for values the editor did not compute.
fn adopt_and_undo(
    project: &mut Project,
    reconciler: &mut Reconciler,
    sent_on: FrameId,
    before: [f32; 3],
    predicted: [f32; 3],
) {
    let clamped = Transform3 {
        translation: Vec3::new(6.0, 0.0, 0.0),
        ..current_transform(project)
    };
    let divergence = reconciler
        .observe(project.lamp, sent_on, clamped)
        .expect("the runtime clamped the value and said so");
    assert!(divergence.distance() > 0.0);
    assert!(
        reconciler.is_empty(),
        "and every prediction the drag made is settled"
    );

    let adopted = divergence.observed.translation.to_array();
    let lamp = project.lamp;
    let binding = project.binding;
    project
        .document
        .with_transaction(
            "Adopt the runtime's value",
            Actor::human("designer"),
            |document| {
                document.set_field(
                    lamp,
                    binding.component,
                    binding.translation,
                    Value::Vec3(adopted),
                )
            },
        )
        .expect("adopting is an ordinary edit");
    assert_eq!(
        translation_of(project).map(f32::to_bits),
        [6.0_f32, 0.0, 0.0].map(f32::to_bits)
    );

    project
        .document
        .undo()
        .expect("undo")
        .expect("the adoption");
    assert_eq!(
        translation_of(project).map(f32::to_bits),
        predicted.map(f32::to_bits)
    );
    project.document.undo().expect("undo").expect("the drag");
    assert_eq!(
        translation_of(project).map(f32::to_bits),
        before.map(f32::to_bits),
        "the whole drag undid in one step"
    );
}

/// Thirty frames of an axis drag, predicting each one. Returns the frame the last one was sent on.
///
/// Extracted so the session above reads as its seven steps rather than as a loop with a test around
/// it — and because a hundred-line test function is one nobody reads twice.
fn drag_along_x(
    project: &mut Project,
    registry: &GizmoRegistry,
    nodes: &[NodeId],
    view: &ViewState,
    reconciler: &mut Reconciler,
) -> FrameId {
    let binding = project.binding;
    let lamp = project.lamp;
    let mut drag = Drag::begin(
        registry,
        &mut project.document,
        &DragRequest {
            manipulator: "translate",
            handle: Handle::AxisX,
            space: GizmoSpace::World,
            pivot: Pivot::Individual,
            nodes,
            binding,
            view,
            pixel: (960.0, 540.0),
            actor: Actor::human("designer"),
            duplicate: false,
            bounds: None,
        },
    )
    .expect("a drag on the selected lamp");

    let snap = SnapSettings::default();
    let mut sent_on = FrameId::from_raw(41);
    for step in 1..=30_u16 {
        // The editor advances a frame per drag update, exactly as an interface thread does, and
        // predicts what it has just drawn. Nothing waits.
        sent_on = FrameId::from_raw(41 + u64::from(step));
        let pixel = (960.0 + f32::from(step) * 8.0, 540.0);
        drag.update(
            registry,
            &mut project.document,
            view,
            pixel,
            &snap,
            DragInput::NONE,
        )
        .expect("the drag continues");
        reconciler.predict(Prediction {
            frame: sent_on,
            node: lamp,
            transform: current_transform(project),
        });
    }
    assert!(
        drag.commit(&mut project.document).expect("it commits"),
        "the drag changed something and is one entry"
    );
    sent_on
}

#[test]
fn a_stalled_runtime_leaves_the_editor_interactive() {
    // The requirement's scenario, composed: the runtime stops, the viewport says the image is
    // stale, and every editor-side operation still works — navigation, capture, and the selection.
    // Nothing in this test can block, because nothing in the crate has a call that could.
    let mut viewport = Viewport::new(
        ViewportId::from_raw(1),
        "Perspective",
        TransportKind::EncodedStream,
    );
    let mailbox = Mailbox::new();
    let mut transport = MailboxTransport::new(TransportKind::EncodedStream, mailbox.clone());

    mailbox.publish(PresentedFrame::new(
        FrameId::from_raw(1),
        ViewState::new(),
        FrameImage::Encoded(vec![0; 1024]),
        1_000_000,
    ));
    viewport.pump(&mut transport, 1_002_000);
    assert!(viewport.advisory(1_002_000).is_none());

    // The runtime dies. Nothing more arrives.
    for now in [1_100_000, 1_500_000, 3_000_000] {
        viewport.pump(&mut transport, now);
    }
    let advisory = viewport.advisory(3_000_000).expect("the user is told");
    assert!(advisory.contains("stale"), "{advisory}");

    // And the editor is still an editor: the camera moves, a view is captured and restored, and the
    // frame the capture names is the last one that actually arrived.
    let navigator = viewport.navigator.clone();
    navigator.orbit(&mut viewport.state, 30.0, 10.0);
    navigator.pan(&mut viewport.state, 12.0, -4.0);
    let capture = viewport.capture(false);
    assert_eq!(capture.frame, 1);
    assert!(capture.represents_shipping_image());
    viewport.restore(&capture).expect("our own capture");
}

/// The runtime's half of a pick: resolve against what it drew, and answer with stable identities.
///
/// A stand-in for `cy::render::pick_ray`, which is what the hosted runtime calls. What is being
/// checked here is the editor's side of the exchange and the encoding between them; the engine's
/// resolution has its own suite in `src/servers/render/tests/test_picking.cpp`.
fn runtime_pick(request: &cy_editor_viewport::picking::PickRequest) -> PickResponse {
    assert!(
        request.filter.max_candidates > 0,
        "the editor should bound what it asks for"
    );
    PickResponse {
        frame: request.frame,
        candidates: vec![PickCandidate {
            identity: LAMP_IDENTITY,
            distance: 20.0,
            transparent: false,
        }],
    }
}

fn current_transform(project: &Project) -> Transform3 {
    Transform3 {
        translation: Vec3::from_array(translation_of(project)),
        ..Transform3::default()
    }
}
