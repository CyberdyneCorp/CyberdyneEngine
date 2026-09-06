//! The forbidden visual patterns, each with the check that catches it.
//!
//! `editor-visual-language` ends with twelve patterns that "SHALL NOT appear", and requires that
//! "each SHALL be checkable". This module is the answer to *where*, and it is written as a table
//! because the honest answer is not "here" for all of them: some are refused by a type having no
//! field to express them, some by a function in this crate, and some by a test in
//! `cy-editor-interface` where the thing being forbidden is an interaction rather than an
//! appearance.
//!
//! [`FORBIDDEN`] carries all twelve with the check that settles each, and
//! [`every_forbidden_pattern_names_a_check`] fails if a row's check is empty — so a thirteenth
//! pattern added to the specification cannot be recorded here without somebody deciding how it is
//! caught.
//!
//! --- WHY "CHECKABLE BY ABSENCE" IS A REAL ANSWER ---------------------------------------------------
//!
//! Four of the twelve are enforced by the type system: there is no gradient, no shadow, no glow and
//! no second toolbar in this crate, so a panel cannot ask for one. That is a stronger guarantee than
//! a lint, because a lint can be silenced and a field that does not exist cannot be set — and it is
//! the same argument `cy-editor-documents` makes for its write token.

use cy_editor_core::problem::{Problem, Result};

use crate::axis::{self, Axis};
use crate::colour::{Rgb, Semantic, Surface, Theme};
use crate::gizmo::Form;
use crate::vocabulary;

/// One forbidden pattern and where it is caught.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Forbidden {
    /// The pattern, in the specification's own words.
    pub pattern: &'static str,
    /// What settles it: a function here, a type with no such field, or a test elsewhere.
    pub check: &'static str,
}

/// The twelve, and what catches each.
pub const FORBIDDEN: [Forbidden; 12] = [
    Forbidden {
        pattern: "A panel with a saturated fill, gradient, or accent that encodes nothing",
        check: "rules::check_panel_style, and PanelStyle has no gradient to set",
    },
    Forbidden {
        pattern: "Colour used for variety where the semantic vocabulary assigns it a meaning",
        check: "colour::no_role_shares_a_hue, and Semantic is the only source of a colour",
    },
    Forbidden {
        pattern: "An axis presented in any mapping other than X red, Y green, Z blue",
        check: "rules::check_axis_mapping, over the one axis::colour in the workspace",
    },
    Forbidden {
        pattern: "A view-orientation widget carrying rings, boxes, or arrows",
        check: "orientation::check, over Form::manipulates",
    },
    Forbidden {
        pattern: "A second full-width toolbar between the header and the viewport",
        check: "chrome::Region has one Toolbar and chrome::viewport_height takes no overlay count",
    },
    Forbidden {
        pattern: "A selection indicator that glows, blooms, or obscures the surface beneath it",
        check: "selection::check, and SelectionAppearance has no glow field",
    },
    Forbidden {
        pattern: "An icon that reproduces another engine's recognisable symbol",
        check: "rules::check_icon_name, over the vocabulary's borrowed terms",
    },
    Forbidden {
        pattern: "Another engine's product vocabulary in interface text",
        check: "vocabulary::check_label",
    },
    Forbidden {
        pattern: "A contextual behaviour that moves, opens, or closes a panel the user did not ask \
                  for",
        check: "cy-editor-interface: docking has no selection input, asserted in shell's tests",
    },
    Forbidden {
        pattern: "A new subsystem's properties appended to the default inspector view without a \
                  disclosure decision",
        check: "cy-editor-interface: every generated row carries a Disclosure, asserted in inspector",
    },
    Forbidden {
        pattern: "A modal dialog for information that could be ambient status",
        check: "cy-editor-interface: Modal::new requires a Decision, asserted in notifications",
    },
    Forbidden {
        pattern: "A generic file icon where the engine could render a preview",
        check: "cy-editor-interface: thumbnails::Thumbnail has no generic variant; a pending one carries its Kind",
    },
];

/// How a panel is painted. **There is no gradient, no shadow and no border.**
///
/// The absences are the requirement: "Panels SHALL be differentiated by small luminance steps,
/// spacing, and subtle separators rather than by borders, cards, or shadows. The system SHALL NOT
/// use large gradients, glass or blur effects, heavy drop shadows, ornamental rules, or deeply
/// nested cards."
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct PanelStyle {
    /// Which surface it sits on.
    pub surface: Surface,
    /// An accent, and the state it encodes. `None` for the ordinary case, which is almost all of
    /// them.
    pub accent: Option<Accent>,
    /// The corner radius in logical pixels. Subtle; see [`PanelStyle::MAXIMUM_RADIUS`].
    pub radius: f32,
}

impl Default for PanelStyle {
    fn default() -> Self {
        Self {
            surface: Surface::Panel,
            accent: None,
            radius: 2.0,
        }
    }
}

impl PanelStyle {
    /// The largest corner radius that still reads as "subtle rounding" rather than as a card.
    pub const MAXIMUM_RADIUS: f32 = 4.0;
}

/// A colour on a panel, and the state it carries.
///
/// The pairing is the mechanism: an accent cannot be constructed without saying what it encodes, so
/// "an accent that encodes nothing" is not a thing a panel can have.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Accent {
    /// The role, which is where the colour comes from.
    pub role: Semantic,
    /// The state it encodes — "unsaved", "live", "3 problems". Never empty.
    pub encodes: &'static str,
}

/// Refuse a panel style that decorates rather than encodes.
///
/// "WHEN a panel introduces a coloured header, gradient, or accent that encodes nothing THEN it
/// SHALL be flagged against this requirement."
pub fn check_panel_style(style: PanelStyle) -> Result<()> {
    if let Some(accent) = style.accent
        && accent.encodes.trim().is_empty()
    {
        return Err(Problem::new(
            "give this panel an accent colour",
            "the accent encodes nothing, which makes it decoration competing with the viewport",
        )
        .with_remedy(
            "say which state the colour means, or use luminance and spacing to separate the panel \
             from its neighbour",
        ));
    }
    if style.radius > PanelStyle::MAXIMUM_RADIUS {
        return Err(Problem::new(
            "round this panel's corners",
            format!(
                "{} logical pixels reads as a card, and cards nest",
                style.radius
            ),
        )
        .with_remedy(format!(
            "keep it at or below {}; hierarchy comes from luminance steps, not from containers",
            PanelStyle::MAXIMUM_RADIUS
        )));
    }
    Ok(())
}

/// Refuse an axis mapping that is not the industry's one.
///
/// The proposal is a set of three colours in axis order, which is what a plugin's debug
/// visualisation amounts to. It is compared against [`axis::colour`] rather than against a copy,
/// because a copy is the thing that drifts.
pub fn check_axis_mapping(theme: Theme, proposed: [Rgb; 3]) -> Result<()> {
    for (index, axis) in Axis::ALL.into_iter().enumerate() {
        let expected = axis::colour(axis, theme);
        if proposed[index] != expected {
            return Err(Problem::new(
                format!(
                    "present the {} axis in {}",
                    axis.label(),
                    proposed[index].to_hex()
                ),
                format!(
                    "the editor's mapping paints it {}, and X red, Y green, Z blue is the one \
                     convention shared with every other tool the user has open",
                    expected.to_hex()
                ),
            )
            .with_remedy(
                "ask cy_editor_visual::axis::colour for the colour rather than choosing one; the \
                 mapping is deliberately not remappable",
            ));
        }
    }
    Ok(())
}

/// Refuse an icon whose name says it reproduces another engine's symbol.
///
/// A name check rather than an image check, because at M5 there are no images — and because the
/// failure this catches is the real one: an icon added as `blueprint.svg` is an icon somebody drew
/// by looking at Unreal.
pub fn check_icon_name(name: &str) -> Result<()> {
    let readable = name.replace(['-', '_', '.'], " ");
    vocabulary::check_label(&readable).map_err(|problem| {
        Problem::new(
            format!("add the icon {name:?}"),
            format!(
                "its name is another engine's vocabulary, which is where a borrowed icon set starts \
                 ({})",
                problem.because
            ),
        )
        .with_remedy(
            "name it for what the engine calls the thing; an icon that reproduces another engine's \
             recognisable symbol makes the product read as a derivative of it",
        )
    })
}

/// What the ambient performance overlay may show.
///
/// "Frame cost SHALL be observable without opening a profiler: a compact overlay MAY present frame
/// rate, frame time, draw time, GPU time and memory. Detailed profiling — per-subsystem graphs over
/// time — belongs to the profiler panel, not the overlay."
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum FrameMetric {
    /// Frames per second.
    Rate,
    /// Milliseconds per frame.
    Time,
    /// Milliseconds spent submitting draws.
    Draw,
    /// Milliseconds spent on the device.
    Gpu,
    /// Bytes resident.
    Memory,
}

impl FrameMetric {
    /// The five the overlay may show, and there is no sixth.
    pub const ALL: [FrameMetric; 5] = [
        FrameMetric::Rate,
        FrameMetric::Time,
        FrameMetric::Draw,
        FrameMetric::Gpu,
        FrameMetric::Memory,
    ];

    /// The label, which states the condition in text as well as in a number.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            FrameMetric::Rate => "FPS",
            FrameMetric::Time => "frame",
            FrameMetric::Draw => "draw",
            FrameMetric::Gpu => "GPU",
            FrameMetric::Memory => "memory",
        }
    }
}

/// Refuse a proposed addition to the ambient overlay that belongs in the profiler.
///
/// "WHEN a new per-subsystem breakdown is proposed for the overlay THEN it SHALL be placed in the
/// profiler panel instead." The overlay's own metrics are an enum, so the only way to propose one is
/// to name it — and a named metric that is not one of the five is exactly the breakdown.
pub fn check_performance_overlay(proposed: &str) -> Result<()> {
    if FrameMetric::ALL
        .iter()
        .any(|metric| metric.label().eq_ignore_ascii_case(proposed))
    {
        return Ok(());
    }
    Err(Problem::new(
        format!("add {proposed:?} to the ambient performance overlay"),
        "the overlay answers whether this frame is affordable; a per-subsystem breakdown answers \
         why, which is a different question and a different panel",
    )
    .with_remedy("put it in the profiler panel, where it can be graphed over time"))
}

/// Refuse a viewport overlay set that has acquired a manipulation shape.
///
/// A thin forwarding to [`crate::orientation::check`], so that a viewport assembling its chrome has
/// one place to ask rather than knowing which module owns which rule.
pub fn check_viewport_overlays(forms: &[Form]) -> Result<()> {
    crate::orientation::check(forms)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::colour::Mode;

    #[test]
    fn every_forbidden_pattern_names_a_check() {
        assert_eq!(
            FORBIDDEN.len(),
            12,
            "the specification lists twelve; a thirteenth is a specification change and belongs \
             here with the check that catches it"
        );
        for entry in FORBIDDEN {
            assert!(!entry.pattern.trim().is_empty());
            assert!(
                !entry.check.trim().is_empty(),
                "{:?} has no check, which makes 'each SHALL be checkable' untrue",
                entry.pattern
            );
        }
    }

    #[test]
    fn a_coloured_title_bar_that_encodes_nothing_is_flagged() {
        // The specification's closing scenario: "WHEN a panel is proposed with a coloured title bar
        // to distinguish it from its neighbours THEN it SHALL be flagged against this requirement,
        // and luminance and spacing SHALL be used instead."
        let problem = check_panel_style(PanelStyle {
            accent: Some(Accent {
                role: Semantic::Active,
                encodes: "  ",
            }),
            ..PanelStyle::default()
        })
        .unwrap_err();
        assert!(problem.remedy.as_deref().unwrap().contains("luminance"));
    }

    #[test]
    fn an_accent_that_encodes_a_state_is_allowed() {
        check_panel_style(PanelStyle {
            accent: Some(Accent {
                role: Semantic::Warning,
                encodes: "this document has unresolved problems",
            }),
            ..PanelStyle::default()
        })
        .unwrap();
    }

    #[test]
    fn a_card_stack_is_rejected() {
        let problem = check_panel_style(PanelStyle {
            radius: 12.0,
            ..PanelStyle::default()
        })
        .unwrap_err();
        assert!(problem.because.contains("card"), "{problem}");
    }

    #[test]
    fn a_plugin_that_invents_its_own_axis_colours_is_flagged() {
        // "WHEN a plugin adds an axis-aligned debug display THEN it SHALL use the same three hues
        // rather than choosing its own."
        let theme = Theme::default();
        let correct = [
            axis::colour(Axis::X, theme),
            axis::colour(Axis::Y, theme),
            axis::colour(Axis::Z, theme),
        ];
        check_axis_mapping(theme, correct).unwrap();

        let swapped = [correct[2], correct[1], correct[0]];
        let problem = check_axis_mapping(theme, swapped).unwrap_err();
        assert!(problem.remedy.as_deref().unwrap().contains("remappable"));
    }

    #[test]
    fn the_light_theme_is_not_a_remapping() {
        let light = Theme::new(Mode::Light, crate::colour::Vision::Standard);
        let its_own = [
            axis::colour(Axis::X, light),
            axis::colour(Axis::Y, light),
            axis::colour(Axis::Z, light),
        ];
        check_axis_mapping(light, its_own).unwrap();
    }

    #[test]
    fn a_borrowed_icon_name_is_rejected() {
        let problem = check_icon_name("blueprint-node.svg").unwrap_err();
        assert!(problem.because.contains("another engine"), "{problem}");
        check_icon_name("script-graph.svg").unwrap();
    }

    #[test]
    fn the_overlay_does_not_become_a_profiler() {
        for metric in FrameMetric::ALL {
            check_performance_overlay(metric.label()).unwrap();
        }
        let problem = check_performance_overlay("navigation build time per region").unwrap_err();
        assert!(problem.remedy.as_deref().unwrap().contains("profiler"));
    }
}
