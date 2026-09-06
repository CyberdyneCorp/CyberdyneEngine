//! The shell: everything the interface is, assembled — and the rules that hold across all of it.
//!
//! The panels are the modules beside this one. What this type adds is the three things that are
//! properties of the interface *as a whole* rather than of any panel:
//!
//! * **Spatial stability.** "Panel content SHALL adapt to what is selected; **panel position SHALL
//!   NOT**." [`Shell::refresh`] takes the editor and updates content; nothing in it can move a
//!   panel, because [`crate::docking::Layout`] has no method that takes a selection.
//!   [`the_selection_changes_content_and_never_position`] asserts the layout is byte-identical
//!   across a selection change, which is the strongest form that claim can take.
//! * **Ambient status.** The header and the footer state the condition **in words** as well as by
//!   colour, per `editor-visual-language`'s "Status is ambient" and `editor-ui-ux`'s accessibility
//!   rule.
//! * **The engine's vocabulary, everywhere.** [`Shell::labels`] is every piece of static interface
//!   text this crate can draw, and a test runs `cy_editor_visual::vocabulary::check_label` over all
//!   of it. That is what "vocabulary is identity" costs to keep: one list and one loop.
//!
//! --- ONE NOTE ON A WORD --------------------------------------------------------------------------
//!
//! The footer says **author**, not "actor", for who made a change. `cy_editor_core::Actor` is the
//! editor's type for provenance and it is a good name for a type; "Actor" in *interface text* is
//! another engine's word for a thing in a scene, and the vocabulary check flags it. Saying "author"
//! costs nothing and keeps the check trustworthy, which is worth more than the word.

use cy_editor_commands::Registry;
use cy_editor_core::problem::Result;
use cy_editor_documents::Document;
use cy_editor_reflection::Catalogue;
use cy_editor_services::{Editor, HostingMode};
use cy_editor_visual::chrome::{Chrome, Overlay, Region};
use cy_editor_visual::{Composition, Density, GizmoMode, Metrics, Scale, Semantic, Theme};

use crate::docking::{PanelId, Workspaces};
use crate::inspector::GeneratedInspector;
use crate::keymap::Keymap;
use crate::notifications::NotificationCentre;
use crate::palette::{Index, Origin};
use crate::problems::Problems;
use crate::progress::ProgressSurface;

/// The title a built-in panel shows, in the engine's own vocabulary.
///
/// A table rather than a derivation from the identifier, because the identifier is a stable key and
/// the title is text a user reads — and the two have different lifetimes. An identifier with no title
/// falls back to itself, which is what a plugin's panel does until it supplies one.
#[must_use]
pub fn panel_title(panel: &PanelId) -> &str {
    match panel.as_str() {
        "hierarchy" => "Hierarchy",
        "content-browser" => "Content Browser",
        "viewport" => "Viewport",
        "inspector" => "Inspector",
        "script-graph" => "Script Graph",
        "animation" => "Animation",
        "console" => "Console",
        "profiler" => "Profiler",
        "problems" => "Problems",
        other => other,
    }
}

/// One forbidden interface pattern and where it is caught.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Forbidden {
    /// The pattern, in `editor-ui-ux`'s own words.
    pub pattern: &'static str,
    /// What settles it: a function, a type with no such field, or a named test.
    pub check: &'static str,
}

/// `editor-ui-ux`'s eight forbidden interface patterns, each with the check that catches it.
///
/// "The following SHALL NOT appear, and **each SHALL be checkable**." Some are refused by a type
/// having no field to express them, which is a stronger guarantee than a lint; the rest are named
/// tests. A ninth pattern added to the specification cannot be recorded here without somebody
/// deciding how it is caught, which is what [`every_forbidden_pattern_names_a_check`] enforces.
pub const FORBIDDEN: [Forbidden; 8] = [
    Forbidden {
        pattern: "An action reachable only from one widget with no command registration",
        check: "palette::every_registered_command_is_reachable_from_the_palette, over the registry",
    },
    Forbidden {
        pattern: "A non-virtualised list, tree, or table over unbounded data",
        check: "virtualise::Window is the only way to slice a list; asserted over 100 000 assets",
    },
    Forbidden {
        pattern: "A modal dialog used for information that could be a notification",
        check: "notifications::Modal::decision refuses fewer than two choices",
    },
    Forbidden {
        pattern: "A per-frame transaction produced by a continuous drag",
        check: "inspector::begin_drag opens one interactive transaction; there is no per-frame commit",
    },
    Forbidden {
        pattern: "Colour as the only encoding of state",
        check: "cy_editor_visual::colour::Semantic carries a glyph and a label beside every colour",
    },
    Forbidden {
        pattern: "A message that states a failure without stating a cause or a remedy",
        check: "problems::Problems::report refuses a Problem with no remedy or no reason",
    },
    Forbidden {
        pattern: "A document dirtied by presentation-only state such as expansion or scroll position",
        check: "inspector::expanding_a_section_is_presentation_state_and_dirties_nothing",
    },
    Forbidden {
        pattern: "A confirmation prompt that does not say what will be lost",
        check: "notifications::Modal::destructive requires what will be lost and refuses an empty one",
    },
];

/// The interface, assembled.
#[derive(Debug)]
pub struct Shell {
    /// The colours in force.
    pub theme: Theme,
    /// How much information a row carries.
    pub density: Density,
    /// The user's interface scale.
    pub scale: Scale,
    /// The named layouts, and the one in force.
    pub workspaces: Workspaces,
    /// What the command palette can find.
    pub palette: Index,
    /// The bindings in force.
    pub keymap: Keymap,
    /// The generated inspector.
    pub inspector: GeneratedInspector,
    /// What the editor has said.
    pub notifications: NotificationCentre,
    /// What is wrong, and where.
    pub problems: Problems,
    /// Which viewport overlays are shown.
    pub chrome: Chrome,
    /// What is running, how far, and what a failure left behind.
    pub progress: ProgressSurface,
    /// What the last frame cost, and where.
    frame: FrameCost,
}

/// What one interface frame cost, and which part of it did.
///
/// `editor-ui-ux`: "The editor SHALL be able to profile **itself** — interface frame time, view
/// model rebuild cost, engine call counts and latencies ... A slow editor interaction SHALL be
/// attributable to a panel, a service, or an engine call rather than being reported as a whole
/// application stall."
///
/// One struct per frame rather than a trace, because the question a user asks is "which panel is
/// making this slow" and the answer is a comparison between four numbers. The engine-call half of
/// that requirement belongs to the SDK and the protocol, which count their own calls; what the
/// interface owes is this.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct FrameCost {
    /// Time spent taking what the editor said.
    pub notifications: std::time::Duration,
    /// Time spent rebuilding the inspector.
    pub inspector: std::time::Duration,
    /// Time spent reading the operation service.
    pub progress: std::time::Duration,
    /// Whether the inspector rebuilt at all this frame.
    pub rebuilt: bool,
}

impl FrameCost {
    /// The whole frame.
    #[must_use]
    pub fn total(&self) -> std::time::Duration {
        self.notifications + self.inspector + self.progress
    }

    /// The part that cost the most, named.
    ///
    /// The sentence a profiler shows: "the inspector", not "the editor".
    #[must_use]
    pub fn slowest(&self) -> &'static str {
        let parts = [
            ("the notification centre", self.notifications),
            ("the inspector", self.inspector),
            ("the progress surface", self.progress),
        ];
        parts
            .iter()
            .max_by_key(|(_, cost)| *cost)
            .map_or("nothing", |(name, _)| *name)
    }
}

impl Shell {
    /// A shell with the shipped defaults and every registered command in its palette.
    pub fn new(registry: &Registry) -> Result<Self> {
        let mut palette = Index::new();
        palette.ingest_commands(registry);
        Ok(Self {
            theme: Theme::default(),
            density: Density::default(),
            scale: Scale::default(),
            workspaces: Workspaces::new(),
            palette,
            keymap: Keymap::cyberdyne(registry)?,
            inspector: GeneratedInspector::new(),
            notifications: NotificationCentre::new(),
            problems: Problems::new(),
            chrome: Chrome::new(),
            progress: ProgressSurface::new(),
            frame: FrameCost::default(),
        })
    }

    /// The metrics the density and scale imply.
    #[must_use]
    pub const fn metrics(&self) -> Metrics {
        Metrics::new(self.density, self.scale)
    }

    /// The default region arrangement, which the header, footer and viewport are laid out in.
    #[must_use]
    pub fn composition(&self) -> Composition {
        Composition::default()
    }

    /// Describe the active document's types to the inspector.
    ///
    /// Called when a document is opened or its schema changes, and again with
    /// `Catalogue::of_world` when a runtime is attached — the inspector does not care which, which
    /// is the point of the catalogue existing.
    pub fn describe_with(&mut self, catalogue: Catalogue) {
        self.inspector.set_catalogue(catalogue);
    }

    /// One frame of interface housekeeping: take what the editor said, rebuild what moved.
    ///
    /// Bounded and non-blocking, like `Editor::pump`, and it **cannot move a panel**: it has no
    /// call that could.
    pub fn refresh(&mut self, editor: &Editor) {
        let started = std::time::Instant::now();
        self.notifications.pump(editor);
        let notifications = started.elapsed();

        let started = std::time::Instant::now();
        let rebuilt = self.inspector.refresh(editor);
        let inspector = started.elapsed();

        let started = std::time::Instant::now();
        self.progress.refresh(editor);
        let progress = started.elapsed();

        self.frame = FrameCost {
            notifications,
            inspector,
            progress,
            rebuilt,
        };
    }

    /// What the last frame cost, and where.
    #[must_use]
    pub const fn frame_cost(&self) -> FrameCost {
        self.frame
    }

    /// Write the layout in force into the workspace, which is what persists it.
    ///
    /// The layout is an opaque string to `cy_editor_services::Workspace` on purpose — a typed layout
    /// tree in the service layer would be the interface toolkit's vocabulary arriving by the back
    /// door — so this is the one place the two meet, and it is a `String` in both directions.
    pub fn persist_layout(&self, editor: &mut Editor) {
        editor
            .workspace
            .set_layout(self.workspaces.current().encode());
    }

    /// Restore the persisted layout, reporting a broken one rather than swallowing it.
    ///
    /// A failure returns the [`Problem`](cy_editor_core::Problem) *and* leaves a working default
    /// behind, which is "a broken layout SHALL never make the editor unusable" and "the editor SHALL
    /// state clearly what was recovered" at once.
    pub fn restore_layout(&mut self, editor: &Editor) -> Result<()> {
        let encoded = editor.workspace.layout();
        if encoded.is_empty() {
            return Ok(());
        }
        self.workspaces.restore(encoded)
    }

    /// The header line: what is open, where it runs, and whether it is live.
    ///
    /// The composition's header row carries the mark and the menus, which are drawn rather than
    /// written; this is the text beside them.
    #[must_use]
    pub fn header(&self, editor: &Editor) -> String {
        let scene = editor
            .workspace
            .active()
            .and_then(|id| editor.documents.get(id))
            .map_or_else(
                || "No world open".to_string(),
                |document| {
                    // The first asset is the primary one and is what names the document.
                    document
                        .assets()
                        .first()
                        .cloned()
                        .unwrap_or_else(|| document.id().to_string())
                },
            );
        let engine = match editor.hosting_mode() {
            HostingMode::NoRuntime => "No runtime",
            HostingMode::Embedded => "Embedded runtime",
            HostingMode::Hosted => "Hosted runtime · Live",
        };
        format!("{scene} · {} · {engine}", self.workspaces.current_name())
    }

    /// The footer line: save state, problems, and background work — in words.
    ///
    /// "WHEN every document is saved THEN the footer SHALL state it in words, not only by an icon
    /// colour", and "WHEN assets are importing THEN progress SHALL be visible in the footer and the
    /// user SHALL continue editing".
    #[must_use]
    pub fn footer(&self, editor: &Editor) -> String {
        let unsaved = editor
            .documents
            .ids()
            .filter(|id| editor.documents.get(*id).is_some_and(Document::is_dirty))
            .count();
        let saved = if unsaved == 0 {
            "All saved".to_string()
        } else {
            format!("{unsaved} unsaved")
        };
        format!(
            "{saved} · {} · {}",
            self.problems.summary(),
            self.progress.footer()
        )
    }

    /// The role the footer's save state is drawn in, so colour agrees with the words.
    #[must_use]
    pub fn save_role(&self, editor: &Editor) -> Semantic {
        if editor.documents.any_dirty() {
            Semantic::Warning
        } else {
            Semantic::Live
        }
    }

    /// Every piece of static interface text this crate can draw.
    ///
    /// The input to the vocabulary check. It is a method rather than a constant because the panel
    /// titles come from the workspace in force, which is where a plugin's panel would appear.
    #[must_use]
    pub fn labels(&self) -> Vec<String> {
        let mut labels: Vec<String> = self
            .workspaces
            .current()
            .panels()
            .iter()
            .map(|panel| panel_title(panel).to_string())
            .collect();
        labels.extend(Overlay::ALL.map(|overlay| overlay.label().to_string()));
        labels.extend(Region::ALL.map(|region| region.contents().to_string()));
        labels.extend(GizmoMode::ALL.map(|mode| mode.label().to_string()));
        labels.extend(
            [
                Origin::Command,
                Origin::Asset,
                Origin::Entity,
                Origin::Setting,
                Origin::Documentation,
                Origin::Recent,
            ]
            .map(|origin| origin.label().to_string()),
        );
        labels.extend(
            [
                Semantic::Active,
                Semantic::Live,
                Semantic::Selection,
                Semantic::Warning,
                Semantic::Error,
            ]
            .map(|role| role.label().to_string()),
        );
        labels.push("All saved".into());
        labels.push("No problems".into());
        labels.push("Author".into());
        labels.retain(|label| !label.is_empty());
        labels
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_core::Actor;
    use cy_editor_core::value::{Value, ValueKind};
    use cy_editor_documents::selection::Selection;
    use cy_editor_services::builtin;
    use cy_editor_visual::vocabulary;

    use super::*;

    fn registry() -> Registry {
        let mut registry = Registry::new();
        builtin::register(&mut registry).unwrap();
        registry
    }

    /// An editor with two nodes of different kinds, so that a selection change changes content.
    fn editor() -> (Editor, Catalogue) {
        let mut editor = Editor::default();
        let id = editor.open_document("worlds/city.cyworld").unwrap();
        let document = editor.documents.get_mut(id).unwrap();
        let mesh = document.schema_mut().declare_type("MeshInstance", false);
        let path = document
            .schema_mut()
            .declare_field(mesh, "mesh", ValueKind::Text, "which mesh is drawn")
            .unwrap();
        let light = document.schema_mut().declare_type("Light", false);
        let intensity = document
            .schema_mut()
            .declare_field(light, "intensity", ValueKind::Float, "how bright it is")
            .unwrap();

        document
            .with_transaction("Build", Actor::human("designer"), |document| {
                let first = document.create_node(None)?;
                document.add_component(
                    first,
                    mesh,
                    vec![(path, Value::Text("meshes/rock.cymesh".into()))],
                )?;
                let second = document.create_node(None)?;
                document.add_component(second, light, vec![(intensity, Value::Float(4.0))])?;
                Ok(())
            })
            .unwrap();

        let catalogue = Catalogue::of_document(editor.documents.get(id).unwrap().schema());
        (editor, catalogue)
    }

    #[test]
    fn the_selection_changes_content_and_never_position() {
        // "WHEN the user selects a terrain object and then a character THEN the inspector's sections
        // SHALL change and every panel SHALL remain where it was."
        let (mut editor, catalogue) = editor();
        let mut shell = Shell::new(&registry()).unwrap();
        shell.describe_with(catalogue);

        let nodes: Vec<_> = editor
            .documents
            .get(editor.workspace.active().unwrap())
            .unwrap()
            .content()
            .nodes()
            .collect();

        let mut select = |shell: &mut Shell, index: usize| {
            let mut selection = Selection::new();
            selection.add_node(nodes[index]);
            editor.selection.set(selection);
            shell.refresh(&editor);
            shell
                .inspector
                .sections()
                .iter()
                .map(|section| section.title.clone())
                .collect::<Vec<_>>()
        };

        let layout_before = shell.workspaces.current().encode();
        let first = select(&mut shell, 0);
        let layout_after_first = shell.workspaces.current().encode();
        let second = select(&mut shell, 1);
        let layout_after_second = shell.workspaces.current().encode();

        assert_eq!(first, ["MeshInstance"]);
        assert_eq!(second, ["Light"], "the content followed the selection");
        assert_eq!(layout_before, layout_after_first);
        assert_eq!(
            layout_after_first, layout_after_second,
            "and not one panel moved"
        );
    }

    #[test]
    fn every_label_the_interface_draws_is_in_the_engines_vocabulary() {
        // "WHEN a panel labels a selection count in another engine's terms THEN it SHALL be flagged
        // against this requirement." One list, one loop, and the check is worth having.
        let shell = Shell::new(&registry()).unwrap();
        for label in shell.labels() {
            vocabulary::check_label(&label)
                .unwrap_or_else(|problem| panic!("the interface says {label:?}: {problem}"));
        }
    }

    #[test]
    fn every_registered_commands_label_and_description_are_in_the_engines_vocabulary() {
        // The other half of the same rule: a command's label is interface text too, and it reaches
        // the menu, the palette and an agent's tool listing.
        for metadata in registry().all() {
            vocabulary::check_label(&metadata.label)
                .unwrap_or_else(|problem| panic!("{}: {problem}", metadata.id));
            vocabulary::check_label(&metadata.description)
                .unwrap_or_else(|problem| panic!("{}: {problem}", metadata.id));
        }
    }

    #[test]
    fn saved_state_is_legible_at_a_glance_in_words() {
        let (mut editor, _) = editor();
        let shell = Shell::new(&registry()).unwrap();
        let id = editor.workspace.active().unwrap();
        editor
            .documents
            .get_mut(id)
            .unwrap()
            .save(|_| Ok(()))
            .unwrap();

        let footer = shell.footer(&editor);
        assert!(footer.contains("All saved"), "{footer}");
        assert!(footer.contains("No problems"), "{footer}");
        assert_eq!(shell.save_role(&editor), Semantic::Live);

        editor
            .documents
            .get_mut(id)
            .unwrap()
            .with_transaction("Create", Actor::human("designer"), |document| {
                document.create_node(None).map(|_| ())
            })
            .unwrap();
        assert!(shell.footer(&editor).contains("1 unsaved"));
        assert_eq!(shell.save_role(&editor), Semantic::Warning);
    }

    #[test]
    fn the_header_says_what_is_open_and_where_it_runs() {
        let (editor, _) = editor();
        let shell = Shell::new(&registry()).unwrap();
        let header = shell.header(&editor);
        assert!(header.contains("worlds/city.cyworld"), "{header}");
        assert!(header.contains("No runtime"), "{header}");
        assert!(header.contains("Scene"), "{header}");
    }

    #[test]
    fn an_idle_shell_rebuilds_nothing() {
        let (editor, catalogue) = editor();
        let mut shell = Shell::new(&registry()).unwrap();
        shell.describe_with(catalogue);
        shell.refresh(&editor);
        let rebuilds = shell.inspector.rebuilds();

        for _ in 0..500 {
            shell.refresh(&editor);
        }
        assert_eq!(shell.inspector.rebuilds(), rebuilds, "nothing moved");
    }

    #[test]
    fn every_forbidden_interface_pattern_names_a_check() {
        assert_eq!(
            FORBIDDEN.len(),
            8,
            "`editor-ui-ux` lists eight; a ninth is a specification change and belongs here with \
             the check that catches it"
        );
        for entry in FORBIDDEN {
            assert!(!entry.pattern.trim().is_empty());
            assert!(
                !entry.check.trim().is_empty(),
                "{:?} has no check, which makes \"each SHALL be checkable\" untrue",
                entry.pattern
            );
        }
    }

    #[test]
    fn a_layout_survives_a_restart_through_the_workspace_that_persists_it() {
        let (mut editor, _) = editor();
        let mut shell = Shell::new(&registry()).unwrap();
        let panel = PanelId::new("timeline").unwrap();
        let viewport = PanelId::new("viewport").unwrap();
        shell
            .workspaces
            .current_mut()
            .dock_beside(&viewport, panel.clone())
            .unwrap();
        shell.persist_layout(&mut editor);

        // A new session: a new shell, the same workspace.
        let mut restarted = Shell::new(&registry()).unwrap();
        assert!(!restarted.workspaces.current().contains(&panel));
        restarted.restore_layout(&editor).unwrap();
        assert!(restarted.workspaces.current().contains(&panel));
    }

    #[test]
    fn a_broken_persisted_layout_leaves_a_working_editor_and_says_so() {
        let (mut editor, _) = editor();
        editor
            .workspace
            .set_layout("cy-layout 1 window 0 docked split x nonsense");
        let mut shell = Shell::new(&registry()).unwrap();

        let problem = shell.restore_layout(&editor).unwrap_err();
        assert!(
            problem.remedy.as_deref().unwrap().contains("reset"),
            "{problem}"
        );
        assert!(
            shell
                .workspaces
                .current()
                .contains(&PanelId::new("viewport").unwrap()),
            "and the editor still has a viewport to work in"
        );
    }

    #[test]
    fn a_slow_frame_is_attributable_to_a_part_rather_than_to_the_editor() {
        // "WHEN the interface stutters THEN the responsible panel or service SHALL be identifiable
        // from an editor profile."
        let (mut editor, catalogue) = editor();
        let mut shell = Shell::new(&registry()).unwrap();
        shell.describe_with(catalogue);

        let node = editor
            .documents
            .get(editor.workspace.active().unwrap())
            .unwrap()
            .content()
            .nodes()
            .next()
            .unwrap();
        let mut selection = Selection::new();
        selection.add_node(node);
        editor.selection.set(selection);

        shell.refresh(&editor);
        let cost = shell.frame_cost();
        assert!(
            cost.rebuilt,
            "the selection moved, so the inspector rebuilt"
        );
        assert!(cost.total() > std::time::Duration::ZERO);
        assert!(
            [
                "the notification centre",
                "the inspector",
                "the progress surface"
            ]
            .contains(&cost.slowest()),
            "a frame's cost is attributed to a named part, not to the editor"
        );
    }

    #[test]
    fn the_viewport_keeps_its_height_however_much_chrome_is_shown() {
        let mut shell = Shell::new(&registry()).unwrap();
        let composition = shell.composition();
        let before = composition.extent(Region::Centre).unwrap().height;
        for overlay in Overlay::ALL {
            shell.chrome.set(overlay, true);
        }
        let after = shell.composition().extent(Region::Centre).unwrap().height;
        assert!(
            (before - after).abs() < f32::EPSILON,
            "chrome is overlay, not toolbar"
        );
    }
}
