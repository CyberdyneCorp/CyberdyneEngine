//! Docking, floating, tabbing and named workspaces, drawn. Task 1.2.
//!
//! `cy_editor_interface::docking` owns the layout as the editor's own tree, and its header already
//! said what this file is for: *"a dock manager renders a `Layout`; it does not decide one."* So
//! this module is a **two-way conversion** and nothing else — `Layout` in, `egui_dock::DockState`
//! out, and the user's rearrangement back the other way so that it can be saved, named, restored and
//! reset by the model that already knows how.
//!
//! --- WHY THE ROUND TRIP MATTERS MORE THAN THE DRAWING ----------------------------------------------
//!
//! `editor-ui-ux` requires named workspaces that survive a restart, and `Workspaces` is where they
//! live. If the toolkit's own tree became the source of truth the moment a user dragged a panel,
//! then saving a workspace would mean serialising the toolkit's structures — and the editor would
//! have acquired a persisted format owned by a third-party crate on its own release cadence. That is
//! the single most expensive way for a toolkit choice to stop being reversible, and it happens by
//! accident, one convenient call at a time.
//!
//! So [`to_dock_state`] and [`from_dock_state`] are a pair, `Layout` is authoritative, and
//! [`tests::a_layout_survives_the_round_trip_through_the_toolkit`] is what keeps them one mechanism
//! rather than two that agree today.
//!
//! --- IDENTITY IS A KEY, NEVER A TITLE ---------------------------------------------------------------
//!
//! The tab type is [`PanelKey`] — task 1.0.5, the idea taken from Dear ImGui's `WindowKey`. egui_dock
//! keys a tab by whatever it is given, and giving it the display title would key a saved workspace
//! on text that is renamed and localised. [`Panels::id`] hands back an `egui::Id` derived from the
//! key, [`Panels::title`] looks the title up separately, and no title ever enters a layout.

use cy_editor_interface::docking::{DockWindow, Layout, Node, PanelId, Split, WindowId};
use cy_editor_interface::panels::PanelKey;
use egui_dock::{DockState, NodeIndex, SurfaceIndex};

/// The placeholder a tree is grown from before its real leaves are installed.
///
/// egui_dock refuses to split a node into an empty one, so building a tree top-down needs a tab to
/// hand it. It is replaced by the recursion immediately and never reaches a frame; a kind starting
/// with a character `PanelKey::new` accepts keeps that construction infallible.
fn placeholder() -> PanelKey {
    PanelKey::new("placeholder").expect("a constant panel kind")
}

/// The editor's layout as the toolkit's dock state.
///
/// Every window in the layout becomes a surface: the first is the main one, and each further window
/// becomes a floating one, which is what `editor-ui-ux`'s "panels SHALL be floatable into separate
/// windows, including onto another monitor" asks for.
#[must_use]
pub fn to_dock_state(layout: &Layout) -> DockState<PanelKey> {
    let mut windows = layout.windows().iter();
    let main = windows.next();
    let mut state = DockState::new(vec![placeholder()]);
    if let Some(window) = main {
        install(state.main_surface_mut(), NodeIndex::root(), &window.root);
    }
    for window in windows {
        let surface = state.add_window(vec![placeholder()]);
        install(&mut state[surface], NodeIndex::root(), &window.root);
    }
    state
}

/// Grow one subtree in place, from a node that currently holds a placeholder leaf.
fn install(tree: &mut egui_dock::Tree<PanelKey>, at: NodeIndex, node: &Node) {
    match node {
        Node::Tabs { panels, active } => {
            let tabs: Vec<PanelKey> = panels.iter().filter_map(key_of).collect();
            if tabs.is_empty() {
                return;
            }
            let active = (*active).min(tabs.len() - 1);
            let mut leaf = egui_dock::LeafNode::new(tabs);
            leaf.active = egui_dock::TabIndex(active);
            tree[at] = egui_dock::Node::Leaf(leaf);
        }
        Node::Divided {
            split,
            ratio,
            first,
            second,
        } => {
            // `Right` and `Below` put the OLD node first with `fraction` of the space, which is
            // exactly what `ratio` means here — the first child's share.
            let direction = match split {
                Split::Horizontal => egui_dock::Split::Right,
                Split::Vertical => egui_dock::Split::Below,
            };
            let [first_at, second_at] = tree.split(
                at,
                direction,
                ratio.clamp(0.05, 0.95),
                egui_dock::Node::leaf_with(vec![placeholder()]),
            );
            install(tree, first_at, first);
            install(tree, second_at, second);
        }
    }
}

/// A panel identifier as a stable key, dropping one the key type would not accept.
fn key_of(panel: &PanelId) -> Option<PanelKey> {
    PanelKey::from_panel_id(panel).ok()
}

/// The toolkit's dock state back as the editor's layout.
///
/// A surface whose tree holds nothing is skipped rather than producing an empty window, because a
/// user who closes the last panel of a floating window has closed the window.
#[must_use]
pub fn from_dock_state(state: &DockState<PanelKey>) -> Layout {
    let mut windows = Vec::new();
    for (position, (index, surface)) in state.iter_surfaces_indexed().enumerate() {
        let Some(tree) = surface.node_tree() else {
            continue;
        };
        let Some(root) = harvest(tree, NodeIndex::root()) else {
            continue;
        };
        windows.push(DockWindow {
            // The window's identity is its position among the surfaces, which is what the layout's
            // own encoding stores. `SurfaceIndex` is a `usize` and this is a `u32`; a workspace with
            // four billion floating windows is not the failure mode worth defending against, and
            // `u32::try_from` here would be an error case with no honest handling.
            id: WindowId(u32::try_from(position).unwrap_or(u32::MAX)),
            root,
            floating: index != SurfaceIndex::main(),
        });
    }
    if windows.is_empty() {
        return Layout::scene_editing();
    }
    Layout::from_windows(windows)
}

/// One subtree of the toolkit's tree as the editor's node.
fn harvest(tree: &egui_dock::Tree<PanelKey>, at: NodeIndex) -> Option<Node> {
    if at.0 >= tree.len() {
        return None;
    }
    match &tree[at] {
        egui_dock::Node::Empty => None,
        egui_dock::Node::Leaf(leaf) => {
            let panels: Vec<PanelId> = leaf
                .tabs
                .iter()
                .filter(|key| key.kind() != "placeholder")
                .filter_map(|key| key.to_panel_id().ok())
                .collect();
            if panels.is_empty() {
                return None;
            }
            let active = leaf.active.0.min(panels.len() - 1);
            Some(Node::Tabs { panels, active })
        }
        egui_dock::Node::Horizontal(split) | egui_dock::Node::Vertical(split) => {
            let direction = if matches!(tree[at], egui_dock::Node::Horizontal(_)) {
                Split::Horizontal
            } else {
                Split::Vertical
            };
            let first = harvest(tree, at.left());
            let second = harvest(tree, at.right());
            match (first, second) {
                (Some(first), Some(second)) => {
                    Some(Node::divided(direction, split.fraction, first, second))
                }
                // A split with one live child is that child. egui_dock leaves the structure behind
                // when a leaf empties, and carrying it into the editor's layout would accumulate
                // divisions of nothing across every save.
                (Some(only), None) | (None, Some(only)) => Some(only),
                (None, None) => None,
            }
        }
    }
}

/// The toolkit's docking style, from the editor's own.
///
/// egui_dock's default draws a tab bar with rounded, raised tabs and a visible border. That is the
/// "cards" the surface system forbids, so the style below flattens it: tabs are separated by
/// luminance and by the active tab's underline, and the only stroke is the hairline that separates
/// the tab bar from the panel below it.
#[must_use]
pub fn style(theme: cy_editor_visual::Theme, base: &egui::Style) -> egui_dock::Style {
    use cy_editor_visual::colour::{Semantic, Surface};

    let mut style = egui_dock::Style::from_egui(base);
    let panel = crate::theme::surface(theme, Surface::Panel);
    let window = crate::theme::surface(theme, Surface::Window);
    let line = crate::theme::separator(theme);

    style.dock_area_padding = None;
    style.main_surface_border_stroke = egui::Stroke::NONE;
    style.main_surface_border_rounding = egui::CornerRadius::ZERO;

    style.separator.width = 1.0;
    style.separator.extra = 4.0;
    style.separator.color_idle = window;
    style.separator.color_hovered = line;
    style.separator.color_dragged = crate::theme::role(theme, Semantic::Active);

    style.tab_bar.bg_fill = window;
    style.tab_bar.height = base.spacing.interact_size.y + base.spacing.item_spacing.y;
    style.tab_bar.hline_color = line;
    style.tab_bar.corner_radius = egui::CornerRadius::ZERO;

    style.tab.tab_body.bg_fill = panel;
    style.tab.tab_body.stroke = egui::Stroke::NONE;
    style.tab.tab_body.corner_radius = egui::CornerRadius::ZERO;
    style.tab.active.bg_fill = panel;
    style.tab.active.text_color = crate::theme::role(theme, Semantic::PrimaryText);
    style.tab.active.corner_radius = egui::CornerRadius::ZERO;
    style.tab.active.outline_color = egui::Color32::TRANSPARENT;
    style.tab.inactive.bg_fill = window;
    style.tab.inactive.text_color = crate::theme::role(theme, Semantic::SecondaryText);
    style.tab.inactive.corner_radius = egui::CornerRadius::ZERO;
    style.tab.inactive.outline_color = egui::Color32::TRANSPARENT;
    style.tab.focused.bg_fill = panel;
    style.tab.focused.text_color = crate::theme::role(theme, Semantic::PrimaryText);
    style.tab.focused.corner_radius = egui::CornerRadius::ZERO;
    style.tab.hovered.bg_fill = crate::theme::lifted(theme, Surface::Window, 0.06);
    style.tab.hovered.text_color = crate::theme::role(theme, Semantic::PrimaryText);
    style.tab.hovered.corner_radius = egui::CornerRadius::ZERO;

    style.overlay.selection_color =
        crate::theme::mix(panel, crate::theme::role(theme, Semantic::Active), 0.35);
    style
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_layout_survives_the_round_trip_through_the_toolkit() {
        // The property the whole module exists for: the editor's tree is authoritative, so it must
        // come back the same shape after a trip through the dock manager. If this ever fails, a
        // saved workspace is silently losing structure every time it is restored and re-saved.
        let layout = Layout::scene_editing();
        let returned = from_dock_state(&to_dock_state(&layout));
        assert_eq!(
            returned.panels(),
            layout.panels(),
            "the panels changed across the round trip"
        );
        assert_eq!(
            returned, layout,
            "the layout tree changed across the round trip"
        );
    }

    #[test]
    fn a_floating_window_survives_the_round_trip_as_a_floating_window() {
        let mut layout = Layout::scene_editing();
        let floated = PanelId::new("problems").unwrap();
        layout.float(&floated).unwrap();
        let returned = from_dock_state(&to_dock_state(&layout));
        assert_eq!(returned.panels().len(), layout.panels().len());
        let window = returned
            .window_of(&floated)
            .expect("the floated panel is in some window");
        assert!(
            returned
                .windows()
                .iter()
                .any(|candidate| candidate.id == window && candidate.floating),
            "the panel came back docked rather than floating"
        );
    }

    #[test]
    fn no_display_title_ever_enters_the_dock_state() {
        // Task 1.0.5's whole point. A tab keyed by "Content Browser" breaks the moment the panel is
        // renamed or localised, and the failure is silent: the layout loads and the panel is gone.
        let state = to_dock_state(&Layout::scene_editing());
        let mut titles = cy_editor_interface::panels::PanelTitles::new();
        for panel in Layout::scene_editing().panels() {
            titles.define(
                panel.as_str(),
                cy_editor_interface::shell::panel_title(&panel),
            );
        }
        for (_, key) in state.iter_all_tabs() {
            let title = titles.title(key);
            assert_ne!(
                key.kind(),
                title,
                "the tab is keyed on its display title; renaming or localising the panel would \
                 lose it from every saved workspace"
            );
            // `PanelTitles::is_layout_safe` answers `false` for every string, on purpose: no
            // display text is ever safe to key a layout on. Asserting it here is what turns that
            // paragraph into a check.
            assert!(!cy_editor_interface::panels::PanelTitles::is_layout_safe(
                &title
            ));
        }
    }

    #[test]
    fn the_dock_style_draws_no_cards() {
        // "No cards, no gradients, no heavy shadows." A rounded, outlined tab is a card, and
        // egui_dock's default supplies one.
        let theme = cy_editor_visual::Theme::default();
        let style = style(theme, &egui::Style::default());
        assert_eq!(style.tab.active.corner_radius, egui::CornerRadius::ZERO);
        assert_eq!(style.tab.inactive.corner_radius, egui::CornerRadius::ZERO);
        assert_eq!(style.main_surface_border_stroke, egui::Stroke::NONE);
        assert_eq!(style.tab.active.outline_color, egui::Color32::TRANSPARENT);
    }
}
