//! X is red, Y is green, Z is blue — everywhere, and not user-remappable.
//!
//! `editor-visual-language`: "The mapping **X = red, Y = green, Z = blue** SHALL hold everywhere an
//! axis is presented: transform gizmos, rotation rings, scale handles, the view-orientation widget,
//! vector property fields in the inspector, coordinate readouts, and debug visualisation ... This
//! mapping SHALL NOT be user-remappable, because it is the one colour convention shared with every
//! other tool in the industry."
//!
//! There is therefore exactly one function that answers "what colour is this axis", it takes no
//! preference and no theme override, and there is nowhere else in the workspace to get an axis
//! colour from. A plugin that wanted its own would have to invent one, which is what the scenario
//! "a new visualisation adopts the mapping" is about, and a review can see an invented colour.
//!
//! --- WHY THE HUE IS FIXED AND THE LIGHTNESS IS NOT ------------------------------------------------
//!
//! The light theme cannot paint the dark theme's `#F2615C` on a white panel and stay legible, so the
//! two themes use two lightnesses of the *same hue*. That is not a remapping — the requirement is
//! about which hue means which axis — and [`hue_is_the_same_in_every_theme`] is the test that keeps
//! the distinction honest: a change that drifted the light theme's Y towards yellow would fail it.
//!
//! --- WHY A COLOUR-BLIND PALETTE DOES NOT MOVE THEM ------------------------------------------------
//!
//! It would cost more than it bought. The axis triad is never presented without its axis *label* —
//! `X`, `Y`, `Z` on the inspector's vector fields, the same three letters on the orientation
//! widget's stubs — so the meaning already survives without hue discrimination, and moving the hues
//! would break the one convention shared with every other tool the user has open.

use crate::colour::{Mode, Rgb, Theme};

/// A principal axis.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum Axis {
    /// The first axis. Red.
    X,
    /// The second axis. Green.
    Y,
    /// The third axis. Blue.
    Z,
}

impl Axis {
    /// The three axes, in the order a vector field lays them out.
    pub const ALL: [Axis; 3] = [Axis::X, Axis::Y, Axis::Z];

    /// The label, which is the encoding that survives when the colour cannot be seen.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            Axis::X => "X",
            Axis::Y => "Y",
            Axis::Z => "Z",
        }
    }

    /// The lane this axis occupies in a `Vec3` or a quaternion's vector part.
    #[must_use]
    pub const fn lane(self) -> usize {
        match self {
            Axis::X => 0,
            Axis::Y => 1,
            Axis::Z => 2,
        }
    }

    /// The axis a lane belongs to, for an inspector labelling a vector field it was handed.
    #[must_use]
    pub const fn of_lane(lane: usize) -> Option<Self> {
        match lane {
            0 => Some(Axis::X),
            1 => Some(Axis::Y),
            2 => Some(Axis::Z),
            _ => None,
        }
    }
}

/// The colour of an axis in a theme. **The only source of one in the workspace.**
///
/// Takes a [`Theme`] for its mode and ignores its [`crate::colour::Vision`], for the reason in the
/// module note: the axis triad is not repainted for colour vision, because it is labelled.
#[must_use]
pub const fn colour(axis: Axis, theme: Theme) -> Rgb {
    let value = match (theme.mode, axis) {
        (Mode::Dark, Axis::X) => 0xF2_61_5C,
        (Mode::Dark, Axis::Y) => 0x4F_C2_6B,
        (Mode::Dark, Axis::Z) => 0x5B_9B_F8,
        (Mode::Light, Axis::X) => 0xC0_33_2F,
        (Mode::Light, Axis::Y) => 0x1F_7A_38,
        (Mode::Light, Axis::Z) => 0x1B_5F_C1,
    };
    Rgb::hex(value)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::colour::{MINIMUM_CONTRAST, Surface, Vision};

    #[test]
    fn the_inspector_and_the_viewport_ask_the_same_function() {
        // The specification's scenario: "WHEN a user drags the red handle in the viewport THEN the
        // red-labelled X field in the inspector SHALL be the value that changes."
        //
        // What a test can hold is the half that makes that possible: there is one function, so a
        // handle and a field cannot be painted from two tables that drifted apart. The other half —
        // that the field the user sees change is the one they dragged — belongs to the inspector,
        // and `cy-editor-interface` asserts it against a real vector field.
        let theme = Theme::default();
        for axis in Axis::ALL {
            assert_eq!(colour(axis, theme), colour(axis, theme));
        }
        assert_ne!(colour(Axis::X, theme), colour(Axis::Y, theme));
        assert_ne!(colour(Axis::Y, theme), colour(Axis::Z, theme));
    }

    #[test]
    fn the_hue_is_the_same_in_every_theme() {
        /// A hue may move this far between themes and still be the same colour to a user; more
        /// than this and the light theme has quietly remapped an axis.
        const TOLERANCE: f64 = 8.0;

        for axis in Axis::ALL {
            let dark = colour(axis, Theme::new(Mode::Dark, Vision::Standard)).hue();
            let light = colour(axis, Theme::new(Mode::Light, Vision::Standard)).hue();
            let drift = (dark - light).abs().min(360.0 - (dark - light).abs());
            assert!(
                drift <= TOLERANCE,
                "{axis:?} is hue {dark:.0} in the dark theme and {light:.0} in the light one, \
                 which is a remapping rather than a lightness"
            );
        }
    }

    #[test]
    fn a_colour_blind_palette_does_not_remap_the_axes() {
        for axis in Axis::ALL {
            assert_eq!(
                colour(axis, Theme::new(Mode::Dark, Vision::Standard)),
                colour(axis, Theme::new(Mode::Dark, Vision::RedGreenSafe)),
                "the axes are labelled, and the mapping is not the editor's to change"
            );
        }
    }

    #[test]
    fn every_axis_is_legible_on_the_panel_its_field_sits_on() {
        // An axis colour is text as often as it is a handle: the `X` label on a vector field is
        // painted in it, so it is held to the same contrast target as the semantic roles.
        for theme in Theme::ALL {
            for axis in Axis::ALL {
                let ratio = colour(axis, theme).contrast(theme.surface(Surface::Panel));
                assert!(
                    ratio >= MINIMUM_CONTRAST,
                    "{theme:?} {axis:?} is {ratio:.2}:1 on a panel"
                );
            }
        }
    }

    #[test]
    fn a_lane_and_an_axis_agree_in_both_directions() {
        for axis in Axis::ALL {
            assert_eq!(Axis::of_lane(axis.lane()), Some(axis));
        }
        assert_eq!(Axis::of_lane(3), None, "a fourth lane is w, not an axis");
    }
}
