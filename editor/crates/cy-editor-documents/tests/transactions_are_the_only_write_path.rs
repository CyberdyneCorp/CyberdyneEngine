//! Task 3.9, from outside the crate: there is no write path to write around.
//!
//! The unit test `audit::tests::the_test_that_writes_around_the_system` is the *inside* half — it
//! uses a bypass that exists only in this crate's own test build, and proves the audit catches it.
//! This file is the outside half, and it checks something the inside cannot: that no such bypass
//! exists in the API a panel, a gizmo, an importer or a plugin can reach.
//!
//! Two checks, and neither is a substitute for the other. The first is a **source scan** over every
//! file in this crate: every public mutating method on `DocumentContent` must take a `WriteToken`.
//! The second is the property the scan is a proxy for, exercised — a document driven through its
//! real API, audited, and clean.
//!
//! A scan is a blunt instrument and being blunt is a virtue here: a new `pub fn foo(&mut self)` on
//! `DocumentContent` fails it, and having to add the token to make the test pass is exactly the
//! outcome wanted.
//!
//! IT SCANS THE WHOLE CRATE, AND THAT IS A FIX. It read `src/content.rs` alone until M5.5's gate
//! put the same `pub fn detach_roots(&mut self)` in `src/content/escape.rs` — a CHILD MODULE of
//! `content`, which Rust's privacy rules let touch every private field of `DocumentContent`. It
//! compiled, all 52 tests passed, and this audit passed with them. One file was never the boundary;
//! the crate is.

use cy_editor_core::Actor;
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::Document;
use cy_editor_documents::audit::Audit;

/// `allocate_node` consumes an ordinal and creates nothing; `content.rs`'s own note says why it is
/// not a content mutation. Everything else on `DocumentContent` that takes `&mut self` and is public
/// must carry the token.
const EXEMPT: [&str; 1] = ["allocate_node"];

/// Every `.rs` file under `src`, recursively. A module added in a subdirectory is inside the
/// privacy boundary whether or not anyone remembered to widen a list.
fn rust_sources(directory: &std::path::Path) -> Vec<std::path::PathBuf> {
    let mut found = Vec::new();
    let entries = std::fs::read_dir(directory).expect("the crate's src directory is readable");
    for entry in entries {
        let path = entry.expect("a readable directory entry").path();
        if path.is_dir() {
            found.extend(rust_sources(&path));
        } else if path.extension().is_some_and(|extension| extension == "rs") {
            found.push(path);
        }
    }
    found.sort();
    found
}

/// One file's public mutators **on `DocumentContent`**, appended to `offenders` as
/// `<file>:<line>: pub fn <name>`.
///
/// Only inside an `impl DocumentContent` block: `Document`'s own API is a different question with a
/// different answer — `begin`, `commit`, `undo` and `save` are the transaction system rather than a
/// way around it, and flagging them would make this audit noise that gets suppressed.
fn scan(source: &str, shown: &str, offenders: &mut Vec<String>) {
    let lines: Vec<&str> = source.lines().collect();
    let mut depth = 0_i32;
    let mut inside = false;
    for (number, line) in lines.iter().enumerate() {
        let trimmed = line.trim();
        if !inside && trimmed.starts_with("impl") && trimmed.contains("DocumentContent") {
            inside = true;
            depth = 0;
        }
        if inside {
            depth += i32::try_from(line.matches('{').count()).unwrap_or(0);
            depth -= i32::try_from(line.matches('}').count()).unwrap_or(0);
        }
        if !inside {
            continue;
        }
        if !trimmed.starts_with("pub fn ") && !trimmed.starts_with("pub const fn ") {
            if depth <= 0 && trimmed.contains('}') {
                inside = false;
            }
            continue;
        }
        if !trimmed.contains("&mut self") {
            continue;
        }
        let name = trimmed
            .split("fn ")
            .nth(1)
            .and_then(|rest| rest.split(['(', '<']).next())
            .unwrap_or_default();
        if EXEMPT.contains(&name) {
            continue;
        }
        // A signature may wrap; look at the declaration and the two lines after it.
        let declaration = lines[number..(number + 3).min(lines.len())].join(" ");
        if !declaration.contains("WriteToken") {
            offenders.push(format!("  {shown}:{}: pub fn {name}", number + 1));
        }
        if depth <= 0 {
            inside = false;
        }
    }
}

#[test]
fn document_content_has_no_public_mutator_that_does_not_take_a_write_token() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
    let sources = rust_sources(&root);
    let mut offenders = Vec::new();
    let mut scanned = 0_usize;
    for path in sources {
        let source = std::fs::read_to_string(&path).expect("a source file is readable");
        if !source.contains("impl DocumentContent") {
            continue;
        }
        scanned += 1;
        let shown = path
            .strip_prefix(&root)
            .unwrap_or(&path)
            .display()
            .to_string();
        scan(&source, &shown, &mut offenders);
    }
    assert!(
        scanned > 0,
        "no file under {} carries `impl DocumentContent`; the scan would pass vacuously",
        root.display()
    );
    assert!(
        offenders.is_empty(),
        "DocumentContent has a public mutating method that does not require a WriteToken:\n{}\n\n\
         Transactions are the only path for persistent mutation. A direct write path does not \
         merely bypass undo; it makes autosave, crash recovery, semantic diff, merge and live \
         editing silently incomplete for whatever it touches, and nothing will point at it.\n\
         Take a &WriteToken, or add the method to EXEMPT in this test with the reason it changes \
         no content.",
        offenders.join("\n")
    );
}

#[test]
fn a_real_session_audits_clean_through_the_public_api_alone() {
    let mut document = Document::new("worlds/city.cyworld");
    let transform = document.schema_mut().declare_type("Transform", false);
    let position = document
        .schema_mut()
        .declare_field(transform, "position", ValueKind::Vec3, "where the node is")
        .unwrap();

    let actor = Actor::human("designer");
    let node = document
        .with_transaction("Create lamp", actor.clone(), |document| {
            let node = document.create_node(None)?;
            document.add_component(node, transform, vec![(position, Value::Vec3([0.0; 3]))])?;
            Ok(node)
        })
        .unwrap();

    // A drag: many intermediate values, one entry.
    document.begin_interaction("Move lamp", actor.clone(), "drag-1");
    for step in 1..=50_u8 {
        document
            .set_field(
                node,
                transform,
                position,
                Value::Vec3([f32::from(step), 0.0, 0.0]),
            )
            .unwrap();
    }
    document.commit().unwrap();

    // An abandoned edit.
    document.begin("Move lamp", actor.clone());
    document
        .set_field(node, transform, position, Value::Vec3([99.0, 0.0, 0.0]))
        .unwrap();
    document.cancel().unwrap();

    // Undo, redo, delete, undo.
    document.undo().unwrap();
    document.redo().unwrap();
    document
        .with_transaction("Delete lamp", actor, |document| document.delete_node(node))
        .unwrap();
    document.undo().unwrap();

    let audit = Audit::of(&document);
    assert!(audit.is_clean(), "{audit:?}");
    assert!(
        audit.writes > 50,
        "a session that touched nothing would audit clean vacuously"
    );
    audit.verify().unwrap();
}
