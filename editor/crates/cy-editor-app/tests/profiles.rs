//! The four profiles mean the same thing in Cargo that they mean in CMake. Task 2.1.
//!
//! M0's spike wrote the Cargo column of `justfile`'s profile table and reserved it unused for five
//! milestones. This is where it is spent, and this test is what stops the two tables from drifting.
//!
//! It reads `justfile` — the same text the build recipes read — and asserts that every row's Cargo
//! profile exists in `editor/Cargo.toml` with the assertion setting the row claims. That is the same
//! arrangement `cmake/profiles.cmake` has for the CMake column: the table is checked rather than
//! merely documented, so a row edited in one place and not the other is a red test rather than a
//! surprise three milestones later.

use std::path::{Path, PathBuf};

fn repository() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .ancestors()
        .nth(3)
        .expect("the app crate is three directories below the repository root")
        .to_path_buf()
}

/// One row of `justfile`'s profile table.
#[derive(Debug)]
struct Row {
    profile: String,
    assertions: bool,
    cargo: String,
}

/// The rows, read out of `justfile` itself.
fn rows() -> Vec<Row> {
    let text =
        std::fs::read_to_string(repository().join("justfile")).expect("justfile is readable");
    let table = text
        .split_once("profile_table := '")
        .expect("justfile declares profile_table")
        .1
        .split_once('\'')
        .expect("profile_table is a quoted block")
        .0;

    let rows: Vec<Row> = table
        .lines()
        .filter(|line| line.contains('|'))
        .map(|line| {
            let columns: Vec<&str> = line.split('|').map(str::trim).collect();
            assert!(columns.len() >= 5, "a profile row has five columns: {line}");
            Row {
                profile: columns[0].to_string(),
                assertions: match columns[2] {
                    "on" => true,
                    "off" => false,
                    other => panic!("the assertions column is on or off, not {other:?}"),
                },
                cargo: columns[4].to_string(),
            }
        })
        .collect();
    assert_eq!(rows.len(), 4, "the workflow has four profiles");
    rows
}

/// The `[profile.<name>]` sections of `editor/Cargo.toml`, with the settings this test checks.
fn cargo_profiles() -> Vec<(String, Option<bool>)> {
    let text = std::fs::read_to_string(repository().join("editor/Cargo.toml"))
        .expect("the workspace manifest is readable");
    let mut profiles = Vec::new();
    let mut current: Option<(String, Option<bool>)> = None;

    for line in text.lines() {
        let line = line.trim();
        if let Some(name) = line
            .strip_prefix("[profile.")
            .and_then(|rest| rest.strip_suffix(']'))
        {
            if let Some(finished) = current.take() {
                profiles.push(finished);
            }
            current = Some((name.to_string(), None));
            continue;
        }
        if line.starts_with('[') {
            if let Some(finished) = current.take() {
                profiles.push(finished);
            }
            continue;
        }
        if let Some((_, value)) = line.split_once("debug-assertions =")
            && let Some(entry) = current.as_mut()
        {
            entry.1 = Some(value.trim() == "true");
        }
    }
    if let Some(finished) = current {
        profiles.push(finished);
    }
    profiles
}

#[test]
fn every_workflow_profile_has_a_cargo_profile() {
    let declared = cargo_profiles();
    for row in rows() {
        assert!(
            declared.iter().any(|(name, _)| *name == row.cargo),
            "the workflow's `{}` profile maps to the Cargo profile `{}`, which editor/Cargo.toml \
             does not declare.\n  Every row of justfile's profile table must exist here, or \
             `just build-editor --profile {}` builds something nobody chose.",
            row.profile,
            row.cargo,
            row.profile
        );
    }
}

#[test]
fn assertions_mean_the_same_thing_in_both_toolchains() {
    // `CY_ASSERT` in C++ and `debug_assertions` in Rust. The two configurations that compile
    // `CY_ASSERT` out are the two that must set `debug-assertions = false`, or "build and test in
    // more than the dev profile" would be testing a different thing in each language.
    let declared = cargo_profiles();
    for row in rows() {
        let (_, assertions) = declared
            .iter()
            .find(|(name, _)| *name == row.cargo)
            .unwrap_or_else(|| panic!("the Cargo profile {} is declared", row.cargo));
        let assertions = assertions.unwrap_or_else(|| {
            panic!(
                "the Cargo profile `{}` does not state debug-assertions.\n  It is the Rust half of \
                 the assertions column and must be stated rather than inherited, so that reading \
                 the manifest answers the question.",
                row.cargo
            )
        });
        assert_eq!(
            assertions,
            row.assertions,
            "the workflow's `{}` profile has assertions {} and the Cargo profile `{}` sets \
             debug-assertions = {assertions}",
            row.profile,
            if row.assertions { "on" } else { "off" },
            row.cargo
        );
    }
}

#[test]
fn this_binary_was_built_with_the_assertions_its_profile_declares() {
    // The check that cannot be made by reading a file: what the compiler actually did. Run under
    // every profile, this is what proves the mapping is in force rather than merely written down.
    let expected = option_env!("CY_EDITOR_PROFILE");
    let Some(expected) = expected else {
        // Built without the recipe naming a profile — a plain `cargo test`. Nothing to check
        // against, and saying so is better than inventing an expectation.
        return;
    };
    let row = rows()
        .into_iter()
        .find(|row| row.profile == expected)
        .unwrap_or_else(|| panic!("CY_EDITOR_PROFILE names {expected}, which is not a profile"));
    assert_eq!(
        cfg!(debug_assertions),
        row.assertions,
        "built under the workflow profile `{expected}`, which declares assertions {}, but \
         debug_assertions is {}",
        if row.assertions { "on" } else { "off" },
        cfg!(debug_assertions)
    );
}
