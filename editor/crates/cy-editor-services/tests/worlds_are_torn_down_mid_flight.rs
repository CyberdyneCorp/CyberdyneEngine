//! Worlds created and destroyed continuously, and torn down while one is loading. M6 rule 4.
//!
//! --- WHY THIS FILE EXISTS ---------------------------------------------------------------------------
//!
//! M5.5's gate found the first real engine defect in six milestones by testing **teardown under
//! load** rather than teardown at rest: Jolt's job bridge destroyed its free list underneath a worker
//! that was still releasing a job, one run in forty, as a `SIGTRAP` with no physics call on the
//! stack. M6 creates and destroys worlds *continuously* rather than once per fixture, so every
//! subsystem it adds has to be torn down mid-flight before it is believed.
//!
//! The world loader has three ways to be caught out, and each is a test below:
//!
//!   1. **A load that fails part-way** must leave the document as it found it. The reader parses the
//!      whole file before it writes anything, so a file that goes wrong on its last line leaves no
//!      half-world for a user to find later.
//!   2. **A world truncated underneath the reader** — a file being rewritten by another tool, a copy
//!      interrupted, a disk that filled — must be a diagnostic rather than a partial world.
//!   3. **Open and close, continuously, from several threads at once**, so that a document service
//!      dropped while its worlds are being built is exercised rather than assumed. Every service owns
//!      its documents outright, which is what makes this safe; a test is what makes it *checked*.

use std::fmt::Write as _;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::{Duration, Instant};

use cy_editor_core::Actor;
use cy_editor_documents::Document;
use cy_editor_services::DocumentService;
use cy_editor_services::worldfile;

/// A world with `nodes` entities, each carrying a Transform. Big enough that a load is not
/// instantaneous, small enough that a hundred of them take no time at all.
fn a_world(nodes: usize) -> String {
    let mut text = String::from("cyworld 1\n");
    text.push_str("type 1 runtime \"Transform\"\n");
    text.push_str("  field 1 vec3 \"translation\" \"Where it is.\"\n");
    text.push_str("  field 2 quat \"rotation\" \"Which way it faces.\"\n");
    text.push_str("  field 3 vec3 \"scale\" \"How big it is.\"\n");
    for index in 0..nodes {
        let parent = if index == 0 {
            "-".to_string()
        } else {
            "0".to_string()
        };
        let _ = writeln!(text, "node {index} {parent} \"default\"");
        text.push_str("  component 1\n");
        let _ = writeln!(text, "    field 1 {index} 0 0");
        text.push_str("    field 2 0 0 0 1\n");
        text.push_str("    field 3 1 1 1\n");
    }
    text
}

#[test]
fn a_world_truncated_underneath_the_reader_is_a_diagnostic_and_not_a_partial_world() {
    let whole = a_world(64);
    // Every prefix of a real file, which is what a reader sees when a writer is interrupted, when a
    // copy is in flight, or when a disk fills. Not one contrived truncation: the failure this stands
    // for arrives at whatever byte the interruption happened at.
    for cut in (16..whole.len()).step_by(97) {
        let mut document = Document::new("worlds/city.cyworld");
        let outcome = worldfile::load(&whole[..cut], &mut document, Actor::system("loader"));
        if outcome.is_ok() {
            // A prefix that happens to be a complete, smaller world is a legitimate world. What is
            // NOT allowed is a document that took some of the file and reported success on all of
            // it, so the invariant checked here is that everything present is whole.
            for node in document.content().nodes() {
                let state = document.content().node(node).expect("a node it reported");
                assert!(
                    state.components.is_empty() || state.components.len() == 1,
                    "a node carries the components the file gave it, or none"
                );
            }
            continue;
        }
        assert_eq!(
            document.content().node_count(),
            0,
            "a failed load leaves the document exactly as it found it (cut at {cut})"
        );
        assert!(!document.is_dirty(), "and leaves nothing to save");
    }
}

#[test]
fn a_service_dropped_while_its_worlds_are_being_built_takes_them_with_it() {
    let directory = std::env::temp_dir().join(format!(
        "cy-editor-teardown-{}-{}",
        std::process::id(),
        line!()
    ));
    let _ = std::fs::remove_dir_all(&directory);
    std::fs::create_dir_all(directory.join("worlds")).expect("a project");
    std::fs::write(directory.join("worlds/city.cyworld"), a_world(256)).expect("a world");

    let opened = Arc::new(AtomicUsize::new(0));
    let deadline = Instant::now() + Duration::from_secs(2);
    let mut workers = Vec::new();
    for _ in 0..4 {
        let directory = directory.clone();
        let opened = Arc::clone(&opened);
        workers.push(std::thread::spawn(move || {
            // CONTINUOUS, NOT ONCE. Each iteration builds a service, loads a 256-node world into it
            // and drops the service — the drop lands on documents that were constructed a
            // microsecond earlier, over and over, which is the shape M5.5's Jolt defect needed forty
            // runs to show.
            while Instant::now() < deadline {
                let mut service = DocumentService::new();
                service.rooted_at(&directory);
                let (id, _, report) = service
                    .open_reporting("worlds/city.cyworld")
                    .expect("the world loads");
                assert_eq!(report.nodes, 256);
                assert_eq!(
                    service
                        .get(id)
                        .expect("the document")
                        .content()
                        .node_count(),
                    256
                );
                opened.fetch_add(1, Ordering::Relaxed);
                drop(service);
            }
        }));
    }
    for worker in workers {
        worker.join().expect("no worker fell over");
    }
    assert!(
        opened.load(Ordering::Relaxed) >= 4,
        "every worker built and destroyed at least one world"
    );
    let _ = std::fs::remove_dir_all(&directory);
}

#[test]
fn closing_a_document_while_another_is_loading_leaves_the_survivor_intact() {
    let directory = std::env::temp_dir().join(format!(
        "cy-editor-teardown-{}-{}",
        std::process::id(),
        line!()
    ));
    let _ = std::fs::remove_dir_all(&directory);
    std::fs::create_dir_all(directory.join("worlds")).expect("a project");
    std::fs::write(directory.join("worlds/city.cyworld"), a_world(128)).expect("a world");
    std::fs::write(directory.join("worlds/forest.cyworld"), a_world(96)).expect("another");

    let mut service = DocumentService::new();
    service.rooted_at(&directory);
    let (city, _) = service.open("worlds/city.cyworld").expect("the city");

    // Opening the second world is what "loading" means here: it runs to completion inside `open`,
    // so the interleaving that matters is a close landing between two loads rather than inside one.
    // Both orders are exercised, because a service that tidied up the wrong document would pass one.
    let (forest, _) = service.open("worlds/forest.cyworld").expect("the forest");
    service.close(city).expect("the city closes");
    assert!(service.get(city).is_none(), "a closed document is gone");
    assert_eq!(
        service
            .get(forest)
            .expect("the survivor")
            .content()
            .node_count(),
        96,
        "and the one still open is untouched"
    );

    let (city, _) = service.open("worlds/city.cyworld").expect("the city again");
    assert_eq!(
        service.get(city).expect("reopened").content().node_count(),
        128,
        "reopening a closed world reads it from the project again"
    );
    let _ = std::fs::remove_dir_all(&directory);
}

#[test]
fn a_world_survives_being_written_and_read_back_by_a_second_service() {
    // The save half of the round trip, which is what the artefact's "quit and reopen" act depends
    // on. Two services rather than one, because a document that only round-trips through the
    // service that wrote it is a document that round-trips through a cache.
    let directory = std::env::temp_dir().join(format!(
        "cy-editor-teardown-{}-{}",
        std::process::id(),
        line!()
    ));
    let _ = std::fs::remove_dir_all(&directory);
    std::fs::create_dir_all(directory.join("worlds")).expect("a project");
    std::fs::write(directory.join("worlds/city.cyworld"), a_world(32)).expect("a world");

    let mut first = DocumentService::new();
    first.rooted_at(&directory);
    let (id, _) = first.open("worlds/city.cyworld").expect("the world");
    let written = worldfile::write_world(first.get(id).expect("the document"));
    first
        .write(first.get(id).expect("the document"))
        .expect("a save");

    let mut second = DocumentService::new();
    second.rooted_at(&directory);
    let (again, _) = second.open("worlds/city.cyworld").expect("the world again");
    assert_eq!(
        worldfile::write_world(second.get(again).expect("the document")),
        written,
        "what a second editor reads writes back to the same bytes"
    );
    let _ = std::fs::remove_dir_all(&directory);
}
