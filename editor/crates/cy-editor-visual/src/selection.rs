//! What selection looks like: a thin gold outline that does not obscure what it surrounds.
//!
//! `editor-visual-language`: "Selection SHALL be indicated by a **thin, high-contrast outline in the
//! attention hue** ... The outline SHALL remain legible against both bright and dark scene content,
//! SHALL NOT bloom or glow, and SHALL NOT obscure the material it surrounds — a user must be able to
//! judge a surface while it is selected."
//!
//! Every one of those is a field or the absence of one. There is no `glow`, no `bloom` and no `fill`
//! in [`SelectionAppearance`] to set; what there is, is a width the check keeps thin and a hue the
//! check keeps at [`Semantic::Selection`]. The one thing that cannot be settled by absence is
//! legibility against bright content, and that is why the outline carries a **contrast companion**:
//! a thin dark edge outside the gold one, so the outline reads on sunlit sand without being made
//! wider or brighter, which is exactly what the scenario forbids.

use cy_editor_core::problem::{Problem, Result};

use crate::colour::{Rgb, Semantic, Theme};

/// Whose selection this is.
///
/// "Where a project distinguishes **editor selection** from simulated **gameplay selection**, the
/// two SHALL be visually distinct, and the distinction SHALL hold while playing in the editor."
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub enum SelectionKind {
    /// What the user has selected in the editor.
    Editor,
    /// What the running game has selected — a strategy project's units, say.
    Gameplay,
}

/// How a selection outline is drawn.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct SelectionAppearance {
    /// Whose selection it is.
    pub kind: SelectionKind,
    /// The outline's width in logical pixels.
    pub width: f32,
    /// The semantic role its colour comes from.
    pub role: Semantic,
    /// Whether the outline is dashed, which is the shape half of the editor/gameplay distinction.
    pub dashed: bool,
}

impl SelectionAppearance {
    /// The widest an outline may be and still be "thin".
    ///
    /// Two logical pixels. Three is where an outline starts to cover the silhouette it is drawn on,
    /// which is the "SHALL NOT obscure the material it surrounds" failure.
    pub const MAXIMUM_WIDTH: f32 = 2.0;

    /// The editor's own selection: a thin solid gold outline.
    #[must_use]
    pub const fn editor() -> Self {
        Self {
            kind: SelectionKind::Editor,
            width: 1.5,
            role: Semantic::Selection,
            dashed: false,
        }
    }

    /// A running game's selection: the active hue, dashed, so the two are distinct by shape as well
    /// as by colour and stay distinct while playing in the editor.
    #[must_use]
    pub const fn gameplay() -> Self {
        Self {
            kind: SelectionKind::Gameplay,
            width: 1.5,
            role: Semantic::Active,
            dashed: true,
        }
    }

    /// The outline's colour.
    #[must_use]
    pub const fn colour(self, theme: Theme) -> Rgb {
        theme.colour(self.role)
    }

    /// The dark companion drawn just outside the outline, so it reads on bright content.
    ///
    /// This is how "legible against bright scene content" is satisfied without "increasing its width
    /// or brightness": the companion is a contrast edge, not more outline, and it is the same one
    /// pixel whatever the scene does.
    #[must_use]
    pub const fn companion(self) -> Rgb {
        Rgb::new(8, 8, 10)
    }
}

/// Refuse a selection appearance that glows, blooms, or covers what it surrounds.
///
/// The forbidden pattern, checkable: "A selection indicator that glows, blooms, or obscures the
/// surface beneath it." Glow and bloom are refused by there being no field for them; what this
/// checks is the two that a value can express — width, and a hue that is not the attention hue.
pub fn check(appearance: SelectionAppearance) -> Result<()> {
    if appearance.width > SelectionAppearance::MAXIMUM_WIDTH {
        return Err(Problem::new(
            "draw the selection outline this wide",
            format!(
                "{} logical pixels covers the silhouette it is meant to describe, and a user has to \
                 judge the material while it is selected",
                appearance.width
            ),
        )
        .with_remedy(format!(
            "keep it at or below {} and rely on the dark companion edge for legibility against \
             bright content",
            SelectionAppearance::MAXIMUM_WIDTH
        )));
    }
    let expected = match appearance.kind {
        SelectionKind::Editor => Semantic::Selection,
        SelectionKind::Gameplay => Semantic::Active,
    };
    if appearance.role != expected {
        return Err(Problem::new(
            "paint the selection outline in this role's colour",
            format!(
                "{:?} selection is drawn in the hue that means {:?}, and the one proposed means {:?}",
                appearance.kind,
                expected.meaning(),
                appearance.role.meaning()
            ),
        )
        .with_remedy(
            "gold is selection and blue is active; a third hue for selection would be a second \
             meaning for a colour that already has one",
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_shipped_appearances_pass_their_own_check() {
        check(SelectionAppearance::editor()).unwrap();
        check(SelectionAppearance::gameplay()).unwrap();
    }

    #[test]
    fn selection_is_legible_on_a_bright_surface_without_getting_wider() {
        // "WHEN an object lit by direct sunlight is selected THEN its outline SHALL remain visible
        // without increasing its width or brightness." The companion edge is what makes that
        // possible, and this is the property it has to have: contrast against the brightest thing a
        // scene can produce.
        let appearance = SelectionAppearance::editor();
        let sunlit = Rgb::new(255, 252, 240);
        let against_content = appearance.companion().contrast(sunlit);
        assert!(
            against_content > 3.0,
            "the companion edge is {against_content:.1}:1 against sunlit content"
        );
        assert!(appearance.width <= SelectionAppearance::MAXIMUM_WIDTH);
    }

    #[test]
    fn editor_and_gameplay_selection_do_not_merge() {
        let editor = SelectionAppearance::editor();
        let gameplay = SelectionAppearance::gameplay();
        assert_ne!(editor.role, gameplay.role, "different hues");
        assert_ne!(editor.dashed, gameplay.dashed, "and different shapes");
        assert_ne!(
            editor.colour(Theme::default()),
            gameplay.colour(Theme::default())
        );
    }

    #[test]
    fn a_thick_outline_is_refused_with_the_reason() {
        let problem = check(SelectionAppearance {
            width: 6.0,
            ..SelectionAppearance::editor()
        })
        .unwrap_err();
        assert!(problem.because.contains("silhouette"), "{problem}");
    }

    #[test]
    fn green_does_not_get_to_mean_selected() {
        // The specification's scenario, as a value: "WHEN a panel proposes green to mean 'selected'
        // THEN it SHALL be flagged, because green means valid or live and gold means selection."
        let problem = check(SelectionAppearance {
            role: Semantic::Live,
            ..SelectionAppearance::editor()
        })
        .unwrap_err();
        assert!(problem.remedy.is_some(), "{problem}");
    }
}
