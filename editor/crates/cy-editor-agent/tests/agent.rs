//! The claims `editor-agent-interface` makes, as tests. M5 task 5.4.
//!
//! Every case here is one of the specification's own scenarios, and the reason they are integration
//! tests rather than unit tests is that most of them are about the JOIN: an agent's invocation and a
//! human's have to reach the same registry, the same transaction system and the same history, and a
//! test that stubbed either half would prove nothing about that.

use cy_editor_agent::budget::Budget;
use cy_editor_agent::resource::Resources;
use cy_editor_agent::session::{
    AgentIdentity, AgentSession, Confirmation, Confirmer, Decision, RefuseEverything,
};
use cy_editor_agent::tool::{Exclusion, Exclusions, project};
use cy_editor_commands::context::{CommandContext, Outcome};
use cy_editor_commands::metadata::{EffectClass, Metadata, ParameterSpec};
use cy_editor_commands::registry::{Arguments, Command, Registry};
use cy_editor_commands::scope::{DocumentScope, Scope};
use cy_editor_core::Actor;
use cy_editor_core::problem::Problem;
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_services::editor::Editor;

/// A registry with one reversible command, one irreversible one, and one that only reads.
fn registry() -> Registry {
    let mut registry = Registry::new();
    registry
        .register(Command::new(
            Metadata::new(
                "scene.create-entity",
                "Create Entity",
                "Scene",
                "Creates an empty entity in the active document, at the root.",
                EffectClass::ReversibleMutation,
            ),
            |context: &mut dyn CommandContext, _: &Arguments| {
                let id = context
                    .active_document()
                    .ok_or_else(|| Problem::new("create an entity", "no document is active"))?;
                let actor = context.actor();
                let document = context
                    .document_mut(id)
                    .ok_or_else(|| Problem::new("create an entity", "the document is gone"))?;
                document.with_transaction("Create Entity", actor, |document| {
                    document.create_node(None).map(|_| ())
                })?;
                Ok(Outcome::new("Created 1 entity"))
            },
        ))
        .unwrap();
    registry
        .register(Command::new(
            Metadata::new(
                "assets.delete",
                "Delete Asset",
                "Assets",
                "Deletes an asset from disk. This cannot be undone from within the editor.",
                EffectClass::IrreversibleMutation,
            )
            .with(ParameterSpec::required(
                "asset",
                ValueKind::Text,
                "The project-relative path of the asset to delete.",
            )),
            |_: &mut dyn CommandContext, _: &Arguments| Ok(Outcome::new("Deleted 1 asset")),
        ))
        .unwrap();
    registry
        .register(Command::new(
            Metadata::new(
                "scene.count",
                "Count Nodes",
                "Scene",
                "Reports how many nodes the active document holds, changing nothing.",
                EffectClass::Read,
            ),
            |context: &mut dyn CommandContext, _: &Arguments| {
                let count = context
                    .active_document()
                    .and_then(|id| context.document(id))
                    .map_or(0, |document| document.content().node_count());
                Ok(Outcome::new(format!("{count} node(s)")).with(
                    "count",
                    Value::Int(i64::try_from(count).unwrap_or(i64::MAX)),
                ))
            },
        ))
        .unwrap();
    registry
}

fn editor_with_document() -> (Editor, cy_editor_core::ids::DocumentId) {
    let mut editor = Editor::new(Actor::human("designer"));
    let id = editor.open_document("worlds/city.cyworld").unwrap();
    (editor, id)
}

fn session(scope: Scope) -> AgentSession {
    AgentSession::new(
        AgentIdentity {
            agent: "claude".to_string(),
            session: "s-3".to_string(),
        },
        "raise the streetlights to 4 m",
        scope,
        Budget::default(),
        "r-1",
        0,
    )
}

/// A confirmer that says yes once and then no, so a test can tell a grant from a prompt.
struct AllowOnce {
    remaining: u32,
    asked: Vec<Confirmation>,
}

impl Confirmer for AllowOnce {
    fn confirm(&mut self, confirmation: &Confirmation) -> Decision {
        self.asked.push(confirmation.clone());
        if self.remaining == 0 {
            return Decision::Refuse;
        }
        self.remaining -= 1;
        Decision::Allow
    }
}

#[test]
fn an_agents_move_is_a_humans_move() {
    // "WHEN an agent translates an object and a human translates it identically with the gizmo THEN
    // the resulting transform SHALL be identical, and each SHALL produce one undoable transaction."
    // The same claim over the same command: the two invocations differ in nothing but the actor.
    let registry = registry();

    let (mut human_editor, human_document) = editor_with_document();
    human_editor
        .invoke(
            &registry,
            "scene.create-entity",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .unwrap();

    let (mut agent_editor, agent_document) = editor_with_document();
    let mut connection = session(Scope::new(
        "editing",
        DocumentScope::All,
        [EffectClass::Read, EffectClass::ReversibleMutation],
    ));
    connection
        .invoke(
            &mut agent_editor,
            &registry,
            &mut RefuseEverything,
            "scene.create-entity",
            &Arguments::new(),
            0,
        )
        .unwrap();

    let human = human_editor.documents.get(human_document).unwrap();
    let agent = agent_editor.documents.get(agent_document).unwrap();
    assert_eq!(human.content().node_count(), agent.content().node_count());
    assert_eq!(human.history().entries().len(), 1);
    assert_eq!(agent.history().entries().len(), 1);
    // One undo reverses either.
    assert_eq!(
        human.history().entries()[0].operations.len(),
        agent.history().entries()[0].operations.len()
    );
}

#[test]
fn every_agent_edit_carries_its_actor_its_session_and_its_intent() {
    // "Every transaction SHALL record the actor that produced it. For an agent, that SHALL include
    // the agent's identity, the session, and the stated intent."
    let registry = registry();
    let (mut editor, document) = editor_with_document();
    let mut connection = session(Scope::new(
        "editing",
        DocumentScope::All,
        [EffectClass::ReversibleMutation],
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
        .unwrap();

    let entry = &editor.documents.get(document).unwrap().history().entries()[0];
    assert!(entry.actor.is_agent());
    assert_eq!(entry.actor.intent(), Some("raise the streetlights to 4 m"));
    match &entry.actor {
        Actor::Agent { agent, session, .. } => {
            assert_eq!(agent, "claude");
            assert_eq!(session, "s-3");
        }
        other => panic!("an agent's edit was attributed to {other:?}"),
    }
}

#[test]
fn the_editor_goes_back_to_its_own_actor_afterwards() {
    // Attribution is per invocation, not a mode the editor is left in. An agent that ran once and
    // left the editor believing every subsequent human edit was the agent's would corrupt the
    // history in a way nobody would notice until a review.
    let registry = registry();
    let (mut editor, document) = editor_with_document();
    let mut connection = session(Scope::new(
        "editing",
        DocumentScope::All,
        [EffectClass::ReversibleMutation],
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
        .unwrap();
    editor
        .invoke(
            &registry,
            "scene.create-entity",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .unwrap();

    let entries = &editor.documents.get(document).unwrap().history().entries();
    assert!(entries[0].actor.is_agent());
    // "WHEN a human edits between two agent edits THEN undo SHALL walk the combined history in
    // order, and no edit SHALL be lost or reordered."
    assert!(!entries[1].actor.is_agent());
    assert_eq!(entries.len(), 2);
}

#[test]
fn a_refusal_teaches() {
    // "WHEN a command is unavailable THEN the agent SHALL receive the specific reason and, where one
    // exists, the operation that would make it available."
    let registry = registry();
    let (mut editor, _) = editor_with_document();
    let mut connection = session(Scope::new(
        "review",
        DocumentScope::All,
        [EffectClass::Read],
    ));
    let refused = connection
        .invoke(
            &mut editor,
            &registry,
            &mut RefuseEverything,
            "scene.create-entity",
            &Arguments::new(),
            0,
        )
        .unwrap_err();
    // The refusal names the scope that excluded it, which is what turns "denied" into something an
    // agent can act on.
    assert!(refused.because.contains("\"review\""), "{refused}");
    assert!(refused.remedy.unwrap().contains("read"));
}

#[test]
fn an_irreversible_operation_is_not_performed_without_a_human() {
    // "WHEN an agent attempts to delete an asset from disk THEN the human SHALL be asked, and the
    // prompt SHALL state what is lost."
    let registry = registry();
    let (mut editor, _) = editor_with_document();
    let mut connection = session(Scope::new(
        "everything",
        DocumentScope::All,
        EffectClass::ALL,
    ));
    let arguments = Arguments::new().with("asset", Value::Text("textures/old.png".to_string()));

    let mut refusing = RefuseEverything;
    let refused = connection
        .invoke(
            &mut editor,
            &registry,
            &mut refusing,
            "assets.delete",
            &arguments,
            0,
        )
        .unwrap_err();
    assert!(refused.because.contains("did not confirm"), "{refused}");

    let mut allowing = AllowOnce {
        remaining: 1,
        asked: Vec::new(),
    };
    connection
        .invoke(
            &mut editor,
            &registry,
            &mut allowing,
            "assets.delete",
            &arguments,
            0,
        )
        .unwrap();
    assert_eq!(allowing.asked.len(), 1);
    assert!(allowing.asked[0].what_is_lost.contains("undo cannot"));
    assert_eq!(allowing.asked[0].effect, EffectClass::IrreversibleMutation);
}

#[test]
fn ordinary_edits_are_not_gated_because_undo_is_the_confirmation() {
    // "Reversible edits SHALL NOT require confirmation. Undo is the confirmation, and prompting for
    // ordinary work trains the human to approve without reading."
    let registry = registry();
    let (mut editor, _) = editor_with_document();
    let mut connection = session(Scope::new(
        "editing",
        DocumentScope::All,
        [EffectClass::ReversibleMutation],
    ));
    let mut confirmer = AllowOnce {
        remaining: 0,
        asked: Vec::new(),
    };
    connection
        .invoke(
            &mut editor,
            &registry,
            &mut confirmer,
            "scene.create-entity",
            &Arguments::new(),
            0,
        )
        .unwrap();
    assert!(
        confirmer.asked.is_empty(),
        "an ordinary edit asked for confirmation"
    );
}

#[test]
fn a_deliberate_grant_covers_a_stated_number_of_invocations_and_then_stops() {
    let registry = registry();
    let (mut editor, _) = editor_with_document();
    let mut connection = session(Scope::new(
        "everything",
        DocumentScope::All,
        EffectClass::ALL,
    ));
    connection.grant(EffectClass::IrreversibleMutation, 2);
    let arguments = Arguments::new().with("asset", Value::Text("a.png".to_string()));

    let mut confirmer = AllowOnce {
        remaining: 0,
        asked: Vec::new(),
    };
    for _ in 0..2 {
        connection
            .invoke(
                &mut editor,
                &registry,
                &mut confirmer,
                "assets.delete",
                &arguments,
                0,
            )
            .unwrap();
    }
    assert!(confirmer.asked.is_empty(), "a grant still prompted");

    // The third falls back to asking, and the confirmer refuses.
    assert!(
        connection
            .invoke(
                &mut editor,
                &registry,
                &mut confirmer,
                "assets.delete",
                &arguments,
                0,
            )
            .is_err()
    );
    assert_eq!(confirmer.asked.len(), 1);
}

#[test]
fn revocation_stops_everything_afterwards() {
    // "WHEN access is revoked mid-operation THEN the in-flight transaction SHALL be abandoned rather
    // than half-applied." The half this type owns: nothing else gets through.
    let registry = registry();
    let (mut editor, _) = editor_with_document();
    let mut connection = session(Scope::unrestricted());
    connection
        .invoke(
            &mut editor,
            &registry,
            &mut RefuseEverything,
            "scene.count",
            &Arguments::new(),
            0,
        )
        .unwrap();
    connection.revoke();
    let refused = connection
        .invoke(
            &mut editor,
            &registry,
            &mut RefuseEverything,
            "scene.count",
            &Arguments::new(),
            0,
        )
        .unwrap_err();
    assert!(refused.because.contains("revoked"), "{refused}");
}

#[test]
fn pausing_is_not_revoking() {
    let registry = registry();
    let (mut editor, _) = editor_with_document();
    let mut connection = session(Scope::unrestricted());
    connection.pause();
    assert!(
        connection
            .invoke(
                &mut editor,
                &registry,
                &mut RefuseEverything,
                "scene.count",
                &Arguments::new(),
                0,
            )
            .is_err()
    );
    connection.resume();
    assert!(
        connection
            .invoke(
                &mut editor,
                &registry,
                &mut RefuseEverything,
                "scene.count",
                &Arguments::new(),
                0,
            )
            .is_ok()
    );
}

#[test]
fn observation_is_free_of_side_effects() {
    // "WHEN an agent reads the scene hierarchy THEN no document SHALL become dirty and no selection
    // SHALL change." The compiler is what enforces it — every read takes `&Editor` — and this is the
    // assertion that the enforcement is actually in force.
    let (mut editor, document) = editor_with_document();
    editor
        .documents
        .get_mut(document)
        .unwrap()
        .with_transaction("Create", Actor::human("designer"), |document| {
            document.create_node(None).map(|_| ())
        })
        .unwrap();
    editor
        .documents
        .get_mut(document)
        .unwrap()
        .save(|_| Ok(()))
        .unwrap();

    let dirty_before = editor.documents.any_dirty();
    let selection_before = editor.selection.revision();
    let history_before = editor
        .documents
        .get(document)
        .unwrap()
        .history()
        .entries()
        .len();

    for (uri, _, _) in Resources::list(&editor) {
        let _ = Resources::read(&editor, &uri).unwrap();
    }

    assert_eq!(editor.documents.any_dirty(), dirty_before);
    assert_eq!(editor.selection.revision(), selection_before);
    assert_eq!(
        editor
            .documents
            .get(document)
            .unwrap()
            .history()
            .entries()
            .len(),
        history_before
    );
}

#[test]
fn the_agent_and_the_inspector_read_the_same_thing() {
    // "WHEN an agent reads an entity's properties while the inspector displays them THEN the values
    // SHALL be identical, because both read the same service."
    let (mut editor, document) = editor_with_document();
    let node = {
        let document = editor.documents.get_mut(document).unwrap();
        document
            .with_transaction("Create", Actor::human("designer"), |document| {
                document.create_node(None)
            })
            .unwrap()
    };

    let hierarchy = Resources::read(&editor, &format!("hierarchy:{document}")).unwrap();
    assert!(hierarchy.content.contains(&node.to_string()));
    // The same node count the document itself reports, because the resource reads the document.
    assert_eq!(
        editor
            .documents
            .get(document)
            .unwrap()
            .content()
            .node_count(),
        1
    );
}

#[test]
fn an_address_that_does_not_exist_says_what_would_have_worked() {
    let (editor, _) = editor_with_document();
    let refused = Resources::read(&editor, "nonsense:").unwrap_err();
    let remedy = refused.remedy.unwrap();
    assert!(remedy.contains("hierarchy"), "{remedy}");
    assert!(remedy.contains("selection"), "{remedy}");

    let malformed = Resources::read(&editor, "hierarchy").unwrap_err();
    assert!(malformed.because.contains("<scheme>:<path>"));
}

#[test]
fn a_session_records_what_it_did_and_replays_against_itself() {
    let registry = registry();
    let (mut editor, _) = editor_with_document();
    let mut connection = session(Scope::new(
        "editing",
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
        .unwrap();
    // A refused invocation is recorded too: a replay in which it succeeds has found a change.
    let _ = connection.invoke(
        &mut editor,
        &registry,
        &mut RefuseEverything,
        "assets.delete",
        &Arguments::new().with("asset", Value::Text("a.png".to_string())),
        0,
    );

    let recording = connection.recording();
    assert_eq!(recording.len(), 2);
    assert!(recording.invocations[0].refusal.is_none());
    assert!(recording.invocations[1].refusal.is_some());
    assert!(!recording.invocations[0].transactions.is_empty());

    let text = recording.export();
    assert!(text.contains("revision r-1"), "{text}");
    assert!(text.contains("invoke scene.create-entity"), "{text}");
    assert!(recording.diverges_from(recording).is_none());
}

#[test]
fn a_throttled_agent_is_told_what_is_left() {
    let registry = registry();
    let (mut editor, _) = editor_with_document();
    let mut connection = AgentSession::new(
        AgentIdentity {
            agent: "claude".to_string(),
            session: "s-9".to_string(),
        },
        "count everything repeatedly",
        Scope::unrestricted(),
        Budget {
            invocations_per_window: 2,
            window_millis: 1000,
            ..Budget::default()
        },
        "r-1",
        0,
    );
    for _ in 0..2 {
        connection
            .invoke(
                &mut editor,
                &registry,
                &mut RefuseEverything,
                "scene.count",
                &Arguments::new(),
                0,
            )
            .unwrap();
    }
    let refused = connection
        .invoke(
            &mut editor,
            &registry,
            &mut RefuseEverything,
            "scene.count",
            &Arguments::new(),
            0,
        )
        .unwrap_err();
    assert!(refused.remedy.unwrap().contains("2 of 2 invocations"));
    // And the editor is still perfectly usable, which is the point of throttling rather than
    // blocking.
    editor
        .invoke(
            &registry,
            "scene.create-entity",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .unwrap();
}

#[test]
fn an_excluded_command_is_refused_with_its_reason_and_still_listed() {
    let registry = registry();
    let (mut editor, _) = editor_with_document();
    let mut connection = session(Scope::unrestricted());
    connection.exclusions_mut().declare(
        Exclusion::new(
            "assets.delete",
            "deleting from disk needs a human at the interface, because undo cannot put the file \
             back",
        )
        .unwrap(),
    );

    let refused = connection
        .invoke(
            &mut editor,
            &registry,
            &mut RefuseEverything,
            "assets.delete",
            &Arguments::new().with("asset", Value::Text("a.png".to_string())),
            0,
        )
        .unwrap_err();
    assert!(refused.because.contains("undo cannot"), "{refused}");

    // Listed rather than hidden: "the command SHALL carry the exclusion and its reason, and the
    // reason SHALL be reportable".
    let tools = connection.tools(&registry);
    let deleting = tools
        .iter()
        .find(|tool| tool.name == "assets.delete")
        .unwrap();
    assert!(!deleting.is_offered());
    assert!(deleting.exclusion.is_some());
}

#[test]
fn a_command_registered_for_a_menu_is_an_agent_tool_with_no_further_work() {
    // "WHEN a contributor registers a command THEN it SHALL be invocable by an agent with no
    // additional registration."
    let mut registry = registry();
    registry
        .register(Command::new(
            Metadata::new(
                "scene.frame-selection",
                "Frame Selection",
                "View",
                "Moves the viewport camera so the selection fills the frame.",
                EffectClass::Read,
            ),
            |_: &mut dyn CommandContext, _: &Arguments| Ok(Outcome::new("Framed")),
        ))
        .unwrap();

    let tools = project(&registry, &Exclusions::default());
    assert!(
        tools
            .iter()
            .any(|tool| tool.name == "scene.frame-selection" && tool.is_offered())
    );
}
