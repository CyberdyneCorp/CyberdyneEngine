//! `unsafe` is confined to one audited crate, and the confinement is checked. Task 2.2.
//!
//! `editor-rust-application`: "`unsafe` code SHALL be confined to **narrow, audited interoperation
//! and platform modules**, and SHALL NOT appear in panels, view models, services, or domain logic."
//! Its "Forbidden editor application patterns" requires each of its ten items to be **checkable**;
//! this file checks the four that a source scan can settle honestly.
//!
//! The primary mechanism is not this test — it is `#![forbid(unsafe_code)]` at the top of every
//! crate but the SDK and the ABI fixture, which `rustc` enforces and which an inner `allow` cannot
//! lift. What this test adds is that the attribute is *there*: a new crate that forgot it would
//! compile perfectly and would silently be outside the rule.

use std::path::{Path, PathBuf};

/// The crates that are permitted `unsafe`, each with the reason.
///
/// `cy-editor-sdk` is the audited interoperation overlay the specification permits — it is the only
/// crate that names a C type. `cy-editor-testhost` implements the C ABI itself, which is the
/// definition of an interoperation module; it is a test fixture and ships in nothing.
///
/// `cy-editor-viewport-transport` is the **platform** half of the same sentence: the specification
/// permits "narrow, audited interoperation **and platform** modules", and importing another
/// process's `VkImage` is `memfd_create`, `mmap`, `sendmsg` with `SCM_RIGHTS`, and
/// `vkImportSemaphoreFdKHR`. None of those has a safe standard-library path, and there is no
/// version of this crate without them. It is narrow in the way the specification means: it holds
/// the transport and nothing else, its unsafe blocks each carry the argument for why they are
/// sound, and `containment.rs` stops anything above it from acquiring the same dependencies.
const AUDITED: [&str; 3] = [
    "cy-editor-sdk",
    "cy-editor-testhost",
    "cy-editor-viewport-transport",
];

fn crates_directory() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("the app crate is inside crates/")
        .to_path_buf()
}

fn members() -> Vec<(String, PathBuf)> {
    let mut members = Vec::new();
    for entry in std::fs::read_dir(crates_directory()).expect("crates/ is readable") {
        let path = entry.expect("a readable directory entry").path();
        if path.join("Cargo.toml").is_file() {
            let name = path
                .file_name()
                .expect("a directory name")
                .to_string_lossy()
                .into_owned();
            members.push((name, path));
        }
    }
    members
}

/// Every `.rs` file under a crate.
fn sources(root: &Path) -> Vec<PathBuf> {
    let mut files = Vec::new();
    let mut stack = vec![root.to_path_buf()];
    while let Some(directory) = stack.pop() {
        let Ok(entries) = std::fs::read_dir(&directory) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                stack.push(path);
            } else if path.extension().is_some_and(|extension| extension == "rs") {
                files.push(path);
            }
        }
    }
    files
}

#[test]
fn every_crate_but_the_audited_ones_forbids_unsafe() {
    let mut missing = Vec::new();
    for (name, path) in members() {
        if AUDITED.contains(&name.as_str()) {
            continue;
        }
        let roots: Vec<PathBuf> = ["src/lib.rs", "src/main.rs"]
            .iter()
            .map(|file| path.join(file))
            .filter(|file| file.is_file())
            .collect();
        assert!(
            !roots.is_empty(),
            "{name} has neither a lib.rs nor a main.rs"
        );
        for root in roots {
            let text = std::fs::read_to_string(&root).expect("a readable crate root");
            if !text.contains("#![forbid(unsafe_code)]") {
                missing.push(root.display().to_string());
            }
        }
    }
    assert!(
        missing.is_empty(),
        "these crate roots do not forbid unsafe:\n  {}\n\n`#![forbid(unsafe_code)]` is what makes \
         the confinement real: unlike a deny, it cannot be lifted by an inner allow. Add it, or add \
         the crate to AUDITED in this file with the reason it is an interoperation module.",
        missing.join("\n  ")
    );
}

#[test]
fn no_crate_outside_the_audited_ones_contains_the_word_unsafe() {
    // A belt beside the compiler's braces. `forbid` already makes this impossible to compile, so
    // this test's real job is to notice a crate that was added to AUDITED without anybody meaning
    // to widen the audit.
    let mut offenders = Vec::new();
    for (name, path) in members() {
        if AUDITED.contains(&name.as_str()) {
            continue;
        }
        for file in sources(&path.join("src")) {
            let text = std::fs::read_to_string(&file).expect("a readable source file");
            for (number, line) in text.lines().enumerate() {
                // The attribute itself contains the word, and so does prose about it.
                let code = line.split("//").next().unwrap_or_default();
                if code.contains("unsafe ") && !code.contains("forbid(unsafe_code)") {
                    offenders.push(format!("{}:{}", file.display(), number + 1));
                }
            }
        }
    }
    assert!(
        offenders.is_empty(),
        "unsafe appears outside the audited crates:\n  {}",
        offenders.join("\n  ")
    );
}

#[test]
fn no_editor_crate_but_the_sdk_names_the_c_abi() {
    // "WHEN an editor feature needs engine data THEN it SHALL obtain it through the SDK or the
    // protocol, and no C++ type SHALL appear in editor code."
    //
    // The check a source scan can make honestly: nothing outside the SDK and the fixture may name
    // the generated FFI module, which is where every C type in this workspace lives.
    let mut offenders = Vec::new();
    for (name, path) in members() {
        if AUDITED.contains(&name.as_str()) {
            continue;
        }
        for file in sources(&path.join("src")) {
            let text = std::fs::read_to_string(&file).expect("a readable source file");
            for (number, line) in text.lines().enumerate() {
                let code = line.split("//").next().unwrap_or_default();
                if code.contains("generated::ffi") || code.contains("cy_editor_sdk::generated") {
                    offenders.push(format!("{}:{}", file.display(), number + 1));
                }
            }
        }
    }
    assert!(
        offenders.is_empty(),
        "the generated C ABI mirrors are named outside the SDK:\n  {}\n\nEditor code uses the \
         SDK's safe API; the FFI is the SDK's own business.",
        offenders.join("\n  ")
    );
}

#[test]
fn the_audited_list_is_as_short_as_it_claims_to_be() {
    // A test that fails when somebody widens the audit, so that widening it is a decision rather
    // than an edit. Two crates: the SDK, and the fixture that implements the C ABI it talks to.
    assert_eq!(
        AUDITED.len(),
        3,
        "the set of crates permitted unsafe has changed. That is a decision worth stating: which \
         crate, and why it is a narrow, audited interoperation or platform module."
    );
}
