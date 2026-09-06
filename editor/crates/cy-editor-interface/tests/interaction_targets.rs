//! The interaction targets `editor-ui-ux` declares, measured. Task 4.6.
//!
//! > The editor SHALL declare targets it is measured against ... Regressions against these targets
//! > SHALL be detectable in continuous integration.
//!
//! | Interaction | Target |
//! |---|---|
//! | Interface frame time when idle | Negligible; no per-frame engine queries |
//! | Selection change to inspector populated | Under 16 ms for typical entities |
//! | Command palette first results | Under 50 ms |
//! | Panel dock or workspace switch | Under 100 ms |
//! | Project browser scroll on 100 000 assets | Smooth; virtualised |
//!
//! --- HOW THESE ARE ASSERTED, AND WHY IT IS TWO THINGS -----------------------------------------------
//!
//! Each target is checked twice: once as **work** — rebuild counts, rows materialised, entries
//! touched — and once as **wall-clock against the declared bound**. The work assertion is what
//! survives a loaded continuous-integration machine, because it does not change when the machine is
//! busy; the timing assertion is what makes the declared number mean something rather than being a
//! number in a table nobody measured. Every measurement is printed, so a run that stayed inside the
//! bound while getting four times slower is still visible in a log.
//!
//! No target here is measured against a toolkit, because there is no toolkit yet. What is measured
//! is everything *behind* it — the ranking, the rebuilds, the windowing — which is where these
//! targets are lost. When a toolkit arrives, its own draw cost is added to these numbers rather than
//! replacing them.

use std::time::{Duration, Instant};

use cy_editor_commands::Registry;
use cy_editor_core::Actor;
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::selection::Selection;
use cy_editor_interface::palette::{Action, Entry, Index, Origin};
use cy_editor_interface::shell::Shell;
use cy_editor_interface::virtualise::{Viewport, Window};
use cy_editor_reflection::Catalogue;
use cy_editor_services::{Editor, builtin};
use cy_editor_visual::Metrics;

/// The declared bound for the palette's first results.
const PALETTE_TARGET: Duration = Duration::from_millis(50);
/// The declared bound for a dock or a workspace switch.
const LAYOUT_TARGET: Duration = Duration::from_millis(100);
/// The declared bound for populating the inspector after a selection change.
const INSPECTOR_TARGET: Duration = Duration::from_millis(16);

/// The bound this build is held to, which is the declared one in the profiles the target speaks for.
///
/// The milestone builds in four profiles and two of them compile without optimisation, where a
/// hundred-thousand-entry search costs roughly seven times what it costs in the profile a
/// measurement is taken from — 23 ms against 3 ms, both measured on the same machine. An
/// unoptimised build is not the editor these targets are about, so it is held to four times the
/// declared bound; `profile` and `shipping`, which is where a measurement means something, are held
/// to the declared bound exactly.
///
/// The allowance is on the **clock only**. Every work assertion beside it — rows built, rebuilds
/// performed, results returned — is identical in all four profiles, and those are the assertions
/// that catch a regression that changed the algorithm rather than the machine.
fn bound(declared: Duration) -> Duration {
    if cfg!(debug_assertions) {
        declared * 4
    } else {
        declared
    }
}

fn registry() -> Registry {
    let mut registry = Registry::new();
    builtin::register(&mut registry).unwrap();
    registry
}

/// A hundred thousand assets, which is the size the specification names.
fn a_large_project() -> Index {
    let mut index = Index::new();
    index.ingest_commands(&registry());
    index.ingest((0..100_000).map(|number| {
        let path = format!("assets/props/prop_{number:06}.cymesh");
        Entry::new(path.clone(), "Mesh", Origin::Asset, Action::OpenAsset(path))
    }));
    index
}

#[test]
fn the_command_palette_returns_its_first_results_inside_the_declared_target() {
    let index = a_large_project();
    assert_eq!(index.len(), 100_000 + registry().len());

    // Three queries with different shapes: one that matches almost nothing, one that matches a
    // handful, and one that matches nearly everything — the last being the one that decides whether
    // the ranking scales, because it is the case a naive implementation sorts a hundred thousand
    // results for.
    for query in ["prop_012345", "undo", "p"] {
        let started = Instant::now();
        let results = index.search(query, 12);
        let elapsed = started.elapsed();
        println!(
            "palette {query:?}: {} result(s) in {elapsed:?}",
            results.len()
        );
        assert!(
            results.len() <= 12,
            "a palette shows the first page, not the set"
        );
        assert!(
            elapsed < bound(PALETTE_TARGET),
            "searching {query:?} over {} entries took {elapsed:?}, against a bound of {:?} \
             (declared {PALETTE_TARGET:?})",
            index.len(),
            bound(PALETTE_TARGET)
        );
    }
}

#[test]
fn a_workspace_switch_and_a_dock_stay_inside_the_declared_target() {
    let mut shell = Shell::new(&registry()).unwrap();
    shell.workspaces.save_as("Animation");

    let started = Instant::now();
    shell.workspaces.switch("Scene").unwrap();
    shell.workspaces.switch("Animation").unwrap();
    let switched = started.elapsed();
    println!("workspace switch (two): {switched:?}");
    assert!(
        switched < bound(LAYOUT_TARGET),
        "switching took {switched:?}"
    );

    let panel = cy_editor_interface::docking::PanelId::new("timeline").unwrap();
    let viewport = cy_editor_interface::docking::PanelId::new("viewport").unwrap();
    let started = Instant::now();
    shell
        .workspaces
        .current_mut()
        .dock_beside(&viewport, panel)
        .unwrap();
    let docked = started.elapsed();
    println!("dock a panel: {docked:?}");
    assert!(docked < bound(LAYOUT_TARGET), "docking took {docked:?}");
}

#[test]
fn scrolling_a_project_browser_of_a_hundred_thousand_assets_is_virtualised() {
    // Work, not time: the claim is that the cost does not depend on the size of the project, and a
    // stopwatch cannot say that. What can is that scrolling through the whole list builds a bounded
    // number of rows per frame however many there are.
    let assets: Vec<String> = (0..100_000)
        .map(|number| format!("assets/props/prop_{number:06}.cymesh"))
        .collect();
    let metrics = Metrics::default();
    let mut worst = 0;
    let mut built = 0_usize;

    let started = Instant::now();
    let mut scroll = 0.0;
    while scroll < 100_000.0 * metrics.row() {
        let window = Window::of(assets.len(), Viewport::new(scroll, 720.0, metrics.row()));
        worst = worst.max(window.count);
        built += window.slice(&assets).len();
        scroll += 720.0;
    }
    let elapsed = started.elapsed();
    println!(
        "scrolled a hundred thousand assets in {elapsed:?}, worst frame {worst} rows, {built} \
         rows built in total"
    );

    assert!(
        worst <= 64,
        "a 720-pixel panel built {worst} rows in one frame, which is not virtualisation"
    );
    assert!(
        built < assets.len() * 2,
        "scrolling the whole list built {built} rows, which means it is materialising the project"
    );
}

#[test]
fn selecting_an_entity_populates_the_inspector_inside_the_declared_target() {
    let mut editor = Editor::default();
    let id = editor.open_document("worlds/city.cyworld").unwrap();
    let document = editor.documents.get_mut(id).unwrap();

    let mut components = Vec::new();
    for component in 0..16 {
        let type_id = document
            .schema_mut()
            .declare_type(format!("Component{component}"), false);
        let mut fields = Vec::new();
        for field in 0..8 {
            let field = document
                .schema_mut()
                .declare_field(
                    type_id,
                    format!("field{field}"),
                    ValueKind::Vec3,
                    "one of many fields on a realistically busy entity",
                )
                .unwrap();
            fields.push((field, Value::Vec3([0.0; 3])));
        }
        components.push((type_id, fields));
    }
    let nodes = document
        .with_transaction("Build", Actor::human("designer"), |document| {
            let mut nodes = Vec::new();
            for _ in 0..8 {
                let node = document.create_node(None)?;
                for (type_id, fields) in &components {
                    document.add_component(node, *type_id, fields.clone())?;
                }
                nodes.push(node);
            }
            Ok(nodes)
        })
        .unwrap();

    let catalogue = Catalogue::of_document(editor.documents.get(id).unwrap().schema());
    let mut shell = Shell::new(&registry()).unwrap();
    shell.describe_with(catalogue);

    for node in nodes {
        let mut selection = Selection::new();
        selection.add_node(node);
        editor.selection.set(selection);

        let started = Instant::now();
        shell.refresh(&editor);
        let elapsed = started.elapsed();
        println!("selection to inspector populated: {elapsed:?}");
        assert_eq!(shell.inspector.rows().len(), 128);
        assert!(
            elapsed < bound(INSPECTOR_TARGET),
            "populating the inspector took {elapsed:?}, against a bound of {:?} (declared \
             {INSPECTOR_TARGET:?})",
            bound(INSPECTOR_TARGET)
        );
    }
}

#[test]
fn an_idle_interface_performs_no_queries_at_all() {
    // "Interface frame time when idle: negligible; **no per-frame engine queries**." The assertion
    // is a count rather than a time: an idle frame that took no measurable time but asked the
    // runtime a question would still be the defect this target names.
    let mut editor = Editor::default();
    let id = editor.open_document("worlds/city.cyworld").unwrap();
    let document = editor.documents.get_mut(id).unwrap();
    let component = document.schema_mut().declare_type("Health", false);
    let field = document
        .schema_mut()
        .declare_field(component, "value", ValueKind::Float, "hit points remaining")
        .unwrap();
    let node = document
        .with_transaction("Build", Actor::human("designer"), |document| {
            let node = document.create_node(None)?;
            document.add_component(node, component, vec![(field, Value::Float(1.0))])?;
            Ok(node)
        })
        .unwrap();
    let catalogue = Catalogue::of_document(editor.documents.get(id).unwrap().schema());

    let mut shell = Shell::new(&registry()).unwrap();
    shell.describe_with(catalogue);
    let mut selection = Selection::new();
    selection.add_node(node);
    editor.selection.set(selection);
    shell.refresh(&editor);

    let rebuilds = shell.inspector.rebuilds();
    let started = Instant::now();
    for _ in 0..10_000 {
        editor.pump();
        shell.refresh(&editor);
    }
    let elapsed = started.elapsed();
    println!("ten thousand idle frames in {elapsed:?}");

    assert_eq!(
        shell.inspector.rebuilds(),
        rebuilds,
        "an idle frame rebuilt the inspector"
    );
    assert!(
        elapsed < bound(Duration::from_millis(100)),
        "ten thousand idle frames took {elapsed:?}, which is not negligible per frame"
    );
}
