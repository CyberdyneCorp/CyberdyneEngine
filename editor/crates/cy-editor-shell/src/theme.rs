//! The visual language, expressed as an `egui::Style`. Task 1.7.
//!
//! This module is the whole of the colour-and-metric adapter, and it is deliberately the only place
//! in the workspace where a `cy_editor_visual` value becomes an `egui` value. Nothing else in this
//! crate builds a colour: it asks [`colour`] or [`role`] for one, and those ask
//! `cy_editor_visual::colour::Theme`, which is the single source `editor-visual-language` requires.
//!
//! --- WHY AN ADAPTER RATHER THAN A PALETTE ---------------------------------------------------------
//!
//! Because the failure this capability exists to prevent is cumulative and local. A panel that
//! needs "a slightly brighter grey for this header" and writes `Color32::from_gray(40)` is
//! reasonable in isolation and is how an editor stops having a colour system. Every such need is
//! answered here by a [`Surface`] or a [`Semantic`], and if neither answers it then the right move
//! is to argue for a new role in `cy-editor-visual` rather than to invent one at a call site.
//!
//! --- THE THREE THINGS THAT ARE NOT NEGOTIABLE ------------------------------------------------------
//!
//! **Surfaces are luminance steps, not borders.** `editor-visual-language`: panels are separated by
//! "small luminance steps, spacing, and subtle separators rather than by borders, cards, or
//! shadows". So [`style`] sets both shadows to nothing, gives frames a corner radius small enough to
//! read as a soft edge rather than as a card, and paints every widget's *background* from a surface
//! rather than drawing it an outline.
//!
//! **The viewport carries the screen's colour.** Every panel is charcoal, which means the interface
//! must contain no large saturated area at all. There is one accent in this file — the selection
//! gold on a selected row — and it is a fill at low opacity plus a text colour, not a block.
//!
//! **Tabular figures.** "Numeric values SHALL use tabular figures so that columns of numbers align."
//! The interface font eframe ships has proportional digits, so a column of them does not align, and
//! there is no font substitution that makes a proportional face tabular. What we can do honestly is
//! set numeric runs in the monospaced family, which is tabular by construction — so this module
//! defines a [`NUMERIC`] text style and every number in this crate is drawn with it. That is the
//! same decision `cy_editor_visual::density::tabular_figures` describes, made operative.

use cy_editor_visual::colour::{Mode, Rgb, Semantic, Surface, Theme};
use cy_editor_visual::density::{Metrics, TextRole};

/// The text style numeric values are drawn in.
///
/// A named style rather than a call to `FontId::monospace` at each site, so that "which text is
/// numeric" is a decision recorded once per widget and visible in a diff.
pub const NUMERIC: &str = "numeric";

/// The text style secondary and derived information is drawn in.
pub const SECONDARY: &str = "secondary";

/// A layout distance in points as the toolkit's margin unit.
///
/// egui stores margins as `i8`. Every margin this editor computes comes from
/// `cy_editor_visual::Metrics`, whose largest value is a padding at the maximum interface scale —
/// about thirty points — so the clamp below never fires in practice. It is written as a saturating
/// conversion anyway, because "it cannot overflow today" is a property of a table somebody may edit,
/// and a wrapped margin is a panel drawn inside out.
#[expect(
    clippy::cast_possible_truncation,
    reason = "clamped into i8's range on the line above, and rounded rather than truncated"
)]
#[must_use]
pub fn margin(points: f32) -> i8 {
    points.round().clamp(0.0, 127.0) as i8
}

/// A count of rows or items as a distance in points.
///
/// The precision an `f32` loses above 2^24 rows is a fraction of a pixel in a list nobody can scroll
/// through by hand — the same argument `cy_editor_interface::virtualise::content_height` makes, and
/// it is made once more here because this is the other place a row index becomes a coordinate.
#[expect(
    clippy::cast_precision_loss,
    reason = "a sub-pixel error beyond 2^24 rows, in a list no pointer can reach"
)]
#[must_use]
pub fn points(count: usize) -> f32 {
    count as f32
}

/// A visual-language colour as the toolkit's.
///
/// The one conversion. `containment.rs` is what keeps it the only one.
#[must_use]
pub fn colour(value: Rgb) -> egui::Color32 {
    egui::Color32::from_rgb(value.red, value.green, value.blue)
}

/// The colour of a semantic role in a theme.
#[must_use]
pub fn role(theme: Theme, semantic: Semantic) -> egui::Color32 {
    colour(theme.colour(semantic))
}

/// The colour of a surface in a theme.
#[must_use]
pub fn surface(theme: Theme, surface: Surface) -> egui::Color32 {
    colour(theme.surface(surface))
}

/// A colour blended toward another by `amount`, in 0..=1.
///
/// Used for separators and for the two states a widget has beyond its resting one. A blend rather
/// than a fifth surface, because `editor-visual-language` fixes the surface set at four and a hover
/// is not a surface — it is the same surface, lifted.
#[must_use]
pub fn mix(from: egui::Color32, to: egui::Color32, amount: f32) -> egui::Color32 {
    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_sign_loss,
        reason = "both ends are u8 and the blend is between them, so the result is in 0..=255"
    )]
    fn blend(first: u8, second: u8, amount: f32) -> u8 {
        let first = f32::from(first);
        let second = f32::from(second);
        (first + (second - first) * amount).round() as u8
    }
    let amount = amount.clamp(0.0, 1.0);
    let blend = |first: u8, second: u8| blend(first, second, amount);
    egui::Color32::from_rgb(
        blend(from.r(), to.r()),
        blend(from.g(), to.g()),
        blend(from.b(), to.b()),
    )
}

/// The separator colour for a theme: the smallest step that reads as a boundary.
///
/// `editor-visual-language` allows "subtle separators" beside luminance steps and forbids borders.
/// The difference is not decorative: a separator is one hairline between two surfaces, and a border
/// is an outline around a thing. This returns the former, and nothing in this crate draws the
/// latter.
#[must_use]
pub fn separator(theme: Theme) -> egui::Color32 {
    let panel = surface(theme, Surface::Panel);
    let text = role(theme, Semantic::PrimaryText);
    mix(panel, text, 0.10)
}

/// The fill of a hovered row or button: the resting surface, lifted.
#[must_use]
pub fn lifted(theme: Theme, base: Surface, amount: f32) -> egui::Color32 {
    let toward = match theme.mode {
        Mode::Dark => egui::Color32::WHITE,
        Mode::Light => egui::Color32::BLACK,
    };
    mix(surface(theme, base), toward, amount)
}

/// The fill behind a selected row.
///
/// Gold, at the opacity that leaves the text on it legible and the row still reading as charcoal.
/// `editor-visual-language`'s selection rule is about the viewport — "a thin gold outline that does
/// not glow" — and its intent carries here: the selection is identified, and the thing selected is
/// still judgeable.
#[must_use]
pub fn selected_fill(theme: Theme) -> egui::Color32 {
    mix(
        surface(theme, Surface::Panel),
        role(theme, Semantic::Selection),
        0.22,
    )
}

/// The fill of a control that floats over the viewport.
///
/// The panel surface, translucent. `editor-visual-language` puts the projection, render mode and
/// show flags *in* the viewport rather than in a second toolbar, and a control that floats has to
/// stay legible without hiding the render it is judged against — so it is the same charcoal as a
/// docked panel, at an opacity that lets the scene through. Not a gradient, not a shadow, not a
/// blur: the surface system has one mechanism and this is it, with an alpha.
#[must_use]
pub fn overlay_fill(theme: Theme) -> egui::Color32 {
    let panel = surface(theme, Surface::Panel);
    egui::Color32::from_rgba_unmultiplied(panel.r(), panel.g(), panel.b(), 0xDC)
}

/// The complete style for a theme at a density.
///
/// Everything a panel in this crate needs, so that a panel body is layout and content and never a
/// colour decision.
#[must_use]
pub fn style(theme: Theme, metrics: Metrics) -> egui::Style {
    let mut style = egui::Style {
        visuals: visuals(theme),
        ..egui::Style::default()
    };
    apply_spacing(&mut style, metrics);
    apply_text(&mut style, metrics);
    // Zero, and it is a requirement rather than a preference: `INPUT_DELAY_BUDGET` is
    // `Duration::ZERO` because "WHEN a panel opens THEN interaction SHALL be possible immediately".
    // egui's animation time gates nothing, but a collapsing section that takes 150 ms to open is
    // exactly the "animation that delays interaction" the rule names.
    style.animation_time = 0.0;
    style
}

/// The colours.
fn visuals(theme: Theme) -> egui::Visuals {
    let dark = theme.mode == Mode::Dark;
    let mut visuals = if dark {
        egui::Visuals::dark()
    } else {
        egui::Visuals::light()
    };

    let panel = surface(theme, Surface::Panel);
    let raised = surface(theme, Surface::Raised);
    let sunken = surface(theme, Surface::Sunken);
    let primary = role(theme, Semantic::PrimaryText);
    let secondary = role(theme, Semantic::SecondaryText);
    let line = separator(theme);

    visuals.dark_mode = dark;
    visuals.panel_fill = panel;
    visuals.window_fill = raised;
    visuals.extreme_bg_color = sunken;
    visuals.faint_bg_color = lifted(theme, Surface::Panel, 0.03);
    visuals.code_bg_color = sunken;
    visuals.override_text_color = Some(primary);
    visuals.weak_text_color = Some(secondary);
    visuals.hyperlink_color = role(theme, Semantic::Active);
    visuals.warn_fg_color = role(theme, Semantic::Warning);
    visuals.error_fg_color = role(theme, Semantic::Error);

    // No shadows, anywhere. The surface system is luminance and spacing; a shadow is the "card"
    // this specification forbids by another name.
    visuals.window_shadow = egui::epaint::Shadow::NONE;
    visuals.popup_shadow = egui::epaint::Shadow::NONE;
    visuals.window_stroke = egui::Stroke::new(1.0, line);
    visuals.window_corner_radius = egui::CornerRadius::same(4);
    visuals.menu_corner_radius = egui::CornerRadius::same(4);
    visuals.striped = false;
    visuals.button_frame = true;
    visuals.collapsing_header_frame = false;
    visuals.indent_has_left_vline = true;

    // Blue is "active, focused, informational", so it is what a focus ring is made of. Selection —
    // gold — is a fill and a text colour, applied per row where a selection is being shown, because
    // egui's `selection` is also the text-edit highlight and gold text highlight would collide with
    // the meaning "this object is selected".
    visuals.selection = egui::style::Selection {
        bg_fill: mix(sunken, role(theme, Semantic::Active), 0.45),
        stroke: egui::Stroke::new(1.0, primary),
    };

    visuals.widgets = widgets(theme);
    visuals
}

/// The three states a widget has, plus the two egui adds for a non-interactive one and an open menu.
fn widgets(theme: Theme) -> egui::style::Widgets {
    let panel = surface(theme, Surface::Panel);
    let raised = surface(theme, Surface::Raised);
    let window = surface(theme, Surface::Window);
    let primary = role(theme, Semantic::PrimaryText);
    let secondary = role(theme, Semantic::SecondaryText);
    let line = separator(theme);
    let mut visuals = egui::style::Widgets::default();

    let corner = egui::CornerRadius::same(3);
    visuals.noninteractive = egui::style::WidgetVisuals {
        bg_fill: panel,
        weak_bg_fill: panel,
        bg_stroke: egui::Stroke::new(1.0, line),
        corner_radius: corner,
        fg_stroke: egui::Stroke::new(1.0, secondary),
        expansion: 0.0,
    };
    visuals.inactive = egui::style::WidgetVisuals {
        bg_fill: raised,
        weak_bg_fill: lifted(theme, Surface::Panel, 0.05),
        bg_stroke: egui::Stroke::NONE,
        corner_radius: corner,
        fg_stroke: egui::Stroke::new(1.0, primary),
        expansion: 0.0,
    };
    visuals.hovered = egui::style::WidgetVisuals {
        bg_fill: lifted(theme, Surface::Raised, 0.08),
        weak_bg_fill: lifted(theme, Surface::Panel, 0.10),
        bg_stroke: egui::Stroke::new(1.0, line),
        corner_radius: corner,
        fg_stroke: egui::Stroke::new(1.0, primary),
        // Zero: a control that grows under the pointer moves every control beside it, which is the
        // "context changes position" failure `editor-ui-ux` names, at widget scale.
        expansion: 0.0,
    };
    visuals.active = egui::style::WidgetVisuals {
        bg_fill: lifted(theme, Surface::Raised, 0.14),
        weak_bg_fill: lifted(theme, Surface::Panel, 0.16),
        bg_stroke: egui::Stroke::new(1.0, role(theme, Semantic::Active)),
        corner_radius: corner,
        fg_stroke: egui::Stroke::new(1.0, primary),
        expansion: 0.0,
    };
    visuals.open = egui::style::WidgetVisuals {
        bg_fill: window,
        weak_bg_fill: lifted(theme, Surface::Panel, 0.08),
        bg_stroke: egui::Stroke::new(1.0, line),
        corner_radius: corner,
        fg_stroke: egui::Stroke::new(1.0, primary),
        expansion: 0.0,
    };
    visuals
}

/// The metrics: row height, gaps, insets, and the smallest pointer target.
fn apply_spacing(style: &mut egui::Style, metrics: Metrics) {
    let gap = metrics.gap();
    let padding = metrics.padding();
    let spacing = &mut style.spacing;
    spacing.item_spacing = egui::vec2(gap, gap * 0.5);
    spacing.window_margin = egui::Margin::same(margin(padding));
    spacing.menu_margin = egui::Margin::same(margin(padding * 0.5));
    spacing.button_padding = egui::vec2(gap, gap * 0.35);
    spacing.indent = metrics.icon();
    spacing.icon_width = metrics.icon();
    spacing.icon_width_inner = metrics.icon() * 0.6;
    spacing.icon_spacing = gap * 0.5;
    spacing.interact_size = egui::vec2(metrics.hit_target(), metrics.hit_target());
    spacing.slider_width = metrics.row() * 6.0;
    spacing.combo_width = metrics.row() * 5.0;
    spacing.text_edit_width = metrics.row() * 8.0;
    spacing.scroll = egui::style::ScrollStyle::solid();
}

/// The text sizes, from the density's own table.
fn apply_text(style: &mut egui::Style, metrics: Metrics) {
    use egui::{FontFamily, FontId, TextStyle};
    let sans = FontFamily::Proportional;
    let mono = FontFamily::Monospace;
    style.text_styles = [
        (
            TextStyle::Body,
            FontId::new(metrics.text(TextRole::Body), sans.clone()),
        ),
        (
            TextStyle::Button,
            FontId::new(metrics.text(TextRole::Body), sans.clone()),
        ),
        (
            TextStyle::Small,
            FontId::new(metrics.text(TextRole::Secondary), sans.clone()),
        ),
        (
            TextStyle::Heading,
            FontId::new(metrics.text(TextRole::Title), sans.clone()),
        ),
        (
            TextStyle::Monospace,
            FontId::new(metrics.text(TextRole::Body), mono.clone()),
        ),
        (
            TextStyle::Name(NUMERIC.into()),
            FontId::new(metrics.text(TextRole::Body), mono),
        ),
        (
            TextStyle::Name(SECONDARY.into()),
            FontId::new(metrics.text(TextRole::Secondary), sans),
        ),
    ]
    .into();
}

#[cfg(test)]
mod tests {
    use super::*;
    use cy_editor_visual::colour::{MINIMUM_SURFACE_STEP, Vision};
    use cy_editor_visual::density::{Density, Scale};

    #[test]
    fn every_shipped_theme_produces_a_style_whose_surfaces_are_still_distinguishable() {
        // The adapter is where a theme could quietly lose the property `cy-editor-visual` checks:
        // panels are told apart by luminance, so two surfaces mapping to one colour would remove
        // every panel boundary at once and the fix reached for would be a border.
        for theme in Theme::ALL {
            let style = style(theme, Metrics::default());
            let panel = style.visuals.panel_fill;
            let window = style.visuals.window_fill;
            assert_ne!(panel, window, "{theme:?} paints panel and window alike");
            let luminance =
                |value: egui::Color32| Rgb::new(value.r(), value.g(), value.b()).luminance();
            assert!(
                (luminance(panel) - luminance(window)).abs() >= MINIMUM_SURFACE_STEP,
                "{theme:?}: the panel and window surfaces differ by less than the visual language's \
                 minimum step"
            );
        }
    }

    #[test]
    fn nothing_in_the_style_carries_a_shadow() {
        // "No gradients, no glass, no heavy drop shadows, no nested cards."
        for theme in Theme::ALL {
            let visuals = style(theme, Metrics::default()).visuals;
            assert_eq!(visuals.window_shadow, egui::epaint::Shadow::NONE);
            assert_eq!(visuals.popup_shadow, egui::epaint::Shadow::NONE);
        }
    }

    #[test]
    fn a_denser_density_produces_shorter_rows_and_smaller_gaps() {
        let compact = style(
            Theme::default(),
            Metrics::new(Density::Compact, Scale::default()),
        );
        let comfortable = style(
            Theme::default(),
            Metrics::new(Density::Comfortable, Scale::default()),
        );
        assert!(compact.spacing.item_spacing.x < comfortable.spacing.item_spacing.x);
        assert!(compact.spacing.interact_size.y < comfortable.spacing.interact_size.y);
    }

    #[test]
    fn numbers_are_set_in_a_family_whose_figures_are_tabular() {
        // The honest form of "tabular figures" with the fonts eframe ships. If this ever becomes a
        // proportional family, columns of numbers stop aligning and nothing else would notice.
        let style = style(Theme::default(), Metrics::default());
        let numeric = style
            .text_styles
            .get(&egui::TextStyle::Name(NUMERIC.into()))
            .expect("the numeric text style is defined");
        assert_eq!(numeric.family, egui::FontFamily::Monospace);
    }

    #[test]
    fn the_red_green_safe_palette_reaches_the_style_rather_than_being_dropped_in_it() {
        let standard = style(Theme::new(Mode::Dark, Vision::Standard), Metrics::default());
        let safe = style(
            Theme::new(Mode::Dark, Vision::RedGreenSafe),
            Metrics::default(),
        );
        // Error is the same in both; "live" is the role that moves, and it is not on the style, so
        // the check that the vision reaches the adapter at all is that the two themes differ
        // somewhere a panel reads.
        assert_eq!(standard.visuals.error_fg_color, safe.visuals.error_fg_color);
        assert_ne!(
            role(Theme::new(Mode::Dark, Vision::Standard), Semantic::Live),
            role(Theme::new(Mode::Dark, Vision::RedGreenSafe), Semantic::Live),
        );
    }
}
