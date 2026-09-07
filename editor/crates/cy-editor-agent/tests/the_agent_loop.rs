//! Observation, attribution, conflict and budget, as tests. M5.5 tasks 3.4 and 3.9 through 3.12.
//!
//! The M5 suite beside this one proves the projection: an agent's invocation is a human's
//! invocation. These are the claims M5.5 adds, and every one of them is again about a join — the
//! agent's image is the human's viewport's image, the agent's history entry is in the human's
//! history, and the human's edit is the one that stands.

use cy_editor_agent::budget::Budget;
use cy_editor_agent::conflict::{Claim, Conflict};
use cy_editor_agent::observe::{ObservationKind, ViewportRequest};
use cy_editor_agent::resource::ResourceKind;
use cy_editor_agent::session::{AgentIdentity, AgentSession, RefuseEverything};
use cy_editor_commands::metadata::EffectClass;
use cy_editor_commands::registry::{Arguments, Registry};
use cy_editor_commands::scope::{DocumentScope, Scope};
use cy_editor_core::Actor;
use cy_editor_core::ids::NodeId;
use cy_editor_core::value::Value;
use cy_editor_services::builtin;
use cy_editor_services::editor::Editor;
use cy_editor_viewport::state::ViewState;
use cy_editor_viewport::transport::{
    Degradation, FrameImage, MailboxTransport, PresentedFrame, TransportKind,
};
use cy_editor_viewport::viewmode::ViewMode;
use cy_editor_viewport::viewport::ViewportId;

/// The eight bytes of a PNG header, which is all the media-type sniff reads.
const PNG: [u8; 8] = [0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a];

fn registry() -> Registry {
    let mut registry = Registry::new();
    builtin::register(&mut registry).expect("the built-in commands satisfy their own metadata");
    registry
}

fn session(scope: Scope) -> AgentSession {
    AgentSession::new(
        AgentIdentity {
            agent: "claude".to_string(),
            session: "s-11".to_string(),
        },
        "raise the streetlights to 4 m",
        scope,
        Budget::default(),
        "r-1",
        0,
    )
}

fn editor_with_a_document() -> (Editor, cy_editor_core::ids::DocumentId) {
    let mut editor = Editor::new(Actor::human("designer"));
    let id = editor
        .open_document("worlds/city.cyworld")
        .expect("a document opens");
    (editor, id)
}

/// Deliver one frame into a viewport, as the runtime's transport would.
fn deliver(editor: &mut Editor, viewport: ViewportId, frame: u64, image: FrameImage) {
    let mailbox = cy_editor_viewport::transport::Mailbox::new();
    let mut state = ViewState::new();
    state.viewport.width = 640;
    state.viewport.height = 360;
    mailbox.publish(PresentedFrame::new(
        cy_editor_protocol::FrameId::from_raw(frame),
        state,
        image,
        0,
    ));
    let mut transport = MailboxTransport::new(TransportKind::SharedTexture, mailbox);
    editor
        .viewports
        .all_mut()
        .get_mut(viewport)
        .expect("the viewport is open")
        .pump(&mut transport, 0);
}

#[test]
fn an_observation_with_no_frame_says_so_rather_than_inventing_one() {
    // `design.md` §2: the viewport "shows a message saying so — not an approximation", and an agent
    // is owed the same honesty more strongly, because it cannot see that an image is a placeholder.
    let (mut editor, _) = editor_with_a_document();
    let mut connection = session(Scope::unrestricted());
    let focused = editor.viewports.focused_id();
    let mut request = ViewportRequest::shipping_frame(editor.viewports.focused());
    request.viewport = focused;

    let refused = connection
        .observe(&mut editor, &request, 0)
        .expect_err("no runtime has rendered anything");
    assert!(
        refused.because.contains("no frame has arrived"),
        "{refused}"
    );
    assert!(
        refused
            .remedy
            .as_deref()
            .unwrap()
            .contains("rather than an approximation"),
        "{refused}"
    );
}

#[test]
fn a_shipping_frame_request_gets_an_image_with_nothing_drawn_over_it() {
    // "WHEN an agent requests an image representing the shipping frame THEN gizmos, selection
    // outlines and overlays SHALL be excluded." The editor's own viewport has overlays on by
    // default, so the request is answered from the connection's own viewport with them cleared —
    // which leaves the person's view exactly as they left it.
    let (mut editor, _) = editor_with_a_document();
    let human_viewport = editor.viewports.focused_id();
    let human_overlays = editor.viewports.focused().overlays.active();
    assert!(
        !human_overlays.is_empty(),
        "the human's viewport draws overlays; that is the case this test is about"
    );

    let mut connection = session(Scope::unrestricted());
    let mut request = ViewportRequest::shipping_frame(editor.viewports.focused());
    request.viewport = human_viewport;

    // The first attempt opens the agent's own viewport and finds nothing in it yet.
    let _ = connection.observe(&mut editor, &request, 0);
    let agent_viewport = editor
        .viewports
        .all()
        .iter()
        .find(|viewport| viewport.name == cy_editor_agent::observe::AGENT_VIEWPORT)
        .map(|viewport| viewport.id)
        .expect("the connection opened its own viewport");
    assert_ne!(agent_viewport, human_viewport);
    deliver(
        &mut editor,
        agent_viewport,
        7,
        FrameImage::Encoded(PNG.to_vec()),
    );

    let observation = connection
        .observe(&mut editor, &request, 0)
        .expect("a frame has arrived");
    assert_eq!(observation.kind, ObservationKind::ShippingFrame);
    assert!(!observation.includes_overlays);
    assert!(observation.represents_the_shipping_frame());
    assert_eq!(observation.media_type(), "image/png");
    assert_eq!(observation.frame.as_u64(), 7);
    assert_eq!(
        editor.viewports.focused().overlays.active(),
        human_overlays,
        "looking did not disturb the person's viewport"
    );
}

#[test]
fn an_image_with_the_editor_drawn_on_it_says_so_rather_than_claiming_to_be_the_frame() {
    // The honesty flag. An agent evaluating a lighting change against an image with a selection
    // outline in it and never finding out is the failure this exists to prevent.
    let (mut editor, _) = editor_with_a_document();
    let focused = editor.viewports.focused_id();
    deliver(&mut editor, focused, 3, FrameImage::Encoded(PNG.to_vec()));

    let mut connection = session(Scope::unrestricted());
    let mut request = ViewportRequest::shipping_frame(editor.viewports.focused());
    request.viewport = focused;
    request.include_overlays = true;

    let observation = connection
        .observe(&mut editor, &request, 0)
        .expect("a frame has arrived");
    assert_eq!(observation.kind, ObservationKind::EditorFrame);
    assert!(observation.includes_overlays);
    assert!(!observation.represents_the_shipping_frame());
    assert!(
        observation.describe().contains("overlays in"),
        "{}",
        observation.describe()
    );
}

#[test]
fn a_debug_view_is_reported_as_one_and_not_as_the_project_s_appearance() {
    // "The interface SHALL also be able to return a depth, normal, or debug visualisation view ...
    // because 'why is this dark' is answerable from a buffer and not from a colour image."
    let (mut editor, _) = editor_with_a_document();
    let mut connection = session(Scope::unrestricted());
    let request = ViewportRequest::debug_view(editor.viewports.focused(), ViewMode::Normals);

    let _ = connection.observe(&mut editor, &request, 0);
    let agent_viewport = editor
        .viewports
        .all()
        .iter()
        .find(|viewport| viewport.name == cy_editor_agent::observe::AGENT_VIEWPORT)
        .map(|viewport| viewport.id)
        .expect("the connection opened its own viewport");
    assert_eq!(
        editor
            .viewports
            .all()
            .get(agent_viewport)
            .expect("open")
            .state
            .view_mode,
        ViewMode::Normals,
        "the request's view mode reached the viewport the runtime renders"
    );

    deliver(
        &mut editor,
        agent_viewport,
        11,
        FrameImage::Encoded(PNG.to_vec()),
    );
    let observation = connection
        .observe(&mut editor, &request, 0)
        .expect("a frame has arrived");
    assert_eq!(
        observation.kind,
        ObservationKind::DebugView(ViewMode::Normals)
    );
    assert!(!observation.represents_the_shipping_frame());
}

#[test]
fn a_device_image_is_not_pretended_to_be_bytes() {
    // A shared texture cost no copy to deliver, which is the whole point of it. Reporting it as an
    // empty image would be the substituted representation the requirement forbids; reporting what
    // it is lets an agent ask for an encoded frame instead.
    let (mut editor, _) = editor_with_a_document();
    let focused = editor.viewports.focused_id();
    deliver(&mut editor, focused, 5, FrameImage::Surface(1));
    let mut connection = session(Scope::unrestricted());
    let mut request = ViewportRequest::shipping_frame(editor.viewports.focused());
    request.viewport = focused;
    request.include_overlays = true;

    let observation = connection
        .observe(&mut editor, &request, 0)
        .expect("a frame has arrived");
    assert!(observation.bytes().is_none());
    assert_eq!(
        observation.media_type(),
        "image/x-cyberdyne-device-image",
        "the image is on the device, and saying so is better than an empty buffer"
    );
    assert_eq!(observation.degraded, Degradation::None);
}

#[test]
fn looking_costs_the_render_budget_and_the_refusal_says_what_is_left() {
    // Task 3.12. A render costs a frame of the editor's own budget; an invocation usually costs
    // nothing, which is why they have separate ceilings.
    let (mut editor, _) = editor_with_a_document();
    let focused = editor.viewports.focused_id();
    deliver(&mut editor, focused, 1, FrameImage::Encoded(PNG.to_vec()));

    let mut connection = AgentSession::new(
        AgentIdentity {
            agent: "claude".to_string(),
            session: "s-12".to_string(),
        },
        "look repeatedly",
        Scope::unrestricted(),
        Budget {
            renders_per_window: 2,
            ..Budget::default()
        },
        "r-1",
        0,
    );
    let mut request = ViewportRequest::shipping_frame(editor.viewports.focused());
    request.viewport = focused;
    request.include_overlays = true;

    connection.observe(&mut editor, &request, 0).expect("one");
    connection.observe(&mut editor, &request, 0).expect("two");
    let refused = connection
        .observe(&mut editor, &request, 0)
        .expect_err("three is over the ceiling");
    let remedy = refused.remedy.unwrap();
    assert!(remedy.contains("2 of 2 renders"), "{remedy}");
    assert!(remedy.contains("resets in"), "{remedy}");
}

#[test]
fn the_budget_is_a_resource_the_agent_can_read_without_spending_anything() {
    // "SHALL report those limits to the agent rather than failing opaquely" — reported where an
    // agent already looks, rather than only in the refusal it gets after it is too late.
    let (editor, _) = editor_with_a_document();
    let connection = session(Scope::unrestricted());

    let listing = connection.resources(&editor);
    assert!(
        listing
            .iter()
            .any(|(uri, kind, _)| uri == "budget:" && *kind == ResourceKind::Budget)
    );
    let budget = connection
        .read_resource(&editor, "budget:", 0)
        .expect("the budget reads");
    assert!(
        budget.content.contains("60 invocations"),
        "{}",
        budget.content
    );
    assert!(budget.content.contains("claude"), "{}", budget.content);
    assert!(
        budget.content.contains("raise the streetlights"),
        "{}",
        budget.content
    );
}

#[test]
fn the_history_resource_tells_a_persons_change_from_an_agents_and_says_what_for() {
    // Task 3.9. "Attribution SHALL be visible where history is visible ... so that a reviewer
    // reading a change can tell what a person did from what an agent did, and why."
    let registry = registry();
    let (mut editor, document) = editor_with_a_document();
    editor
        .invoke(
            &registry,
            "scene.create-entity",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("the person creates one");

    let mut connection = session(Scope::new(
        "authoring",
        DocumentScope::All,
        [EffectClass::Read, EffectClass::ReversibleMutation],
    ));
    connection
        .invoke(
            &mut editor,
            &registry,
            &mut RefuseEverything,
            "scene.create-entity",
            &Arguments::new(),
            0,
        )
        .expect("the agent creates one");

    let history = connection
        .read_resource(&editor, &format!("history:{document}"), 0)
        .expect("the history reads");
    assert_eq!(
        history.content.matches("[human]").count(),
        1,
        "{}",
        history.content
    );
    assert_eq!(
        history.content.matches("[agent]").count(),
        1,
        "{}",
        history.content
    );
    assert!(
        history.content.contains("raise the streetlights to 4 m"),
        "the intent is on the entry: {}",
        history.content
    );
}

#[test]
fn a_human_edit_supersedes_an_agents_claim_and_the_agent_is_told_why() {
    // Task 3.11. "Where a human action and an agent action conflict, the human action SHALL win,
    // and the agent SHALL be told its operation was superseded and why."
    let registry = registry();
    let (mut editor, _) = editor_with_a_document();
    let created = editor
        .invoke(
            &registry,
            "scene.create-entity",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("the person creates a node");
    let node = match created.values.get("entity") {
        Some(Value::Text(text)) => {
            NodeId::from_u128(u128::from_str_radix(text, 16).expect("a printed identity"))
        }
        other => panic!("no entity in the outcome: {other:?}"),
    };

    let mut connection = session(Scope::new(
        "authoring",
        DocumentScope::All,
        [EffectClass::Read, EffectClass::ReversibleMutation],
    ));
    connection
        .claim(&editor, &[node])
        .expect("the agent says what it is working on");

    // The person deletes it while the agent is thinking. Nothing blocked; the editor stayed usable.
    editor
        .invoke(
            &registry,
            "scene.delete-entity",
            &Scope::unrestricted(),
            &Arguments::new().with("entity", Value::Text(node.to_string())),
        )
        .expect("the person deletes it");

    let refused = connection
        .invoke(
            &mut editor,
            &registry,
            &mut RefuseEverything,
            "edit.select",
            &Arguments::new().with("entity", Value::Text(node.to_string())),
            0,
        )
        .expect_err("the agent's operation was superseded");
    assert!(refused.because.contains("superseded"), "{refused}");
    assert!(refused.because.contains("Delete entity"), "{refused}");
    assert!(
        refused
            .remedy
            .as_deref()
            .unwrap()
            .contains("human's change stands"),
        "{refused}"
    );

    // And the supersession costs nothing: the connection has not spent an invocation on it.
    assert_eq!(connection.budget(0).invocations_used, 0);
}

#[test]
fn an_agents_own_edits_do_not_supersede_its_claim() {
    // A claim that its holder's own work invalidated would make every second invocation fail, which
    // is the shape of over-eager conflict detection that teaches people to turn it off.
    let registry = registry();
    let (mut editor, _) = editor_with_a_document();
    let created = editor
        .invoke(
            &registry,
            "scene.create-entity",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("a node exists");
    let node = match created.values.get("entity") {
        Some(Value::Text(text)) => {
            NodeId::from_u128(u128::from_str_radix(text, 16).expect("a printed identity"))
        }
        other => panic!("no entity in the outcome: {other:?}"),
    };

    let mut connection = session(Scope::new(
        "authoring",
        DocumentScope::All,
        [EffectClass::Read, EffectClass::ReversibleMutation],
    ));
    connection.claim(&editor, &[node]).expect("a claim");
    for _ in 0..3 {
        connection
            .invoke(
                &mut editor,
                &registry,
                &mut RefuseEverything,
                "scene.create-entity",
                &Arguments::new(),
                0,
            )
            .expect("the agent keeps working");
    }
    assert!(
        connection
            .claimed()
            .expect("still claimed")
            .check(&editor)
            .is_clear()
    );
}

#[test]
fn a_claim_on_untouched_objects_is_not_a_conflict() {
    let registry = registry();
    let (mut editor, _) = editor_with_a_document();
    let first = editor
        .invoke(
            &registry,
            "scene.create-entity",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("one");
    let node = match first.values.get("entity") {
        Some(Value::Text(text)) => {
            NodeId::from_u128(u128::from_str_radix(text, 16).expect("a printed identity"))
        }
        other => panic!("no entity: {other:?}"),
    };
    let claim = Claim::stake(&editor, &[node]).expect("a claim");

    // The person works on the other side of the level.
    editor
        .invoke(
            &registry,
            "scene.create-entity",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("two");
    assert_eq!(claim.check(&editor), Conflict::None);
}

#[test]
fn a_writing_agent_never_stops_the_person_from_working() {
    // "WHEN an agent is performing a long operation THEN the editor SHALL remain responsive and the
    // human SHALL be able to interrupt it." The long operation here is a build, which is what
    // `project.build` starts; the person keeps editing throughout and then pauses the connection.
    let registry = registry();
    let (mut editor, _) = editor_with_a_document();
    let mut connection = session(Scope::unrestricted());

    // The build is refused for want of sources, which is beside the point: what matters is that the
    // call returned rather than blocking, and that everything below still works.
    let _ = connection.invoke(
        &mut editor,
        &registry,
        &mut RefuseEverything,
        "project.build",
        &Arguments::new(),
        0,
    );
    editor
        .invoke(
            &registry,
            "scene.create-entity",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("the person is not locked out");

    connection.pause();
    let refused = connection
        .invoke(
            &mut editor,
            &registry,
            &mut RefuseEverything,
            "scene.create-entity",
            &Arguments::new(),
            0,
        )
        .expect_err("a paused connection does nothing");
    assert!(refused.because.contains("paused"), "{refused}");
    editor
        .invoke(
            &registry,
            "scene.create-entity",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("the person is still not locked out");
}

#[test]
fn a_reversible_source_write_is_not_confirmed_and_an_irreversible_one_is() {
    // The whole point of computing the class: confirmation stays rare and therefore meaningful.
    // `RefuseEverything` is the confirmer, so anything that asks is refused — which is how this test
    // tells "did not ask" from "asked and was allowed".
    let registry = registry();
    let sandbox = std::env::temp_dir().join(format!(
        "cy-agent-effect-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |elapsed| elapsed.as_nanos())
    ));
    std::fs::create_dir_all(sandbox.join("game")).expect("a writable sandbox");
    let mut editor = Editor::new(Actor::human("designer"))
        .with_project(cy_editor_services::project::ProjectService::new(&sandbox));
    let mut connection = session(
        Scope::new(
            "authoring",
            DocumentScope::All,
            [EffectClass::Read, EffectClass::ReversibleMutation],
        )
        .with_directory("game/"),
    );

    connection
        .invoke(
            &mut editor,
            &registry,
            &mut RefuseEverything,
            "source.write",
            &Arguments::new()
                .with("path", Value::Text("game/Player.swift".into()))
                .with("contents", Value::Text("struct Player {}".into())),
            0,
        )
        .expect("creating a file undoes to deleting it, so nobody is asked");

    std::fs::write(sandbox.join("game/blob.bin"), [0xff, 0xfe, 0x00]).expect("a writable sandbox");
    let refused = connection
        .invoke(
            &mut editor,
            &registry,
            &mut RefuseEverything,
            "source.write",
            &Arguments::new()
                .with("path", Value::Text("game/blob.bin".into()))
                .with("contents", Value::Text("replaced".into())),
            0,
        )
        .expect_err("the editor cannot put this one back");
    // Refused by the SCOPE, which is the first gate a computed irreversible class meets: this
    // connection was never granted that class. That is the enforcement the requirement asks for.
    assert!(refused.because.contains("authoring"), "{refused}");
    assert!(
        refused.because.contains("irreversible-mutation"),
        "{refused}"
    );

    let _ = std::fs::remove_dir_all(&sandbox);
}

/// A builder that waits to be let go, so that a build stays running while a test looks at it.
struct Blocking(std::sync::Mutex<std::sync::mpsc::Receiver<()>>);

impl cy_editor_services::project::ModuleBuilder for Blocking {
    fn describe(&self) -> String {
        "a builder that waits to be let go".to_string()
    }

    fn build(
        &self,
        _request: &cy_editor_services::project::BuildRequest,
    ) -> cy_editor_core::problem::Result<std::path::PathBuf> {
        let _ = self
            .0
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .recv_timeout(std::time::Duration::from_secs(10));
        Ok(std::path::PathBuf::from("libCyGame_g1.so"))
    }
}

/// A connection with one concurrency slot, over a project whose builds block.
fn one_slot(sandbox: &std::path::Path) -> (Editor, AgentSession, std::sync::mpsc::Sender<()>) {
    let (release, held) = std::sync::mpsc::channel::<()>();
    let editor = Editor::new(Actor::human("designer")).with_project(
        cy_editor_services::project::ProjectService::new(sandbox)
            .with_builder(std::sync::Arc::new(Blocking(std::sync::Mutex::new(held)))),
    );
    let mut connection = AgentSession::new(
        AgentIdentity {
            agent: "claude".to_string(),
            session: "s-13".to_string(),
        },
        "build repeatedly",
        Scope::unrestricted(),
        Budget {
            concurrent_operations: 1,
            ..Budget::default()
        },
        "r-1",
        0,
    );
    connection.grant(EffectClass::ExternalEffect, 4);
    (editor, connection, release)
}

#[test]
fn background_work_holds_a_concurrency_slot_until_it_settles() {
    // Task 3.12's third limit. The slot is taken before the invocation and given back when the work
    // settles, so an agent that starts builds faster than they finish is refused with what is left
    // rather than filling the machine with compilers.
    let registry = registry();
    let sandbox = std::env::temp_dir().join(format!(
        "cy-agent-slots-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |elapsed| elapsed.as_nanos())
    ));
    std::fs::create_dir_all(sandbox.join("game")).expect("a writable sandbox");
    let (mut editor, mut connection, release) = one_slot(&sandbox);

    let mut build = |connection: &mut AgentSession, editor: &mut Editor| {
        connection.invoke(
            editor,
            &registry,
            &mut RefuseEverything,
            "project.build",
            &Arguments::new(),
            0,
        )
    };

    build(&mut connection, &mut editor).expect("the first build is queued");
    assert_eq!(connection.operations_running(), 1);

    let refused = build(&mut connection, &mut editor).expect_err("the second exceeds the ceiling");
    assert!(
        refused
            .remedy
            .as_deref()
            .unwrap()
            .contains("1 of 1 operations running"),
        "{refused}"
    );

    // Reading is never blocked by it: an agent has to be able to find out when its own work
    // finished, and a bound that stopped it would deadlock the agent it was protecting against.
    connection
        .invoke(
            &mut editor,
            &registry,
            &mut RefuseEverything,
            "edit.select",
            &Arguments::new().with("entity", Value::Text("1".repeat(32))),
            0,
        )
        .expect("a read costs no slot");

    // Let it go, and the slot comes back. The build queued afterwards finds the channel already
    // closed and returns at once, which is what this test wants: it is checking the accounting.
    drop(release);
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(10);
    while connection.operations_running() == 1 && std::time::Instant::now() < deadline {
        editor.pump();
        std::thread::sleep(std::time::Duration::from_millis(2));
    }
    assert_eq!(connection.operations_running(), 0);
    build(&mut connection, &mut editor).expect("the slot came back");

    let _ = std::fs::remove_dir_all(&sandbox);
}
