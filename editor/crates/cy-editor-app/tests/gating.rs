//! The agent interface is optional at build time, and this is what keeps it optional. Task 3.1.
//!
//! `editor-agent-interface`, "Agents are editor clients, not a privileged peer":
//!
//! > The interface SHALL be optional at build time and absent from a shipped runtime.
//!
//! > #### Scenario: The interface is removable
//! > - **WHEN** the editor is built without the agent interface
//! > - **THEN** no agent transport SHALL be compiled, linked, or listening
//!
//! --- WHY A TEST AND NOT A COMMENT -------------------------------------------------------------------
//!
//! Because "optional" erodes the same way containment does, and for the same reason: making
//! `cy-editor-mcp` non-optional is a one-word edit that fixes a build error somewhere, and every step
//! toward it is locally reasonable. The manifest is read as text here, in the manner of
//! `layering.rs`, `containment.rs` and `safety.rs`, so that the wiring is checked rather than
//! remembered.
//!
//! --- AND WHY IT IS ON BY DEFAULT --------------------------------------------------------------------
//!
//! `[features] default = ["agent-interface"]`, deliberately and checked below. A delivered capability
//! that has to be asked for is a capability nothing exercises — M3 shipped a renderer nothing tested
//! and M4 repeated it three times over. Optional means *removable*, not *off*.

use std::path::{Path, PathBuf};

fn manifest() -> String {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("Cargo.toml");
    std::fs::read_to_string(&path).expect("this crate's own manifest is readable")
}

fn crates_directory() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("the app crate is inside crates/")
        .to_path_buf()
}

/// The one line of a manifest section that names a key, if there is one.
fn line_naming(text: &str, section: &str, key: &str) -> Option<String> {
    let mut current = String::new();
    for line in text.lines() {
        let trimmed = line.trim();
        if trimmed.starts_with('[') && trimmed.ends_with(']') {
            current = trimmed.trim_matches(['[', ']']).to_string();
            continue;
        }
        if current == section
            && trimmed
                .split_once('=')
                .is_some_and(|(name, _)| name.trim() == key)
        {
            return Some(trimmed.to_string());
        }
    }
    None
}

#[test]
fn the_transport_is_an_optional_dependency_behind_a_feature() {
    let manifest = manifest();
    let dependency = line_naming(&manifest, "dependencies", "cy-editor-mcp")
        .expect("the binary depends on the transport");
    assert!(
        dependency.contains("optional = true"),
        "the transport must be an OPTIONAL dependency, or a build without the agent interface \
         still compiles and links it:\n  {dependency}"
    );

    let feature = line_naming(&manifest, "features", "agent-interface")
        .expect("there is an agent-interface feature");
    assert!(
        feature.contains("dep:cy-editor-mcp"),
        "the feature must be what enables the dependency:\n  {feature}"
    );
}

#[test]
fn the_feature_is_on_by_default() {
    // Optional means removable, not off. See the module note.
    let default = line_naming(&manifest(), "features", "default")
        .expect("the crate declares its default features");
    assert!(
        default.contains("agent-interface"),
        "the agent interface is a delivered capability and is on by default:\n  {default}"
    );
}

#[test]
fn this_build_has_the_interface_it_says_it_has() {
    // The cfg and the manifest agree, checked in the build that is actually running rather than
    // read out of a file. Under `--no-default-features` this asserts nothing, deliberately: the
    // transport is not linked, so there is nothing to ask it — and what such a run is really
    // checking is that every OTHER test in this crate still passes without it, which they do at the
    // same time and by construction.
    let offered = line_naming(&manifest(), "features", "default")
        .is_some_and(|line| line.contains("agent-interface"));
    assert!(
        !cfg!(feature = "agent-interface") || offered,
        "this build has the agent interface, so the manifest must offer it"
    );
}

#[test]
fn nothing_below_the_binary_names_the_transport() {
    // "No MCP type SHALL appear in the editor's command, document, or view-model layers." The
    // dependency direction is what enforces that, and this is what keeps the direction: only the
    // binary may name the transport crate, so nothing below it can see a wire type even by
    // accident.
    let mut offenders = Vec::new();
    for entry in std::fs::read_dir(crates_directory()).expect("crates/ is readable") {
        let path = entry.expect("a readable directory entry").path();
        let manifest_path = path.join("Cargo.toml");
        if !manifest_path.is_file() {
            continue;
        }
        let name = path
            .file_name()
            .expect("a directory name")
            .to_string_lossy()
            .into_owned();
        if name == "cy-editor-app" || name == "cy-editor-mcp" {
            continue;
        }
        let text = std::fs::read_to_string(&manifest_path).expect("a readable manifest");
        if text.contains("cy-editor-mcp") {
            offenders.push(name);
        }
    }
    assert!(
        offenders.is_empty(),
        "these crates name the MCP transport:\n  {}\n\nOnly the binary may. A crate below it that \
         can see the wire format is one the wire format cannot be replaced without touching, which \
         is the whole reason the transport sits behind cy_editor_agent::AgentTransport.",
        offenders.join("\n  ")
    );
}

#[test]
fn the_projection_is_not_optional() {
    // `cy-editor-agent` holds the tools, the read surface, the session and the budget, and no
    // transport. Making IT optional would put the editor's own model of what an agent can see
    // behind a flag, which saves nothing and makes the model harder to test.
    let dependency = line_naming(&manifest(), "dependencies", "cy-editor-agent")
        .expect("the binary depends on the projection");
    assert!(
        !dependency.contains("optional"),
        "the projection is not the transport and is not optional:\n  {dependency}"
    );
}
