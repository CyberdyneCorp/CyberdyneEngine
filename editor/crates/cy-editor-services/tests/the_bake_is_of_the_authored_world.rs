// SPDX-License-Identifier: MIT
//! The lightmap bake is of the world as authored: the editor writes the level's `.cylightmap` from
//! the open world on every bake request, and `cy_build lightmap` bakes that.
//!
//! Integration, for the reason `the_authoring_loop` gives: every claim is about a join — the
//! commands a person's click reaches, the document they edit, the file the engine's tool reads.
//!
//! THE CASE TO READ FIRST is `the_description_is_the_authored_world_byte_for_byte`. The file it
//! compares against, `tools/build/tests/data/editor_level/`, is the one
//! `integration.build_content` parses and bakes on the engine's side, so a line this writer spells
//! differently from what the engine's parser reads fails on one side of the process boundary or
//! the other.

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use cy_editor_commands::registry::{Arguments, Registry};
use cy_editor_commands::scope::Scope;
use cy_editor_core::Actor;
use cy_editor_core::progress::{Cancellation, OperationState};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::Document;
use cy_editor_services::builtin;
use cy_editor_services::editor::Editor;
use cy_editor_services::lightmaps::{
    self, BakeStep, CliLightmapBaker, LightmapBakeOutcome, LightmapBakeService, LightmapBaker,
};
use cy_editor_services::project::ProjectService;

/// The engine side's copy of the world this file authors: its description and its primitives.
fn fixture() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../tools/build/tests/data/editor_level")
}

const DESCRIPTION: &str = "levels/room.cylightmap";
const PRIMITIVES: [&str; 2] = [
    "assets/primitives/Floor.cyprim",
    "assets/primitives/Crate.cyprim",
];

/// A directory that removes itself, declared as a project so the editor will write into it.
struct Sandbox(PathBuf);

impl Sandbox {
    fn new(name: &str) -> Self {
        let unique = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |elapsed| elapsed.as_nanos());
        let path = std::env::temp_dir().join(format!(
            "cy-lighting-{name}-{}-{unique}",
            std::process::id()
        ));
        std::fs::create_dir_all(&path).expect("a writable temporary directory");
        std::fs::write(path.join("project.json"), "{}").expect("a project manifest");
        Self(path)
    }

    fn path(&self) -> &Path {
        &self.0
    }

    fn read(&self, relative: &str) -> String {
        std::fs::read_to_string(self.0.join(relative)).expect("the file the editor wrote")
    }
}

impl Drop for Sandbox {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn registry() -> Registry {
    let mut registry = Registry::new();
    builtin::register(&mut registry).expect("the built-in commands satisfy their own metadata");
    registry
}

/// An editor over a declared project with an open, empty world at `levels/room.cyworld`.
fn editor_with_a_world(sandbox: &Sandbox) -> Editor {
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(sandbox.path()));
    let mut document = Document::new("levels/room.cyworld");
    let component = document.schema_mut().declare_type("Transform", false);
    for (name, kind) in [
        ("translation", ValueKind::Vec3),
        ("rotation", ValueKind::Quat),
        ("scale", ValueKind::Vec3),
    ] {
        document
            .schema_mut()
            .declare_field(component, name, kind, "part of a transform")
            .expect("a fresh schema");
    }
    let id = editor.documents.insert(document);
    editor.workspace.opened(id);
    editor
}

fn invoke(
    editor: &mut Editor,
    registry: &Registry,
    id: &str,
    arguments: &Arguments,
) -> cy_editor_commands::Outcome {
    editor
        .invoke(registry, id, &Scope::unrestricted(), arguments)
        .unwrap_or_else(|problem| panic!("{id} was refused: {problem:?}"))
}

fn text(outcome: &cy_editor_commands::Outcome, key: &str) -> String {
    outcome.values[key]
        .as_text()
        .unwrap_or_else(|| panic!("{key} is text"))
        .to_string()
}

/// The authored room: a floor at twice the level's density, a crate that only occludes, a static
/// point lamp, a movable sun and one irradiance volume.
struct Room {
    lamp: String,
    floor: String,
}

fn author_the_room(editor: &mut Editor, registry: &Registry) -> Room {
    let floor = text(
        &invoke(
            editor,
            registry,
            "scene.create-primitive",
            &Arguments::new()
                .with("shape", Value::Text("plane".into()))
                .with("name", Value::Text("Floor".into()))
                .with("extent", Value::Vec3([4.0, 1.0, 4.0])),
        ),
        "entity",
    );
    let crate_box = text(
        &invoke(
            editor,
            registry,
            "scene.create-primitive",
            &Arguments::new()
                .with("shape", Value::Text("box".into()))
                .with("name", Value::Text("Crate".into()))
                .with("origin", Value::Text("base".into()))
                .with("at", Value::Vec3([1.0, 0.0, 0.5])),
        ),
        "entity",
    );
    let lamp = text(
        &invoke(
            editor,
            registry,
            "scene.create-light",
            &Arguments::new()
                .with("kind", Value::Text("point".into()))
                .with("at", Value::Vec3([0.0, 2.0, 0.0])),
        ),
        "entity",
    );
    let sun = text(
        &invoke(
            editor,
            registry,
            "scene.create-light",
            &Arguments::new().with("kind", Value::Text("directional".into())),
        ),
        "entity",
    );
    invoke(
        editor,
        registry,
        "lighting.object.set-resolution",
        &Arguments::new()
            .with("entity", Value::Text(floor.clone()))
            .with("scale", Value::Float(2.0)),
    );
    invoke(
        editor,
        registry,
        "lighting.object.set-resolution",
        &Arguments::new()
            .with("entity", Value::Text(crate_box))
            .with("scale", Value::Float(1.0))
            .with("receives", Value::Bool(false)),
    );
    for (light, mobility) in [(&lamp, "static"), (&sun, "movable")] {
        invoke(
            editor,
            registry,
            "lighting.light.set-mobility",
            &Arguments::new()
                .with("entity", Value::Text(light.clone()))
                .with("mobility", Value::Text(mobility.into())),
        );
    }
    invoke(
        editor,
        registry,
        "lighting.volume.create",
        &Arguments::new()
            .with("at", Value::Vec3([-1.0, 0.5, -1.0]))
            .with("count_x", Value::Int(3))
            .with("count_y", Value::Int(2))
            .with("count_z", Value::Int(3))
            .with("rays", Value::Int(32)),
    );
    Room { lamp, floor }
}

/// A small, quick bake: the fixture's own settings.
fn quick() -> Arguments {
    Arguments::new()
        .with("mode", Value::Text("irradiance".into()))
        .with("samples", Value::Int(8))
        .with("bounces", Value::Int(1))
        .with("density", Value::Float(4.0))
        .with("page", Value::Int(128))
}

fn write_description(editor: &mut Editor, registry: &Registry) -> cy_editor_commands::Outcome {
    invoke(
        editor,
        registry,
        "lighting.write-lightmap-description",
        &quick(),
    )
}

#[test]
fn the_description_is_the_authored_world_byte_for_byte() {
    let sandbox = Sandbox::new("fixture");
    let registry = registry();
    let mut editor = editor_with_a_world(&sandbox);
    author_the_room(&mut editor, &registry);

    let written = write_description(&mut editor, &registry);
    assert_eq!(text(&written, "description"), DESCRIPTION);
    assert_eq!(written.values["rewritten"], Value::Bool(true));
    let description = sandbox.read(DESCRIPTION);
    assert_eq!(text(&written, "text"), description);
    let expected = std::fs::read_to_string(fixture().join(DESCRIPTION))
        .expect("the engine's copy of the description");
    assert_eq!(
        description, expected,
        "the editor no longer writes what `integration.build_content` parses; if the change is \
         meant, update tools/build/tests/data/editor_level/ and the engine case together"
    );
    for primitive in PRIMITIVES {
        let engine = std::fs::read_to_string(fixture().join(primitive)).unwrap();
        assert_eq!(sandbox.read(primitive), engine, "{primitive}");
    }
}

#[test]
fn what_the_author_changes_is_what_the_description_says() {
    let sandbox = Sandbox::new("changes");
    let registry = registry();
    let mut editor = editor_with_a_world(&sandbox);
    let room = author_the_room(&mut editor, &registry);
    let before = text(&write_description(&mut editor, &registry), "text");
    let lamp_line = |description: &str| {
        description
            .lines()
            .find(|line| line.starts_with("light point"))
            .expect("the lamp's line")
            .to_string()
    };
    assert!(lamp_line(&before).contains(" static id "), "{before}");

    // Mobility: the lamp's line changes and nothing else does; undo puts it back.
    invoke(
        &mut editor,
        &registry,
        "lighting.light.set-mobility",
        &Arguments::new()
            .with("entity", Value::Text(room.lamp.clone()))
            .with("mobility", Value::Text("stationary".into())),
    );
    let moved = text(&write_description(&mut editor, &registry), "text");
    assert!(lamp_line(&moved).contains(" stationary id "), "{moved}");
    assert_eq!(
        before
            .lines()
            .filter(|line| !line.starts_with("light point"))
            .collect::<Vec<_>>(),
        moved
            .lines()
            .filter(|line| !line.starts_with("light point"))
            .collect::<Vec<_>>()
    );
    invoke(&mut editor, &registry, "edit.undo", &Arguments::new());
    assert_eq!(
        text(&write_description(&mut editor, &registry), "text"),
        before
    );

    // Resolution: the floor's instance carries the new scale.
    invoke(
        &mut editor,
        &registry,
        "lighting.object.set-resolution",
        &Arguments::new()
            .with("entity", Value::Text(room.floor))
            .with("scale", Value::Float(0.5)),
    );
    let finer = text(&write_description(&mut editor, &registry), "text");
    let floor = finer
        .lines()
        .find(|line| line.contains("Floor.cyprim"))
        .expect("the floor's instance");
    assert!(floor.contains("\"m0\" 0.5 "), "{floor}");
}

#[test]
fn an_unchanged_world_rewrites_nothing() {
    let sandbox = Sandbox::new("unchanged");
    let registry = registry();
    let mut editor = editor_with_a_world(&sandbox);
    author_the_room(&mut editor, &registry);
    assert_eq!(
        write_description(&mut editor, &registry).values["rewritten"],
        Value::Bool(true)
    );
    let path = sandbox.path().join(DESCRIPTION);
    let stamp = std::fs::metadata(&path).unwrap().modified().unwrap();
    std::thread::sleep(Duration::from_millis(20));
    assert_eq!(
        write_description(&mut editor, &registry).values["rewritten"],
        Value::Bool(false)
    );
    assert_eq!(std::fs::metadata(&path).unwrap().modified().unwrap(), stamp);
    // A different setting is a different description.
    let denser = invoke(
        &mut editor,
        &registry,
        "lighting.write-lightmap-description",
        &quick().with("density", Value::Float(8.0)),
    );
    assert_eq!(denser.values["rewritten"], Value::Bool(true));
}

/// A baker that records what it was asked to bake and bakes nothing.
struct Recording(Mutex<Vec<String>>);

impl LightmapBaker for Recording {
    fn describe(&self) -> String {
        "a recording double".into()
    }

    fn bake(
        &self,
        root: &Path,
        description: &str,
        output: &str,
        _: &mut dyn FnMut(BakeStep),
        _: &Cancellation,
    ) -> cy_editor_core::problem::Result<Option<LightmapBakeOutcome>> {
        let text = std::fs::read_to_string(root.join(description)).unwrap_or_default();
        self.0.lock().unwrap().push(text);
        Ok(Some(LightmapBakeOutcome {
            output: output.into(),
            objects: 1,
            pages: 1,
            texels: 1,
            device_bytes: 1,
            mips: 0,
            padding_short: 0,
            volumes: 1,
            probes: 18,
            seconds: 0.0,
            cached: false,
        }))
    }
}

#[test]
fn a_bake_with_no_description_bakes_the_world_as_it_is_now() {
    let sandbox = Sandbox::new("request");
    let registry = registry();
    let baker = Arc::new(Recording(Mutex::new(Vec::new())));
    let mut editor = editor_with_a_world(&sandbox).with_lightmap_baker(
        LightmapBakeService::with_baker(sandbox.path(), baker.clone()),
    );
    let room = author_the_room(&mut editor, &registry);

    for mobility in ["static", "movable"] {
        invoke(
            &mut editor,
            &registry,
            "lighting.light.set-mobility",
            &Arguments::new()
                .with("entity", Value::Text(room.lamp.clone()))
                .with("mobility", Value::Text(mobility.into())),
        );
        let started = invoke(&mut editor, &registry, "lighting.bake-lightmaps", &quick());
        assert_eq!(text(&started, "description"), DESCRIPTION);
        let Some(Value::Int(request)) = started.values.get("request").cloned() else {
            panic!("a request: {started:?}");
        };
        let operation = editor
            .operations
            .all()
            .iter()
            .find(|operation| operation.id() == u64::try_from(request).unwrap())
            .cloned()
            .unwrap();
        assert_eq!(
            operation.block_until_settled(Duration::from_secs(10)),
            OperationState::Completed
        );
    }
    let descriptions = baker.0.lock().unwrap().clone();
    assert_eq!(descriptions.len(), 2);
    assert!(
        descriptions[0].contains(" static id "),
        "{}",
        descriptions[0]
    );
    assert!(
        descriptions[1].contains(" movable id "),
        "{}",
        descriptions[1]
    );
    assert!(
        !descriptions[1].contains(" static id "),
        "{}",
        descriptions[1]
    );
}

#[test]
fn a_bake_of_no_world_is_refused_by_name() {
    let sandbox = Sandbox::new("no-world");
    let registry = registry();
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(sandbox.path()));
    let problem = editor
        .invoke(
            &registry,
            "lighting.bake-lightmaps",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect_err("nothing to bake");
    assert!(
        problem.to_string().contains("no world is open"),
        "{problem}"
    );
    let problem = editor
        .invoke(
            &registry,
            "lighting.write-lightmap-description",
            &Scope::unrestricted(),
            &Arguments::new().with("samples", Value::Int(0)),
        )
        .expect_err("a count the engine refuses");
    assert!(problem.to_string().contains("samples"), "{problem}");
}

// --- The real tool ---------------------------------------------------------------------------------

/// What one real bake of the sandbox's world reported, and the cooked lightmap it left.
fn bake_for_real(
    baker: &CliLightmapBaker,
    sandbox: &Sandbox,
    editor: &mut Editor,
    registry: &Registry,
) -> (LightmapBakeOutcome, Vec<u8>) {
    write_description(editor, registry);
    let output = LightmapBakeService::default_output(DESCRIPTION);
    let outcome = baker
        .bake(
            sandbox.path(),
            DESCRIPTION,
            &output,
            &mut |_| {},
            &Cancellation::new(),
        )
        .unwrap_or_else(|problem| panic!("the bake failed: {problem:?}"))
        .expect("an uncancelled bake bakes");
    let cooked = std::fs::read(sandbox.path().join(&output)).unwrap();
    (outcome, cooked)
}

#[test]
fn the_engine_bakes_what_was_authored_when_it_is_built() {
    // Over the real `cy_build`, when the tree has built one; the engine's own cases
    // (`integration.build_content`, `integration.build_lightmap_cli`) hold the tool itself.
    let here = Path::new(env!("CARGO_MANIFEST_DIR"));
    let baker = CliLightmapBaker::found_near(here);
    if baker.describe().starts_with("no cy_build") {
        eprintln!("no cy_build built near this crate; skipping");
        return;
    }
    let sandbox = Sandbox::new("real");
    let registry = registry();
    let mut editor = editor_with_a_world(&sandbox);
    let room = author_the_room(&mut editor, &registry);

    let (first, cooked) = bake_for_real(&baker, &sandbox, &mut editor, &registry);
    assert!(!first.cached);
    assert_eq!(
        first.objects, 1,
        "the floor receives; the crate only occludes"
    );
    assert_eq!((first.volumes, first.probes), (1, 18));
    let probes = lightmaps::read_probes(sandbox.path(), &first.output).unwrap();
    assert_eq!(probes.len(), 1);
    assert_eq!(probes[0].counts, [3, 2, 3]);
    assert_eq!(probes[0].probes.len(), 18);
    assert!(probes[0].probes.iter().any(|probe| probe.radiance[0] > 0.0));

    // Nothing changed: nothing is baked, and the cooked lightmap is the one already there.
    let (again, same) = bake_for_real(&baker, &sandbox, &mut editor, &registry);
    assert!(again.cached, "an unchanged world was baked again");
    assert_eq!(same, cooked);

    // The lamp's mobility, as the editor set it, changes what the engine bakes of it.
    invoke(
        &mut editor,
        &registry,
        "lighting.light.set-mobility",
        &Arguments::new()
            .with("entity", Value::Text(room.lamp))
            .with("mobility", Value::Text("movable".into())),
    );
    let (movable, dark) = bake_for_real(&baker, &sandbox, &mut editor, &registry);
    assert!(!movable.cached);
    assert_ne!(dark, cooked, "a movable lamp baked what a static one did");

    // The floor's resolution, as the editor set it, changes its share of the atlas.
    invoke(
        &mut editor,
        &registry,
        "lighting.object.set-resolution",
        &Arguments::new()
            .with("entity", Value::Text(room.floor))
            .with("scale", Value::Float(4.0)),
    );
    let (finer, _) = bake_for_real(&baker, &sandbox, &mut editor, &registry);
    assert!(
        finer.texels > movable.texels,
        "a floor at twice the resolution traced {} texels against {}",
        finer.texels,
        movable.texels
    );
}
