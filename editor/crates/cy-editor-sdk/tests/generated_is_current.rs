//! The SDK and the ABI cannot drift. Task 2.3.
//!
//! `editor-rust-application`: "The SDK SHALL be **generated or maintained against the ABI
//! description**, so that the two cannot drift ... WHEN the ABI changes THEN the SDK SHALL be
//! regenerated or updated and the mismatch SHALL be a build failure."
//!
//! This is that failure. It runs the real generator in `--check` mode, which builds the file set
//! through the same code path a write uses and compares it against what is committed — the property
//! `tools/gen/README.md` requires of every generator here, so that a currency check cannot disagree
//! with the generator it checks.
//!
//! It is a test rather than a `build.rs` deliberately. A build script would run Python on every
//! build of every crate that depends on the SDK, would fail a checkout that has no Python for a
//! reason unrelated to what was being built, and would make the generated files a build artefact
//! rather than a reviewed one. `native-abi` wants the diff reviewable; a test gives that, and
//! `cargo test` is what CI runs anyway.

use std::path::{Path, PathBuf};
use std::process::Command;

/// The repository root, found by walking up from this crate.
fn repository() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .ancestors()
        .nth(3)
        .expect("the SDK crate is three directories below the repository root")
        .to_path_buf()
}

#[test]
fn the_committed_bindings_are_what_regeneration_produces() {
    let root = repository();
    let generator = root.join("tools/gen/rust/sdk_gen.py");
    assert!(
        generator.is_file(),
        "the generator is missing at {}",
        generator.display()
    );

    let output = Command::new("python3")
        .arg(&generator)
        .arg("--check")
        .current_dir(&root)
        .output()
        .expect("python3 is required to build this repository and is on PATH");

    assert!(
        output.status.success(),
        "the committed Rust SDK bindings are stale.\n\
         Regenerate them with `just build-editor --generate` and commit the result.\n\n\
         --- generator output ---\n{}{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr),
    );
}

#[test]
fn the_generator_is_deterministic() {
    // `build-system-and-platforms` requires generation to be deterministic — identical inputs,
    // byte-identical outputs. Checked by running the check twice: a generator that stamped a time
    // or a path would pass once and fail the second time, which is exactly how the property is
    // lost without anyone noticing.
    let root = repository();
    let generator = root.join("tools/gen/rust/sdk_gen.py");
    for attempt in 0..2 {
        let status = Command::new("python3")
            .arg(&generator)
            .arg("--check")
            .arg("--quiet")
            .current_dir(&root)
            .status()
            .expect("python3 is on PATH");
        assert!(
            status.success(),
            "regeneration disagreed with itself on attempt {attempt}"
        );
    }
}
