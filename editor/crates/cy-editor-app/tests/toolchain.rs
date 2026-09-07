//! The minimum supported Rust version is written in two files, and they must agree. Task 1.0.4.
//!
//! It moved from 1.92 to 1.95 at M5.5, because the interface toolkit chosen there — egui 0.36 over
//! wgpu 30 — refuses to build on 1.92.
//!
//! --- WHY TWO FILES, AND WHY A TEST BETWEEN THEM -----------------------------------------------------
//!
//! They do different jobs and neither can do the other's:
//!
//!   * `editor/Cargo.toml`'s `rust-version` makes Cargo REFUSE an older toolchain, with a sentence
//!     naming the crate and the version. Without it, the failure is a compiler error inside a
//!     dependency, which is a much worse thing to hand somebody.
//!   * `rust-toolchain.toml` makes rustup SELECT the right one, so that a developer whose default is
//!     older gets a build rather than a refusal. This machine's default is 1.92, which is exactly
//!     the case that would otherwise be discovered late.
//!
//! A version in two files is a version that will disagree with itself, and the disagreement is
//! silent: Cargo would accept a toolchain rustup no longer selects, or refuse one it does. That is
//! what this file is for. Continuous integration is not a third copy — `just build-editor-toolchain`
//! reads `rust-toolchain.toml`, so the workflow names no version at all.

use std::path::{Path, PathBuf};

fn editor_directory() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(Path::parent)
        .expect("the app crate is inside editor/crates/")
        .to_path_buf()
}

fn repository_root() -> PathBuf {
    editor_directory()
        .parent()
        .expect("editor/ is inside the repository")
        .to_path_buf()
}

/// The value of the first `key = "value"` line in a file.
fn declared(path: &Path, key: &str) -> String {
    let text = std::fs::read_to_string(path)
        .unwrap_or_else(|error| panic!("{} is readable: {error}", path.display()));
    let Some((_, value)) = text
        .lines()
        .filter_map(|line| line.split_once('='))
        .find(|(name, _)| name.trim() == key)
    else {
        panic!("{} declares no {key}", path.display());
    };
    value.trim().trim_matches('"').to_string()
}

#[test]
fn the_declared_version_and_the_selected_toolchain_agree() {
    let declared_msrv = declared(&editor_directory().join("Cargo.toml"), "rust-version");
    let selected = declared(&repository_root().join("rust-toolchain.toml"), "channel");

    assert!(
        selected.starts_with(&declared_msrv),
        "editor/Cargo.toml declares rust-version {declared_msrv:?} and rust-toolchain.toml selects \
         {selected:?}.\n\nOne refuses an older toolchain and the other chooses which toolchain is \
         used; when they disagree, the build either refuses what it selected or accepts what it \
         does not. Change both, or neither."
    );
}

#[test]
fn the_minimum_is_the_one_the_toolkit_needs() {
    // A test that pins a number, so that lowering it is a decision. egui 0.36 does not build on
    // 1.92, and the editor's whole interface layer is downstream of that.
    let declared_msrv = declared(&editor_directory().join("Cargo.toml"), "rust-version");
    let (major, minor) = declared_msrv
        .split_once('.')
        .expect("a version is major.minor");
    let minor: u32 = minor
        .split('.')
        .next()
        .expect("a minor version")
        .parse()
        .expect("a number");
    assert_eq!(major, "1");
    assert!(
        minor >= 95,
        "the minimum supported Rust version is {declared_msrv}, and egui 0.36 — the toolkit M5.5 \
         chose and measured — refuses to build below 1.95"
    );
}

#[test]
fn continuous_integration_reads_the_version_rather_than_repeating_it() {
    // The third copy that is not there. A workflow that pinned a version of its own would be the
    // one place a stale number survives, because nothing local runs it.
    let workflow = repository_root().join(".github/workflows/ci.yml");
    let Ok(text) = std::fs::read_to_string(&workflow) else {
        return; // Not in this checkout; nothing to check.
    };
    let offenders: Vec<String> = text
        .lines()
        .filter(|line| line.contains("rustup") && line.contains("1."))
        .map(|line| line.trim().to_string())
        .collect();
    assert!(
        offenders.is_empty(),
        "the workflow names a Rust version of its own:\n  {}\n\nIt should call \
         `just build-editor-toolchain`, which reads rust-toolchain.toml, so that there is one \
         number in one place.",
        offenders.join("\n  ")
    );
}
