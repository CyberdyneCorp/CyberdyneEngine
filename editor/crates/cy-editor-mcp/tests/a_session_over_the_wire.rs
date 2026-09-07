//! One agent, one editor, and the whole authoring loop over the protocol. M5.5 task 3.1.
//!
//! Driven as bytes on both sides rather than by calling the server's methods, because the claims
//! here are about the **wire**: that a tool list is the registry, that a call produces a
//! transaction, that a refusal teaches, and that an image comes back as an image. A test that called
//! `dispatch` directly would prove the projection and nothing about the protocol.

use std::sync::{Arc, Mutex};

use cy_editor_agent::budget::Budget;
use cy_editor_agent::session::{AgentIdentity, AgentSession, RefuseEverything};
use cy_editor_commands::metadata::EffectClass;
use cy_editor_commands::registry::Registry;
use cy_editor_commands::scope::{DocumentScope, Scope};
use cy_editor_core::Actor;
use cy_editor_mcp::json::{Json, parse};
use cy_editor_mcp::{McpServer, PROTOCOL_VERSION, serve};
use cy_editor_services::editor::Editor;
use cy_editor_services::project::ProjectService;
use cy_editor_viewport::state::ViewState;
use cy_editor_viewport::transport::{
    FrameImage, Mailbox, MailboxTransport, PresentedFrame, TransportKind,
};

/// A writer a test can read back, shared with the server that writes into it.
#[derive(Clone, Default)]
struct Sink(Arc<Mutex<Vec<u8>>>);

impl std::io::Write for Sink {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        self.0
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .extend_from_slice(bytes);
        Ok(bytes.len())
    }

    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

impl Sink {
    /// Every reply, decoded, in the order they were written.
    fn replies(&self) -> Vec<Json> {
        let held = self
            .0
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        String::from_utf8_lossy(&held)
            .lines()
            .filter(|line| !line.trim().is_empty())
            .map(|line| parse(line).expect("every line this server writes is JSON"))
            .collect()
    }
}

/// A directory that removes itself.
struct Sandbox(std::path::PathBuf);

impl Sandbox {
    fn new(name: &str) -> Self {
        let unique = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |elapsed| elapsed.as_nanos());
        let path =
            std::env::temp_dir().join(format!("cy-mcp-{name}-{}-{unique}", std::process::id()));
        std::fs::create_dir_all(path.join("game")).expect("a writable temporary directory");
        Self(path)
    }
}

impl Drop for Sandbox {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn registry() -> Registry {
    let mut registry = Registry::new();
    cy_editor_services::builtin::register(&mut registry)
        .expect("the built-in commands satisfy their own metadata");
    registry
}

fn session() -> AgentSession {
    AgentSession::new(
        AgentIdentity {
            agent: "claude".to_string(),
            session: "s-1".to_string(),
        },
        "compose the opening scene",
        Scope::new(
            "authoring",
            DocumentScope::All,
            [EffectClass::Read, EffectClass::ReversibleMutation],
        )
        .with_directory("game/"),
        Budget::default(),
        "r-1",
        0,
    )
}

/// Run a whole conversation and hand back what the server said.
fn converse(lines: &[&str], editor: &mut Editor) -> Vec<Json> {
    let sink = Sink::default();
    let mut server = McpServer::new(sink.clone(), session());
    let script = lines.join("\n");
    serve(
        script.as_bytes(),
        &mut server,
        editor,
        &registry(),
        &mut RefuseEverything,
    )
    .expect("the conversation runs to the end of the input");
    sink.replies()
}

/// The `result` of the nth reply.
fn result(replies: &[Json], index: usize) -> &Json {
    replies
        .get(index)
        .unwrap_or_else(|| panic!("no reply {index}; there were {}", replies.len()))
        .get("result")
}

const INITIALIZE: &str =
    r#"{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}}"#;

#[test]
fn a_client_is_told_what_this_server_is_and_what_it_can_do() {
    let mut editor = Editor::new(Actor::human("designer"));
    let replies = converse(&[INITIALIZE], &mut editor);
    let announced = result(&replies, 0);
    assert_eq!(
        announced.get("protocolVersion").as_text().unwrap(),
        PROTOCOL_VERSION
    );
    assert_eq!(
        announced.get("serverInfo").get("name").as_text().unwrap(),
        cy_editor_mcp::SERVER_NAME
    );
    assert!(!announced.get("capabilities").get("tools").is_null());
    assert!(!announced.get("capabilities").get("resources").is_null());
    // The instructions tell an agent to LOOK, which is what makes the loop authoring rather than
    // data entry — `design.md` §3.
    assert!(
        announced
            .get("instructions")
            .as_text()
            .unwrap()
            .contains("viewport:"),
    );
}

#[test]
fn calling_a_tool_before_initialising_is_refused_with_what_to_do() {
    let mut editor = Editor::new(Actor::human("designer"));
    let replies = converse(
        &[r#"{"jsonrpc":"2.0","id":9,"method":"tools/list"}"#],
        &mut editor,
    );
    assert_eq!(
        replies[0].get("error").get("data").get("remedy").as_text(),
        Some("send initialize first, and read the protocol version it answers with")
    );
}

#[test]
fn the_tool_list_is_the_registry_and_carries_every_effect_class() {
    // "Every registered command SHALL be exposed as a tool, automatically" — so the count is the
    // registry's count, and a hand-written entry would make these two disagree.
    let mut editor = Editor::new(Actor::human("designer"));
    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/list"}"#,
        ],
        &mut editor,
    );
    let tools = match result(&replies, 1).get("tools") {
        Json::Array(items) => items.clone(),
        other => panic!("tools is not an array: {other:?}"),
    };
    assert_eq!(tools.len(), registry().len());

    let create = tools
        .iter()
        .find(|tool| tool.get("name").as_text() == Some("scene.create-entity"))
        .expect("the command a menu registered is a tool with no further work");
    let description = create.get("description").as_text().unwrap();
    assert!(
        description.contains("reversible-mutation"),
        "each tool states its effect class before it is invoked: {description}"
    );
    assert!(description.contains("undo reverses it"), "{description}");
    assert_eq!(
        create.get("inputSchema").get("type").as_text(),
        Some("object")
    );

    // A command that computes its class says so, so an agent does not budget its confirmations
    // against the worst case.
    let write = tools
        .iter()
        .find(|tool| tool.get("name").as_text() == Some("source.write"))
        .expect("source.write is registered");
    assert!(
        write
            .get("description")
            .as_text()
            .unwrap()
            .contains("at most"),
        "{:?}",
        write.get("description")
    );
    // And its typed parameters became a schema, generated from the command's own metadata.
    let path = write.get("inputSchema").get("properties").get("path");
    assert_eq!(path.get("type").as_text(), Some("string"));
    assert!(
        path.get("description")
            .as_text()
            .unwrap()
            .contains("project-relative"),
    );
}

#[test]
fn a_tool_call_produces_one_transaction_attributed_to_the_agent() {
    let mut editor = Editor::new(Actor::human("designer"));
    let document = editor
        .open_document("worlds/city.cyworld")
        .expect("a document opens");
    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"scene.create-entity","arguments":{}}}"#,
        ],
        &mut editor,
    );
    let called = result(&replies, 1);
    assert_eq!(called.get("isError"), &Json::Bool(false));
    // The structured half beside the prose one: an agent that had to parse a sentence to learn
    // which entity was created would eventually get it wrong.
    assert!(!called.get("structuredContent").get("entity").is_null());

    let entry = editor
        .documents
        .get(document)
        .expect("open")
        .history()
        .entries()
        .last()
        .expect("one entry");
    assert!(entry.actor.is_agent());
    assert_eq!(entry.actor.intent(), Some("compose the opening scene"));
}

#[test]
fn a_refusal_comes_back_as_a_result_the_model_can_read_rather_than_a_swallowed_error() {
    // The protocol's own rule for tools, and the reason for it: a transport-level error is handled
    // by the client library and never reaches the model, which then cannot act on the reason.
    let mut editor = Editor::new(Actor::human("designer"));
    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"scene.create-entity","arguments":{}}}"#,
        ],
        &mut editor,
    );
    let called = result(&replies, 1);
    assert_eq!(called.get("isError"), &Json::Bool(true));
    let text = match called.get("content") {
        Json::Array(items) => items[0].get("text").as_text().unwrap().to_string(),
        other => panic!("content is not an array: {other:?}"),
    };
    assert!(text.contains("no document is open"), "{text}");
    assert!(
        text.contains("What would help: open a document first"),
        "{text}"
    );
}

#[test]
fn a_command_outside_the_connections_scope_is_refused_by_name() {
    let mut editor = Editor::new(Actor::human("designer"));
    editor
        .open_document("worlds/city.cyworld")
        .expect("a document opens");
    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"file.save","arguments":{}}}"#,
        ],
        &mut editor,
    );
    let text = match result(&replies, 1).get("content") {
        Json::Array(items) => items[0].get("text").as_text().unwrap().to_string(),
        other => panic!("content is not an array: {other:?}"),
    };
    assert!(text.contains("authoring"), "the scope is named: {text}");
}

#[test]
fn an_unknown_method_is_a_protocol_error_listing_what_there_is() {
    let mut editor = Editor::new(Actor::human("designer"));
    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/invent"}"#,
        ],
        &mut editor,
    );
    let failure = replies[1].get("error");
    assert_eq!(failure.get("code").as_number(), Some(-32601.0));
    assert!(
        failure
            .get("data")
            .get("remedy")
            .as_text()
            .unwrap()
            .contains("resources/read"),
    );
}

#[test]
fn a_notification_is_never_answered() {
    // Replying to one produces a response matching no request, which most clients log and ignore —
    // so the defect is invisible until something stricter arrives.
    let mut editor = Editor::new(Actor::human("designer"));
    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","method":"notifications/initialized"}"#,
            r#"{"jsonrpc":"2.0","id":3,"method":"ping"}"#,
        ],
        &mut editor,
    );
    assert_eq!(replies.len(), 2, "the notification produced no reply");
    assert_eq!(replies[1].get("id"), &Json::Number(3.0));
}

#[test]
fn a_malformed_line_is_answered_rather_than_ending_the_session() {
    let mut editor = Editor::new(Actor::human("designer"));
    let replies = converse(
        &[
            INITIALIZE,
            "this is not JSON",
            r#"{"jsonrpc":"2.0","id":4,"method":"ping"}"#,
        ],
        &mut editor,
    );
    assert!(!replies[1].get("error").is_null());
    assert_eq!(replies[1].get("id"), &Json::Null);
    assert_eq!(replies[2].get("id"), &Json::Number(4.0));
}

#[test]
fn the_resource_list_carries_the_read_surface_including_what_can_be_looked_at() {
    let mut editor = Editor::new(Actor::human("designer"));
    editor
        .open_document("worlds/city.cyworld")
        .expect("a document opens");
    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"resources/list"}"#,
        ],
        &mut editor,
    );
    let uris: Vec<String> = match result(&replies, 1).get("resources") {
        Json::Array(items) => items
            .iter()
            .map(|item| item.get("uri").as_text().unwrap().to_string())
            .collect(),
        other => panic!("resources is not an array: {other:?}"),
    };
    for wanted in [
        "selection:",
        "documents:",
        "diagnostics:",
        "play:",
        "operations:",
        "sources:",
        "build:",
        "budget:",
        "viewport:",
        "viewport:overlays",
    ] {
        assert!(
            uris.iter().any(|uri| uri == wanted),
            "{wanted} is missing from {uris:?}"
        );
    }
    assert!(uris.iter().any(|uri| uri.starts_with("hierarchy:")));
    assert!(uris.iter().any(|uri| uri.starts_with("history:")));
}

#[test]
fn reading_the_scene_changes_nothing() {
    // "WHEN an agent reads the scene hierarchy THEN no document SHALL become dirty and no selection
    // SHALL change." Held here at the wire, because a transport that cached or normalised on the way
    // through would be a second place the guarantee could break.
    let mut editor = Editor::new(Actor::human("designer"));
    let document = editor
        .open_document("worlds/city.cyworld")
        .expect("a document opens");
    let before = editor.summary_revision();
    let replies = converse(
        &[
            INITIALIZE,
            &format!(
                r#"{{"jsonrpc":"2.0","id":2,"method":"resources/read","params":{{"uri":"hierarchy:{document}"}}}}"#
            ),
        ],
        &mut editor,
    );
    assert!(!result(&replies, 1).get("contents").is_null());
    assert!(!editor.documents.any_dirty());
    assert_eq!(editor.summary_revision(), before);
}

#[test]
fn the_viewport_comes_back_as_an_image_with_its_media_type() {
    // Task 3.4, at the wire. The image is the engine's own frame, delivered through the transport
    // the human's viewport uses, and the reply says which kind of image it is.
    //
    // One server across both halves, deliberately: the connection's own viewport is opened on its
    // first look and reused thereafter, and a test that opened a second session would be looking
    // through a second viewport nothing had rendered into.
    let mut editor = Editor::new(Actor::human("designer"));
    let sink = Sink::default();
    let mut server = McpServer::new(sink.clone(), session());
    let registry = registry();
    let look = r#"{"jsonrpc":"2.0","id":2,"method":"resources/read","params":{"uri":"viewport:"}}"#;

    for line in [INITIALIZE, look] {
        server
            .handle(line, &mut editor, &registry, &mut RefuseEverything)
            .expect("the server answers");
    }
    let text = match result(&sink.replies(), 1).get("content") {
        Json::Array(items) => items[0].get("text").as_text().unwrap().to_string(),
        other => panic!("a refusal is content: {other:?}"),
    };
    assert!(text.contains("no frame has arrived"), "{text}");

    // Now the runtime renders one into the viewport the connection opened.
    let agent_viewport = editor
        .viewports
        .all()
        .iter()
        .find(|viewport| viewport.name == cy_editor_agent::observe::AGENT_VIEWPORT)
        .map(|viewport| viewport.id)
        .expect("the connection opened its own viewport");
    let mailbox = Mailbox::new();
    mailbox.publish(PresentedFrame::new(
        cy_editor_protocol::FrameId::from_raw(21),
        ViewState::new(),
        FrameImage::Encoded(vec![0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a]),
        0,
    ));
    let mut transport = MailboxTransport::new(TransportKind::SharedTexture, mailbox);
    editor
        .viewports
        .all_mut()
        .get_mut(agent_viewport)
        .expect("open")
        .pump(&mut transport, 0);

    server
        .handle(look, &mut editor, &registry, &mut RefuseEverything)
        .expect("the server answers");
    let replies = sink.replies();
    let entry = match result(&replies, 2).get("contents") {
        Json::Array(items) => items[0].clone(),
        other => panic!("contents is not an array: {other:?}"),
    };
    assert_eq!(entry.get("uri").as_text(), Some("viewport:"));
    assert_eq!(entry.get("mimeType").as_text(), Some("image/png"));
    assert_eq!(entry.get("blob").as_text(), Some("iVBORw0KGgo="));
    assert_eq!(
        editor
            .viewports
            .all()
            .iter()
            .filter(|viewport| viewport.name == cy_editor_agent::observe::AGENT_VIEWPORT)
            .count(),
        1,
        "one connection opens one viewport, however many times it looks"
    );
}

#[test]
fn the_whole_authoring_loop_runs_over_the_wire() {
    // `design.md` §3, as one conversation:
    //
    //     compose a scene -> write a gameplay script -> build and reload -> play -> LOOK -> decide
    //
    // The build is refused here for want of a Swift toolchain on every machine, and the reload for
    // want of a runtime; both refusals are part of the loop rather than a gap in it, because each
    // says what would make it work. Everything else runs end to end.
    let sandbox = Sandbox::new("loop");
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor
        .open_document("worlds/city.cyworld")
        .expect("a document opens");

    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"scene.create-entity","arguments":{}}}"#,
            r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"source.write","arguments":{"path":"game/Player.swift","contents":"struct Player {}"}}}"#,
            r#"{"jsonrpc":"2.0","id":4,"method":"resources/read","params":{"uri":"sources:game/Player.swift"}}"#,
            r#"{"jsonrpc":"2.0","id":5,"method":"resources/read","params":{"uri":"build:"}}"#,
            r#"{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"play.enter","arguments":{}}}"#,
            r#"{"jsonrpc":"2.0","id":7,"method":"resources/read","params":{"uri":"play:"}}"#,
            r#"{"jsonrpc":"2.0","id":8,"method":"resources/read","params":{"uri":"budget:"}}"#,
        ],
        &mut editor,
    );

    assert_eq!(result(&replies, 1).get("isError"), &Json::Bool(false));
    assert_eq!(
        result(&replies, 2).get("isError"),
        &Json::Bool(false),
        "the script was written: {:?}",
        result(&replies, 2)
    );
    assert_eq!(
        std::fs::read_to_string(sandbox.0.join("game/Player.swift")).expect("written"),
        "struct Player {}"
    );

    let read_back = match result(&replies, 3).get("contents") {
        Json::Array(items) => items[0].get("text").as_text().unwrap().to_string(),
        other => panic!("contents is not an array: {other:?}"),
    };
    assert_eq!(read_back, "struct Player {}");

    let build = match result(&replies, 4).get("contents") {
        Json::Array(items) => items[0].get("text").as_text().unwrap().to_string(),
        other => panic!("contents is not an array: {other:?}"),
    };
    assert!(build.contains("never-built"), "{build}");

    assert_eq!(result(&replies, 5).get("isError"), &Json::Bool(false));
    let play = match result(&replies, 6).get("contents") {
        Json::Array(items) => items[0].get("text").as_text().unwrap().to_string(),
        other => panic!("contents is not an array: {other:?}"),
    };
    assert!(play.contains("connected: no"), "{play}");

    let budget = match result(&replies, 7).get("contents") {
        Json::Array(items) => items[0].get("text").as_text().unwrap().to_string(),
        other => panic!("contents is not an array: {other:?}"),
    };
    assert!(budget.contains("invocations"), "{budget}");
    assert!(budget.contains("compose the opening scene"), "{budget}");
}
