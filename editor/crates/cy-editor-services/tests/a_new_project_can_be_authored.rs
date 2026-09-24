// SPDX-License-Identifier: MIT
//! A project made from the `empty` template is one a person can author in.
//!
//! --- THE DEFECT THIS PINS -------------------------------------------------------------------------
//!
//! The `empty` template used to write `project.json` and a bare `cyworld 1` world and nothing else.
//! With no `types.cytypes` the opened world had an empty schema, so `scene.create-primitive` wrote
//! an entity with a `MeshRenderer` and no `Transform`: it sat at the origin, and neither the gizmo
//! nor `scene.translate` had anything to move. The sample project never showed it, because it
//! carries the engine's type manifest.

use std::path::{Path, PathBuf};

use cy_editor_commands::registry::{Arguments, Registry};
use cy_editor_commands::scope::Scope;
use cy_editor_core::Actor;
use cy_editor_core::value::Value;
use cy_editor_services::editor::Editor;
use cy_editor_services::project::ProjectService;
use cy_editor_services::{Template, builtin, worldfile};
use cy_editor_viewport::gizmo::TransformBinding;

const WORLD: &str = "worlds/main.cyworld";

struct Sandbox(PathBuf);

impl Sandbox {
    fn new(name: &str) -> Self {
        let unique = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |elapsed| elapsed.as_nanos());
        let path = std::env::temp_dir()
            .join(format!("cy-new-project-{}-{unique}", std::process::id()))
            .join(name);
        std::fs::create_dir_all(&path).expect("a writable temporary directory");
        Self(path)
    }

    fn path(&self) -> &Path {
        &self.0
    }
}

impl Drop for Sandbox {
    fn drop(&mut self) {
        if let Some(parent) = self.0.parent() {
            let _ = std::fs::remove_dir_all(parent);
        }
    }
}

fn registry() -> Registry {
    let mut registry = Registry::new();
    builtin::register(&mut registry).expect("the built-in commands satisfy their own metadata");
    registry
}

#[test]
fn every_template_carries_the_engines_type_manifest_and_names_the_project() {
    for template in Template::builtin() {
        let sandbox = Sandbox::new("MyGame");
        template.create(sandbox.path()).expect("a new project");

        let manifest = std::fs::read_to_string(sandbox.path().join(worldfile::TYPE_MANIFEST))
            .unwrap_or_else(|_| panic!("the {} template wrote no type manifest", template.name));
        assert!(
            manifest.contains("runtime \"Transform\""),
            "{}",
            template.name
        );

        let project = std::fs::read_to_string(sandbox.path().join(ProjectService::MANIFEST))
            .expect("the project manifest");
        assert!(project.contains("\"name\": \"MyGame\""), "{project}");
        assert!(project.contains("\"worlds\": \"worlds\""), "{project}");
        assert!(ProjectService::declares(sandbox.path()));
    }
}

#[test]
fn a_primitive_created_in_an_empty_project_has_a_transform_and_is_saved_with_one() {
    let sandbox = Sandbox::new("Empty");
    Template::named("empty")
        .expect("the empty template")
        .create(sandbox.path())
        .expect("a new project");

    let registry = registry();
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(sandbox.path()));
    let id = editor.open_document(WORLD).expect("the empty world opens");
    let binding = TransformBinding::of_schema(editor.documents.get(id).unwrap().schema())
        .expect("the empty world's schema declares a Transform the gizmo can bind to");

    editor
        .invoke(
            &registry,
            "scene.create-primitive",
            &Scope::unrestricted(),
            &Arguments::new().with("shape", Value::Text("box".to_string())),
        )
        .expect("the box is created");
    let node = editor
        .selection
        .get()
        .nodes()
        .next()
        .expect("creating a primitive selects it");
    let translation = editor
        .documents
        .get(id)
        .unwrap()
        .content()
        .field(node, binding.component, binding.translation)
        .cloned();
    assert_eq!(translation, Some(Value::Vec3([0.0, 0.0, 0.0])));

    editor
        .invoke(
            &registry,
            "file.save",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("the save");
    let saved = std::fs::read_to_string(sandbox.path().join(WORLD)).expect("the saved world");
    assert!(saved.contains("runtime \"Transform\""), "{saved}");
    assert!(saved.contains("runtime \"MeshRenderer\""), "{saved}");
}
