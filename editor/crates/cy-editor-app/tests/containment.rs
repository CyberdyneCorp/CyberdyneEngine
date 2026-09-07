//! The interface toolkit stays an implementation detail, and this is what makes that true after
//! there is one. Task 1.0.3.
//!
//! `editor-rust-application` says the toolkit is an implementation detail and deliberately declines
//! to name one. That deferral was free for five milestones because the workspace had no
//! dependencies at all. M5.5 chose egui, and from the moment a dependency exists the sentence is
//! only true if something enforces it.
//!
//! --- WHY A TEST AND NOT A REVIEW COMMENT ----------------------------------------------------------
//!
//! Because `use egui::Color32` in `cy-editor-visual` is **locally reasonable every single time**.
//! The crate is about colour; egui has a colour type; the alternative is a conversion. Every step
//! is sensible and the destination is a workspace whose "swap the toolkit by changing one render
//! crate" is no longer true — discovered, as always, at the moment somebody tries to swap it.
//!
//! The M5.5 design note put a number on the boundary: fourteen crates, none of which names a
//! toolkit, so a change of toolkit is one render crate plus four adapters. This test is what keeps
//! that number from growing quietly.
//!
//! --- WHAT IT ENFORCES ------------------------------------------------------------------------------
//!
//!   1. **At most one crate names a toolkit.** Not a crate named in this file — *whichever* crate
//!      it is. The test derives the render crate rather than hard-coding it, so it is correct
//!      before that crate exists, after it is renamed, and if a second one is ever added, which is
//!      the case it fails on.
//!   2. **Only the render crate and the viewport transport name a graphics API.** The transport is
//!      named here, once, with its reason: importing another process's `VkImage` requires calling
//!      Vulkan, and the headless probe that exercises the same import code is not the render crate.
//!      That is a deliberate widening of "the one render crate", and widening it again should be a
//!      decision made in this file rather than a dependency added in another.
//!   3. **Nothing at layer 2 or below names either.** The models — core, documents, protocol,
//!      commands, the viewport model, the visual language — are specified to be testable with no
//!      window, no graphics device and no interface toolkit. A dependency is how that stops being
//!      true, and this is the check that costs nothing to run and everything to lose.

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

/// Crates that draw an interface. Naming any of them makes a crate *the* render crate.
const TOOLKIT: [&str; 8] = [
    "egui",
    "eframe",
    "egui_dock",
    "egui-wgpu",
    "egui-winit",
    "egui_extras",
    "winit",
    "accesskit",
];

/// Crates that name a graphics device.
const GRAPHICS: [&str; 6] = ["wgpu", "wgpu-hal", "wgpu-types", "wgpu-core", "ash", "naga"];

/// The one crate besides the render crate that may name a graphics API, and why.
///
/// It is the editor's platform module: dma-buf import, Vulkan timeline semaphores, `memfd` and
/// `SCM_RIGHTS`. It exists separately from the render crate because the headless probe and the
/// editor's window must exercise *the same* import and synchronisation code — a clean run of one
/// proves nothing about a path the other took instead.
const GRAPHICS_EXCEPTION: &str = "cy-editor-viewport-transport";

/// The highest layer that is specified to run with no window, no device and no toolkit.
const HEADLESS_THROUGH_LAYER: u32 = 2;

struct Member {
    name: String,
    layer: u32,
    dependencies: BTreeSet<String>,
}

fn crates_directory() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("the app crate is inside crates/")
        .to_path_buf()
}

/// Every workspace member's name, layer, and the names of everything it depends on.
///
/// Read as text rather than through `cargo metadata`, for the same reasons `layering.rs` gives: no
/// network, no lockfile, no build, and a shape this workspace does not write is a parse failure
/// rather than a silently skipped dependency.
fn members() -> Vec<Member> {
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

fn parse(text: &str, path: &Path) -> Member {
    let mut name = None;
    let mut layer = None;
    let mut dependencies = BTreeSet::new();
    let mut section = String::new();

    for line in text.lines() {
        let line = line.trim();
        if line.starts_with('[') && line.ends_with(']') {
            section = line.trim_matches(['[', ']']).to_string();
            continue;
        }
        let Some((key, _)) = line.split_once('=') else {
            continue;
        };
        let key = key.trim();
        match section.as_str() {
            "package" if key == "name" => {
                name = Some(
                    line.split_once('=')
                        .expect("a key and a value")
                        .1
                        .trim()
                        .trim_matches('"')
                        .to_string(),
                );
            }
            "package.metadata.cy" if key == "layer" => {
                layer = Some(
                    line.split_once('=')
                        .expect("a key and a value")
                        .1
                        .trim()
                        .parse()
                        .expect("a layer is a number"),
                );
            }
            // Ends with rather than equals, so that a platform-specific table —
            // `[target.'cfg(target_os = "linux")'.dependencies]` — is read like any other. A
            // containment rule that a `cfg` could step around would not be one.
            section if section.ends_with("dependencies") => {
                dependencies.insert(key.to_string());
            }
            _ => {}
        }
    }

    Member {
        name: name.unwrap_or_else(|| panic!("{} has no package name", path.display())),
        layer: layer.unwrap_or_else(|| panic!("{} declares no layer", path.display())),
        dependencies,
    }
}

fn names_any(member: &Member, set: &[&str]) -> Vec<String> {
    set.iter()
        .filter(|candidate| member.dependencies.contains(**candidate))
        .map(|candidate| (*candidate).to_string())
        .collect()
}

#[test]
fn at_most_one_crate_names_an_interface_toolkit() {
    let members = members();
    let render_crates: Vec<&Member> = members
        .iter()
        .filter(|member| !names_any(member, &TOOLKIT).is_empty())
        .collect();

    assert!(
        render_crates.len() <= 1,
        "more than one crate names an interface toolkit:\n  {}\n\nThe toolkit is an implementation \
         detail (`editor-rust-application`), and it stays one only while exactly one crate can see \
         it. A second is how 'swap the toolkit by changing one render crate' stops being true.",
        render_crates
            .iter()
            .map(|member| format!(
                "{} — {}",
                member.name,
                names_any(member, &TOOLKIT).join(" ")
            ))
            .collect::<Vec<_>>()
            .join("\n  ")
    );
}

#[test]
fn only_the_render_crate_and_the_transport_name_a_graphics_api() {
    let members = members();
    // The render crate is whichever one names a toolkit, derived rather than hard-coded so that
    // this test is correct before it exists and after it is renamed.
    let render_crate = members
        .iter()
        .find(|member| !names_any(member, &TOOLKIT).is_empty())
        .map(|member| member.name.clone());

    let mut offenders = Vec::new();
    for member in &members {
        let graphics = names_any(member, &GRAPHICS);
        if graphics.is_empty() {
            continue;
        }
        if member.name == GRAPHICS_EXCEPTION || Some(&member.name) == render_crate.as_ref() {
            continue;
        }
        offenders.push(format!("{} — {}", member.name, graphics.join(" ")));
    }
    assert!(
        offenders.is_empty(),
        "these crates name a graphics API and are neither the render crate nor the viewport \
         transport:\n  {}\n\nThe render crate draws; {GRAPHICS_EXCEPTION} imports the runtime's \
         image. Everything else in this workspace is specified to work with no graphics device at \
         all, and a dependency is how that stops being true.",
        offenders.join("\n  ")
    );
}

#[test]
fn nothing_headless_names_a_toolkit_or_a_device() {
    // The erosion this whole file exists for, at the layer where it is most tempting:
    // `cy-editor-visual` is layer 1 and is about colour, and egui has a colour type.
    let mut offenders = Vec::new();
    for member in members() {
        if member.layer > HEADLESS_THROUGH_LAYER {
            continue;
        }
        let named: Vec<String> = names_any(&member, &TOOLKIT)
            .into_iter()
            .chain(names_any(&member, &GRAPHICS))
            .collect();
        if !named.is_empty() {
            offenders.push(format!(
                "{} (layer {}) — {}",
                member.name,
                member.layer,
                named.join(" ")
            ));
        }
    }
    assert!(
        offenders.is_empty(),
        "these crates are specified to run with no window, no graphics device and no interface \
         toolkit, and now depend on one:\n  {}\n\nConvert at the boundary instead: the render crate \
         may name both, and it is the only place that should.",
        offenders.join("\n  ")
    );
}

#[test]
fn the_transport_draws_nothing() {
    // The exception granted above is for importing an image, not for drawing one. A transport that
    // acquired a toolkit dependency would have quietly become a second render crate.
    let members = members();
    let Some(transport) = members
        .iter()
        .find(|member| member.name == GRAPHICS_EXCEPTION)
    else {
        return; // Not in this checkout; nothing to check and nothing broken.
    };
    let toolkit = names_any(transport, &TOOLKIT);
    assert!(
        toolkit.is_empty(),
        "{GRAPHICS_EXCEPTION} names {}. It may import the runtime's image; drawing is the render \
         crate's, and a transport that draws is a second renderer — which \
         `editor-viewport-and-gizmos` forbids outright.",
        toolkit.join(" ")
    );
}

#[test]
fn the_exception_list_is_as_short_as_it_claims_to_be() {
    // The same shape as `safety.rs`'s audit test: widening the set of crates that may see a
    // graphics API should be a decision recorded here, not an edit somewhere else. One exception,
    // with its reason in the constant's documentation.
    assert_eq!(
        [GRAPHICS_EXCEPTION].len(),
        1,
        "the set of crates that may name a graphics API without drawing has changed"
    );
}
