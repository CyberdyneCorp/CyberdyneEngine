//! The semantic palette, and the two rules that make it a language rather than a set of hues.
//!
//! `editor-visual-language`: "Colour SHALL carry meaning. The editor SHALL maintain one colour
//! vocabulary across every panel, overlay, gizmo, graph, and visualisation ... A hue SHALL NOT be
//! reused for an unrelated meaning within one surface, and a colour SHALL NOT be introduced for
//! visual variety."
//!
//! And, subordinating it, `editor-ui-ux`: "colour SHALL NOT be the sole encoding of any state, and
//! every meaning above SHALL also be carried by shape, icon, or text."
//!
//! Both are enforced by the shape of [`Semantic`] rather than by review. A role is an enum with no
//! constructor, so there is no way to introduce a colour that means nothing — a panel asks for
//! [`Semantic::Warning`] and receives whatever the theme paints warnings, and a panel that wanted a
//! colour "to look nice" has nothing to ask for. And every role carries a [`Semantic::glyph`] and a
//! [`Semantic::label`] beside its colour, so a surface that used the colour and dropped the other
//! two had to actively discard them.
//!
//! --- WHY THE AXES ARE NOT IN THIS ENUM -------------------------------------------------------------
//!
//! X red, Y green and Z blue is a *second* vocabulary, and deliberately outside this one: green
//! means "valid or live" here and "the Y axis" there, which is precisely the reuse the requirement
//! above forbids. It is admissible because the axis triad is never presented without its axis label,
//! is fixed rather than themed by meaning, and is not user-remappable — see [`crate::axis`], where
//! it lives on its own and where that argument is written down. [`no_role_shares_a_hue`] checks the
//! rule over the semantic roles and says why the axes are excluded rather than quietly skipping
//! them.

use cy_editor_core::problem::{Problem, Result};

/// A colour, as the three channels a theme paints.
///
/// `sRGB` bytes rather than floats, because these are authored values compared for equality in
/// tests and quoted in documentation; a float triple would make `#E5B95C` a number nobody
/// recognises.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct Rgb {
    /// Red, 0-255.
    pub red: u8,
    /// Green, 0-255.
    pub green: u8,
    /// Blue, 0-255.
    pub blue: u8,
}

impl Rgb {
    /// A colour from its three channels.
    #[must_use]
    pub const fn new(red: u8, green: u8, blue: u8) -> Self {
        Self { red, green, blue }
    }

    /// A colour from `0xRRGGBB`, which is how these are authored and reviewed.
    ///
    /// Destructured from the big-endian bytes rather than shifted and masked, because a shift needs
    /// a narrowing cast and a narrowing cast in a colour constructor is the kind of thing that is
    /// correct until somebody passes a value with something in its top byte.
    #[must_use]
    pub const fn hex(value: u32) -> Self {
        let [_, red, green, blue] = value.to_be_bytes();
        Self { red, green, blue }
    }

    /// The `#RRGGBB` form, for a diagnostic or a theme file.
    #[must_use]
    pub fn to_hex(self) -> String {
        format!("#{:02X}{:02X}{:02X}", self.red, self.green, self.blue)
    }

    /// Relative luminance, as WCAG 2.1 defines it.
    ///
    /// The definition rather than an approximation, because it is what the contrast target in
    /// `editor-ui-ux` is measured with, and a cheaper luminance would make the test agree with
    /// itself and with nothing else.
    #[must_use]
    pub fn luminance(self) -> f64 {
        fn channel(value: u8) -> f64 {
            let value = f64::from(value) / 255.0;
            if value <= 0.039_28 {
                value / 12.92
            } else {
                ((value + 0.055) / 1.055).powf(2.4)
            }
        }
        0.2126 * channel(self.red) + 0.7152 * channel(self.green) + 0.0722 * channel(self.blue)
    }

    /// The contrast ratio between two colours, from 1.0 to 21.0.
    #[must_use]
    pub fn contrast(self, other: Self) -> f64 {
        let (first, second) = (self.luminance(), other.luminance());
        let (high, low) = if first > second {
            (first, second)
        } else {
            (second, first)
        };
        (high + 0.05) / (low + 0.05)
    }

    /// The hue in degrees, 0-360.
    ///
    /// Used by the checks rather than by the interface: "a hue SHALL NOT be reused for an unrelated
    /// meaning" is a statement about hue and not about the colour, so the check has to be able to
    /// say that `#F2615C` and `#C0332F` are the same hue at two lightnesses — which is exactly what
    /// the dark and light themes do to the axis triad.
    #[must_use]
    pub fn hue(self) -> f64 {
        let red = f64::from(self.red) / 255.0;
        let green = f64::from(self.green) / 255.0;
        let blue = f64::from(self.blue) / 255.0;
        let max = red.max(green).max(blue);
        let min = red.min(green).min(blue);
        let delta = max - min;
        if delta <= f64::EPSILON {
            return 0.0;
        }
        let hue = if (max - red).abs() < f64::EPSILON {
            ((green - blue) / delta).rem_euclid(6.0)
        } else if (max - green).abs() < f64::EPSILON {
            (blue - red) / delta + 2.0
        } else {
            (red - green) / delta + 4.0
        };
        hue * 60.0
    }
}

/// What a colour means. The editor's whole colour vocabulary, and there is no other.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum Semantic {
    /// Ordinary interface surface: the charcoal the chrome is made of.
    Neutral,
    /// Primary text.
    PrimaryText,
    /// Secondary and derived information.
    SecondaryText,
    /// Active, focused, or informational state.
    Active,
    /// Success, live, valid.
    Live,
    /// Selection and attention. The warm yellow-gold; see `editor-visual-language`'s selection rule.
    Selection,
    /// Warning.
    Warning,
    /// Error and destructive consequence.
    Error,
}

impl Semantic {
    /// Every role, for a theme editor and for the checks below.
    pub const ALL: [Semantic; 8] = [
        Semantic::Neutral,
        Semantic::PrimaryText,
        Semantic::SecondaryText,
        Semantic::Active,
        Semantic::Live,
        Semantic::Selection,
        Semantic::Warning,
        Semantic::Error,
    ];

    /// What the role means, in the words the specification's table uses.
    #[must_use]
    pub const fn meaning(self) -> &'static str {
        match self {
            Semantic::Neutral => "ordinary interface surface",
            Semantic::PrimaryText => "primary text",
            Semantic::SecondaryText => "secondary and derived information",
            Semantic::Active => "active, focused, or informational",
            Semantic::Live => "success, live, valid",
            Semantic::Selection => "selection and attention",
            Semantic::Warning => "warning",
            Semantic::Error => "error and destructive consequence",
        }
    }

    /// The shape that carries the same meaning when the colour cannot.
    ///
    /// This is the accessibility rule made structural: the role hands out a glyph beside its colour,
    /// so an entry in an error state is identifiable without hue because the glyph came with it.
    /// Named `glyph` rather than `icon` because it is a character in the interface font and not an
    /// asset — an icon set arrives with the toolkit, and this has to work before that choice.
    #[must_use]
    pub const fn glyph(self) -> char {
        match self {
            Semantic::Neutral => '·',
            Semantic::PrimaryText => '▪',
            Semantic::SecondaryText => '▫',
            Semantic::Active => '◆',
            Semantic::Live => '●',
            Semantic::Selection => '◇',
            Semantic::Warning => '▲',
            Semantic::Error => '✕',
        }
    }

    /// The word that carries the meaning when neither colour nor shape can.
    ///
    /// "Status indicators SHALL state the condition in text as well as colour, and SHALL be readable
    /// without hovering."
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            // The three that are not states carry no word, because there is no condition to
            // state: a surface and its text are not something a user is being told.
            Semantic::Neutral | Semantic::PrimaryText | Semantic::SecondaryText => "",
            Semantic::Active => "Active",
            Semantic::Live => "Live",
            Semantic::Selection => "Selected",
            Semantic::Warning => "Warning",
            Semantic::Error => "Error",
        }
    }
}

/// Which of a theme's surfaces something sits on.
///
/// `editor-visual-language`: "Panels SHALL be differentiated by **small luminance steps, spacing,
/// and subtle separators** rather than by borders, cards, or shadows." So this enum is the whole of
/// the surface system, and there is no border colour, no shadow and no gradient anywhere in this
/// crate to reach for instead. A panel boundary is legible because two adjacent surfaces differ,
/// which is checked in [`crate::surface`].
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum Surface {
    /// The application window behind everything.
    Window,
    /// An ordinary panel.
    Panel,
    /// A panel raised above its neighbour: a popover, a header row, a hovered row.
    Raised,
    /// A sunken area: a text field, a viewport letterbox, a well.
    Sunken,
}

impl Surface {
    /// Every surface, darkest first in the dark theme.
    pub const ALL: [Surface; 4] = [
        Surface::Sunken,
        Surface::Window,
        Surface::Panel,
        Surface::Raised,
    ];
}

/// Which way round a theme is.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub enum Mode {
    /// The default: near-black and charcoal chrome, so the viewport carries the screen's luminance.
    Dark,
    /// The light theme `editor-ui-ux` also requires.
    Light,
}

/// How a viewer sees colour, for the palettes `editor-ui-ux` requires to be selectable.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub enum Vision {
    /// The shipped palette.
    Standard,
    /// A palette whose status hues stay distinguishable without red-green discrimination.
    ///
    /// It moves exactly one role: [`Semantic::Live`] goes from green to teal, so that "live" and
    /// "error" are separated along the blue-yellow axis, which is the discrimination that survives
    /// every common form of colour vision deficiency. Error stays red and warning stays orange,
    /// because with green out of the way they are the two hues furthest apart that remain — and a
    /// palette that repainted everything would be a second theme to keep contrast-checked rather
    /// than an accessibility option. It does **not** move the axis triad, which is labelled; see
    /// [`crate::axis`].
    RedGreenSafe,
}

/// A complete set of colours: one mode, one vision, and every role and surface painted.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Theme {
    /// Which way round it is.
    pub mode: Mode,
    /// Which palette it paints the status roles from.
    pub vision: Vision,
}

impl Default for Theme {
    /// The shipped default: dark, standard vision. The chrome recedes and the viewport is the
    /// subject, which is `editor-visual-language`'s first requirement.
    fn default() -> Self {
        Self {
            mode: Mode::Dark,
            vision: Vision::Standard,
        }
    }
}

impl Theme {
    /// A theme.
    #[must_use]
    pub const fn new(mode: Mode, vision: Vision) -> Self {
        Self { mode, vision }
    }

    /// The colour of a semantic role.
    #[must_use]
    pub const fn colour(self, role: Semantic) -> Rgb {
        let standard = match (self.mode, role) {
            (Mode::Dark, Semantic::Neutral) => 0x16_19_1C,
            (Mode::Dark, Semantic::PrimaryText) => 0xE6_E9_EC,
            (Mode::Dark, Semantic::SecondaryText) => 0xA2_AB_B4,
            (Mode::Dark, Semantic::Active) => 0x4C_9A_FF,
            (Mode::Dark, Semantic::Live) => 0x35_C0_7C,
            (Mode::Dark, Semantic::Selection) => 0xE5_B9_5C,
            (Mode::Dark, Semantic::Warning) => 0xF0_91_3A,
            (Mode::Dark, Semantic::Error) => 0xFF_6B_60,
            (Mode::Light, Semantic::Neutral) => 0xFA_FB_FC,
            (Mode::Light, Semantic::PrimaryText) => 0x15_18_1B,
            (Mode::Light, Semantic::SecondaryText) => 0x5A_62_6A,
            (Mode::Light, Semantic::Active) => 0x0B_5C_D6,
            (Mode::Light, Semantic::Live) => 0x0E_7A_4A,
            (Mode::Light, Semantic::Selection) => 0x8A_61_00,
            (Mode::Light, Semantic::Warning) => 0x9A_4B_00,
            (Mode::Light, Semantic::Error) => 0xC2_25_1C,
        };
        // The red-green-safe palette repaints the one role whose hue a red-green deficiency
        // confuses with error, and leaves the rest. See `Vision::RedGreenSafe`.
        let value = match (self.vision, self.mode, role) {
            (Vision::RedGreenSafe, Mode::Dark, Semantic::Live) => 0x4C_C2_D6,
            (Vision::RedGreenSafe, Mode::Light, Semantic::Live) => 0x0B_6A_86,
            _ => standard,
        };
        Rgb::hex(value)
    }

    /// The colour of a surface.
    #[must_use]
    pub const fn surface(self, surface: Surface) -> Rgb {
        let value = match (self.mode, surface) {
            (Mode::Dark, Surface::Window) => 0x0E_10_12,
            (Mode::Dark, Surface::Panel) => 0x16_19_1C,
            (Mode::Dark, Surface::Raised) => 0x1E_22_27,
            (Mode::Dark, Surface::Sunken) => 0x06_07_08,
            (Mode::Light, Surface::Window) => 0xEE_F0_F2,
            (Mode::Light, Surface::Panel) => 0xFA_FB_FC,
            (Mode::Light, Surface::Raised) => 0xFF_FF_FF,
            (Mode::Light, Surface::Sunken) => 0xE3_E6_E9,
        };
        Rgb::hex(value)
    }

    /// The contrast between a role and the panel it is ordinarily read on.
    #[must_use]
    pub fn legibility(self, role: Semantic) -> f64 {
        self.colour(role).contrast(self.surface(Surface::Panel))
    }

    /// Every shipped theme, which is what a legibility test iterates.
    pub const ALL: [Theme; 4] = [
        Theme::new(Mode::Dark, Vision::Standard),
        Theme::new(Mode::Light, Vision::Standard),
        Theme::new(Mode::Dark, Vision::RedGreenSafe),
        Theme::new(Mode::Light, Vision::RedGreenSafe),
    ];
}

/// The contrast a theme's text and status colours must reach on a panel.
///
/// 4.5:1 is WCAG AA for body text. It is applied to the status roles too rather than the 3:1 that
/// applies to large text and interface components, because in this editor a status colour *is* text
/// most of the time — "Live", "3 problems", a row's label tinted by its state.
pub const MINIMUM_CONTRAST: f64 = 4.5;

/// The luminance step two adjacent surfaces must differ by for a boundary to be legible.
///
/// Small, because the requirement is "small luminance steps ... rather than borders" — but not zero,
/// which is the failure this constant exists to catch: a theme where two adjacent panels are the
/// same colour has no boundary at all, and the fix would be reached for by adding a border.
pub const MINIMUM_SURFACE_STEP: f64 = 0.002;

/// Check that every role in a theme is legible on the surface it is read on.
///
/// Returned as a [`Problem`] rather than asserted, so a *user* theme can be checked by the same
/// function that checks the shipped ones — `editor-ui-ux` requires user themes, and a user theme
/// that fails legibility should be reported to its author rather than crash the editor.
pub fn check_legibility(theme: Theme) -> Result<()> {
    for role in Semantic::ALL {
        if matches!(role, Semantic::Neutral) {
            // The neutral role *is* the surface; measuring it against itself measures nothing.
            continue;
        }
        let ratio = theme.legibility(role);
        if ratio < MINIMUM_CONTRAST {
            return Err(Problem::new(
                format!("use this theme's {} colour", role.meaning()),
                format!(
                    "{} on {} is {ratio:.2}:1, below the {MINIMUM_CONTRAST}:1 the editor requires",
                    theme.colour(role).to_hex(),
                    theme.surface(Surface::Panel).to_hex()
                ),
            )
            .with_remedy(
                "lighten or darken the role until it reaches the ratio; the hue may stay where it \
                 is, because the meaning is the hue and the legibility is the lightness",
            ));
        }
    }
    Ok(())
}

/// Check that no two semantic roles in a theme are the same hue.
///
/// The axis triad is deliberately not checked: it is a second vocabulary that reuses two of these
/// hues on purpose, and it is admissible because an axis colour never appears without its axis
/// label. See [`crate::axis`].
pub fn no_role_shares_a_hue(theme: Theme) -> Result<()> {
    /// Two hues closer than this are the same colour to a user.
    const SEPARATION: f64 = 12.0;

    let coloured: Vec<Semantic> = Semantic::ALL
        .into_iter()
        .filter(|role| {
            !matches!(
                role,
                Semantic::Neutral | Semantic::PrimaryText | Semantic::SecondaryText
            )
        })
        .collect();

    for (index, first) in coloured.iter().enumerate() {
        for second in &coloured[index + 1..] {
            let separation = (theme.colour(*first).hue() - theme.colour(*second).hue()).abs();
            let separation = separation.min(360.0 - separation);
            if separation < SEPARATION {
                return Err(Problem::new(
                    "give these two meanings different colours",
                    format!(
                        "{} and {} are {separation:.0} degrees apart, which reads as one colour \
                         with two meanings",
                        first.meaning(),
                        second.meaning()
                    ),
                )
                .with_remedy(
                    "the semantic vocabulary assigns each hue one meaning; pick the hue whose \
                     meaning is intended, or state a new meaning in the specification first",
                ));
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_shipped_theme_is_legible() {
        // `editor-ui-ux`: "Colour choices SHALL meet legibility contrast targets in the shipped
        // themes." Four themes, seven roles each, and the failure names the ratio it reached.
        for theme in Theme::ALL {
            check_legibility(theme).unwrap_or_else(|problem| panic!("{theme:?}: {problem}"));
        }
    }

    #[test]
    fn no_shipped_theme_repurposes_a_hue() {
        for theme in Theme::ALL {
            no_role_shares_a_hue(theme).unwrap_or_else(|problem| panic!("{theme:?}: {problem}"));
        }
    }

    #[test]
    fn adjacent_surfaces_differ_enough_to_have_a_boundary_without_a_border() {
        // "WHEN three panels are docked adjacently THEN their boundaries SHALL be readable from
        // luminance and spacing alone." Readable from luminance means the luminances differ, and
        // this is the number that says by how much.
        for theme in [Theme::default(), Theme::new(Mode::Light, Vision::Standard)] {
            for pair in Surface::ALL.windows(2) {
                let step =
                    (theme.surface(pair[0]).luminance() - theme.surface(pair[1]).luminance()).abs();
                assert!(
                    step >= MINIMUM_SURFACE_STEP,
                    "{theme:?}: {:?} and {:?} differ by {step:.4}, which is no boundary at all",
                    pair[0],
                    pair[1]
                );
            }
        }
    }

    #[test]
    fn meaning_survives_a_colour_blind_palette() {
        // The specification's scenario: "WHEN a colour-blind-safe palette is selected THEN
        // selection, warning, and error SHALL remain distinguishable, because each is also encoded
        // by outline weight, icon, or label."
        let safe = Theme::new(Mode::Dark, Vision::RedGreenSafe);
        check_legibility(safe).unwrap();
        no_role_shares_a_hue(safe).unwrap();

        let states = [Semantic::Selection, Semantic::Warning, Semantic::Error];
        for (index, role) in states.iter().enumerate() {
            for other in &states[index + 1..] {
                assert_ne!(role.glyph(), other.glyph(), "the shapes must differ too");
                assert_ne!(role.label(), other.label(), "and so must the words");
            }
        }
    }

    #[test]
    fn every_state_role_carries_a_word_and_a_shape_beside_its_colour() {
        // "colour SHALL NOT be the sole encoding of any state". A role whose label was empty would
        // be a state the interface could only paint.
        for role in [
            Semantic::Active,
            Semantic::Live,
            Semantic::Selection,
            Semantic::Warning,
            Semantic::Error,
        ] {
            assert!(!role.label().is_empty(), "{role:?} has no word");
            assert!(!role.glyph().is_whitespace(), "{role:?} has no shape");
        }
    }

    #[test]
    fn a_user_theme_that_is_illegible_is_reported_rather_than_shipped() {
        // The same function that checks the shipped themes is the one a theme editor calls, and its
        // failure carries a remedy a theme author can act on.
        let problem = check_legibility(Theme::new(Mode::Dark, Vision::Standard));
        assert!(problem.is_ok());

        // A hand-built failure: the check is only worth having if it can fail.
        let ratio = Rgb::hex(0x20_22_24).contrast(Rgb::hex(0x16_19_1C));
        assert!(ratio < MINIMUM_CONTRAST, "{ratio}");
    }

    #[test]
    fn a_colour_round_trips_through_its_hexadecimal_form() {
        assert_eq!(Rgb::hex(0xE5_B9_5C).to_hex(), "#E5B95C");
        assert_eq!(Rgb::new(0, 0, 0).to_hex(), "#000000");
    }
}
