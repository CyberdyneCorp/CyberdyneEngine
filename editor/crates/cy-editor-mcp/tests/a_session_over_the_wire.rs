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
use cy_editor_core::codec::Writer;
use cy_editor_mcp::json::{Json, parse};
use cy_editor_mcp::{McpServer, PROTOCOL_VERSION, serve};
use cy_editor_protocol::{Message, ServiceEventKind, Session, read_frame, write_frame};
use cy_editor_services::backend::MaterialRequestState;
use cy_editor_services::editor::Editor;
use cy_editor_services::notifications::NotificationService;
use cy_editor_services::project::ProjectService;
use cy_editor_services::runtime::RuntimeSession;
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
    cy_editor_interface::specialised::material_authoring_commands::register(&mut registry)
        .expect("material graph commands satisfy their metadata");
    cy_editor_interface::specialised::vfx_authoring_commands::register(&mut registry)
        .expect("VFX graph commands satisfy their metadata");
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

/// `converse`, with the agent's write scope granted `directory` instead of `game/`.
fn converse_within(lines: &[&str], editor: &mut Editor, directory: &str) -> Vec<Json> {
    let sink = Sink::default();
    let session = AgentSession::new(
        AgentIdentity {
            agent: "author".to_string(),
            session: "s-1".to_string(),
        },
        "author a reusable VFX module",
        Scope::new(
            "authoring",
            DocumentScope::All,
            [EffectClass::Read, EffectClass::ReversibleMutation],
        )
        .with_directory(directory),
        Budget::default(),
        "r-1",
        0,
    );
    let mut server = McpServer::new(sink.clone(), session);
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

/// Give the editor a catalogue through the same backend request/response path as the engine.
fn install_vfx_stage_catalogue(editor: &mut Editor) -> (std::io::PipeReader, std::io::PipeWriter) {
    let (editor_reader, mut runtime_writer) = std::io::pipe().unwrap();
    let (mut runtime_reader, editor_writer) = std::io::pipe().unwrap();
    editor.runtime = RuntimeSession::over(Session::over(editor_reader, editor_writer));

    let mut vfx = Writer::new();
    vfx.u32(2);
    vfx.u32(2);
    vfx.u32(4);
    for (identity, name, pin, direction, property) in [
        (1001, "vfx.constant", "out", 1, "value"),
        (1002, "vfx.parameter", "out", 1, "parameter"),
        (1029, "vfx.set_attribute", "value", 0, "attribute"),
        (1031, "vfx.spawn_count", "value", 0, ""),
    ] {
        vfx.u32(identity);
        vfx.u32(1);
        vfx.text(name);
        vfx.u32(1);
        vfx.u32(1);
        vfx.u8(direction);
        vfx.text(pin);
        vfx.text("value");
        if property.is_empty() {
            vfx.u32(0);
        } else {
            vfx.u32(1);
            vfx.u32(1);
            vfx.u8(0);
            vfx.text(property);
            vfx.text(if property == "value" { "0" } else { "" });
            vfx.text(property);
            vfx.text(if property == "value" {
                "vfx-literal"
            } else {
                "identifier"
            });
            vfx.text("");
            vfx.u32(0);
            vfx.text("compile");
            vfx.text("vfx");
            vfx.u64(0);
            vfx.u8(0);
            vfx.u8(0);
            for _ in 0..3 {
                vfx.f64(0.0);
            }
        }
    }
    for (operation, payload) in [
        ("material.catalogue.get", material_edit_catalogue()),
        ("vfx.catalogue.get", vfx.finish()),
    ] {
        assert!(editor.backend.maintain(&editor.runtime).is_none());
        let request = Message::decode(&read_frame(&mut runtime_reader).unwrap().unwrap()).unwrap();
        let Message::ServiceRequest {
            request,
            operation: actual,
            ..
        } = request
        else {
            panic!("catalogue discovery must use a service request");
        };
        assert_eq!(actual, operation);
        write_frame(
            &mut runtime_writer,
            &Message::ServiceEvent {
                request,
                kind: ServiceEventKind::Completed,
                schema_version: 1,
                payload,
            }
            .encode(),
        )
        .unwrap();
        let mut notifications = NotificationService::new();
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        let received = |editor: &Editor| {
            if operation == "material.catalogue.get" {
                editor.backend.material_catalogue().is_some()
            } else {
                editor.backend.vfx_catalogue().is_some()
            }
        };
        while !received(editor) && std::time::Instant::now() < deadline {
            for event in editor.runtime.pump(&mut notifications) {
                assert!(editor.backend.accept(&event).is_none());
            }
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
        assert!(received(editor), "{operation} must arrive from the runtime");
    }
    assert!(editor.backend.vfx_catalogue().is_some());
    (runtime_reader, runtime_writer)
}

fn material_edit_catalogue() -> Vec<u8> {
    let mut material = Writer::new();
    material.u32(1);
    material.u32(1);
    material.u32(2);
    material.u32(42);
    material.u32(3);
    material.text("material.constant");
    material.u32(1);
    material.u32(9);
    material.u8(1);
    material.text("out");
    material.text("value");
    material.u32(1);
    material.u32(2);
    material.u8(2);
    material.text("value");
    material.text("0");
    material.text("");
    material.text("Constant value");
    material.u32(43);
    material.u32(3);
    material.text("material.sink");
    material.u32(1);
    material.u32(10);
    material.u8(0);
    material.text("in");
    material.text("value");
    material.u32(0);
    material.finish()
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
    for command in [
        "asset.import",
        "asset.import-external",
        "asset.place",
        "asset.assign",
    ] {
        assert!(
            tools
                .iter()
                .any(|tool| tool.get("name").as_text() == Some(command)),
            "{command} is projected from the same registry onto MCP"
        );
    }
}

#[test]
fn vfx_preview_controls_and_status_are_projected_over_mcp() {
    let mut editor = Editor::new(Actor::human("designer"));
    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/list"}"#,
            r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"vfx.preview.status","arguments":{}}}"#,
            r#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"vfx.preview.control","arguments":{"action":"invalid"}}}"#,
            r#"{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"vfx.preview.load","arguments":{"source":"invalid"}}}"#,
        ],
        &mut editor,
    );
    let Json::Array(tools) = result(&replies, 1).get("tools") else {
        panic!("tools/list must contain the registry projection");
    };
    for command in [
        "vfx.preview.load",
        "vfx.preview.control",
        "vfx.preview.step",
        "vfx.preview.parameter.set",
        "vfx.preview.status",
    ] {
        assert!(
            tools
                .iter()
                .any(|tool| tool.get("name").as_text() == Some(command)),
            "{command} must be available over MCP"
        );
    }
    let status = result(&replies, 2).get("content");
    let Json::Array(content) = status else {
        panic!("status must be tool content");
    };
    assert!(
        content[0]
            .get("text")
            .as_text()
            .unwrap()
            .contains("pending = false")
    );
    for (index, diagnostic) in [(3, "action must be"), (4, "expected cyvfxdoc 1")] {
        assert_eq!(result(&replies, index).get("isError"), &Json::Bool(true));
        let Json::Array(content) = result(&replies, index).get("content") else {
            panic!("refusal must be tool content");
        };
        assert!(
            content[0]
                .get("text")
                .as_text()
                .unwrap()
                .contains(diagnostic)
        );
    }
}

#[test]
fn vfx_hierarchy_and_parameter_edits_use_the_same_undo_history_over_mcp() {
    use cy_editor_interface::specialised::vfx::VfxDocument;

    let sandbox = Sandbox::new("vfx-authoring");
    let reference = "game/sparks.cyvfxdoc";
    let original = VfxDocument::new("sparks").unwrap().encode_text().unwrap();
    std::fs::write(sandbox.0.join(reference), &original).unwrap();
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor.open_document("worlds/city.cyworld").unwrap();

    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"vfx.emitter.add","arguments":{"reference":"game/sparks.cyvfxdoc","name":"embers","target":"cpu","renderer":"Sprite"}}}"#,
            r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"vfx.parameter.set","arguments":{"reference":"game/sparks.cyvfxdoc","name":"speed","kind":"float","values":[2,0,0,0],"exposed":true}}}"#,
            r#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"vfx.node.add","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"embers","stage":"spawn","node_type":"vfx.constant","x":12,"y":30}}}"#,
        ],
        &mut editor,
    );
    for index in [1, 2] {
        assert_eq!(result(&replies, index).get("isError"), &Json::Bool(false));
    }
    assert_eq!(result(&replies, 3).get("isError"), &Json::Bool(true));
    let Json::Array(content) = result(&replies, 3).get("content") else {
        panic!("a missing catalogue must explain the refusal");
    };
    assert!(
        content[0]
            .get("text")
            .as_text()
            .unwrap()
            .contains("engine VFX node catalogue is unavailable")
    );
    let saved =
        VfxDocument::decode_text(&std::fs::read_to_string(sandbox.0.join(reference)).unwrap())
            .unwrap();
    assert_eq!(saved.emitters[0].name, "embers");
    assert_eq!(saved.parameters[0].value[0].to_bits(), 2.0_f32.to_bits());
    let undone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&undone, 1).get("isError"), &Json::Bool(false));
    let after_undo =
        VfxDocument::decode_text(&std::fs::read_to_string(sandbox.0.join(reference)).unwrap())
            .unwrap();
    assert_eq!(after_undo.emitters[0].name, "embers");
    assert!(after_undo.parameters.is_empty());
    let redone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"edit.redo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&redone, 1).get("isError"), &Json::Bool(false));
    assert_eq!(
        std::fs::read_to_string(sandbox.0.join(reference)).unwrap(),
        saved.encode_text().unwrap()
    );
    assert_eq!(
        editor
            .documents
            .get(editor.workspace.active().unwrap())
            .unwrap()
            .history()
            .entries()
            .len(),
        2
    );
}

fn vertex_material_sources() -> (String, String) {
    let mut source = include_str!(
        "../../../../samples/05b-editor-window/project/materials/copper_clay.cymatcanvas"
    )
    .replace("material copper_clay", "material sway");
    source.push_str("node 5 material.constant\n");
    source.push_str("prop 5 type float3\n");
    source.push_str("prop 5 value 0 0.25 0 0\n");
    source.push_str("node 6 material.vertex_output\n");
    source.push_str("link 5 out 6 offset\n");
    let graph =
        include_str!("../../../../samples/05b-editor-window/project/materials/copper_clay.cygraph")
            .replace("graph \"copper_clay\"", "graph \"sway\"")
            .replace(
                "link 1 \"out\" -> 3 \"colour\"",
                concat!(
                    "node 5 \"material.constant\" v1 {\n",
                    "    prop \"type\" : \"name\" = \"float3\"\n",
                    "    prop \"value\" : \"vec4\" = (0, 0.25, 0, 0, 0)\n",
                    "}\nnode 6 \"material.vertex_output\" v1 {\n}\n",
                    "link 5 \"out\" -> 6 \"offset\"\n",
                    "link 1 \"out\" -> 3 \"colour\""
                ),
            );
    (source, graph)
}

fn material_graph_call(id: u32, name: &str, reference: &str, source: &str) -> String {
    Json::object([
        ("jsonrpc", Json::text("2.0")),
        ("id", Json::Number(f64::from(id))),
        ("method", Json::text("tools/call")),
        (
            "params",
            Json::object([
                ("name", Json::text(name)),
                (
                    "arguments",
                    Json::object([
                        ("reference", Json::text(reference)),
                        ("source", Json::text(source)),
                    ]),
                ),
            ]),
        ),
    ])
    .render()
}

#[test]
fn material_node_add_uses_engine_catalogue_and_undoes_over_mcp() {
    let sandbox = Sandbox::new("material-node-add");
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor.open_document("worlds/city.cyworld").unwrap();
    let (mut runtime_reader, mut runtime_writer) = install_vfx_stage_catalogue(&mut editor);
    let reference = "game/sway.cygraph";
    let source = "cymatcanvas 1\nmaterial sway\n";
    let graph = "cygraph 1\ngraph \"sway\" version 1\ncapability\ndeterministic true\n";
    let save = material_graph_call(2, "material.graph.save", reference, source);
    assert_eq!(
        result(&converse(&[INITIALIZE, &save], &mut editor), 1).get("isError"),
        &Json::Bool(false)
    );
    reply_material_author(&mut runtime_reader, &mut runtime_writer, graph);
    wait_for_material_source(&mut editor, reference, source);

    let refused = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"material.node.add","arguments":{"reference":"game/sway.cygraph","node_type":"material.unknown","x":12,"y":30}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&refused, 1).get("isError"), &Json::Bool(true));
    let add = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"material.node.add","arguments":{"reference":"game/sway.cygraph","node_type":"material.constant","x":12,"y":30}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&add, 1).get("isError"), &Json::Bool(false));
    reply_material_author(&mut runtime_reader, &mut runtime_writer, graph);
    let added = "cymatcanvas 1\nmaterial sway\nnode 1 material.constant\n# layout 1 12 30\n";
    wait_for_material_source(&mut editor, reference, added);
    let undo = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&undo, 1).get("isError"), &Json::Bool(false));
    assert_eq!(
        std::fs::read_to_string(sandbox.0.join("game/sway.cymatcanvas")).unwrap(),
        source
    );
    let redo = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"edit.redo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&redo, 1).get("isError"), &Json::Bool(false));
    assert_eq!(
        std::fs::read_to_string(sandbox.0.join("game/sway.cymatcanvas")).unwrap(),
        added
    );
}

#[test]
fn material_node_edits_round_trip_as_individual_mcp_transactions() {
    let sandbox = Sandbox::new("material-node-edits");
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor.open_document("worlds/city.cyworld").unwrap();
    let (mut runtime_reader, mut runtime_writer) = install_vfx_stage_catalogue(&mut editor);
    let reference = "game/sway.cygraph";
    let initial = "cymatcanvas 1\nmaterial sway\n";
    let graph = "cygraph 1\ngraph \"sway\" version 1\ncapability\ndeterministic true\n";
    let save = material_graph_call(2, "material.graph.save", reference, initial);
    assert_eq!(
        result(&converse(&[INITIALIZE, &save], &mut editor), 1).get("isError"),
        &Json::Bool(false)
    );
    reply_material_author(&mut runtime_reader, &mut runtime_writer, graph);
    wait_for_material_source(&mut editor, reference, initial);

    let edits = [
        r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"material.node.add","arguments":{"reference":"game/sway.cygraph","node_type":"material.constant","x":12,"y":30}}}"#,
        r#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"material.node.add","arguments":{"reference":"game/sway.cygraph","node_type":"material.sink","x":90,"y":30}}}"#,
        r#"{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"material.node.connect","arguments":{"reference":"game/sway.cygraph","from":1,"from_pin":"out","to":2,"to_pin":"in"}}}"#,
        r#"{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"material.node.move","arguments":{"reference":"game/sway.cygraph","node":1,"x":15,"y":40}}}"#,
        r#"{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"material.node.property.set","arguments":{"reference":"game/sway.cygraph","node":1,"property":"value","value":"0.5"}}}"#,
        r#"{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"material.node.disconnect","arguments":{"reference":"game/sway.cygraph","from":1,"from_pin":"out","to":2,"to_pin":"in"}}}"#,
        r#"{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"material.node.remove","arguments":{"reference":"game/sway.cygraph","node":2}}}"#,
    ];
    let mut source = initial.to_owned();
    let mut sources = Vec::new();
    for edit in edits {
        source = invoke_material_edit(
            &mut editor,
            &mut runtime_reader,
            &mut runtime_writer,
            graph,
            reference,
            edit,
            &source,
        );
        sources.push(source.clone());
    }
    assert!(sources[1].contains("node 2 material.sink"));
    assert!(sources[2].contains("link 1 out 2 in"));
    assert!(sources[3].contains("# layout 1 15 40"));
    assert!(sources[4].contains("prop 1 value 0.5"));
    assert!(!sources[5].contains("link 1 out 2 in"));
    assert!(!sources[6].contains("node 2 material.sink"));
    assert_eq!(
        editor
            .documents
            .get(editor.workspace.active().unwrap())
            .unwrap()
            .history()
            .entries()
            .len(),
        8,
        "the first save and seven node gestures each record one undo entry"
    );
    assert_material_property_refusal_preserves_history(&mut editor, &sandbox.0, &sources[6]);
    let undo = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&undo, 1).get("isError"), &Json::Bool(false));
    assert_eq!(
        std::fs::read_to_string(sandbox.0.join("game/sway.cymatcanvas")).unwrap(),
        sources[5]
    );
    let redo = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"edit.redo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&redo, 1).get("isError"), &Json::Bool(false));
    assert_eq!(
        std::fs::read_to_string(sandbox.0.join("game/sway.cymatcanvas")).unwrap(),
        sources[6]
    );
}

#[test]
fn material_draft_gestures_undo_before_canonical_authoring() {
    let sandbox = Sandbox::new("material-draft-history");
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor.open_document("worlds/city.cyworld").unwrap();
    let (_runtime_reader, _runtime_writer) = install_vfx_stage_catalogue(&mut editor);
    let reference = "game/draft.cygraph";
    let blank = "cymatcanvas 1\nmaterial draft\n";
    let draft = material_graph_call(2, "material.canvas.draft.save", reference, blank);
    assert_eq!(
        result(&converse(&[INITIALIZE, &draft], &mut editor), 1).get("isError"),
        &Json::Bool(false)
    );
    assert!(!sandbox.0.join(reference).exists());
    assert_eq!(
        std::fs::read_to_string(sandbox.0.join("game/draft.cymatcanvas")).unwrap(),
        blank
    );

    let add = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"material.node.add","arguments":{"reference":"game/draft.cygraph","node_type":"material.constant","x":12,"y":30}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&add, 1).get("isError"), &Json::Bool(false));
    let with_node = std::fs::read_to_string(sandbox.0.join("game/draft.cymatcanvas")).unwrap();
    assert!(with_node.contains("node 1 material.constant"));
    assert!(!sandbox.0.join(reference).exists());
    let undo = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&undo, 1).get("isError"), &Json::Bool(false));
    assert_eq!(
        std::fs::read_to_string(sandbox.0.join("game/draft.cymatcanvas")).unwrap(),
        blank
    );
    let redo = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"edit.redo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&redo, 1).get("isError"), &Json::Bool(false));
    assert_eq!(
        std::fs::read_to_string(sandbox.0.join("game/draft.cymatcanvas")).unwrap(),
        with_node
    );
}

fn assert_material_property_refusal_preserves_history(
    editor: &mut Editor,
    root: &std::path::Path,
    source: &str,
) {
    let invalid = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"material.node.property.set","arguments":{"reference":"game/sway.cygraph","node":1,"property":"value","value":"not-a-number"}}}"#,
        ],
        editor,
    );
    assert_eq!(result(&invalid, 1).get("isError"), &Json::Bool(true));
    assert_eq!(
        std::fs::read_to_string(root.join("game/sway.cymatcanvas")).unwrap(),
        source
    );
    assert_eq!(
        editor
            .documents
            .get(editor.workspace.active().unwrap())
            .unwrap()
            .history()
            .entries()
            .len(),
        8,
        "a refused property edit must not add a transaction"
    );
}

fn invoke_material_edit(
    editor: &mut Editor,
    runtime_reader: &mut std::io::PipeReader,
    runtime_writer: &mut std::io::PipeWriter,
    graph: &str,
    reference: &str,
    call: &str,
    before: &str,
) -> String {
    let replies = converse(&[INITIALIZE, call], editor);
    assert_eq!(result(&replies, 1).get("isError"), &Json::Bool(false));
    reply_material_author(runtime_reader, runtime_writer, graph);
    wait_for_material_source_change(editor, reference, before)
}

fn wait_for_material_source_change(editor: &mut Editor, reference: &str, before: &str) -> String {
    let source_path = editor
        .project
        .root()
        .join(reference)
        .with_extension("cymatcanvas");
    for _ in 0..100 {
        editor.pump();
        if let Ok(source) = std::fs::read_to_string(&source_path)
            && source != before
        {
            return source;
        }
        std::thread::sleep(std::time::Duration::from_millis(1));
    }
    panic!("material canvas did not change: {}", source_path.display());
}

fn reply_material_author(
    runtime_reader: &mut std::io::PipeReader,
    runtime_writer: &mut std::io::PipeWriter,
    graph: &str,
) {
    let request = material_request(runtime_reader, "material.author");
    let mut payload = Writer::new();
    payload.u32(1);
    payload.u8(1);
    payload.text(graph);
    write_frame(
        runtime_writer,
        &Message::ServiceEvent {
            request,
            kind: ServiceEventKind::Completed,
            schema_version: 1,
            payload: payload.finish(),
        }
        .encode(),
    )
    .unwrap();
}

fn wait_for_material_source(editor: &mut Editor, reference: &str, expected: &str) {
    let source_path = editor
        .project
        .root()
        .join(reference)
        .with_extension("cymatcanvas");
    for _ in 0..100 {
        editor.pump();
        if std::fs::read_to_string(&source_path).ok().as_deref() == Some(expected) {
            return;
        }
        std::thread::sleep(std::time::Duration::from_millis(1));
    }
    panic!("material canvas was not saved: {}", source_path.display());
}

fn material_request(
    reader: &mut std::io::PipeReader,
    expected: &str,
) -> cy_editor_protocol::RequestId {
    material_request_with_payload(reader, expected).0
}

fn material_request_with_payload(
    reader: &mut std::io::PipeReader,
    expected: &str,
) -> (cy_editor_protocol::RequestId, Vec<u8>) {
    (0..4)
        .find_map(|_| {
            let message = Message::decode(&read_frame(reader).unwrap().unwrap()).unwrap();
            match message {
                Message::ServiceRequest {
                    request,
                    operation,
                    payload,
                    ..
                } if operation == expected => Some((request, payload)),
                Message::ServiceRequest { operation, .. }
                    if operation == "vfx.authoring-capabilities.get" =>
                {
                    None
                }
                Message::ServiceRequest { operation, .. } => {
                    panic!("unexpected material request: {operation}")
                }
                Message::SyncWorld { .. } | Message::Apply { .. } => None,
                other => panic!("unexpected message while waiting for {expected}: {other:?}"),
            }
        })
        .unwrap_or_else(|| panic!("the MCP call must request {expected} from the engine"))
}

#[test]
fn vertex_material_canvas_saves_and_undoes_over_mcp() {
    use cy_editor_documents::operation::Operation;
    use cy_editor_services::primitives::{MaterialBinding, create_mesh_instance};
    use cy_editor_viewport::gizmo::Transform3;

    let sandbox = Sandbox::new("vertex-material-wire");
    let reference = "game/sway.cygraph";
    let (source, graph) = vertex_material_sources();

    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    let document_id = editor.open_document("worlds/city.cyworld").unwrap();
    editor
        .documents
        .get_mut(document_id)
        .unwrap()
        .with_transaction("Assign material", Actor::human("designer"), |document| {
            let node =
                create_mesh_instance(document, None, "meshes/box.cyprim", Transform3::default())?;
            let binding = MaterialBinding::of_schema(document.schema()).unwrap();
            document.record(Operation::SetAssetReference {
                node,
                component: binding.component,
                field: binding.material,
                before: String::new(),
                after: reference.into(),
            })?;
            Ok(())
        })
        .unwrap();
    let (mut runtime_reader, mut runtime_writer) = install_vfx_stage_catalogue(&mut editor);
    let save = material_graph_call(2, "material.graph.save", reference, &source);
    let replies = converse(&[INITIALIZE, &save], &mut editor);
    assert_eq!(result(&replies, 1).get("isError"), &Json::Bool(false));
    let (request, payload) = material_request_with_payload(&mut runtime_reader, "material.author");
    assert_eq!(
        payload,
        [
            b"cymatrequest 1\ngeometry StaticMesh\n".as_slice(),
            source.as_bytes()
        ]
        .concat()
    );
    let mut payload = Writer::new();
    payload.u32(1);
    payload.u8(1);
    payload.text(&graph);
    write_frame(
        &mut runtime_writer,
        &Message::ServiceEvent {
            request,
            kind: ServiceEventKind::Completed,
            schema_version: 1,
            payload: payload.finish(),
        }
        .encode(),
    )
    .unwrap();
    let graph_path = sandbox.0.join(reference);
    let canvas_path = sandbox.0.join("game/sway.cymatcanvas");
    for _ in 0..100 {
        editor.pump();
        if graph_path.exists() && canvas_path.exists() {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(1));
    }
    assert_eq!(std::fs::read_to_string(&graph_path).unwrap(), graph);
    assert_eq!(std::fs::read_to_string(&canvas_path).unwrap(), source);
    assert_eq!(
        editor
            .documents
            .get(document_id)
            .unwrap()
            .history()
            .entries()
            .len(),
        2,
        "the graph save and generated property sync share one undo entry"
    );
    let read = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"material.graph.read","arguments":{"reference":"game/sway.cygraph"}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&read, 1).get("isError"), &Json::Bool(false));
    assert!(read[1].render().contains("material.vertex_output"));

    assert_material_save_undo_redo(&mut editor, &graph_path, &canvas_path, &graph, &source);
}

fn assert_material_save_undo_redo(
    editor: &mut Editor,
    graph_path: &std::path::Path,
    canvas_path: &std::path::Path,
    graph: &str,
    source: &str,
) {
    let undo = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
        ],
        editor,
    );
    assert_eq!(result(&undo, 1).get("isError"), &Json::Bool(false));
    assert!(!graph_path.exists());
    assert!(!canvas_path.exists());
    let redo = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"edit.redo","arguments":{}}}"#,
        ],
        editor,
    );
    assert_eq!(result(&redo, 1).get("isError"), &Json::Bool(false));
    assert_eq!(std::fs::read_to_string(graph_path).unwrap(), graph);
    assert_eq!(std::fs::read_to_string(canvas_path).unwrap(), source);
}

#[test]
fn vertex_material_canvas_previews_over_mcp_without_saving() {
    let sandbox = Sandbox::new("vertex-material-preview-wire");
    let reference = "game/sway.cygraph";
    let (source, _) = vertex_material_sources();
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    let document = editor.open_document("worlds/city.cyworld").unwrap();
    let (mut runtime_reader, mut runtime_writer) = install_vfx_stage_catalogue(&mut editor);
    let preview = material_graph_call(2, "material.graph.preview", reference, &source);
    let replies = converse(&[INITIALIZE, &preview], &mut editor);
    assert_eq!(result(&replies, 1).get("isError"), &Json::Bool(false));
    let request = material_request(&mut runtime_reader, "material.preview.set");
    let mut payload = Writer::new();
    payload.u32(1);
    payload.u8(1);
    write_frame(
        &mut runtime_writer,
        &Message::ServiceEvent {
            request,
            kind: ServiceEventKind::Completed,
            schema_version: 1,
            payload: payload.finish(),
        }
        .encode(),
    )
    .unwrap();
    for _ in 0..100 {
        editor.pump();
        if matches!(
            editor.backend.material_request_state(),
            MaterialRequestState::Previewed { .. }
        ) {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(1));
    }
    assert!(matches!(
        editor.backend.material_request_state(),
        MaterialRequestState::Previewed { .. }
    ));
    let status = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"material.graph.status","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert!(status[1].render().contains("previewed"));
    assert!(!sandbox.0.join(reference).exists());
    assert!(!sandbox.0.join("game/sway.cymatcanvas").exists());
    assert!(
        editor
            .documents
            .get(document)
            .unwrap()
            .history()
            .entries()
            .is_empty()
    );
}

#[test]
fn vfx_system_is_created_with_two_emitters_and_reopened_over_mcp() {
    use cy_editor_interface::specialised::vfx::{SimulationPath, VfxDocument};

    let sandbox = Sandbox::new("vfx-create-two-emitters");
    let reference = "game/two_emitters.cyvfxdoc";
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor.open_document("worlds/city.cyworld").unwrap();
    let _runtime = install_vfx_stage_catalogue(&mut editor);
    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"vfx.document.create","arguments":{"reference":"game/two_emitters.cyvfxdoc","name":"two_emitters"}}}"#,
            r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"vfx.emitter.add","arguments":{"reference":"game/two_emitters.cyvfxdoc","name":"embers_cpu","target":"cpu","renderer":"Sprite"}}}"#,
            r#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"vfx.emitter.add","arguments":{"reference":"game/two_emitters.cyvfxdoc","name":"embers_gpu","target":"gpu","renderer":"Sprite"}}}"#,
            r#"{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"vfx.document.create","arguments":{"reference":"game/two_emitters.cyvfxdoc","name":"replacement"}}}"#,
            r#"{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"vfx.document.read","arguments":{"reference":"game/two_emitters.cyvfxdoc"}}}"#,
        ],
        &mut editor,
    );
    for index in 1..=3 {
        assert_eq!(result(&replies, index).get("isError"), &Json::Bool(false));
    }
    assert_eq!(result(&replies, 4).get("isError"), &Json::Bool(true));
    assert_eq!(result(&replies, 5).get("isError"), &Json::Bool(false));
    author_vfx_spawn_stage(&mut editor, reference, "embers_cpu");
    author_vfx_spawn_stage(&mut editor, reference, "embers_gpu");
    let source = std::fs::read_to_string(sandbox.0.join(reference)).unwrap();
    let saved = VfxDocument::decode_text(&source).unwrap();
    assert_eq!(saved.emitters.len(), 2);
    assert_eq!(saved.emitters[0].path, SimulationPath::CpuRequired);
    assert_eq!(saved.emitters[1].path, SimulationPath::GpuPreferred);
    for emitter in &saved.emitters {
        let spawn = &emitter.stages[0].canvas;
        assert!(spawn.contains("node 1 vfx.constant"));
        assert!(spawn.contains("node 2 vfx.spawn_count"));
        assert!(spawn.contains("link 1 out 2 value"));
        assert!(spawn.contains("prop 1 value 3"));
    }
    assert_eq!(
        editor
            .documents
            .get(editor.workspace.active().unwrap())
            .unwrap()
            .history()
            .entries()
            .len(),
        11,
    );

    for _ in 0..11 {
        let reply = converse(
            &[
                INITIALIZE,
                r#"{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
            ],
            &mut editor,
        );
        assert_eq!(result(&reply, 1).get("isError"), &Json::Bool(false));
    }
    assert!(!sandbox.0.join(reference).exists());
    for _ in 0..11 {
        let reply = converse(
            &[
                INITIALIZE,
                r#"{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"edit.redo","arguments":{}}}"#,
            ],
            &mut editor,
        );
        assert_eq!(result(&reply, 1).get("isError"), &Json::Bool(false));
    }
    assert_eq!(
        std::fs::read_to_string(sandbox.0.join(reference)).unwrap(),
        source
    );
    let reopened = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"vfx.document.read","arguments":{"reference":"game/two_emitters.cyvfxdoc"}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&reopened, 1).get("isError"), &Json::Bool(false));
    assert_eq!(
        result(&reopened, 1)
            .get("structuredContent")
            .get("source")
            .as_text(),
        Some(source.as_str()),
    );
}

fn author_vfx_spawn_stage(editor: &mut Editor, reference: &str, emitter: &str) {
    fn request(id: u32, name: &str, arguments: Json) -> String {
        Json::object([
            ("jsonrpc", Json::text("2.0")),
            ("id", Json::Number(f64::from(id))),
            ("method", Json::text("tools/call")),
            (
                "params",
                Json::object([("name", Json::text(name)), ("arguments", arguments)]),
            ),
        ])
        .render()
    }
    fn stage(reference: &str, emitter: &str, more: &[(&'static str, Json)]) -> Json {
        let mut fields = vec![
            ("reference", Json::text(reference)),
            ("emitter", Json::text(emitter)),
            ("stage", Json::text("spawn")),
        ];
        fields.extend_from_slice(more);
        Json::object(fields)
    }
    let calls = [
        request(
            2,
            "vfx.node.add",
            stage(
                reference,
                emitter,
                &[
                    ("node_type", Json::text("vfx.constant")),
                    ("x", Json::Number(12.0)),
                    ("y", Json::Number(30.0)),
                ],
            ),
        ),
        request(
            3,
            "vfx.node.add",
            stage(
                reference,
                emitter,
                &[
                    ("node_type", Json::text("vfx.spawn_count")),
                    ("x", Json::Number(90.0)),
                    ("y", Json::Number(30.0)),
                ],
            ),
        ),
        request(
            4,
            "vfx.node.property.set",
            stage(
                reference,
                emitter,
                &[
                    ("node", Json::Number(1.0)),
                    ("property", Json::text("value")),
                    ("value", Json::text("3")),
                ],
            ),
        ),
        request(
            5,
            "vfx.node.connect",
            stage(
                reference,
                emitter,
                &[
                    ("from", Json::Number(1.0)),
                    ("from_pin", Json::text("out")),
                    ("to", Json::Number(2.0)),
                    ("to_pin", Json::text("value")),
                ],
            ),
        ),
    ];
    let lines: Vec<&str> = std::iter::once(INITIALIZE)
        .chain(calls.iter().map(String::as_str))
        .collect();
    let replies = converse(&lines, editor);
    for index in 1..=4 {
        assert_eq!(result(&replies, index).get("isError"), &Json::Bool(false));
    }
}

#[test]
#[expect(
    clippy::too_many_lines,
    reason = "full sample requires a linear MCP command sequence"
)]
fn committed_two_emitter_sample_can_be_authored_through_mcp_commands() {
    use cy_editor_interface::specialised::vfx::VfxDocument;

    fn call(lines: &mut Vec<String>, name: &str, fields: Vec<(&'static str, Json)>) {
        let id = u32::try_from(lines.len() + 1).unwrap();
        let arguments = Json::object(fields);
        lines.push(
            Json::object([
                ("jsonrpc", Json::text("2.0")),
                ("id", Json::Number(f64::from(id))),
                ("method", Json::text("tools/call")),
                (
                    "params",
                    Json::object([("name", Json::text(name)), ("arguments", arguments)]),
                ),
            ])
            .render(),
        );
    }

    fn stage_fields(reference: &str, emitter: &str, stage: &str) -> Vec<(&'static str, Json)> {
        vec![
            ("reference", Json::text(reference)),
            ("emitter", Json::text(emitter)),
            ("stage", Json::text(stage)),
        ]
    }

    fn normalise_canvas(canvas: &str) -> String {
        let mut facts: Vec<&str> = canvas
            .lines()
            .filter(|line| !line.starts_with("# layout "))
            .collect();
        facts.sort_unstable();
        facts.join("\n")
    }

    let sandbox = Sandbox::new("vfx-committed-sample-over-mcp");
    let reference = "game/issue15_two_emitters.cyvfxdoc";
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor.open_document("worlds/city.cyworld").unwrap();
    let _runtime = install_vfx_stage_catalogue(&mut editor);
    let mut lines = vec![INITIALIZE.to_string()];
    call(
        &mut lines,
        "vfx.document.create",
        vec![
            ("reference", Json::text(reference)),
            ("name", Json::text("Issue15Sparks")),
        ],
    );
    for (emitter, target) in [("CpuEmitter", "cpu"), ("GpuEmitter", "gpu")] {
        call(
            &mut lines,
            "vfx.emitter.add",
            vec![
                ("reference", Json::text(reference)),
                ("name", Json::text(emitter)),
                ("target", Json::text(target)),
                ("renderer", Json::text("Sprite")),
            ],
        );
    }
    call(
        &mut lines,
        "vfx.parameter.set",
        vec![
            ("reference", Json::text(reference)),
            ("name", Json::text("speed")),
            ("kind", Json::text("float")),
            (
                "values",
                Json::Array(
                    vec![2.0, 0.0, 0.0, 0.0]
                        .into_iter()
                        .map(Json::Number)
                        .collect(),
                ),
            ),
            ("exposed", Json::Bool(true)),
        ],
    );
    call(
        &mut lines,
        "vfx.channel.set",
        vec![
            ("reference", Json::text(reference)),
            ("name", Json::text("on_death")),
            ("max_events_per_frame", Json::Number(128.0)),
            ("max_chain_depth", Json::Number(2.0)),
            ("readback", Json::Bool(false)),
        ],
    );
    let attributes = [
        ("position", "vec3", -100.0, 100.0, 0.0),
        ("lifetime", "float", 0.0, 8.0, 0.0),
        ("size", "float", 0.0, 1.0, 0.0),
        ("color", "vec4", 0.0, 1.0, f64::from(1.0_f32 / 255.0)),
        ("emission", "float", 0.0, 40000.0, 0.0),
    ];
    let constants = [
        ("position", "0 0 0"),
        ("lifetime", "2"),
        ("size", "0.22"),
        ("color", "1 0.45 0.12 0.9"),
        ("emission", "14000"),
    ];
    for emitter in ["CpuEmitter", "GpuEmitter"] {
        call(
            &mut lines,
            "vfx.emitter.capacity.set",
            vec![
                ("reference", Json::text(reference)),
                ("emitter", Json::text(emitter)),
                ("capacity", Json::Number(2048.0)),
            ],
        );
        call(
            &mut lines,
            "vfx.interface.bind",
            vec![
                ("reference", Json::text(reference)),
                ("emitter", Json::text(emitter)),
                ("interface", Json::text("texture")),
            ],
        );
        for (name, kind, minimum, maximum, tolerance) in attributes {
            call(
                &mut lines,
                "vfx.attribute.set",
                vec![
                    ("reference", Json::text(reference)),
                    ("emitter", Json::text(emitter)),
                    ("name", Json::text(name)),
                    ("kind", Json::text(kind)),
                    ("minimum", Json::Number(minimum)),
                    ("maximum", Json::Number(maximum)),
                    ("tolerance", Json::Number(tolerance)),
                    ("precision", Json::text("Auto")),
                ],
            );
        }
        for (node_type, x) in [("vfx.parameter", 12.0), ("vfx.spawn_count", 210.0)] {
            let mut fields = stage_fields(reference, emitter, "spawn");
            fields.extend([
                ("node_type", Json::text(node_type)),
                ("x", Json::Number(x)),
                ("y", Json::Number(34.0)),
            ]);
            call(&mut lines, "vfx.node.add", fields);
        }
        let mut fields = stage_fields(reference, emitter, "spawn");
        fields.extend([
            ("node", Json::Number(1.0)),
            ("property", Json::text("parameter")),
            ("value", Json::text("speed")),
        ]);
        call(&mut lines, "vfx.node.property.set", fields);
        let mut fields = stage_fields(reference, emitter, "spawn");
        fields.extend([
            ("from", Json::Number(1.0)),
            ("from_pin", Json::text("out")),
            ("to", Json::Number(2.0)),
            ("to_pin", Json::text("value")),
        ]);
        call(&mut lines, "vfx.node.connect", fields);
        for (index, (attribute, value)) in constants.into_iter().enumerate() {
            let index = u32::try_from(index).unwrap();
            let y = 34.0 + 70.0 * f64::from(index);
            for (node_type, x) in [("vfx.constant", 12.0), ("vfx.set_attribute", 210.0)] {
                let mut fields = stage_fields(reference, emitter, "initialise");
                fields.extend([
                    ("node_type", Json::text(node_type)),
                    ("x", Json::Number(x)),
                    ("y", Json::Number(y)),
                ]);
                call(&mut lines, "vfx.node.add", fields);
            }
            let source = f64::from(index * 2 + 1);
            let target = source + 1.0;
            for (node, property, value) in
                [(source, "value", value), (target, "attribute", attribute)]
            {
                let mut fields = stage_fields(reference, emitter, "initialise");
                fields.extend([
                    ("node", Json::Number(node)),
                    ("property", Json::text(property)),
                    ("value", Json::text(value)),
                ]);
                call(&mut lines, "vfx.node.property.set", fields);
            }
            let mut fields = stage_fields(reference, emitter, "initialise");
            fields.extend([
                ("from", Json::Number(source)),
                ("from_pin", Json::text("out")),
                ("to", Json::Number(target)),
                ("to_pin", Json::text("value")),
            ]);
            call(&mut lines, "vfx.node.connect", fields);
        }
    }
    for (batch, calls) in lines[1..].chunks(40).enumerate() {
        if batch != 0 {
            std::thread::sleep(std::time::Duration::from_millis(1100));
        }
        let requests: Vec<&str> = std::iter::once(INITIALIZE)
            .chain(calls.iter().map(String::as_str))
            .collect();
        let replies = converse(&requests, &mut editor);
        for index in 1..=calls.len() {
            assert_eq!(
                result(&replies, index).get("isError"),
                &Json::Bool(false),
                "request {}: {}",
                batch * 40 + index,
                result(&replies, index).render()
            );
        }
    }

    let saved_source = std::fs::read_to_string(sandbox.0.join(reference)).unwrap();
    let refused = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"vfx.node.property.set","arguments":{"reference":"game/issue15_two_emitters.cyvfxdoc","emitter":"CpuEmitter","stage":"initialise","node":1,"property":"value","value":"1 2 3 4 5"}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&refused, 1).get("isError"), &Json::Bool(true));
    assert!(
        result(&refused, 1)
            .render()
            .contains("one to four numeric components")
    );
    assert_eq!(
        std::fs::read_to_string(sandbox.0.join(reference)).unwrap(),
        saved_source
    );
    let actual = VfxDocument::decode_text(&saved_source).unwrap();
    let expected = VfxDocument::decode_text(include_str!(
        "../../../../samples/05b-editor-window/project/effects/issue15_two_emitters.cyvfxdoc"
    ))
    .unwrap();
    let mut actual_semantic = actual;
    let mut expected_semantic = expected;
    for document in [&mut actual_semantic, &mut expected_semantic] {
        for emitter in &mut document.emitters {
            for stage in &mut emitter.stages {
                stage.canvas = normalise_canvas(&stage.canvas);
            }
        }
    }
    assert_eq!(actual_semantic, expected_semantic);
    let reopened = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"vfx.document.read","arguments":{"reference":"game/issue15_two_emitters.cyvfxdoc"}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&reopened, 1).get("isError"), &Json::Bool(false));
}

#[test]
fn vfx_stage_wire_and_property_round_trip_over_mcp() {
    use cy_editor_interface::specialised::vfx::VfxDocument;

    let sandbox = Sandbox::new("vfx-stage-wire");
    let reference = "game/sparks.cyvfxdoc";
    std::fs::write(
        sandbox.0.join(reference),
        VfxDocument::new("sparks").unwrap().encode_text().unwrap(),
    )
    .unwrap();
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor.open_document("worlds/city.cyworld").unwrap();
    install_vfx_stage_catalogue(&mut editor);

    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"vfx.emitter.add","arguments":{"reference":"game/sparks.cyvfxdoc","name":"embers","target":"cpu","renderer":"Sprite"}}}"#,
            r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"vfx.node.add","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"embers","stage":"spawn","node_type":"vfx.constant","x":12,"y":30}}}"#,
            r#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"vfx.node.add","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"embers","stage":"spawn","node_type":"vfx.spawn_count","x":90,"y":30}}}"#,
            r#"{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"vfx.node.property.set","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"embers","stage":"spawn","node":1,"property":"value","value":"3"}}}"#,
            r#"{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"vfx.node.connect","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"embers","stage":"spawn","from":1,"from_pin":"out","to":2,"to_pin":"value"}}}"#,
        ],
        &mut editor,
    );
    for index in 1..=5 {
        assert_eq!(result(&replies, index).get("isError"), &Json::Bool(false));
    }
    let read = || {
        VfxDocument::decode_text(&std::fs::read_to_string(sandbox.0.join(reference)).unwrap())
            .unwrap()
    };
    let connected = read();
    let canvas = &connected.emitters[0].stages[0].canvas;
    assert!(canvas.contains("node 1 vfx.constant"));
    assert!(canvas.contains("node 2 vfx.spawn_count"));
    assert!(canvas.contains("link 1 out 2 value"));
    assert!(canvas.contains("prop 1 value 3"));

    let undone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&undone, 1).get("isError"), &Json::Bool(false));
    assert!(
        !read().emitters[0].stages[0]
            .canvas
            .contains("link 1 out 2 value")
    );
    let redone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"edit.redo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&redone, 1).get("isError"), &Json::Bool(false));
    assert_eq!(read(), connected);

    exercise_vfx_stage_moves(&mut editor, &sandbox, &connected);
}

fn exercise_vfx_stage_moves(
    editor: &mut Editor,
    sandbox: &Sandbox,
    connected: &cy_editor_interface::specialised::vfx::VfxDocument,
) {
    use cy_editor_interface::specialised::vfx::VfxDocument;

    let read = || {
        VfxDocument::decode_text(
            &std::fs::read_to_string(sandbox.0.join("game/sparks.cyvfxdoc")).unwrap(),
        )
        .unwrap()
    };

    let moved = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"vfx.node.move","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"embers","stage":"spawn","node":1,"x":40,"y":50}}}"#,
        ],
        editor,
    );
    assert_eq!(result(&moved, 1).get("isError"), &Json::Bool(false));
    assert!(
        read().emitters[0].stages[0]
            .canvas
            .contains("# layout 1 40 50")
    );
    let undo_move = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":13,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
        ],
        editor,
    );
    assert_eq!(result(&undo_move, 1).get("isError"), &Json::Bool(false));
    assert_eq!(&read(), connected);

    let disconnected = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"vfx.node.disconnect","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"embers","stage":"spawn","from":1,"from_pin":"out","to":2,"to_pin":"value"}}}"#,
        ],
        editor,
    );
    assert_eq!(result(&disconnected, 1).get("isError"), &Json::Bool(false));
    assert!(
        !read().emitters[0].stages[0]
            .canvas
            .contains("link 1 out 2 value")
    );
    let removed = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"vfx.node.remove","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"embers","stage":"spawn","node":1}}}"#,
        ],
        editor,
    );
    assert_eq!(result(&removed, 1).get("isError"), &Json::Bool(false));
    assert!(
        !read().emitters[0].stages[0]
            .canvas
            .contains("node 1 vfx.constant")
    );
    for expected in ["node 1 vfx.constant", "link 1 out 2 value"] {
        let replies = converse(
            &[
                INITIALIZE,
                r#"{"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
            ],
            editor,
        );
        assert_eq!(result(&replies, 1).get("isError"), &Json::Bool(false));
        assert!(read().emitters[0].stages[0].canvas.contains(expected));
    }
    assert_eq!(&read(), connected);
}

#[test]
fn vfx_module_canvas_round_trips_over_mcp() {
    use cy_editor_interface::specialised::vfx_module::VfxModule;

    let sandbox = Sandbox::new("vfx-module-canvas");
    let reference = "game/shared_spawn.cyvfxmodule";
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor.open_document("worlds/city.cyworld").unwrap();
    install_vfx_stage_catalogue(&mut editor);

    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"vfx.module.create","arguments":{"reference":"game/shared_spawn.cyvfxmodule","name":"shared_spawn","stage":"spawn"}}}"#,
            r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"vfx.module.node.add","arguments":{"reference":"game/shared_spawn.cyvfxmodule","node_type":"vfx.constant","x":12,"y":30}}}"#,
            r#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"vfx.module.node.add","arguments":{"reference":"game/shared_spawn.cyvfxmodule","node_type":"vfx.spawn_count","x":90,"y":30}}}"#,
            r#"{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"vfx.module.node.property.set","arguments":{"reference":"game/shared_spawn.cyvfxmodule","node":1,"property":"value","value":"5"}}}"#,
            r#"{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"vfx.module.node.connect","arguments":{"reference":"game/shared_spawn.cyvfxmodule","from":1,"from_pin":"out","to":2,"to_pin":"value"}}}"#,
        ],
        &mut editor,
    );
    for index in 1..=5 {
        assert_eq!(result(&replies, index).get("isError"), &Json::Bool(false));
    }
    let read = || {
        VfxModule::decode_text(&std::fs::read_to_string(sandbox.0.join(reference)).unwrap())
            .unwrap()
    };
    let connected = read();
    assert!(connected.canvas.contains("prop 1 value 5"));
    assert!(connected.canvas.contains("link 1 out 2 value"));
    let undone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&undone, 1).get("isError"), &Json::Bool(false));
    assert!(!read().canvas.contains("link 1 out 2 value"));
    let redone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"edit.redo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&redone, 1).get("isError"), &Json::Bool(false));
    assert_eq!(read(), connected);

    let moved = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"vfx.module.node.move","arguments":{"reference":"game/shared_spawn.cyvfxmodule","node":1,"x":40,"y":50}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&moved, 1).get("isError"), &Json::Bool(false));
    assert!(read().canvas.contains("# layout 1 40 50"));
    let undo_move = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&undo_move, 1).get("isError"), &Json::Bool(false));
    assert_eq!(read(), connected);
}

#[test]
fn vfx_capacity_attributes_and_channels_round_trip_with_mcp_undo() {
    use cy_editor_interface::specialised::vfx::VfxDocument;

    let sandbox = Sandbox::new("vfx-metadata-authoring");
    let reference = "game/sparks.cyvfxdoc";
    std::fs::write(
        sandbox.0.join(reference),
        VfxDocument::new("sparks").unwrap().encode_text().unwrap(),
    )
    .unwrap();
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor.open_document("worlds/city.cyworld").unwrap();

    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"vfx.emitter.add","arguments":{"reference":"game/sparks.cyvfxdoc","name":"embers","target":"cpu","renderer":"Sprite"}}}"#,
            r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"vfx.emitter.capacity.set","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"embers","capacity":2048}}}"#,
            r#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"vfx.attribute.set","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"embers","name":"velocity","kind":"vec3","minimum":-8,"maximum":8,"tolerance":0.01,"precision":"Float16"}}}"#,
            r#"{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"vfx.channel.set","arguments":{"reference":"game/sparks.cyvfxdoc","name":"impact","max_events_per_frame":32,"max_chain_depth":3,"readback":true}}}"#,
        ],
        &mut editor,
    );
    for index in 1..=4 {
        assert_eq!(result(&replies, index).get("isError"), &Json::Bool(false));
    }
    let read = || {
        VfxDocument::decode_text(&std::fs::read_to_string(sandbox.0.join(reference)).unwrap())
            .unwrap()
    };
    assert_eq!(read().emitters[0].capacity, 2048);
    assert_eq!(read().emitters[0].attributes[0].precision, "Float16");
    assert_eq!(read().channels[0].name, "impact");

    let refused = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"vfx.emitter.capacity.set","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"embers","capacity":0}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&refused, 1).get("isError"), &Json::Bool(true));
    assert_eq!(read().emitters[0].capacity, 2048);

    let undone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&undone, 1).get("isError"), &Json::Bool(false));
    assert!(read().channels.is_empty());
    assert_eq!(read().emitters[0].attributes[0].name, "velocity");

    let redone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"edit.redo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&redone, 1).get("isError"), &Json::Bool(false));
    assert_eq!(read().channels[0].name, "impact");

    let removed = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"vfx.attribute.remove","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"embers","name":"velocity"}}}"#,
            r#"{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"vfx.channel.remove","arguments":{"reference":"game/sparks.cyvfxdoc","name":"impact"}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&removed, 1).get("isError"), &Json::Bool(false));
    assert_eq!(result(&removed, 2).get("isError"), &Json::Bool(false));
    assert!(read().emitters[0].attributes.is_empty());
    assert!(read().channels.is_empty());

    let parameter = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"vfx.parameter.set","arguments":{"reference":"game/sparks.cyvfxdoc","name":"speed","kind":"float","values":[2,0,0,0],"exposed":true}}}"#,
            r#"{"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"vfx.parameter.remove","arguments":{"reference":"game/sparks.cyvfxdoc","name":"speed"}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&parameter, 1).get("isError"), &Json::Bool(false));
    assert_eq!(result(&parameter, 2).get("isError"), &Json::Bool(false));
    assert!(read().parameters.is_empty());
}

#[test]
fn vfx_canvas_removal_commands_are_projected_over_mcp() {
    let mut editor = Editor::new(Actor::human("designer"));
    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/list"}"#,
        ],
        &mut editor,
    );
    let Json::Array(tools) = result(&replies, 1).get("tools") else {
        panic!("tools/list must contain VFX canvas commands");
    };
    for command in [
        "vfx.node.disconnect",
        "vfx.node.remove",
        "vfx.module.node.add",
        "vfx.module.node.connect",
        "vfx.module.node.disconnect",
        "vfx.module.node.remove",
        "vfx.module.node.property.set",
    ] {
        assert!(
            tools
                .iter()
                .any(|tool| tool.get("name").as_text() == Some(command)),
            "{command} must be available over MCP"
        );
    }
}

#[test]
fn vfx_renderer_target_and_interface_edits_round_trip_over_mcp() {
    use cy_editor_interface::specialised::vfx::{SimulationPath, VfxDocument};

    let sandbox = Sandbox::new("vfx-emitter-settings");
    let reference = "game/sparks.cyvfxdoc";
    std::fs::write(
        sandbox.0.join(reference),
        VfxDocument::new("sparks").unwrap().encode_text().unwrap(),
    )
    .unwrap();
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor.open_document("worlds/city.cyworld").unwrap();
    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"vfx.emitter.add","arguments":{"reference":"game/sparks.cyvfxdoc","name":"embers","target":"gpu","renderer":"Sprite"}}}"#,
            r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"vfx.emitter.configure","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"embers","target":"cpu","renderer":"Mesh"}}}"#,
            r#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"vfx.interface.bind","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"embers","interface":"texture"}}}"#,
        ],
        &mut editor,
    );
    for index in 1..=3 {
        assert_eq!(result(&replies, index).get("isError"), &Json::Bool(false));
    }
    let read_document = || {
        VfxDocument::decode_text(&std::fs::read_to_string(sandbox.0.join(reference)).unwrap())
            .unwrap()
    };
    let saved = read_document();
    assert_eq!(saved.emitters[0].path, SimulationPath::CpuRequired);
    assert_eq!(saved.emitters[0].renderer, "Mesh");
    assert_eq!(saved.emitters[0].interfaces, ["texture"]);

    let refused = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"vfx.interface.bind","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"embers","interface":"texture"}}}"#,
            r#"{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"vfx.emitter.configure","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"embers","target":"automatic","renderer":"Mesh"}}}"#,
        ],
        &mut editor,
    );
    for index in 1..=2 {
        assert_eq!(result(&refused, index).get("isError"), &Json::Bool(true));
    }
    assert_eq!(read_document(), saved);

    let undone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&undone, 1).get("isError"), &Json::Bool(false));
    assert!(read_document().emitters[0].interfaces.is_empty());
    let redone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"edit.redo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&redone, 1).get("isError"), &Json::Bool(false));
    assert_eq!(read_document(), saved);

    let removed = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"vfx.interface.unbind","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"embers","interface":"texture"}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&removed, 1).get("isError"), &Json::Bool(false));
    assert!(read_document().emitters[0].interfaces.is_empty());
}

#[test]
fn vfx_emitter_removal_preserves_other_emitters_and_undoes_over_mcp() {
    use cy_editor_interface::specialised::vfx::VfxDocument;

    let sandbox = Sandbox::new("vfx-emitter-removal");
    let reference = "game/sparks.cyvfxdoc";
    std::fs::write(
        sandbox.0.join(reference),
        VfxDocument::new("sparks").unwrap().encode_text().unwrap(),
    )
    .unwrap();
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor.open_document("worlds/city.cyworld").unwrap();
    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"vfx.emitter.add","arguments":{"reference":"game/sparks.cyvfxdoc","name":"smoke","target":"cpu","renderer":"Sprite"}}}"#,
            r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"vfx.emitter.add","arguments":{"reference":"game/sparks.cyvfxdoc","name":"embers","target":"gpu","renderer":"Mesh"}}}"#,
            r#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"vfx.emitter.remove","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"smoke"}}}"#,
        ],
        &mut editor,
    );
    for index in 1..=3 {
        assert_eq!(result(&replies, index).get("isError"), &Json::Bool(false));
    }
    let read_document = || {
        VfxDocument::decode_text(&std::fs::read_to_string(sandbox.0.join(reference)).unwrap())
            .unwrap()
    };
    let removed = read_document();
    assert_eq!(removed.emitters.len(), 1);
    assert_eq!(removed.emitters[0].name, "embers");
    assert_eq!(removed.emitters[0].renderer, "Mesh");

    let refused = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"vfx.emitter.remove","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"missing"}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&refused, 1).get("isError"), &Json::Bool(true));
    assert_eq!(read_document(), removed);

    let undone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&undone, 1).get("isError"), &Json::Bool(false));
    assert_eq!(read_document().emitters.len(), 2);
    let redone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"edit.redo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&redone, 1).get("isError"), &Json::Bool(false));
    assert_eq!(read_document(), removed);
}

#[test]
fn reusable_vfx_module_edits_and_attachment_round_trip_over_mcp() {
    use cy_editor_interface::specialised::vfx::VfxDocument;
    use cy_editor_interface::specialised::vfx_module::VfxModule;

    let sandbox = Sandbox::new("vfx-module-authoring");
    let system = "game/sparks.cyvfxdoc";
    let module = "game/shared_drag.cyvfxmodule";
    std::fs::write(
        sandbox.0.join(system),
        VfxDocument::new("sparks").unwrap().encode_text().unwrap(),
    )
    .unwrap();
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor.open_document("worlds/city.cyworld").unwrap();

    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"vfx.emitter.add","arguments":{"reference":"game/sparks.cyvfxdoc","name":"embers","target":"cpu","renderer":"Sprite"}}}"#,
            r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"vfx.module.create","arguments":{"reference":"game/shared_drag.cyvfxmodule","name":"shared_drag","stage":"update"}}}"#,
            r#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"vfx.module.input.add","arguments":{"reference":"game/shared_drag.cyvfxmodule","name":"velocity","kind":"vec3"}}}"#,
            r#"{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"vfx.module.dependency.add","arguments":{"reference":"game/shared_drag.cyvfxmodule","name":"shared_noise"}}}"#,
            r#"{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"vfx.module.attach","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"embers","module_reference":"game/shared_drag.cyvfxmodule"}}}"#,
        ],
        &mut editor,
    );
    for index in 1..=5 {
        assert_eq!(result(&replies, index).get("isError"), &Json::Bool(false));
    }
    let saved_module =
        VfxModule::decode_text(&std::fs::read_to_string(sandbox.0.join(module)).unwrap()).unwrap();
    assert_eq!(saved_module.inputs[0].kind, "vec3");
    assert_eq!(saved_module.dependencies, ["shared_noise"]);
    let saved_system =
        VfxDocument::decode_text(&std::fs::read_to_string(sandbox.0.join(system)).unwrap())
            .unwrap();
    assert_eq!(saved_system.emitters[0].modules, ["shared_drag"]);
    assert_eq!(saved_system.module_assets[0].path, module);

    let duplicate = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"vfx.module.create","arguments":{"reference":"game/shared_drag.cyvfxmodule","name":"replacement","stage":"spawn"}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&duplicate, 1).get("isError"), &Json::Bool(true));
    assert_eq!(
        VfxModule::decode_text(&std::fs::read_to_string(sandbox.0.join(module)).unwrap()).unwrap(),
        saved_module
    );
    let undone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&undone, 1).get("isError"), &Json::Bool(false));
    let after_undo =
        VfxDocument::decode_text(&std::fs::read_to_string(sandbox.0.join(system)).unwrap())
            .unwrap();
    assert!(after_undo.emitters[0].modules.is_empty());
    assert!(after_undo.module_assets.is_empty());
    let redone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"edit.redo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&redone, 1).get("isError"), &Json::Bool(false));
    assert_eq!(
        VfxDocument::decode_text(&std::fs::read_to_string(sandbox.0.join(system)).unwrap())
            .unwrap(),
        saved_system
    );
}

#[test]
fn reusable_vfx_module_stage_change_round_trips_with_history_over_mcp() {
    use cy_editor_interface::specialised::vfx::Stage;
    use cy_editor_interface::specialised::vfx_module::VfxModule;

    let sandbox = Sandbox::new("vfx-module-stage");
    let reference = "game/shared_drag.cyvfxmodule";
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor.open_document("worlds/city.cyworld").unwrap();
    let read_stage = || {
        VfxModule::decode_text(&std::fs::read_to_string(sandbox.0.join(reference)).unwrap())
            .unwrap()
            .stage
    };

    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"vfx.module.create","arguments":{"reference":"game/shared_drag.cyvfxmodule","name":"shared_drag","stage":"update"}}}"#,
            r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"vfx.module.stage.set","arguments":{"reference":"game/shared_drag.cyvfxmodule","stage":"spawn"}}}"#,
            r#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"vfx.module.stage.set","arguments":{"reference":"game/shared_drag.cyvfxmodule","stage":"unknown"}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&replies, 1).get("isError"), &Json::Bool(false));
    assert_eq!(result(&replies, 2).get("isError"), &Json::Bool(false));
    assert_eq!(result(&replies, 3).get("isError"), &Json::Bool(true));
    assert_eq!(read_stage(), Stage::Spawn);

    let undone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&undone, 1).get("isError"), &Json::Bool(false));
    assert_eq!(read_stage(), Stage::Update);
    let redone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"edit.redo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&redone, 1).get("isError"), &Json::Bool(false));
    assert_eq!(read_stage(), Stage::Spawn);
}

#[test]
fn vfx_module_declarations_can_be_removed_and_undone_over_mcp() {
    use cy_editor_interface::specialised::vfx::Stage;
    use cy_editor_interface::specialised::vfx_module::{ModuleInput, VfxModule};

    let sandbox = Sandbox::new("vfx-module-removal");
    let reference = "game/shared_drag.cyvfxmodule";
    let mut module = VfxModule::new("shared_drag", Stage::Update).unwrap();
    module.inputs.push(ModuleInput {
        name: "velocity".into(),
        kind: "vec3".into(),
    });
    module.dependencies.push("shared_noise".into());
    std::fs::write(sandbox.0.join(reference), module.encode_text().unwrap()).unwrap();
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor.open_document("worlds/city.cyworld").unwrap();

    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"vfx.module.input.remove","arguments":{"reference":"game/shared_drag.cyvfxmodule","name":"velocity"}}}"#,
            r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"vfx.module.dependency.remove","arguments":{"reference":"game/shared_drag.cyvfxmodule","name":"shared_noise"}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&replies, 1).get("isError"), &Json::Bool(false));
    assert_eq!(result(&replies, 2).get("isError"), &Json::Bool(false));
    let read = || {
        VfxModule::decode_text(&std::fs::read_to_string(sandbox.0.join(reference)).unwrap())
            .unwrap()
    };
    assert!(read().inputs.is_empty());
    assert!(read().dependencies.is_empty());
    let undone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&undone, 1).get("isError"), &Json::Bool(false));
    assert!(read().inputs.is_empty());
    assert_eq!(read().dependencies, ["shared_noise"]);
}

/// `text` as a JSON string literal.
fn quoted(text: &str) -> String {
    let mut out = String::from("\"");
    for character in text.chars() {
        match character {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            other => out.push(other),
        }
    }
    out.push('"');
    out
}

fn hex_envelope(header: &str, bytes: Vec<u8>) -> String {
    use std::fmt::Write as _;
    let mut text = header.to_owned();
    for byte in bytes {
        write!(text, "{byte:02x}").expect("writing to a string");
    }
    text
}

/// A reusable Update module `shared_drag` that writes `value` to every particle's size.
fn drag_module(value: &str) -> String {
    let mut out = cy_editor_core::codec::Writer::new();
    out.u32(1);
    out.text("shared_drag");
    out.u8(2);
    out.u32(0);
    out.u32(0);
    out.text(&format!(
        "cyvfxcanvas 1\nmodule shared_drag\nnode 1 vfx.constant\nprop 1 value {value}\n\
         node 2 vfx.set_attribute\nprop 2 attribute size\nlink 1 out 2 value\n"
    ));
    hex_envelope("cyvfxmodule 1\n", out.finish())
}

/// A one-emitter version 3 system whose emitter references `shared_drag` at `module`.
fn system_using_drag(module: &str) -> String {
    let mut out = cy_editor_core::codec::Writer::new();
    out.u32(3);
    out.text("sparks");
    out.u32(1);
    out.text("smoke");
    out.u8(0);
    out.text("Sprite");
    out.u32(0);
    out.u32(1);
    out.text("shared_drag");
    for word in [0, 1024, 0, 0, 0] {
        out.u32(word);
    }
    out.u32(1);
    out.text("shared_drag");
    out.text(module);
    hex_envelope("cyvfxdoc 1\n", out.finish())
}

fn tool_call(id: u32, name: &str, arguments: &[(&str, &str)]) -> String {
    let arguments: Vec<String> = arguments
        .iter()
        .map(|(key, value)| format!("{}:{}", quoted(key), quoted(value)))
        .collect();
    format!(
        r#"{{"jsonrpc":"2.0","id":{id},"method":"tools/call","params":{{"name":"{name}","arguments":{{{}}}}}}}"#,
        arguments.join(",")
    )
}

fn tool_text(replies: &[Json], index: usize) -> String {
    let Json::Array(content) = result(replies, index).get("content") else {
        panic!(
            "reply {index} is not tool content: {:?}",
            replies.get(index)
        );
    };
    content[0]
        .get("text")
        .as_text()
        .unwrap_or_default()
        .to_owned()
}

#[test]
fn a_vfx_module_and_its_user_save_reopen_undo_and_redo_over_mcp() {
    let sandbox = Sandbox::new("vfx-module");
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor
        .open_document("worlds/city.cyworld")
        .expect("a document opens");
    let module = "effects/modules/shared_drag.cyvfxmodule";
    let system = "effects/sparks.cyvfxdoc";
    let first = drag_module("0.5");
    let edited = drag_module("0.25");
    let source = system_using_drag(module);
    let lines = [
        INITIALIZE.to_owned(),
        r#"{"jsonrpc":"2.0","id":2,"method":"tools/list"}"#.to_owned(),
        tool_call(
            3,
            "vfx.module.save",
            &[("reference", module), ("source", &first)],
        ),
        tool_call(
            4,
            "vfx.document.save",
            &[("reference", system), ("source", &source)],
        ),
        tool_call(
            5,
            "vfx.module.save",
            &[("reference", module), ("source", &edited)],
        ),
        tool_call(6, "vfx.module.read", &[("reference", module)]),
        tool_call(7, "vfx.document.read", &[("reference", system)]),
        tool_call(8, "edit.undo", &[]),
        tool_call(9, "vfx.module.read", &[("reference", module)]),
        tool_call(10, "edit.redo", &[]),
        tool_call(11, "vfx.module.read", &[("reference", module)]),
    ];
    let lines: Vec<&str> = lines.iter().map(String::as_str).collect();
    let replies = converse_within(&lines, &mut editor, "effects/");

    let Json::Array(tools) = result(&replies, 1).get("tools") else {
        panic!("tools/list must contain the registry projection");
    };
    for command in ["vfx.module.save", "vfx.module.read"] {
        assert!(
            tools
                .iter()
                .any(|tool| tool.get("name").as_text() == Some(command)),
            "{command} must be available over MCP"
        );
    }
    for index in 2..=10 {
        assert_eq!(
            result(&replies, index).get("isError"),
            &Json::Bool(false),
            "reply {index}: {}",
            tool_text(&replies, index)
        );
    }
    assert!(tool_text(&replies, 5).contains(&edited));
    assert!(tool_text(&replies, 6).contains(&source));
    assert!(tool_text(&replies, 8).contains(&first));
    assert!(tool_text(&replies, 10).contains(&edited));
    assert_eq!(
        std::fs::read_to_string(sandbox.0.join(module)).expect("the module is saved"),
        edited
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
    // One server across both halves, deliberately: it reads the focused viewport after the
    // runtime delivers a frame, without opening an unpumped agent viewport.
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

    // The desktop transport pumps the focused viewport, so MCP reads that same runtime frame.
    let agent_viewport = editor.viewports.focused_id();
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
        0,
        "ordinary frame reads do not open an unpumped viewport"
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
            r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"source.write","arguments":{"path":"game/Player.swift","contents":"struct Player {}","expected_fingerprint":"missing","base":""}}}"#,
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

#[test]
fn an_mcp_save_with_a_stale_fingerprint_reports_conflict_without_overwriting() {
    let sandbox = Sandbox::new("source-conflict");
    let path = sandbox.0.join("game/Player.swift");
    let base = "struct Player { var hp = 1 }";
    let disk = "struct Player { var hp = 3 }";
    let buffer = "struct Player { var hp = 2 }";
    std::fs::write(&path, base).expect("a writable temporary source");
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    let expected = editor
        .sources
        .fingerprint("game/Player.swift")
        .expect("the source path is valid")
        .to_string();
    std::fs::write(&path, disk).expect("an external editor can change the source");
    let call = format!(
        r#"{{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{{"name":"source.write","arguments":{{"path":"game/Player.swift","contents":"{buffer}","expected_fingerprint":"{expected}","base":"{base}"}}}}}}"#
    );

    let replies = converse(&[INITIALIZE, &call], &mut editor);
    let result = result(&replies, 1);
    assert_eq!(result.get("isError"), &Json::Bool(false));
    let conflict = result.get("structuredContent");
    assert_eq!(conflict.get("conflict").as_text(), Some("true"));
    assert_eq!(conflict.get("base").as_text(), Some(base));
    assert_eq!(conflict.get("buffer").as_text(), Some(buffer));
    assert_eq!(conflict.get("disk").as_text(), Some(disk));
    assert_eq!(
        std::fs::read_to_string(path).expect("the source remains readable"),
        disk,
        "an MCP call must not silently replace an external edit"
    );
}

#[test]
fn a_headless_server_refuses_the_window_and_says_what_would_work() {
    let mut editor = Editor::new(Actor::human("designer"));
    let window = r#"{"jsonrpc":"2.0","id":2,"method":"resources/read","params":{"uri":"editor:window?panel=viewport"}}"#;
    let replies = converse(&[INITIALIZE, window], &mut editor);
    let text = match result(&replies, 1).get("content") {
        Json::Array(items) => items[0].get("text").as_text().unwrap().to_string(),
        other => panic!("a refusal is content: {other:?}"),
    };
    assert!(text.contains("headless"), "{text}");
    assert!(text.contains("--mcp"), "{text}");
}

#[test]
fn emitter_parameter_commands_save_reopen_and_undo_over_mcp() {
    use cy_editor_interface::specialised::vfx::VfxDocument;

    let sandbox = Sandbox::new("vfx-emitter-parameters");
    let reference = "game/sparks.cyvfxdoc";
    std::fs::write(
        sandbox.0.join(reference),
        VfxDocument::new("sparks").unwrap().encode_text().unwrap(),
    )
    .unwrap();
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor.open_document("worlds/city.cyworld").unwrap();
    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"vfx.emitter.add","arguments":{"reference":"game/sparks.cyvfxdoc","name":"embers","target":"cpu","renderer":"Sprite"}}}"#,
            r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"vfx.emitter.parameter.set","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"embers","name":"speed","kind":"float","values":[2,0,0,0],"exposed":true}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&replies, 1).get("isError"), &Json::Bool(false));
    assert_eq!(result(&replies, 2).get("isError"), &Json::Bool(false));
    let read = || {
        VfxDocument::decode_text(&std::fs::read_to_string(sandbox.0.join(reference)).unwrap())
            .unwrap()
    };
    assert_eq!(
        read().emitter_parameters[0].parameter.value[0].to_bits(),
        2.0_f32.to_bits()
    );
    let refused = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"vfx.emitter.parameter.set","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"missing","name":"speed","kind":"float","values":[3,0,0,0],"exposed":true}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&refused, 1).get("isError"), &Json::Bool(true));
    assert_eq!(
        read().emitter_parameters[0].parameter.value[0].to_bits(),
        2.0_f32.to_bits()
    );
    let undone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&undone, 1).get("isError"), &Json::Bool(false));
    assert!(read().emitter_parameters.is_empty());
    let redone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"edit.redo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&redone, 1).get("isError"), &Json::Bool(false));
    assert_eq!(read().emitter_parameters.len(), 1);
    let removed = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"vfx.emitter.remove","arguments":{"reference":"game/sparks.cyvfxdoc","emitter":"embers"}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&removed, 1).get("isError"), &Json::Bool(false));
    assert!(read().emitter_parameters.is_empty());
}

#[test]
fn scene_effect_instances_keep_independent_overrides_through_mcp_and_world_reopen() {
    use cy_editor_core::value::Value;
    use cy_editor_documents::selection::Selection;
    use cy_editor_interface::inspector::{Control, GeneratedInspector};
    use cy_editor_reflection::Catalogue;

    let sandbox = Sandbox::new("vfx-scene-instances");
    write_scene_effect_sample(&sandbox);
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor.open_document("worlds/city.cyworld").unwrap();

    let create = r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"scene.vfx-effect.create","arguments":{"asset":"game/sparks.cyvfxdoc"}}}"#;
    let first = converse(&[INITIALIZE, create], &mut editor);
    assert_eq!(result(&first, 1).get("isError"), &Json::Bool(false));
    let a = editor.selection.get().nodes().next().unwrap();
    let second = converse(&[INITIALIZE, create], &mut editor);
    assert_eq!(result(&second, 1).get("isError"), &Json::Bool(false));
    let b = editor.selection.get().nodes().next().unwrap();
    assert_ne!(a, b);
    assert_invalid_scene_integer_overrides(&mut editor, a);

    let active = editor.workspace.active().unwrap();
    let mut selected = Selection::new();
    selected.add_node(a);
    editor.selection.set(selected);
    let document = editor.documents.get(active).unwrap();
    let effect = document.schema().type_named("cy::vfx::Effect").unwrap();
    let effect_type = effect.id;
    let intensity = effect.field_named("system.intensity.float").unwrap().id;
    let mut inspector =
        GeneratedInspector::with_catalogue(Catalogue::of_document(document.schema()));
    inspector.refresh(&editor);
    let section = inspector
        .sections()
        .iter()
        .find(|section| section.title == "cy::vfx::Effect")
        .expect("the scene effect has a generated Inspector section");
    assert!(
        section
            .rows
            .iter()
            .any(|row| row.name == "emitter.embers.speed.float" && row.control == Control::Number)
    );
    inspector.begin_edit(effect_type, intensity, Value::Float(5.0));
    assert_eq!(inspector.commit_edit(&mut editor).unwrap(), 1);
    let document = editor.documents.get(active).unwrap();
    assert_eq!(
        document.content().field(a, effect_type, intensity),
        Some(&Value::Float(5.0))
    );
    assert_eq!(
        document.content().field(b, effect_type, intensity),
        Some(&Value::Float(1.0))
    );

    for (node, value) in [(a, 3), (b, 7)] {
        let request = format!(
            "{{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{{\"name\":\"scene.vfx-effect.parameter.set\",\"arguments\":{{\"entity\":\"{node}\",\"name\":\"intensity\",\"values\":[{value},0,0,0]}}}}}}"
        );
        let replies = converse(&[INITIALIZE, &request], &mut editor);
        assert_eq!(result(&replies, 1).get("isError"), &Json::Bool(false));
    }
    let local = format!(
        "{{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{{\"name\":\"scene.vfx-effect.parameter.set\",\"arguments\":{{\"entity\":\"{a}\",\"emitter\":\"embers\",\"name\":\"speed\",\"values\":[9,0,0,0]}}}}}}"
    );
    let replies = converse(&[INITIALIZE, &local], &mut editor);
    assert_eq!(result(&replies, 1).get("isError"), &Json::Bool(false));

    let folded = format!(
        "{{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{{\"name\":\"scene.vfx-effect.parameter.set\",\"arguments\":{{\"entity\":\"{a}\",\"name\":\"gravity\",\"values\":[2,0,0,0]}}}}}}"
    );
    let refusal = converse(&[INITIALIZE, &folded], &mut editor);
    assert_eq!(result(&refusal, 1).get("isError"), &Json::Bool(true));
    let undo = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&undo, 1).get("isError"), &Json::Bool(false));
    let active = editor.workspace.active().unwrap();
    let document = editor.documents.get(active).unwrap();
    let effect = document.schema().type_named("cy::vfx::Effect").unwrap();
    let speed = effect.field_named("emitter.embers.speed.float").unwrap().id;
    assert_eq!(
        document.content().field(a, effect.id, speed),
        Some(&Value::Float(2.0))
    );
    let redo = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"edit.redo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&redo, 1).get("isError"), &Json::Bool(false));

    assert_scene_effect_reopen(&editor, a, b);
}

fn write_scene_effect_sample(sandbox: &Sandbox) {
    use cy_editor_interface::specialised::vfx::{
        Emitter, EmitterParameter, Parameter, SimulationPath, VfxDocument,
    };

    let mut vfx = VfxDocument::new("sparks").unwrap();
    vfx.parameters.push(Parameter {
        name: "intensity".into(),
        kind: "float".into(),
        value: [1.0, 0.0, 0.0, 0.0],
        exposed: true,
    });
    vfx.parameters.push(Parameter {
        name: "gravity".into(),
        kind: "float".into(),
        value: [9.8, 0.0, 0.0, 0.0],
        exposed: false,
    });
    vfx.parameters.push(Parameter {
        name: "bursts".into(),
        kind: "int".into(),
        value: [2.0, 0.0, 0.0, 0.0],
        exposed: true,
    });
    vfx.emitters.push(Emitter {
        name: "embers".into(),
        path: SimulationPath::CpuRequired,
        renderer: "Sprite".into(),
        stages: Vec::new(),
        modules: Vec::new(),
        interfaces: Vec::new(),
        capacity: 1024,
        attributes: Vec::new(),
    });
    vfx.emitter_parameters.push(EmitterParameter {
        emitter: "embers".into(),
        parameter: Parameter {
            name: "speed".into(),
            kind: "float".into(),
            value: [2.0, 0.0, 0.0, 0.0],
            exposed: true,
        },
    });
    std::fs::write(
        sandbox.0.join("game/sparks.cyvfxdoc"),
        vfx.encode_text().unwrap(),
    )
    .unwrap();
}

fn assert_invalid_scene_integer_overrides(
    editor: &mut Editor,
    entity: cy_editor_core::ids::NodeId,
) {
    use cy_editor_core::value::Value;

    for value in ["2.5", "2147483648"] {
        let request = format!(
            "{{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":{{\"name\":\"scene.vfx-effect.parameter.set\",\"arguments\":{{\"entity\":\"{entity}\",\"name\":\"bursts\",\"values\":[{value},0,0,0]}}}}}}"
        );
        let replies = converse(&[INITIALIZE, &request], editor);
        assert_eq!(result(&replies, 1).get("isError"), &Json::Bool(true));
        let document = editor
            .documents
            .get(editor.workspace.active().unwrap())
            .unwrap();
        let effect = document.schema().type_named("cy::vfx::Effect").unwrap();
        let bursts = effect.field_named("system.bursts.int").unwrap();
        assert_eq!(
            document.content().field(entity, effect.id, bursts.id),
            Some(&Value::Int(2))
        );
    }
}

fn assert_scene_effect_reopen(
    editor: &Editor,
    a: cy_editor_core::ids::NodeId,
    b: cy_editor_core::ids::NodeId,
) {
    use cy_editor_core::value::Value;
    use cy_editor_documents::Document;

    let active = editor.workspace.active().unwrap();
    let saved = cy_editor_services::worldfile::write_world(editor.documents.get(active).unwrap());
    let mut reopened = Document::new("worlds/city.cyworld");
    cy_editor_services::worldfile::load(&saved, &mut reopened, Actor::human("reopen")).unwrap();
    let component = reopened.schema().type_named("cy::vfx::Effect").unwrap();
    let intensity = component.field_named("system.intensity.float").unwrap().id;
    let speed = component
        .field_named("emitter.embers.speed.float")
        .unwrap()
        .id;
    assert_eq!(
        reopened.content().field(a, component.id, intensity),
        Some(&Value::Float(3.0))
    );
    assert_eq!(
        reopened.content().field(b, component.id, intensity),
        Some(&Value::Float(7.0))
    );
    assert_eq!(
        reopened.content().field(a, component.id, speed),
        Some(&Value::Float(9.0))
    );
    assert_eq!(
        reopened.content().field(b, component.id, speed),
        Some(&Value::Float(2.0))
    );
}

/// The terrain panel's commands, driven over the wire the way the panel drives them through the
/// registry: create a root, add a layer, then undo and redo the layer in the one history.
#[test]
fn terrain_authoring_is_an_undoable_mcp_peer_of_the_terrain_panel() {
    use cy_editor_services::terrain::TerrainStack;

    let mut editor = Editor::new(Actor::human("designer"));
    editor.open_document("worlds/terrain.cyworld").unwrap();
    let created = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"terrain.create","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&created, 1).get("isError"), &Json::Bool(false));
    let terrain = editor
        .selection
        .get()
        .nodes()
        .next()
        .expect("terrain.create selects the root it created");
    let layers = |editor: &Editor| {
        let document = editor
            .documents
            .get(editor.workspace.active().unwrap())
            .unwrap();
        TerrainStack::read(document, terrain)
            .expect("the root is a terrain")
            .layers
            .len()
    };
    assert_eq!(layers(&editor), 0);

    let add = format!(
        r#"{{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{{"name":"terrain.layer.add","arguments":{{"terrain":"{terrain}","name":"Grass","material":"materials/grass.cymat"}}}}}}"#
    );
    let added = converse(&[INITIALIZE, &add], &mut editor);
    assert_eq!(result(&added, 1).get("isError"), &Json::Bool(false));
    assert_eq!(layers(&editor), 1);

    let undone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&undone, 1).get("isError"), &Json::Bool(false));
    assert_eq!(
        layers(&editor),
        0,
        "undo removes the layer and keeps the root"
    );

    let redone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"edit.redo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&redone, 1).get("isError"), &Json::Bool(false));
    assert_eq!(layers(&editor), 1);
}

/// A person at the interface who confirms whatever is asked.
struct ConfirmEverything;

impl cy_editor_agent::session::Confirmer for ConfirmEverything {
    fn confirm(
        &mut self,
        _confirmation: &cy_editor_agent::session::Confirmation,
    ) -> cy_editor_agent::session::Decision {
        cy_editor_agent::session::Decision::Allow
    }
}

/// `converse`, with external effects in scope and a person confirming them.
fn converse_with_external_effects(lines: &[&str], editor: &mut Editor) -> Vec<Json> {
    let sink = Sink::default();
    let session = AgentSession::new(
        AgentIdentity {
            agent: "lighter".to_string(),
            session: "s-1".to_string(),
        },
        "bake the level's lightmaps",
        Scope::new(
            "lighting",
            DocumentScope::All,
            [
                EffectClass::Read,
                EffectClass::ReversibleMutation,
                EffectClass::ExternalEffect,
            ],
        )
        .with_directory("game/"),
        Budget::default(),
        "r-1",
        0,
    );
    let mut server = McpServer::new(sink.clone(), session);
    serve(
        lines.join("\n").as_bytes(),
        &mut server,
        editor,
        &registry(),
        &mut ConfirmEverything,
    )
    .expect("the conversation runs to the end of the input");
    sink.replies()
}

fn tool_reply(replies: &[Json], index: usize) -> (String, bool) {
    let reply = result(replies, index);
    let text = match reply.get("content") {
        Json::Array(items) => items
            .first()
            .and_then(|item| item.get("text").as_text())
            .unwrap_or_default()
            .to_string(),
        other => panic!("content is not an array: {other:?}"),
    };
    (text, reply.get("isError") == &Json::Bool(true))
}

/// The lighting editor's panel invokes these three and nothing else
/// (`cy-editor-shell`'s `LightingTool`); an agent reaches the same three here.
#[test]
fn the_lighting_editor_bake_and_density_view_are_mcp_tools() {
    let mut editor = Editor::new(Actor::human("designer"));
    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/list"}"#,
            r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"lighting.bake-lightmaps","arguments":{"description":"game/levels/corner.cylightmap"}}}"#,
            r#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"lighting.cancel-lightmap-bake","arguments":{}}}"#,
            r#"{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"viewport.view-mode.lightmap-density","arguments":{}}}"#,
        ],
        &mut editor,
    );
    let tools = match result(&replies, 1).get("tools") {
        Json::Array(items) => items.clone(),
        other => panic!("tools is not an array: {other:?}"),
    };
    let described = |name: &str| {
        tools
            .iter()
            .find(|tool| tool.get("name").as_text() == Some(name))
            .and_then(|tool| tool.get("description").as_text())
            .unwrap_or_else(|| panic!("{name} is not a tool"))
            .to_string()
    };
    assert!(described("lighting.bake-lightmaps").contains("external-effect"));
    assert!(described("lighting.cancel-lightmap-bake").contains("Effect: read"));
    assert!(described("viewport.view-mode.lightmap-density").contains("Effect: read"));

    // A bake writes a cooked file, so a connection must be granted external effects to start one.
    let (refused, is_error) = tool_reply(&replies, 2);
    assert!(is_error && refused.contains("external-effect"), "{refused}");
    let (nothing, is_error) = tool_reply(&replies, 3);
    assert!(
        is_error && nothing.contains("no lightmap bake"),
        "{nothing}"
    );
    let (view, is_error) = tool_reply(&replies, 4);
    assert!(!is_error && view.contains("LightmapDensity"), "{view}");

    // In scope and confirmed, the bake is still held to the connection's directory.
    let replies = converse_with_external_effects(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"lighting.bake-lightmaps","arguments":{"description":"elsewhere/corner.cylightmap"}}}"#,
        ],
        &mut editor,
    );
    let (outside, is_error) = tool_reply(&replies, 1);
    assert!(
        is_error && outside.contains("does not include it") && outside.contains("game/"),
        "{outside}"
    );
}

/// One MCP tool call, answering whether the server reported an error, and the reply's text.
fn call_tool(
    editor: &mut Editor,
    id: u32,
    name: &str,
    arguments: &[(&str, &str)],
) -> (bool, String) {
    let replies = converse(&[INITIALIZE, &tool_call(id, name, arguments)], editor);
    let failed = result(&replies, 1).get("isError") == &Json::Bool(true);
    (failed, tool_text(&replies, 1))
}

/// Two entities with dynamic bodies, created over the wire.
fn two_bodies_over_the_wire(
    editor: &mut Editor,
) -> (cy_editor_core::ids::NodeId, cy_editor_core::ids::NodeId) {
    let mut bodies = Vec::new();
    for id in 2..4 {
        let (failed, text) = call_tool(editor, id, "scene.create-entity", &[]);
        assert!(!failed, "{text}");
        let node = editor
            .selection
            .get()
            .nodes()
            .next()
            .expect("the created entity");
        let (failed, text) = call_tool(
            editor,
            id + 10,
            "scene.add-body",
            &[("entity", &node.to_string())],
        );
        assert!(!failed, "{text}");
        bodies.push(node);
    }
    (bodies[0], bodies[1])
}

/// The door's joint's upper limit, compared by bits: the values crossed the wire as text.
fn upper_limit_is(editor: &Editor, door: cy_editor_core::ids::NodeId, expected: f32) -> bool {
    let document = editor
        .documents
        .get(editor.workspace.active().unwrap())
        .unwrap();
    cy_editor_services::joints::joint_of(document, door)
        .is_some_and(|joint| joint.limit[1].to_bits() == expected.to_bits())
}

/// The physics panel's commands, driven over the wire as the panel drives them through the
/// registry: two bodies, a hinge between them, a field changed, undo and redo in the one history,
/// the joint removed and restored, and a physics debug layer shown in the viewport.
#[test]
fn physics_authoring_is_an_undoable_mcp_peer_of_the_physics_panel() {
    let mut editor = Editor::new(Actor::human("designer"));
    editor.open_document("worlds/joints.cyworld").unwrap();
    let (door, frame) = two_bodies_over_the_wire(&mut editor);
    let door_text = door.to_string();
    let entity = ("entity", door_text.as_str());

    let (failed, text) = call_tool(
        &mut editor,
        20,
        "physics.joint.add",
        &[entity, ("kind", "hinge"), ("target", &frame.to_string())],
    );
    assert!(!failed, "{text}");
    assert!(upper_limit_is(&editor, door, -1.0), "a hinge starts free");

    let set = [entity, ("field", "limit_max"), ("value", "0.5")];
    let (failed, text) = call_tool(&mut editor, 21, "physics.joint.set", &set);
    assert!(!failed, "{text}");
    assert!(upper_limit_is(&editor, door, 0.5));

    assert!(!call_tool(&mut editor, 22, "edit.undo", &[]).0);
    assert!(
        upper_limit_is(&editor, door, -1.0),
        "undo takes back the one field"
    );
    assert!(!call_tool(&mut editor, 23, "edit.redo", &[]).0);
    assert!(upper_limit_is(&editor, door, 0.5));

    assert!(!call_tool(&mut editor, 24, "physics.joint.remove", &[entity]).0);
    assert!(!upper_limit_is(&editor, door, 0.5), "the joint is gone");
    assert!(!call_tool(&mut editor, 25, "edit.undo", &[]).0);
    assert!(
        upper_limit_is(&editor, door, 0.5),
        "undo restores what was removed"
    );

    // A refusal comes back as a result the model can read, and changes nothing.
    let negative = [entity, ("field", "break_force"), ("value", "-3")];
    let (failed, text) = call_tool(&mut editor, 26, "physics.joint.set", &negative);
    assert!(failed, "a negative break force is refused");
    assert!(text.contains("negative"), "{text}");

    let (failed, text) = call_tool(
        &mut editor,
        27,
        "viewport.physics.colliders",
        &[("state", "on")],
    );
    assert!(!failed, "{text}");
    assert!(
        editor
            .viewports
            .focused()
            .physics
            .contains(cy_editor_viewport::PhysicsLayer::Colliders)
    );
}

/// The frames the editor writes to the fake runtime, read on a thread of their own so a test can
/// give up waiting instead of blocking on the pipe forever.
fn runtime_frames(mut runtime_reader: std::io::PipeReader) -> std::sync::mpsc::Receiver<Vec<u8>> {
    let (sender, frames) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        while let Ok(Some(frame)) = read_frame(&mut runtime_reader) {
            if sender.send(frame).is_err() {
                break;
            }
        }
    });
    frames
}

/// The next `terrain.evaluate` the editor sent the fake runtime, skipping catalogue discovery.
/// Fails, rather than hangs, when none arrives within five seconds.
fn next_terrain_request(
    frames: &std::sync::mpsc::Receiver<Vec<u8>>,
) -> (cy_editor_protocol::RequestId, Vec<u8>) {
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    loop {
        let left = deadline.saturating_duration_since(std::time::Instant::now());
        let frame = frames
            .recv_timeout(left)
            .expect("the editor sends terrain.evaluate within five seconds");
        if let Message::ServiceRequest {
            request,
            operation,
            payload,
            ..
        } = Message::decode(&frame).unwrap()
            && operation == "terrain.evaluate"
        {
            return (request, payload);
        }
    }
}

/// An engine reply over a 3 x 3 lattice: `height` everywhere, holes where listed, one stale
/// region per entry.
fn terrain_reply(generation: u64, height: u16, holes: &[usize], stale: usize) -> Vec<u8> {
    let mut reply = Writer::new();
    reply.u32(1);
    reply.u64(generation);
    reply.u32(3);
    reply.f32(128.0);
    reply.f32(-512.0);
    reply.f32(1536.0);
    reply.u32(8 - 2 * u32::try_from(holes.len()).unwrap());
    reply.u32(u32::try_from(holes.len()).unwrap());
    reply.u32(u32::try_from(holes.len()).unwrap());
    reply.u32(u32::try_from(stale).unwrap());
    for _ in 0..stale {
        for value in [40.0_f32, 30.0, 90.0, 60.0] {
            reply.f32(value);
        }
    }
    for _ in 0..9 {
        reply.u8((height & 0xFF) as u8);
        reply.u8((height >> 8) as u8);
    }
    for _ in 0..4 {
        for byte in [0_u8, 0, 0, 0, 255, 0, 0, 0] {
            reply.u8(byte);
        }
    }
    for quad in 0..4 {
        reply.u8(u8::from(holes.contains(&quad)));
    }
    reply.finish()
}

fn answer_terrain(
    editor: &mut Editor,
    runtime_writer: &mut std::io::PipeWriter,
    request: cy_editor_protocol::RequestId,
    payload: Vec<u8>,
) {
    let generation = cy_editor_core::codec::Reader::new(&payload[4..])
        .u64()
        .unwrap();
    write_frame(
        runtime_writer,
        &Message::ServiceEvent {
            request,
            kind: ServiceEventKind::Completed,
            schema_version: 1,
            payload,
        }
        .encode(),
    )
    .unwrap();
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while editor
        .terrain
        .evaluation()
        .map(|evaluation| evaluation.generation)
        != Some(generation)
        && std::time::Instant::now() < deadline
    {
        editor.pump();
        std::thread::sleep(std::time::Duration::from_millis(1));
    }
    assert_eq!(
        editor
            .terrain
            .evaluation()
            .map(|evaluation| evaluation.generation),
        Some(generation),
        "the engine's terrain reply must arrive"
    );
}

/// The brush tools over the wire: an agent sculpts and cuts a hole with `terrain.brush.apply`, the
/// editor sends each resulting stack to the engine's `terrain.evaluate`, `terrain.status` reports
/// what the engine answered, and `edit.undo` sends the engine the pre-stroke stack byte for byte.
#[test]
#[expect(
    clippy::too_many_lines,
    reason = "one conversation: stroke, status, hole, status, undo, status, undo, in order"
)]
fn terrain_brushes_are_evaluated_by_the_engine_and_undo_over_mcp() {
    use cy_editor_core::codec::Reader;

    let mut editor = Editor::new(Actor::human("designer"));
    editor.open_document("worlds/terrain.cyworld").unwrap();
    let (editor_reader, mut runtime_writer) = std::io::pipe().unwrap();
    let (runtime_reader, editor_writer) = std::io::pipe().unwrap();
    editor.runtime = RuntimeSession::over(Session::over(editor_reader, editor_writer));
    let frames = runtime_frames(runtime_reader);

    let created = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"terrain.create","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&created, 1).get("isError"), &Json::Bool(false));
    let terrain = editor
        .edited_terrain()
        .expect("terrain.create selects its root");
    let (request, before_stroke) = next_terrain_request(&frames);
    let mut header = Reader::new(&before_stroke);
    assert_eq!(header.u32().unwrap(), 1, "format");
    assert_eq!(header.u128().unwrap(), terrain.as_u128());
    answer_terrain(
        &mut editor,
        &mut runtime_writer,
        request,
        terrain_reply(1, 100, &[], 0),
    );

    let raise = format!(
        r#"{{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{{"name":"terrain.brush.apply","arguments":{{"terrain":"{terrain}","tool":"raise","points":"0.4 0.3; 0.5 0.35 0.8","radius":8,"strength":0.8,"falloff":0.5}}}}}}"#
    );
    let raised = converse(&[INITIALIZE, &raise], &mut editor);
    assert_eq!(result(&raised, 1).get("isError"), &Json::Bool(false));
    let (request, stroked) = next_terrain_request(&frames);
    assert_ne!(stroked, before_stroke);
    let mut body = Reader::new(&stroked[4 + 16 + 12..]);
    assert_eq!(body.u32().unwrap(), 1, "one modifier");
    let _identity = body.u128().unwrap();
    assert_eq!(body.u8().unwrap(), 0, "raise");
    assert_eq!(body.u8().unwrap(), 1, "enabled");
    assert_eq!(body.u8().unwrap(), 0, "a sculpt names no layer");
    assert!((body.f32().unwrap() - 8.0).abs() < f32::EPSILON);
    answer_terrain(
        &mut editor,
        &mut runtime_writer,
        request,
        terrain_reply(2, 140, &[], 1),
    );

    let status = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"terrain.status","arguments":{}}}"#,
        ],
        &mut editor,
    );
    let reported = result(&status, 1).get("structuredContent");
    assert_eq!(reported.get("evaluated").as_text(), Some("true"));
    assert_eq!(reported.get("generation").as_text(), Some("2"));
    assert_eq!(reported.get("navigation_stale").as_text(), Some("true"));
    assert_eq!(
        reported.get("navigation_stale_regions").as_text(),
        Some("40 30 90 60")
    );
    let raised_digest = reported
        .get("heights_digest")
        .as_text()
        .unwrap()
        .to_string();

    let hole = format!(
        r#"{{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{{"name":"terrain.brush.apply","arguments":{{"terrain":"{terrain}","tool":"hole","points":"0.5 0.5"}}}}}}"#
    );
    let holed = converse(&[INITIALIZE, &hole], &mut editor);
    assert_eq!(result(&holed, 1).get("isError"), &Json::Bool(false));
    let (request, with_hole) = next_terrain_request(&frames);
    let mut body = Reader::new(&with_hole[4 + 16 + 12..]);
    assert_eq!(body.u32().unwrap(), 2, "raise, then the hole above it");
    answer_terrain(
        &mut editor,
        &mut runtime_writer,
        request,
        terrain_reply(3, 140, &[1], 1),
    );
    let status = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"terrain.status","arguments":{}}}"#,
        ],
        &mut editor,
    );
    let reported = result(&status, 1).get("structuredContent");
    assert_eq!(reported.get("collision_holes").as_text(), Some("1"));
    assert_eq!(reported.get("rendered_hole_quads").as_text(), Some("1"));

    // Undo both strokes over MCP: each sends the engine the stack as it was, and the last is the
    // pre-stroke request byte for byte.
    let undone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&undone, 1).get("isError"), &Json::Bool(false));
    let (request, after_one_undo) = next_terrain_request(&frames);
    assert_eq!(
        after_one_undo, stroked,
        "undoing the hole sends the raised stack again"
    );
    answer_terrain(
        &mut editor,
        &mut runtime_writer,
        request,
        terrain_reply(4, 140, &[], 1),
    );
    let status = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"terrain.status","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(
        result(&status, 1)
            .get("structuredContent")
            .get("heights_digest")
            .as_text(),
        Some(raised_digest.as_str())
    );
    let undone = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"edit.undo","arguments":{}}}"#,
        ],
        &mut editor,
    );
    assert_eq!(result(&undone, 1).get("isError"), &Json::Bool(false));
    let (_, after_both) = next_terrain_request(&frames);
    assert_eq!(after_both, before_stroke);
}

/// Paint names its layer by order, and a sculpt or hole stroke naming one is refused.
#[test]
fn terrain_paint_over_mcp_requires_a_layer_and_a_hole_refuses_one() {
    let mut editor = Editor::new(Actor::human("designer"));
    editor.open_document("worlds/terrain.cyworld").unwrap();
    converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"terrain.create","arguments":{}}}"#,
        ],
        &mut editor,
    );
    let terrain = editor.edited_terrain().unwrap();
    let add = format!(
        r#"{{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{{"name":"terrain.layer.add","arguments":{{"terrain":"{terrain}","name":"Rock","material":"materials/rock.cymat"}}}}}}"#
    );
    let added = converse(&[INITIALIZE, &add], &mut editor);
    let layer = result(&added, 1)
        .get("structuredContent")
        .get("layer")
        .as_text()
        .unwrap()
        .to_string();
    let call = |id: u32, tool: &str, layer: &str| {
        format!(
            r#"{{"jsonrpc":"2.0","id":{id},"method":"tools/call","params":{{"name":"terrain.brush.apply","arguments":{{"terrain":"{terrain}","tool":"{tool}","layer":"{layer}","points":"0.2 0.2"}}}}}}"#
        )
    };
    let replies = converse(
        &[
            INITIALIZE,
            &call(4, "paint", ""),
            &call(5, "hole", &layer),
            &call(6, "paint", &layer),
        ],
        &mut editor,
    );
    assert_eq!(result(&replies, 1).get("isError"), &Json::Bool(true));
    assert_eq!(result(&replies, 2).get("isError"), &Json::Bool(true));
    assert_eq!(result(&replies, 3).get("isError"), &Json::Bool(false));
    let document = editor
        .documents
        .get(editor.workspace.active().unwrap())
        .unwrap();
    let request = cy_editor_services::terrain_engine::evaluation_request(document, terrain)
        .unwrap()
        .unwrap();
    let mut body = cy_editor_core::codec::Reader::new(&request[4 + 16 + 12..]);
    assert_eq!(body.u32().unwrap(), 1);
    let _identity = body.u128().unwrap();
    assert_eq!(body.u8().unwrap(), 4, "paint");
    assert_eq!(body.u8().unwrap(), 1, "enabled");
    assert_eq!(
        body.u8().unwrap(),
        1,
        "the first layer is layer 1; 0 is the base"
    );
}

// --- Navigation authoring (issue #28) -------------------------------------------------------------

/// Every `navigation.*` command `cy_editor_services::navmesh` registers.
const NAVIGATION_TOOLS: [&str; 18] = [
    "navigation.world.create",
    "navigation.settings.get",
    "navigation.settings.set",
    "navigation.overlay.set",
    "navigation.bake",
    "navigation.bake.status",
    "navigation.surface.add",
    "navigation.surface.set",
    "navigation.obstacle.add",
    "navigation.obstacle.set",
    "navigation.area.add",
    "navigation.area.set",
    "navigation.link.add",
    "navigation.link.set",
    "navigation.component.remove",
    "navigation.path.query",
    "navigation.flowfield.query",
    "navigation.point.pick",
];

#[test]
fn navigation_tools_are_projected_over_mcp() {
    let mut editor = Editor::new(Actor::human("designer"));
    let replies = converse(
        &[
            INITIALIZE,
            r#"{"jsonrpc":"2.0","id":2,"method":"tools/list"}"#,
        ],
        &mut editor,
    );
    let Json::Array(tools) = result(&replies, 1).get("tools") else {
        panic!("tools/list must contain the registry projection");
    };
    for command in NAVIGATION_TOOLS {
        assert!(
            tools
                .iter()
                .any(|tool| tool.get("name").as_text() == Some(command)),
            "{command} must be available over MCP"
        );
    }
}

/// What the fake runtime saw before the navigation request it waited for.
struct NavigationRequest {
    request: cy_editor_protocol::RequestId,
    payload: Vec<u8>,
    synced: Vec<Message>,
}

/// Read what the editor sent until the service request `expected`, keeping world syncs.
fn navigation_request(reader: &mut std::io::PipeReader, expected: &str) -> NavigationRequest {
    let mut synced = Vec::new();
    for _ in 0..64 {
        let message = Message::decode(&read_frame(reader).unwrap().unwrap()).unwrap();
        match message {
            Message::ServiceRequest {
                request,
                operation,
                payload,
                schema_version,
            } if operation == expected => {
                assert_eq!(schema_version, 1);
                return NavigationRequest {
                    request,
                    payload,
                    synced,
                };
            }
            Message::ServiceRequest { operation, .. } => {
                assert!(
                    !operation.starts_with("navigation."),
                    "unexpected {operation} while waiting for {expected}"
                );
            }
            message @ (Message::SyncWorld { .. } | Message::Apply { .. }) => synced.push(message),
            _ => {}
        }
    }
    panic!("the editor never asked the engine for {expected}");
}

fn reply_navigation(
    writer: &mut std::io::PipeWriter,
    request: cy_editor_protocol::RequestId,
    kind: ServiceEventKind,
    payload: Vec<u8>,
) {
    write_frame(
        writer,
        &Message::ServiceEvent {
            request,
            kind,
            schema_version: 1,
            payload,
        }
        .encode(),
    )
    .unwrap();
}

/// A `navigation.bake` COMPLETED payload for one non-empty tile, as `encode_completed` writes it.
fn navigation_bake_completed(world: u32, identity: u64, fingerprint: u64) -> Vec<u8> {
    let mut payload = Writer::new();
    payload.u32(world);
    payload.u64(fingerprint);
    payload.u64(identity);
    payload.text(&format!("navigation/{identity:016x}.cynavmesh"));
    for counter in [12_u32, 0, 0, 40, 6, 14] {
        payload.u32(counter);
    }
    payload.u64(1000);
    payload.u8(1);
    payload.u32(1);
    payload.u32(0);
    payload.u32(0);
    payload.u32(1);
    for coordinate in [0_u32, 0, 0] {
        payload.u32(coordinate);
    }
    payload.u32(6);
    payload.u8(0);
    payload.u64(0xD1CE);
    payload.finish()
}

fn navigation_path_answer(found: bool) -> Vec<u8> {
    let mut payload = Writer::new();
    payload.u8(u8::from(found));
    payload.u8(0);
    payload.u8(0);
    payload.f32(if found { 11.5 } else { 0.0 });
    payload.u32(7);
    payload.u32(if found { 2 } else { 0 });
    if found {
        for point in [[1.0_f32, 0.0, 8.0], [15.0, 0.0, 8.0]] {
            for lane in point {
                payload.f32(lane);
            }
            payload.u8(0);
        }
    }
    payload.finish()
}

fn recorded_bake_identity(editor: &Editor) -> u64 {
    let document = editor
        .documents
        .get(editor.workspace.active().unwrap())
        .unwrap();
    cy_editor_services::navmesh::NavmeshSettings::find(document, 1)
        .map_or(0, |settings| settings.bake_identity)
}

fn active_history(editor: &Editor) -> Vec<String> {
    editor
        .documents
        .get(editor.workspace.active().unwrap())
        .unwrap()
        .history()
        .entries()
        .iter()
        .map(|entry| entry.name.clone())
        .collect()
}

fn pump_until(editor: &mut Editor, done: impl Fn(&Editor) -> bool) {
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while !done(editor) && std::time::Instant::now() < deadline {
        editor.pump();
        std::thread::sleep(std::time::Duration::from_millis(1));
    }
    assert!(
        done(editor),
        "the editor did not settle the engine's answer"
    );
}

type Gesture = (&'static str, Vec<(&'static str, &'static str)>);

/// The authoring gestures of criterion 4, before the bake.
fn navigation_gestures() -> Vec<Gesture> {
    vec![
        ("navigation.world.create", vec![]),
        (
            "navigation.settings.set",
            vec![("values", "agent_radius=0.25; tile_size=8; backend=engine")],
        ),
        (
            "navigation.obstacle.add",
            vec![("values", "shape.offset=4, 0, 4; shape.radius=1")],
        ),
        (
            "navigation.overlay.set",
            vec![("overlays", "polygons, obstacles")],
        ),
    ]
}

fn assert_tools_succeeded(replies: &[Json], indices: std::ops::RangeInclusive<usize>) {
    for index in indices {
        assert_eq!(
            result(replies, index).get("isError"),
            &Json::Bool(false),
            "{}",
            tool_text(replies, index)
        );
    }
}

/// The `navigation.bake` request the settings of criterion 4 produce: world 1, the edited radius
/// and tile size, every layer and tag, the engine back end.
fn edited_bake_request() -> Vec<u8> {
    let mut expected = Writer::new();
    expected.u32(1);
    for value in [0.25_f32, 2.0, 45.0, 0.4, 0.3, 0.2, 8.0] {
        expected.f32(value);
    }
    expected.u64(u64::MAX);
    expected.u64(u64::MAX);
    expected.u8(1);
    expected.finish()
}

fn one_tile_progress() -> Vec<u8> {
    let mut progress = Writer::new();
    progress.u32(1);
    progress.u32(1);
    for field in [0_u32, 0, 0, 6] {
        progress.u32(field);
    }
    progress.u8(0);
    progress.u64(0xD1CE);
    progress.finish()
}

/// After undoing the bake, the overlay and the obstacle: the settings edit is all that is left.
fn assert_only_the_settings_edit_remains(editor: &Editor) {
    use cy_editor_services::navmesh::NavmeshSettings;
    let document = editor
        .documents
        .get(editor.workspace.active().unwrap())
        .unwrap();
    let settings = NavmeshSettings::find(document, 1).unwrap();
    assert!((settings.settings.agent_radius - 0.25).abs() < f32::EPSILON);
    assert_eq!(settings.overlay, 0, "the overlay edit is undone");
    assert_eq!(
        document.content().nodes().count(),
        1,
        "the obstacle node is undone"
    );
}

/// The history the desktop records for the same gestures, through the same registry.
fn desktop_history(gestures: &[Gesture]) -> Vec<String> {
    let mut desktop = Editor::new(Actor::human("designer"));
    desktop.open_document("worlds/nav.cyworld").unwrap();
    let registry = registry();
    for (name, arguments) in gestures {
        let arguments = arguments.iter().fold(
            cy_editor_commands::registry::Arguments::new(),
            |arguments, (key, value)| {
                arguments.with(*key, cy_editor_core::value::Value::Text((*value).into()))
            },
        );
        desktop
            .invoke(&registry, name, &Scope::unrestricted(), &arguments)
            .unwrap();
    }
    active_history(&desktop)
}

/// Run the gestures and a bake over MCP; the fake runtime answers the bake. Returns the history.
fn author_and_bake_over_mcp(
    editor: &mut Editor,
    runtime_reader: &mut std::io::PipeReader,
    runtime_writer: &mut std::io::PipeWriter,
) -> Vec<String> {
    let mut lines: Vec<String> = vec![INITIALIZE.to_string()];
    for (id, (name, arguments)) in (2_u32..).zip(navigation_gestures()) {
        lines.push(tool_call(id, name, &arguments));
    }
    lines.push(tool_call(9, "navigation.bake", &[]));
    let lines: Vec<&str> = lines.iter().map(String::as_str).collect();
    let replies = converse(&lines, editor);
    assert_tools_succeeded(&replies, 1..=5);
    assert_eq!(active_history(editor).len(), 4, "one entry per gesture");

    let bake = navigation_request(runtime_reader, "navigation.bake");
    assert_eq!(
        bake.payload,
        edited_bake_request(),
        "the bake carries the edited settings"
    );
    reply_navigation(
        runtime_writer,
        bake.request,
        ServiceEventKind::Progress,
        one_tile_progress(),
    );
    reply_navigation(
        runtime_writer,
        bake.request,
        ServiceEventKind::Completed,
        navigation_bake_completed(1, 0xBA4E_0001, 0xF1_0001),
    );
    pump_until(editor, |editor| {
        recorded_bake_identity(editor) == 0xBA4E_0001
    });
    active_history(editor)
}

/// Issue #28 criterion 4: settings, component and bake edits undo and redo over MCP, one history
/// entry each, and the history is the one the desktop produces for the same gestures.
#[test]
fn navigation_settings_component_and_bake_edits_undo_and_redo_over_mcp() {
    let mut editor = Editor::new(Actor::human("designer"));
    editor.open_document("worlds/nav.cyworld").unwrap();
    let (mut runtime_reader, mut runtime_writer) = install_vfx_stage_catalogue(&mut editor);
    let history = author_and_bake_over_mcp(&mut editor, &mut runtime_reader, &mut runtime_writer);
    assert_eq!(
        history.len(),
        5,
        "the completed bake is exactly one entry: {history:?}"
    );
    let document = editor
        .documents
        .get(editor.workspace.active().unwrap())
        .unwrap();
    assert!(
        document.history().entries()[4].actor.is_agent(),
        "the bake is the agent's"
    );

    let undo = tool_call(10, "edit.undo", &[]);
    let replies = converse(&[INITIALIZE, &undo], &mut editor);
    assert_tools_succeeded(&replies, 1..=1);
    assert_eq!(
        recorded_bake_identity(&editor),
        0,
        "undo restores the unbaked identity"
    );
    let replies = converse(&[INITIALIZE, &undo, &undo], &mut editor);
    assert_tools_succeeded(&replies, 1..=2);
    assert_only_the_settings_edit_remains(&editor);

    let redo = tool_call(11, "edit.redo", &[]);
    let replies = converse(&[INITIALIZE, &redo, &redo, &redo], &mut editor);
    assert_tools_succeeded(&replies, 1..=3);
    assert_eq!(
        recorded_bake_identity(&editor),
        0xBA4E_0001,
        "redo restores the bake"
    );
    assert_eq!(active_history(&editor), history);
    assert_eq!(
        desktop_history(&navigation_gestures()),
        history[..4].to_vec()
    );
    drop(runtime_writer);
}

/// Every operation of every transaction the editor applied to the runtime's world.
fn applied_operations(synced: &[Message]) -> Vec<cy_editor_documents::operation::Operation> {
    synced
        .iter()
        .filter_map(|message| match message {
            Message::Apply { transaction, .. } => Some(
                cy_editor_documents::transaction::Transaction::decode(
                    &mut cy_editor_core::codec::Reader::new(transaction),
                )
                .unwrap()
                .operations,
            ),
            _ => None,
        })
        .flatten()
        .collect()
}

/// The `navigation.path.query` request from (1, 0, 8) to (15, 0, 8) with the default extents.
fn corridor_path_request() -> Vec<u8> {
    let mut expected = Writer::new();
    expected.u32(1);
    for lane in [1.0_f32, 0.0, 8.0, 15.0, 0.0, 8.0, 1.0, 2.0, 1.0] {
        expected.f32(lane);
    }
    expected.finish()
}

fn corridor_path_call(id: u32) -> String {
    tool_call(
        id,
        "navigation.path.query",
        &[("start", "1, 0, 8"), ("end", "15, 0, 8")],
    )
}

/// Answer the path query the runtime was asked, and wait for the editor to hold the answer.
fn answer_path(
    editor: &mut Editor,
    runtime_writer: &mut std::io::PipeWriter,
    request: cy_editor_protocol::RequestId,
    found: bool,
) {
    reply_navigation(
        runtime_writer,
        request,
        ServiceEventKind::Completed,
        navigation_path_answer(found),
    );
    pump_until(editor, |editor| {
        editor
            .navmesh
            .path()
            .is_some_and(|path| path.found == found)
            && editor.navmesh.pending_request().is_none()
    });
}

/// Issue #28 criterion 3, editor side: an obstacle added and removed over MCP reaches the engine
/// as a synced component, and each test path after it is asked of the engine.
#[test]
fn navigation_obstacle_add_and_remove_reach_the_engine_over_mcp() {
    use cy_editor_core::value::Value;
    use cy_editor_documents::operation::Operation;

    let mut editor = Editor::new(Actor::human("designer"));
    editor.open_document("worlds/nav.cyworld").unwrap();
    let (mut runtime_reader, mut runtime_writer) = install_vfx_stage_catalogue(&mut editor);
    let add = tool_call(
        3,
        "navigation.obstacle.add",
        &[("values", "shape.offset=8, 0, 8; shape.radius=1.5")],
    );
    let create = tool_call(2, "navigation.world.create", &[]);
    let replies = converse(
        &[INITIALIZE, &create, &add, &corridor_path_call(4)],
        &mut editor,
    );
    assert_tools_succeeded(&replies, 1..=3);
    let node = editor
        .selection
        .get()
        .nodes()
        .next()
        .expect("the new obstacle is selected");
    let schema = editor
        .documents
        .get(editor.workspace.active().unwrap())
        .unwrap()
        .schema();
    let obstacle = schema.type_named("NavObstacle").unwrap().id;
    let offset = schema
        .type_named("NavObstacle")
        .unwrap()
        .field_named("shape.offset")
        .unwrap()
        .id;

    let blocked = navigation_request(&mut runtime_reader, "navigation.path.query");
    assert_eq!(blocked.payload, corridor_path_request());
    let added = applied_operations(&blocked.synced)
        .into_iter()
        .any(|operation| {
            matches!(
                operation,
                Operation::AddComponent { node: at, component, after }
                    if at == node
                        && component == obstacle
                        && after.contains(&(offset, Value::Vec3([8.0, 0.0, 8.0])))
            )
        });
    assert!(
        added,
        "the engine received the NavObstacle before the path query"
    );
    answer_path(&mut editor, &mut runtime_writer, blocked.request, false);

    let remove = tool_call(
        5,
        "navigation.component.remove",
        &[("node", &node.to_string()), ("component", "obstacle")],
    );
    let replies = converse(&[INITIALIZE, &remove, &corridor_path_call(6)], &mut editor);
    assert_tools_succeeded(&replies, 1..=2);
    let restored = navigation_request(&mut runtime_reader, "navigation.path.query");
    let removed = applied_operations(&restored.synced)
        .into_iter()
        .any(|operation| {
            matches!(
                operation,
                Operation::RemoveComponent { node: at, component, .. }
                    if at == node && component == obstacle
            )
        });
    assert!(
        removed,
        "the engine received the removal before the second path query"
    );
    answer_path(&mut editor, &mut runtime_writer, restored.request, true);
    let status = tool_call(7, "navigation.bake.status", &[]);
    let replies = converse(&[INITIALIZE, &status], &mut editor);
    assert!(tool_text(&replies, 1).contains("path_found"));
    drop(runtime_writer);
}

// --- Audio (#29) -----------------------------------------------------------------------------------

/// The engine's audio fixtures: the bytes `cy_test_integration_editor_backend_audio` submits and
/// the replies it produced. See `src/editor_backend/tests/test_audio_service.cpp`.
fn engine_audio_fixture(name: &str) -> Vec<u8> {
    let path = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../../src/editor_backend/tests/data")
        .join(name);
    std::fs::read(&path).unwrap_or_else(|error| panic!("{}: {error}", path.display()))
}

/// What the audio double was asked: every `audio.*` operation with its payload, and the plays.
#[derive(Default)]
struct AudioAsked {
    requests: Vec<(String, Vec<u8>)>,
    plays: Vec<String>,
}

/// A runtime that answers `audio.*` with the ENGINE'S OWN replies, byte for byte: the state after
/// the canonical mixer and preview, and after Play started the world's sources.
fn audio_runtime_double(editor: &mut Editor) -> Arc<Mutex<AudioAsked>> {
    let (editor_reader, runtime_writer) = std::io::pipe().unwrap();
    let (runtime_reader, editor_writer) = std::io::pipe().unwrap();
    editor.runtime = RuntimeSession::over(Session::over(editor_reader, editor_writer));
    let asked = Arc::new(Mutex::new(AudioAsked::default()));
    let recorded = Arc::clone(&asked);
    let state = engine_audio_fixture("audio_state_v1.wire");
    let playing = engine_audio_fixture("audio_state_play_v1.wire");
    let vocabulary = engine_audio_fixture("audio_capabilities_v1.wire");
    std::thread::spawn(move || {
        let (mut reader, mut writer) = (runtime_reader, runtime_writer);
        let mut sounding = false;
        let _ = cy_editor_protocol::server::serve(&mut reader, &mut writer, |message| {
            let mut asked = recorded.lock().unwrap();
            Some(match message {
                Message::ServiceRequest {
                    request,
                    operation,
                    payload,
                    ..
                } => {
                    let (kind, reply) = if operation == "audio.capabilities.get" {
                        (ServiceEventKind::Completed, vocabulary.clone())
                    } else if operation.starts_with("audio.") {
                        let reply = if sounding { &playing } else { &state };
                        (ServiceEventKind::Completed, reply.clone())
                    } else {
                        let mut failure = Writer::new();
                        failure.u32(1);
                        failure.text("operation-unsupported");
                        failure.text("the audio double serves audio only");
                        (ServiceEventKind::Failed, failure.finish())
                    };
                    if operation.starts_with("audio.") {
                        asked.requests.push((operation, payload));
                    }
                    vec![Message::ServiceEvent {
                        request,
                        kind,
                        schema_version: 1,
                        payload: reply,
                    }]
                }
                Message::Play {
                    request,
                    state,
                    mode,
                } => {
                    sounding = state == "playing";
                    asked.plays.push(state.clone());
                    vec![Message::Playing {
                        request,
                        state,
                        mode,
                        detail: "1 entities; audio: 1 source(s) on null".into(),
                    }]
                }
                Message::Apply { request, .. } | Message::SyncWorld { request, .. } => {
                    vec![Message::Applied {
                        request,
                        frame: cy_editor_protocol::FrameId::from_raw(1),
                        observed: Vec::new(),
                    }]
                }
                _ => Vec::new(),
            })
        });
    });
    asked
}

/// Pump the editor until the double has answered everything the editor queued.
fn settle_audio(editor: &mut Editor) {
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    loop {
        editor.pump();
        if !editor.backend.audio.pending() && editor.backend.audio.vocabulary().is_some() {
            return;
        }
        assert!(
            std::time::Instant::now() < deadline,
            "the audio requests were never answered"
        );
        std::thread::sleep(std::time::Duration::from_millis(1));
    }
}

fn last_audio_payload(asked: &Arc<Mutex<AudioAsked>>, operation: &str) -> Vec<u8> {
    let asked = asked.lock().unwrap();
    let Some((_, payload)) = asked
        .requests
        .iter()
        .rev()
        .find(|(sent, _)| sent == operation)
    else {
        panic!("the engine was never sent {operation}");
    };
    payload.clone()
}

/// A structured result, which the wire carries as text.
fn structured(replies: &[Json], index: usize, key: &str) -> String {
    result(replies, index)
        .get("structuredContent")
        .get(key)
        .as_text()
        .unwrap_or_else(|| panic!("no {key} in the result"))
        .to_owned()
}

fn structured_number(replies: &[Json], index: usize, key: &str) -> f64 {
    structured(replies, index, key)
        .parse()
        .unwrap_or_else(|_| panic!("{key} is not a number"))
}

#[test]
fn the_mixer_is_an_undoable_mcp_peer_of_the_panel_and_reaches_the_engine() {
    let sandbox = Sandbox::new("audio-mixer");
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor.open_document("worlds/audio.cyworld").unwrap();
    let asked = audio_runtime_double(&mut editor);

    // The canonical mixer, authored call by call — the same text the engine's suite applies.
    let authored = converse(
        &[
            INITIALIZE,
            &tool_call(2, "audio.bus.add", &[("name", "Music")]),
            &tool_call(3, "audio.bus.add", &[("name", "SFX")]),
            &tool_call(4, "audio.bus.add", &[("name", "Reverb")]),
            r#"{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"audio.bus.volume","arguments":{"name":"Music","volume":0.5}}}"#,
            r#"{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"audio.bus.volume","arguments":{"name":"SFX","volume":0.8}}}"#,
            r#"{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"audio.bus.volume","arguments":{"name":"Reverb","volume":0.6}}}"#,
            r#"{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"audio.bus.send","arguments":{"from":"SFX","to":"Reverb","level":0.3}}}"#,
            r#"{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"audio.bus.effect.add","arguments":{"bus":"Master","kind":"limiter","a":0.95,"b":0}}}"#,
            r#"{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"audio.bus.effect.add","arguments":{"bus":"SFX","kind":"low-pass","a":0,"b":0.5}}}"#,
            // A send back into its own source is the cycle the engine would never finish mixing.
            r#"{"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"audio.bus.send","arguments":{"from":"Reverb","to":"SFX","level":0.5}}}"#,
        ],
        &mut editor,
    );
    for index in 1..=9 {
        assert_eq!(
            result(&authored, index).get("isError"),
            &Json::Bool(false),
            "{}",
            tool_text(&authored, index)
        );
    }
    assert_eq!(result(&authored, 10).get("isError"), &Json::Bool(true));
    assert!(tool_text(&authored, 10).contains("cycle"));
    let canonical = engine_audio_fixture("audio_mixer_v1.cymixer");
    let on_disk = std::fs::read(sandbox.0.join("game/audio/mixer.cymixer")).unwrap();
    assert_eq!(
        String::from_utf8_lossy(&on_disk),
        String::from_utf8_lossy(&canonical),
        "the MCP-authored mixer is the text the engine's suite applies"
    );

    settle_audio(&mut editor);
    assert_eq!(
        last_audio_payload(&asked, "audio.mixer.apply"),
        canonical,
        "every save went to the engine, the last one last"
    );
    let status = converse(
        &[INITIALIZE, &tool_call(12, "audio.status", &[])],
        &mut editor,
    );
    // The engine's numbers, not the editor's: decoded from the reply `unit` produced.
    assert_eq!(structured(&status, 1, "backend"), "null");
    assert!((structured_number(&status, 1, "bus.Music.volume") - 0.5).abs() < 1e-6);
    assert!(
        structured(&status, 1, "bus.SFX.route").contains("sends=[Reverb:0.3] effects=[low-pass]")
    );

    // Undo goes back one transaction, and the engine is sent what the file now says.
    let undone = converse(&[INITIALIZE, &tool_call(13, "edit.undo", &[])], &mut editor);
    assert_eq!(result(&undone, 1).get("isError"), &Json::Bool(false));
    let reverted = std::fs::read_to_string(sandbox.0.join("game/audio/mixer.cymixer")).unwrap();
    assert!(!reverted.contains("effect SFX"), "{reverted}");
    settle_audio(&mut editor);
    assert_eq!(
        last_audio_payload(&asked, "audio.mixer.apply"),
        reverted.into_bytes()
    );
    let redone = converse(&[INITIALIZE, &tool_call(14, "edit.redo", &[])], &mut editor);
    assert_eq!(result(&redone, 1).get("isError"), &Json::Bool(false));
    settle_audio(&mut editor);
    assert_eq!(last_audio_payload(&asked, "audio.mixer.apply"), canonical);
    assert_eq!(
        std::fs::read(sandbox.0.join("game/audio/mixer.cymixer")).unwrap(),
        canonical
    );
}

#[test]
fn a_cue_and_a_spatial_source_preview_through_the_engine_over_mcp() {
    let sandbox = Sandbox::new("audio-cue");
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor.open_document("worlds/audio.cyworld").unwrap();
    let asked = audio_runtime_double(&mut editor);
    std::fs::create_dir_all(sandbox.0.join("game/audio")).unwrap();
    std::fs::write(
        sandbox.0.join("game/audio/mixer.cymixer"),
        engine_audio_fixture("audio_mixer_v1.cymixer"),
    )
    .unwrap();

    let saved = converse_within(
        &[
            INITIALIZE,
            &tool_call(
                2,
                "audio.cue.save",
                &[
                    ("reference", "audio/cues/ping.cycue"),
                    ("clip", "tone:660:0.5"),
                    ("bus", "SFX"),
                ],
            ),
            r#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"audio.source.create","arguments":{"cue":"audio/cues/ping.cycue","at":[3,0,0],"min_distance":1,"max_distance":50,"attenuation":"inverse"}}}"#,
        ],
        &mut editor,
        "audio/",
    );
    assert_eq!(
        result(&saved, 1).get("isError"),
        &Json::Bool(false),
        "{}",
        tool_text(&saved, 1)
    );
    assert_eq!(
        std::fs::read(sandbox.0.join("audio/cues/ping.cycue")).unwrap(),
        engine_audio_fixture("audio_cue_v1.cycue")
    );
    assert_eq!(result(&saved, 2).get("isError"), &Json::Bool(false));
    let entity = structured(&saved, 2, "entity");

    // The viewport camera starts at the origin looking down -Z, so the request is exactly the one
    // the engine's suite submits: the source three metres to the listener's right.
    let previewed = converse(
        &[
            INITIALIZE,
            &tool_call(4, "audio.source.preview", &[("entity", &entity)]),
        ],
        &mut editor,
    );
    assert_eq!(
        result(&previewed, 1).get("isError"),
        &Json::Bool(false),
        "{}",
        tool_text(&previewed, 1)
    );
    settle_audio(&mut editor);
    assert_eq!(
        last_audio_payload(&asked, "audio.cue.preview"),
        engine_audio_fixture("audio_cue_preview_v1.wire")
    );
    let status = converse(
        &[INITIALIZE, &tool_call(5, "audio.status", &[])],
        &mut editor,
    );
    assert_eq!(
        structured(&status, 1, "preview.cue"),
        "audio/cues/ping.cycue"
    );
    assert_eq!(structured(&status, 1, "preview.playing"), "true");
    let gain = structured_number(&status, 1, "preview.gain");
    assert!((gain - 1.0 / 3.0).abs() < 1e-4, "{gain}");
    let (left, right) = (
        structured_number(&status, 1, "preview.left"),
        structured_number(&status, 1, "preview.right"),
    );
    assert!(right > 0.99 && left < 0.01);

    // Undo removes the source the preview played from, in the world's own history.
    let undone = converse(&[INITIALIZE, &tool_call(6, "edit.undo", &[])], &mut editor);
    assert_eq!(result(&undone, 1).get("isError"), &Json::Bool(false));
    let gone = converse(
        &[
            INITIALIZE,
            &tool_call(7, "audio.source.preview", &[("entity", &entity)]),
        ],
        &mut editor,
    );
    assert_eq!(result(&gone, 1).get("isError"), &Json::Bool(true));
}

#[test]
fn play_reports_the_sources_the_engine_is_sounding() {
    let sandbox = Sandbox::new("audio-play");
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(&sandbox.0));
    editor.open_document("worlds/audio.cyworld").unwrap();
    let asked = audio_runtime_double(&mut editor);
    let played = converse(
        &[
            INITIALIZE,
            &tool_call(2, "play.enter", &[]),
            &tool_call(3, "audio.refresh", &[]),
        ],
        &mut editor,
    );
    assert_eq!(result(&played, 1).get("isError"), &Json::Bool(false));
    assert_eq!(result(&played, 2).get("isError"), &Json::Bool(false));
    settle_audio(&mut editor);
    assert_eq!(asked.lock().unwrap().plays, ["playing"]);
    let status = converse(
        &[INITIALIZE, &tool_call(4, "audio.status", &[])],
        &mut editor,
    );
    assert_eq!(structured(&status, 1, "playing"), "true");
    assert_eq!(structured(&status, 1, "play_voices"), "1");
}
