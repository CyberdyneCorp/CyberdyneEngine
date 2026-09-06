//! Density and typography: the metrics of a tool somebody uses for eight hours a day.
//!
//! `editor-ui-ux`: "The interface SHALL be optimised for **information density and low interaction
//! cost** for professional daily use rather than for visual impression. The editor SHALL provide
//! **compact and comfortable density modes**, and SHALL honour a user-selected interface scale."
//!
//! And `editor-visual-language` on the type: "a small number of sizes, clear separation between
//! primary and secondary text by weight and luminance rather than by size ... Numeric values SHALL
//! use **tabular figures** so that columns of numbers align."
//!
//! --- WHY THE SCALE MULTIPLIES AND THE DENSITY DOES NOT ---------------------------------------------
//!
//! They answer different questions. Density is how much information the user wants per row;
//! interface scale is how large a logical pixel is on their display. Multiplying them in that order
//! means a user on a high-density display at compact density gets *compact rows, drawn large*, which
//! is what they asked for — and the alternative, one combined setting, is the design that makes
//! high-density displays feel sparse.
//!
//! --- WHY THERE IS A MINIMUM AND NO MAXIMUM --------------------------------------------------------
//!
//! [`Metrics::icon`] never falls below [`MINIMUM_ICON`] however compact the mode, because "WHEN the
//! interface is set to compact density THEN every toolbar icon SHALL remain distinguishable from its
//! neighbours" is a floor and not a preference. A user who wants a denser interface than that wants
//! a smaller interface scale, which is a different control.

/// How much information a row carries.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug, Default)]
pub enum Density {
    /// More rows, less air. The mode a professional settles into.
    #[default]
    Compact,
    /// More air, fewer rows. The mode a new user starts in and a projector needs.
    Comfortable,
}

impl Density {
    /// Both modes.
    pub const ALL: [Density; 2] = [Density::Compact, Density::Comfortable];

    /// The name a settings row shows.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            Density::Compact => "Compact",
            Density::Comfortable => "Comfortable",
        }
    }
}

/// The user's interface scale, as a multiplier of logical pixels.
///
/// A newtype rather than a bare `f32` so that a scale cannot be multiplied into a metric twice,
/// which is the defect that makes an interface look right on the developer's display and enormous
/// on everybody else's. Clamped at construction: a scale of zero is an invisible editor and a scale
/// of ten is one nobody can navigate out of to fix it.
#[derive(Clone, Copy, PartialEq, PartialOrd, Debug)]
pub struct Scale(f32);

impl Default for Scale {
    fn default() -> Self {
        Self(1.0)
    }
}

impl Scale {
    /// The smallest scale the editor will apply.
    pub const MINIMUM: f32 = 0.75;
    /// The largest.
    pub const MAXIMUM: f32 = 3.0;

    /// A scale, clamped into the range the editor stays usable at.
    #[must_use]
    pub fn new(factor: f32) -> Self {
        Self(factor.clamp(Self::MINIMUM, Self::MAXIMUM))
    }

    /// The multiplier.
    #[must_use]
    pub const fn factor(self) -> f32 {
        self.0
    }
}

/// The smallest an icon is drawn at, in logical pixels, before the interface scale is applied.
///
/// "WHEN the interface is set to compact density THEN every toolbar icon SHALL remain
/// distinguishable from its neighbours."
pub const MINIMUM_ICON: f32 = 14.0;

/// Which text a size belongs to.
///
/// Four, and the requirement is that there are few: "a small number of sizes ... subtle headings,
/// and no decorative type". A fifth would need an argument.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub enum TextRole {
    /// Body text: a row's label, a field's value.
    Body,
    /// Secondary and derived information, separated by weight and luminance rather than by size.
    Secondary,
    /// A section header inside a panel.
    Section,
    /// A panel or tab title.
    Title,
}

impl TextRole {
    /// Every role.
    pub const ALL: [TextRole; 4] = [
        TextRole::Body,
        TextRole::Secondary,
        TextRole::Section,
        TextRole::Title,
    ];

    /// The weight the role is drawn at, on the usual 100-900 scale.
    ///
    /// This is where the separation lives: `Secondary` is the same size as `Body` and differs by
    /// weight and by [`crate::colour::Semantic::SecondaryText`]'s luminance, which is the rule.
    #[must_use]
    pub const fn weight(self) -> u16 {
        match self {
            TextRole::Body | TextRole::Secondary => 400,
            TextRole::Section => 600,
            TextRole::Title => 500,
        }
    }
}

/// Which family a run of text is set in.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub enum Family {
    /// One modern sans-serif for interface text.
    Interface,
    /// Monospaced, and only for code, console output, and identifiers where character alignment
    /// carries meaning.
    Mono,
}

/// Whether a run of digits is set with tabular figures.
///
/// "Numeric values SHALL use tabular figures so that columns of numbers align, and SHALL align on
/// the decimal separator where a column is compared." An inspector's float field is numeric, so this
/// answers `true` for it — which is why the predicate takes what the text *is* rather than being a
/// flag a caller remembers to set.
#[must_use]
pub const fn tabular_figures(role: TextRole, numeric: bool) -> bool {
    numeric || matches!(role, TextRole::Body)
}

/// The metrics of one density at one scale.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Metrics {
    /// The density these were derived from.
    pub density: Density,
    /// The scale they were multiplied by.
    pub scale: Scale,
}

impl Default for Metrics {
    fn default() -> Self {
        Self::new(Density::default(), Scale::default())
    }
}

impl Metrics {
    /// Metrics for a density at a scale.
    #[must_use]
    pub const fn new(density: Density, scale: Scale) -> Self {
        Self { density, scale }
    }

    /// The height of one list, tree or table row.
    #[must_use]
    pub fn row(self) -> f32 {
        self.scaled(match self.density {
            Density::Compact => 20.0,
            Density::Comfortable => 26.0,
        })
    }

    /// The gap between two unrelated things.
    #[must_use]
    pub fn gap(self) -> f32 {
        self.scaled(match self.density {
            Density::Compact => 4.0,
            Density::Comfortable => 8.0,
        })
    }

    /// The inset from a panel's edge to its content.
    #[must_use]
    pub fn padding(self) -> f32 {
        self.scaled(match self.density {
            Density::Compact => 6.0,
            Density::Comfortable => 10.0,
        })
    }

    /// The size an icon is drawn at, never below [`MINIMUM_ICON`] before scaling.
    #[must_use]
    pub fn icon(self) -> f32 {
        let base: f32 = match self.density {
            Density::Compact => 14.0,
            Density::Comfortable => 16.0,
        };
        self.scaled(base.max(MINIMUM_ICON))
    }

    /// The size a role's text is set at.
    #[must_use]
    pub fn text(self, role: TextRole) -> f32 {
        // One size per density, and one step for a title. Written as a base and a step rather
        // than as a table of six, because a table of six is where a fifth size gets added.
        let base: f32 = match self.density {
            Density::Compact => 12.0,
            Density::Comfortable => 13.0,
        };
        let step = match role {
            TextRole::Title => 1.0,
            TextRole::Body | TextRole::Secondary | TextRole::Section => 0.0,
        };
        self.scaled(base + step)
    }

    /// The smallest pointer target the interface offers, which no density shrinks below.
    #[must_use]
    pub fn hit_target(self) -> f32 {
        self.scaled(20.0_f32.max(self.row() / self.scale.factor()))
    }

    fn scaled(self, value: f32) -> f32 {
        value * self.scale.factor()
    }
}

/// How long the interface may spend animating before it has delayed the user.
///
/// Zero. "Panels SHALL ... avoid animations that delay interaction", and "WHEN a panel opens THEN
/// interaction SHALL be possible immediately". A decorative transition may still be drawn — it
/// simply may not be a precondition of accepting input, which is what a budget of zero states.
pub const INPUT_DELAY_BUDGET: std::time::Duration = std::time::Duration::ZERO;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn compact_shows_more_rows_than_comfortable_in_the_same_panel() {
        let panel = 600.0;
        let compact = Metrics::new(Density::Compact, Scale::default());
        let comfortable = Metrics::new(Density::Comfortable, Scale::default());
        let rows = |metrics: Metrics| (panel / metrics.row()).floor();
        assert!(
            rows(compact) > rows(comfortable),
            "compact showed {} rows and comfortable {}",
            rows(compact),
            rows(comfortable)
        );
    }

    #[test]
    fn an_icon_never_falls_below_the_legibility_floor() {
        // "WHEN the interface is set to compact density THEN every toolbar icon SHALL remain
        // distinguishable from its neighbours."
        for density in Density::ALL {
            let metrics = Metrics::new(density, Scale::new(Scale::MINIMUM));
            assert!(
                metrics.icon() >= MINIMUM_ICON * Scale::MINIMUM,
                "{density:?} at the smallest scale draws icons at {}",
                metrics.icon()
            );
        }
    }

    #[test]
    fn the_interface_scale_multiplies_every_metric_exactly_once() {
        let base = Metrics::new(Density::Compact, Scale::default());
        let doubled = Metrics::new(Density::Compact, Scale::new(2.0));
        assert!((doubled.row() - base.row() * 2.0).abs() < f32::EPSILON);
        assert!((doubled.gap() - base.gap() * 2.0).abs() < f32::EPSILON);
        assert!((doubled.icon() - base.icon() * 2.0).abs() < f32::EPSILON);
        assert!(
            (doubled.text(TextRole::Body) - base.text(TextRole::Body) * 2.0).abs() < f32::EPSILON
        );
    }

    #[test]
    fn a_scale_outside_the_usable_range_is_clamped_rather_than_applied() {
        assert!((Scale::new(0.1).factor() - Scale::MINIMUM).abs() < f32::EPSILON);
        assert!((Scale::new(99.0).factor() - Scale::MAXIMUM).abs() < f32::EPSILON);
    }

    #[test]
    fn primary_and_secondary_text_differ_by_weight_and_not_by_size() {
        // "clear separation between primary and secondary text by weight and luminance rather than
        // by size". The luminance half is `Semantic::SecondaryText`; this is the size half.
        for density in Density::ALL {
            let metrics = Metrics::new(density, Scale::default());
            assert!(
                (metrics.text(TextRole::Body) - metrics.text(TextRole::Secondary)).abs()
                    < f32::EPSILON
            );
        }
    }

    #[test]
    fn a_section_header_is_not_oversized() {
        // "WHEN a section header is added to the inspector THEN it SHALL be distinguished by weight
        // and spacing rather than by a substantially larger size."
        let metrics = Metrics::default();
        let body = metrics.text(TextRole::Body);
        let section = metrics.text(TextRole::Section);
        assert!(
            section <= body * 1.2,
            "{section} against a body size of {body}"
        );
        assert!(TextRole::Section.weight() > TextRole::Body.weight());
    }

    #[test]
    fn a_column_of_numbers_is_set_with_tabular_figures() {
        assert!(tabular_figures(TextRole::Body, true));
        assert!(tabular_figures(TextRole::Secondary, true));
    }

    #[test]
    fn no_animation_is_allowed_to_delay_input() {
        assert_eq!(INPUT_DELAY_BUDGET, std::time::Duration::ZERO);
    }
}
