//! Docking, floating, tabbing, multiple windows — and named workspaces over the top of them.
//!
//! `editor-ui-ux`: "The editor SHALL support **dockable, floating, tabbed, and multi-window
//! panels**, with panels movable between windows and across monitors ... named **workspaces** —
//! saved layouts ... switchable without losing document state ... A workspace SHALL be resettable to
//! its default, and a broken layout SHALL never make the editor unusable."
//!
//! --- WHY THIS IS A TREE AND NOT A TOOLKIT'S DOCK MANAGER --------------------------------------------
//!
//! Because the toolkit is not chosen yet, and this is the half of docking that is not the toolkit's:
//! which panels exist, how they are nested, which tab is active, which window each is in, and how
//! all of that survives a restart. A dock manager renders a [`Layout`]; it does not decide one. When
//! a toolkit is chosen it reads this type, and the tests below keep meaning what they mean.
//!
//! --- WHY A WORKSPACE HOLDS NO DOCUMENTS -------------------------------------------------------------
//!
//! "switchable without losing document state" is a property this type has by having no document in
//! it. [`Workspaces::switch`] cannot close a document because it cannot reach one — the open set
//! lives in `cy_editor_services::Workspace`, and the two are persisted separately for the same
//! reason a camera position is: presentation state must not be able to touch authoritative state.
//!
//! --- WHY THE ENCODING IS A FLAT TOKEN STREAM ------------------------------------------------------
//!
//! [`Layout::encode`] writes prefix notation with an explicit tab count, so decoding is a recursive
//! descent over whitespace-separated tokens with no brackets to balance and no ambiguity to resolve.
//! It is written by hand because this workspace has no third-party crates, and it is deliberately
//! the dullest format that round-trips: a layout is read once at start-up, and a clever encoding
//! would buy nothing and cost a parser nobody can debug at the moment it matters — which is when the
//! layout is broken and the editor will not open.

use std::collections::BTreeMap;
use std::fmt::Write as _;

use cy_editor_core::problem::{Problem, Result};
use cy_editor_visual::chrome::{Composition, Region};

/// A panel's stable identifier: `hierarchy`, `inspector`, `content-browser`.
///
/// A string rather than an enum because plugins add panels, and an enum would make "a plugin's panel
/// is a panel" false. Validated on construction so that an identifier cannot contain the separator
/// the encoding uses, which is the failure that turns one broken panel into an unopenable editor.
#[derive(Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct PanelId(String);

impl PanelId {
    /// A panel identifier, refusing one the layout encoding could not round-trip.
    pub fn new(id: impl Into<String>) -> Result<Self> {
        let id = id.into();
        if id.is_empty() || id.chars().any(char::is_whitespace) {
            return Err(Problem::new(
                format!("name a panel {id:?}"),
                "a panel identifier is one word with no whitespace",
            )
            .with_remedy("use a hyphenated name such as content-browser"));
        }
        Ok(Self(id))
    }

    /// The identifier as text.
    #[must_use]
    pub fn as_str(&self) -> &str {
        &self.0
    }
}

impl std::fmt::Display for PanelId {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(&self.0)
    }
}

/// Which way a split divides its two children.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub enum Split {
    /// Side by side.
    Horizontal,
    /// One above the other.
    Vertical,
}

impl Split {
    /// The single character the encoding uses.
    const fn token(self) -> &'static str {
        match self {
            Split::Horizontal => "x",
            Split::Vertical => "y",
        }
    }
}

/// One node of a window's layout tree.
#[derive(Clone, PartialEq, Debug)]
pub enum Node {
    /// Two children, divided.
    Divided {
        /// Which way.
        split: Split,
        /// The fraction of the space the first child takes.
        ratio: f32,
        /// The first child: left, or top.
        first: Box<Node>,
        /// The second child: right, or bottom.
        second: Box<Node>,
    },
    /// A stack of tabbed panels.
    Tabs {
        /// The panels in the stack, in tab order.
        panels: Vec<PanelId>,
        /// Which one is in front.
        active: usize,
    },
}

impl Node {
    /// A single-panel tab stack.
    #[must_use]
    pub fn panel(id: PanelId) -> Self {
        Node::Tabs {
            panels: vec![id],
            active: 0,
        }
    }

    /// A stack of tabbed panels with the first in front.
    #[must_use]
    pub fn tabs(panels: Vec<PanelId>) -> Self {
        Node::Tabs { panels, active: 0 }
    }

    /// Two nodes, divided.
    #[must_use]
    pub fn divided(split: Split, ratio: f32, first: Node, second: Node) -> Self {
        Node::Divided {
            split,
            ratio,
            first: Box::new(first),
            second: Box::new(second),
        }
    }

    /// Every panel in this subtree, in layout order.
    #[must_use]
    pub fn panels(&self) -> Vec<PanelId> {
        let mut found = Vec::new();
        self.collect(&mut found);
        found
    }

    fn collect(&self, into: &mut Vec<PanelId>) {
        match self {
            Node::Divided { first, second, .. } => {
                first.collect(into);
                second.collect(into);
            }
            Node::Tabs { panels, .. } => into.extend(panels.iter().cloned()),
        }
    }

    /// Remove a panel from this subtree, collapsing a stack or a split that is left empty.
    fn remove(self, panel: &PanelId) -> Option<Node> {
        match self {
            Node::Tabs { panels, active } => {
                let kept: Vec<PanelId> = panels.into_iter().filter(|held| held != panel).collect();
                if kept.is_empty() {
                    None
                } else {
                    let active = active.min(kept.len() - 1);
                    Some(Node::Tabs {
                        panels: kept,
                        active,
                    })
                }
            }
            Node::Divided {
                split,
                ratio,
                first,
                second,
            } => match (first.remove(panel), second.remove(panel)) {
                (Some(first), Some(second)) => Some(Node::divided(split, ratio, first, second)),
                (Some(only), None) | (None, Some(only)) => Some(only),
                (None, None) => None,
            },
        }
    }

    /// Add a panel to the tab stack that holds `beside`, or return it unplaced.
    fn add_beside(&mut self, beside: &PanelId, panel: PanelId) -> Option<PanelId> {
        match self {
            Node::Tabs { panels, active } => {
                if panels.iter().any(|held| held == beside) {
                    panels.push(panel);
                    *active = panels.len() - 1;
                    None
                } else {
                    Some(panel)
                }
            }
            Node::Divided { first, second, .. } => match first.add_beside(beside, panel) {
                Some(panel) => second.add_beside(beside, panel),
                None => None,
            },
        }
    }

    /// Bring a panel's tab to the front, if this subtree holds it.
    fn activate(&mut self, panel: &PanelId) -> bool {
        match self {
            Node::Tabs { panels, active } => {
                if let Some(index) = panels.iter().position(|held| held == panel) {
                    *active = index;
                    true
                } else {
                    false
                }
            }
            Node::Divided { first, second, .. } => first.activate(panel) || second.activate(panel),
        }
    }
}

/// A window's identity, unique within a layout.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct WindowId(pub u32);

/// One window: the main one, or a floating one on another monitor.
#[derive(Clone, PartialEq, Debug)]
pub struct DockWindow {
    /// Which window.
    pub id: WindowId,
    /// Its layout tree.
    pub root: Node,
    /// Whether it floats. The first window does not.
    pub floating: bool,
}

/// Where every panel is.
#[derive(Clone, PartialEq, Debug)]
pub struct Layout {
    windows: Vec<DockWindow>,
    next_window: u32,
}

impl Default for Layout {
    fn default() -> Self {
        Self::scene_editing()
    }
}

impl Layout {
    /// The default workspace, built from the composition `editor-visual-language` fixes.
    ///
    /// The regions come from `cy_editor_visual::chrome::Composition` rather than being written
    /// again here, so that "the viewport occupies the largest single region" is a property of one
    /// table rather than of two that agree today.
    #[must_use]
    pub fn scene_editing() -> Self {
        let panel = |id: &str| Node::panel(PanelId::new(id).expect("a built-in panel identifier"));
        let left = Node::divided(
            Split::Vertical,
            0.55,
            panel("hierarchy"),
            panel("content-browser"),
        );
        let centre = Node::divided(
            Split::Vertical,
            0.70,
            panel("viewport"),
            Node::tabs(vec![
                PanelId::new("script-graph").expect("a built-in panel identifier"),
                PanelId::new("animation").expect("a built-in panel identifier"),
            ]),
        );
        let right = Node::divided(
            Split::Vertical,
            0.62,
            panel("inspector"),
            Node::tabs(vec![
                PanelId::new("console").expect("a built-in panel identifier"),
                PanelId::new("profiler").expect("a built-in panel identifier"),
                PanelId::new("problems").expect("a built-in panel identifier"),
            ]),
        );
        let composition = Composition::default();
        let left_share = composition
            .extent(Region::LeftUpper)
            .map_or(0.18, |extent| extent.width);
        let right_share = composition
            .extent(Region::Right)
            .map_or(0.22, |extent| extent.width);

        let root = Node::divided(
            Split::Horizontal,
            left_share,
            left,
            Node::divided(
                Split::Horizontal,
                1.0 - right_share / (1.0 - left_share),
                centre,
                right,
            ),
        );
        Self {
            windows: vec![DockWindow {
                id: WindowId(0),
                root,
                floating: false,
            }],
            next_window: 1,
        }
    }

    /// The windows, in creation order.
    #[must_use]
    pub fn windows(&self) -> &[DockWindow] {
        &self.windows
    }

    /// Every panel in every window.
    #[must_use]
    pub fn panels(&self) -> Vec<PanelId> {
        self.windows
            .iter()
            .flat_map(|window| window.root.panels())
            .collect()
    }

    /// Which window holds a panel.
    #[must_use]
    pub fn window_of(&self, panel: &PanelId) -> Option<WindowId> {
        self.windows
            .iter()
            .find(|window| window.root.panels().contains(panel))
            .map(|window| window.id)
    }

    /// Whether a panel is anywhere in the layout.
    #[must_use]
    pub fn contains(&self, panel: &PanelId) -> bool {
        self.window_of(panel).is_some()
    }

    /// Dock a panel into the tab stack that holds `beside`.
    pub fn dock_beside(&mut self, beside: &PanelId, panel: PanelId) -> Result<()> {
        if self.contains(&panel) {
            self.remove(&panel);
        }
        let mut unplaced = Some(panel);
        for window in &mut self.windows {
            unplaced = match unplaced {
                Some(panel) => window.root.add_beside(beside, panel),
                None => break,
            };
        }
        match unplaced {
            None => Ok(()),
            Some(panel) => Err(Problem::new(
                format!("dock {panel} beside {beside}"),
                format!("{beside} is not in this workspace"),
            )
            .with_remedy("open the panel you want to dock beside first, or dock into a window")),
        }
    }

    /// Move a panel into a new floating window, on another monitor if the user drags it there.
    pub fn float(&mut self, panel: &PanelId) -> Result<WindowId> {
        if !self.contains(panel) {
            return Err(Problem::not_found(format!("a panel named {panel}"))
                .with_remedy("open it before floating it"));
        }
        self.remove(panel);
        let id = WindowId(self.next_window);
        self.next_window += 1;
        self.windows.push(DockWindow {
            id,
            root: Node::panel(panel.clone()),
            floating: true,
        });
        Ok(id)
    }

    /// Move a panel into an existing window, beside the panel named there.
    ///
    /// This is "panels movable **between windows**": the panel leaves the window it was in, the
    /// window collapses if it is left empty, and nothing else moves.
    pub fn move_to(&mut self, panel: &PanelId, beside: &PanelId) -> Result<()> {
        self.dock_beside(beside, panel.clone())
    }

    /// Bring a panel's tab to the front.
    pub fn activate(&mut self, panel: &PanelId) -> bool {
        self.windows
            .iter_mut()
            .any(|window| window.root.activate(panel))
    }

    /// Remove a panel, collapsing whatever it leaves empty.
    pub fn remove(&mut self, panel: &PanelId) {
        for window in &mut self.windows {
            if let Some(root) = window.root.clone().remove(panel) {
                window.root = root;
            } else {
                // An empty window's tree cannot be represented, so the window goes; the main
                // window keeps an empty tab stack rather than disappearing, because an editor with
                // no window is not a recoverable state.
                window.root = Node::Tabs {
                    panels: Vec::new(),
                    active: 0,
                };
            }
        }
        self.windows.retain(|window| {
            !window.floating && window.id == WindowId(0) || !window.root.panels().is_empty()
        });
    }

    /// The layout as the opaque string `cy_editor_services::Workspace` persists.
    #[must_use]
    pub fn encode(&self) -> String {
        let mut out = String::from("cy-layout 1");
        for window in &self.windows {
            let _ = write!(
                out,
                " window {} {}",
                window.id.0,
                if window.floating {
                    "floating"
                } else {
                    "docked"
                }
            );
            encode_node(&window.root, &mut out);
        }
        out
    }

    /// Read a layout back, refusing anything it does not understand.
    ///
    /// Every failure carries a remedy naming the reset, because the only thing a user can do with a
    /// broken layout is take the default one — and an error that does not say so is what makes a
    /// broken layout an unusable editor.
    pub fn decode(text: &str) -> Result<Self> {
        let mut tokens = text.split_whitespace();
        if tokens.next() != Some("cy-layout") || tokens.next() != Some("1") {
            return Err(broken("it does not begin with a cy-layout 1 header"));
        }
        let mut windows = Vec::new();
        let mut next_window = 0;
        while let Some(keyword) = tokens.next() {
            if keyword != "window" {
                return Err(broken(&format!("expected a window and found {keyword:?}")));
            }
            let id: u32 = tokens
                .next()
                .and_then(|token| token.parse().ok())
                .ok_or_else(|| broken("a window has no identifier"))?;
            let floating = match tokens.next() {
                Some("floating") => true,
                Some("docked") => false,
                other => {
                    return Err(broken(&format!(
                        "a window is {other:?}, not docked or floating"
                    )));
                }
            };
            let root = decode_node(&mut tokens)?;
            next_window = next_window.max(id + 1);
            windows.push(DockWindow {
                id: WindowId(id),
                root,
                floating,
            });
        }
        if windows.is_empty() {
            return Err(broken("it contains no windows"));
        }
        Ok(Self {
            windows,
            next_window,
        })
    }
}

/// A layout that could not be read, with the one remedy there is.
fn broken(because: &str) -> Problem {
    Problem::new("read the saved layout", because.to_string()).with_remedy(
        "reset the workspace to its default; a layout is presentation state and nothing in a \
         document depends on it",
    )
}

fn encode_node(node: &Node, out: &mut String) {
    match node {
        Node::Divided {
            split,
            ratio,
            first,
            second,
        } => {
            let _ = write!(out, " split {} {ratio}", split.token());
            encode_node(first, out);
            encode_node(second, out);
        }
        Node::Tabs { panels, active } => {
            let _ = write!(out, " tabs {active} {}", panels.len());
            for panel in panels {
                out.push(' ');
                out.push_str(panel.as_str());
            }
        }
    }
}

fn decode_node<'tokens>(tokens: &mut impl Iterator<Item = &'tokens str>) -> Result<Node> {
    match tokens.next() {
        Some("split") => decode_split(tokens),
        Some("tabs") => decode_tabs(tokens),
        other => Err(broken(&format!(
            "expected split or tabs and found {other:?}"
        ))),
    }
}

fn decode_split<'tokens>(tokens: &mut impl Iterator<Item = &'tokens str>) -> Result<Node> {
    let split = match tokens.next() {
        Some("x") => Split::Horizontal,
        Some("y") => Split::Vertical,
        other => return Err(broken(&format!("a split is {other:?}, not x or y"))),
    };
    let ratio: f32 = tokens
        .next()
        .and_then(|token| token.parse().ok())
        .ok_or_else(|| broken("a split has no ratio"))?;
    if !(0.0..=1.0).contains(&ratio) {
        return Err(broken("a split's ratio is outside zero to one"));
    }
    let first = decode_node(tokens)?;
    let second = decode_node(tokens)?;
    Ok(Node::divided(split, ratio, first, second))
}

fn decode_tabs<'tokens>(tokens: &mut impl Iterator<Item = &'tokens str>) -> Result<Node> {
    let active: usize = tokens
        .next()
        .and_then(|token| token.parse().ok())
        .ok_or_else(|| broken("a tab stack has no active index"))?;
    let count: usize = tokens
        .next()
        .and_then(|token| token.parse().ok())
        .ok_or_else(|| broken("a tab stack has no panel count"))?;
    let mut panels = Vec::with_capacity(count.min(64));
    for _ in 0..count {
        let id = tokens
            .next()
            .ok_or_else(|| broken("a tab stack ends early"))?;
        panels.push(PanelId::new(id)?);
    }
    if !panels.is_empty() && active >= panels.len() {
        return Err(broken("a tab stack's active tab is not one of its tabs"));
    }
    Ok(Node::Tabs { panels, active })
}

/// The named layouts, and which one is in force.
///
/// "named **workspaces** — saved layouts for tasks such as scene editing, animation, materials,
/// sequencing, and profiling — switchable **without losing document state**."
#[derive(Clone, PartialEq, Debug)]
pub struct Workspaces {
    layouts: BTreeMap<String, Layout>,
    current: String,
}

impl Default for Workspaces {
    fn default() -> Self {
        Self::new()
    }
}

impl Workspaces {
    /// The shipped workspaces, with scene editing in force.
    #[must_use]
    pub fn new() -> Self {
        let mut layouts = BTreeMap::new();
        layouts.insert("Scene".to_string(), Layout::scene_editing());
        Self {
            layouts,
            current: "Scene".to_string(),
        }
    }

    /// The layout in force.
    #[must_use]
    pub fn current(&self) -> &Layout {
        self.layouts
            .get(&self.current)
            .expect("the current workspace is always one of the layouts")
    }

    /// The layout in force, for editing.
    pub fn current_mut(&mut self) -> &mut Layout {
        self.layouts
            .get_mut(&self.current)
            .expect("the current workspace is always one of the layouts")
    }

    /// The name of the workspace in force.
    #[must_use]
    pub fn current_name(&self) -> &str {
        &self.current
    }

    /// Every workspace's name, in order.
    pub fn names(&self) -> impl Iterator<Item = &str> {
        self.layouts.keys().map(String::as_str)
    }

    /// Save the layout in force under a name.
    pub fn save_as(&mut self, name: impl Into<String>) {
        let name = name.into();
        let layout = self.current().clone();
        self.layouts.insert(name.clone(), layout);
        self.current = name;
    }

    /// Switch to a named workspace. **Touches no document**; see the module note.
    pub fn switch(&mut self, name: &str) -> Result<()> {
        if !self.layouts.contains_key(name) {
            return Err(Problem::not_found(format!("a workspace named {name:?}"))
                .with_remedy("save the current layout under that name first"));
        }
        self.current = name.to_string();
        Ok(())
    }

    /// Put the workspace in force back to its default.
    ///
    /// "A workspace SHALL be resettable to its default, and a broken layout SHALL never make the
    /// editor unusable."
    pub fn reset(&mut self) {
        let name = self.current.clone();
        self.layouts.insert(name, Layout::scene_editing());
    }

    /// Restore a persisted layout, falling back to the default and **saying so**.
    ///
    /// The failure is returned rather than swallowed, so the shell can post it as a notification:
    /// an editor that quietly discarded a layout somebody spent an afternoon arranging is one they
    /// stop trusting with the arrangement.
    pub fn restore(&mut self, encoded: &str) -> Result<()> {
        match Layout::decode(encoded) {
            Ok(layout) => {
                let name = self.current.clone();
                self.layouts.insert(name, layout);
                Ok(())
            }
            Err(problem) => {
                self.reset();
                Err(problem)
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn panel(id: &str) -> PanelId {
        PanelId::new(id).unwrap()
    }

    #[test]
    fn the_default_workspace_holds_the_panels_the_composition_names() {
        let layout = Layout::scene_editing();
        for expected in [
            "hierarchy",
            "content-browser",
            "viewport",
            "inspector",
            "console",
        ] {
            assert!(
                layout.contains(&panel(expected)),
                "the default workspace has no {expected}"
            );
        }
        assert_eq!(layout.windows().len(), 1);
    }

    #[test]
    fn a_panel_moves_to_another_window_and_back() {
        // "with panels movable between windows and across monitors".
        let mut layout = Layout::scene_editing();
        let floated = layout.float(&panel("console")).unwrap();

        assert_eq!(layout.windows().len(), 2);
        assert_eq!(layout.window_of(&panel("console")), Some(floated));
        assert!(layout.windows()[1].floating);

        layout
            .move_to(&panel("console"), &panel("inspector"))
            .unwrap();
        assert_eq!(layout.window_of(&panel("console")), Some(WindowId(0)));
        assert_eq!(
            layout.windows().len(),
            1,
            "the window the panel left is gone rather than left empty"
        );
    }

    #[test]
    fn docking_beside_a_panel_that_is_not_open_says_so() {
        let mut layout = Layout::scene_editing();
        let problem = layout
            .dock_beside(&panel("sequencer"), panel("curves"))
            .unwrap_err();
        assert!(problem.remedy.is_some(), "{problem}");
        assert!(!layout.contains(&panel("curves")));
    }

    #[test]
    fn a_layout_round_trips_through_its_persisted_form() {
        let mut layout = Layout::scene_editing();
        layout.float(&panel("profiler")).unwrap();
        layout.activate(&panel("problems"));

        let encoded = layout.encode();
        let decoded = Layout::decode(&encoded).unwrap();
        assert_eq!(decoded, layout);
        assert_eq!(decoded.encode(), encoded);
    }

    #[test]
    fn a_broken_layout_never_makes_the_editor_unusable() {
        // "WHEN a layout is unusable THEN resetting to a default workspace SHALL restore a working
        // editor." Restoring reports the failure and leaves a working workspace behind.
        let mut workspaces = Workspaces::new();
        let problem = workspaces
            .restore("cy-layout 1 window 0 docked split x nonsense")
            .unwrap_err();
        assert!(
            problem.remedy.as_deref().unwrap().contains("reset"),
            "{problem}"
        );
        assert!(workspaces.current().contains(&panel("viewport")));

        for rubbish in [
            "",
            "{}",
            "cy-layout 2 window",
            "cy-layout 1 window 0 docked tabs 5 1 a",
        ] {
            let mut workspaces = Workspaces::new();
            assert!(
                workspaces.restore(rubbish).is_err(),
                "{rubbish:?} was accepted"
            );
            assert!(workspaces.current().contains(&panel("inspector")));
        }
    }

    #[test]
    fn switching_workspaces_changes_the_arrangement_and_nothing_else() {
        // "WHEN a user switches from scene editing to animation THEN the panel arrangement SHALL
        // change without closing documents." There is no document in this type to close, which is
        // the implementation of that requirement rather than a caveat about it.
        let mut workspaces = Workspaces::new();
        workspaces.save_as("Animation");
        workspaces
            .current_mut()
            .dock_beside(&panel("viewport"), panel("timeline"))
            .unwrap();

        assert!(workspaces.current().contains(&panel("timeline")));
        workspaces.switch("Scene").unwrap();
        assert!(!workspaces.current().contains(&panel("timeline")));
        workspaces.switch("Animation").unwrap();
        assert!(workspaces.current().contains(&panel("timeline")));
    }

    #[test]
    fn resetting_restores_the_default_arrangement() {
        let mut workspaces = Workspaces::new();
        workspaces.current_mut().remove(&panel("inspector"));
        assert!(!workspaces.current().contains(&panel("inspector")));

        workspaces.reset();
        assert!(workspaces.current().contains(&panel("inspector")));
    }

    #[test]
    fn a_panel_identifier_the_encoding_could_not_round_trip_is_refused() {
        assert!(PanelId::new("content browser").is_err());
        assert!(PanelId::new("").is_err());
        assert!(PanelId::new("content-browser").is_ok());
    }

    #[test]
    fn switching_to_a_workspace_that_does_not_exist_says_what_would_make_it_work() {
        let mut workspaces = Workspaces::new();
        let problem = workspaces.switch("Sequencing").unwrap_err();
        assert!(problem.remedy.is_some(), "{problem}");
        assert_eq!(workspaces.current_name(), "Scene");
    }
}
