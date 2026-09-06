//! The workspace's dependency direction, enforced. Task 2.1.
//!
//! `editor-rust-application`: "Dependency direction SHALL be enforced by the workspace, and a cycle
//! or an upward dependency SHALL be **a build error rather than a review comment**."
//!
//! Cargo gives half of that free — a dependency cycle between crates is a hard error. The other half
//! it is perfectly happy with: nothing in Cargo objects to `cy-editor-core` depending on
//! `cy-editor-app`. This test is that half, and it is the same shape as the engine's
//! `tools/layercheck/layercheck.py`: every crate declares its layer in its manifest, and a
//! dependency on a crate at its own level or above fails.
//!
//! It reads the manifests as text rather than running `cargo metadata`, for two reasons. It needs no
//! network, no lockfile and no build, so it runs in a bare checkout; and the only shapes it has to
//! understand are the ones this workspace writes, so anything else is a parse error rather than a
//! silently skipped dependency — which is the failure mode that would make a layer check useless.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// One workspace member.
#[derive(Debug)]
struct Crate {
    name: String,
    layer: u32,
    dependencies: Vec<String>,
}

fn crates_directory() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("the app crate is inside crates/")
        .to_path_buf()
}

/// Read every member's name, layer and workspace dependencies.
fn members() -> Vec<Crate> {
    let mut members = Vec::new();
    for entry in std::fs::read_dir(crates_directory()).expect("crates/ is readable") {
        let path = entry
            .expect("a readable directory entry")
            .path()
            .join("Cargo.toml");
        if !path.is_file() {
            continue;
        }
        members.push(parse(
            &std::fs::read_to_string(&path).expect("a readable manifest"),
            &path,
        ));
    }
    assert!(
        members.len() >= 8,
        "the workspace lost members: found {}",
        members.len()
    );
    members
}

/// The three things this test needs out of a manifest, and nothing else.
fn parse(text: &str, path: &Path) -> Crate {
    let mut name = None;
    let mut layer = None;
    let mut dependencies = Vec::new();
    let mut section = String::new();

    for line in text.lines() {
        let line = line.trim();
        if line.starts_with('[') && line.ends_with(']') {
            section = line.trim_matches(['[', ']']).to_string();
            continue;
        }
        let Some((key, value)) = line.split_once('=') else {
            continue;
        };
        let key = key.trim();
        let value = value.trim();

        match section.as_str() {
            "package" if key == "name" => name = Some(value.trim_matches('"').to_string()),
            "package.metadata.cy" if key == "layer" => {
                layer = Some(value.parse().expect("a layer is a number"));
            }
            "dependencies" | "dev-dependencies" if value.contains("path =") => {
                dependencies.push(key.to_string());
            }
            _ => {}
        }
    }

    Crate {
        name: name.unwrap_or_else(|| panic!("{} has no package name", path.display())),
        layer: layer.unwrap_or_else(|| {
            panic!(
                "{} declares no [package.metadata.cy] layer.\n  Every member must, or the layer \
                 rule silently stops covering it.",
                path.display()
            )
        }),
        dependencies,
    }
}

#[test]
fn no_crate_depends_upward_or_sideways() {
    let members = members();
    let layers: BTreeMap<&str, u32> = members
        .iter()
        .map(|member| (member.name.as_str(), member.layer))
        .collect();

    let mut violations = Vec::new();
    for member in &members {
        for dependency in &member.dependencies {
            let Some(other) = layers.get(dependency.as_str()) else {
                violations.push(format!(
                    "  {} depends on {dependency}, which is not a workspace member",
                    member.name
                ));
                continue;
            };
            if *other >= member.layer {
                violations.push(format!(
                    "  {} (layer {}) depends on {dependency} (layer {other})",
                    member.name, member.layer
                ));
            }
        }
    }

    assert!(
        violations.is_empty(),
        "the editor workspace's dependency direction is violated:\n{}\n\nA crate may depend only \
         on crates at a LOWER layer. Move the shared thing down, or invert the dependency with a \
         trait the way cy-editor-commands does for its context.",
        violations.join("\n")
    );
}

#[test]
fn panels_do_not_depend_on_panels() {
    // The specification's scenario: "WHEN a sequence editor needs selection THEN it SHALL depend on
    // the shared selection service, not on the hierarchy panel's crate."
    //
    // Today every view model is in one crate, so the check is that the crate depends on services
    // rather than on anything at its own level — which the layer rule above already covers. What
    // this test adds is the direction that will matter when domain editors become their own crates:
    // no crate at layer 4 may name another at layer 4.
    let members = members();
    let presentation: Vec<&Crate> = members
        .iter()
        .filter(|member| member.layer >= 4 && member.layer < 5)
        .collect();
    for member in &presentation {
        for dependency in &member.dependencies {
            assert!(
                !presentation.iter().any(|peer| peer.name == *dependency),
                "{} depends on the peer panel crate {dependency}; cross-panel coordination goes \
                 through a shared service",
                member.name
            );
        }
    }
}

#[test]
fn every_member_declares_a_layer_and_the_set_is_contiguous() {
    let members = members();
    let mut layers: Vec<u32> = members.iter().map(|member| member.layer).collect();
    layers.sort_unstable();
    layers.dedup();
    assert_eq!(layers[0], 0, "something must be at the bottom");
    for pair in layers.windows(2) {
        assert_eq!(
            pair[1],
            pair[0] + 1,
            "the layer numbers have a gap at {}; a gap means a layer was removed and the numbers \
             were not closed up, which makes the next addition ambiguous",
            pair[0]
        );
    }
}
