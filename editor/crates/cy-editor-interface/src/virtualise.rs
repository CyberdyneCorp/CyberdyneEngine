//! Virtualisation: the cost of a list scales with what is visible, not with what exists.
//!
//! `editor-ui-ux` states it twice — once as a requirement, "Lists, trees, and tables SHALL be
//! **virtualised**, and their cost SHALL scale with what is visible rather than with what exists",
//! and once as a forbidden pattern, "A non-virtualised list, tree, or table over unbounded data".
//! The target it belongs to is "Project browser scroll on 100 000 assets: smooth; virtualised".
//!
//! There is no widget here — there is no toolkit yet — so what this module is, is the arithmetic
//! that decides *which rows exist at all*, and the type the panels take instead of a row count. A
//! panel that asked for every row would have to ask for `rows.len()` explicitly, and that is a thing
//! a review can see; a panel that takes a [`Window`] cannot accidentally materialise a hundred
//! thousand of anything.
//!
//! --- WHY THERE IS OVERSCAN ------------------------------------------------------------------------
//!
//! Two rows above and below the viewport, so a scroll of one row does not expose an unbuilt row
//! before the next rebuild. It is a constant rather than a setting because tuning it is a
//! measurement to make against a real toolkit, and a knob nobody has measured is a knob somebody
//! sets wrongly.

/// Where a list is scrolled to and how much of it is on screen, in logical pixels.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Viewport {
    /// How far down the list is scrolled.
    pub scroll: f32,
    /// The visible height.
    pub height: f32,
    /// The height of one row, from [`cy_editor_visual::Metrics::row`].
    pub row: f32,
}

impl Viewport {
    /// A viewport.
    #[must_use]
    pub const fn new(scroll: f32, height: f32, row: f32) -> Self {
        Self {
            scroll,
            height,
            row,
        }
    }
}

/// How many rows are built beyond each edge of the viewport.
pub const OVERSCAN: usize = 2;

/// The rows a panel actually builds.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Window {
    /// The index of the first row built.
    pub first: usize,
    /// How many rows are built.
    pub count: usize,
}

impl Window {
    /// The rows to build for a list of `total` rows at this viewport.
    ///
    /// Independent of `total` except for the clamp at the end, which is the property that matters: a
    /// list of a hundred and a list of a hundred thousand produce the same `count`.
    #[must_use]
    pub fn of(total: usize, viewport: Viewport) -> Self {
        if total == 0 || viewport.row <= 0.0 || viewport.height <= 0.0 {
            return Self { first: 0, count: 0 };
        }
        let first_visible = as_index((viewport.scroll.max(0.0) / viewport.row).floor());
        let visible = as_index((viewport.height / viewport.row).ceil()) + 1;
        let first = first_visible.saturating_sub(OVERSCAN).min(total);
        let count = (visible + 2 * OVERSCAN).min(total - first);
        Self { first, count }
    }

    /// The half-open range of indices this window covers.
    #[must_use]
    pub const fn range(self) -> std::ops::Range<usize> {
        self.first..self.first + self.count
    }

    /// The slice of a list this window covers.
    #[must_use]
    pub fn slice<T>(self, rows: &[T]) -> &[T] {
        let end = (self.first + self.count).min(rows.len());
        let start = self.first.min(end);
        &rows[start..end]
    }
}

/// The total height a list of `total` rows occupies, for a scrollbar that is honest about the size
/// of a list it has not built.
#[expect(
    clippy::cast_precision_loss,
    reason = "a scrollbar's extent beyond 2^24 rows is drawn to the nearest representable pixel, \
              which is a sub-pixel error on a list nobody can scroll through by hand anyway"
)]
#[must_use]
pub fn content_height(total: usize, row: f32) -> f32 {
    row * total as f32
}

/// A non-negative row count from a float, clamped rather than wrapped.
///
/// One place, so that the two conversions in [`Window::of`] cannot disagree about what a negative
/// scroll or a non-finite height means — both of which arrive from a real toolkit sooner or later.
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "the guard above the cast is what makes it total: non-finite and negative are zero, \
              and the clamp is below usize::MAX on every platform this builds for"
)]
fn as_index(value: f32) -> usize {
    if !value.is_finite() || value <= 0.0 {
        return 0;
    }
    value.min(f32::from(u16::MAX) * f32::from(u16::MAX)) as usize
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A compact viewport: 600 logical pixels of panel at the compact row height.
    fn panel(scroll: f32) -> Viewport {
        Viewport::new(scroll, 600.0, 20.0)
    }

    #[test]
    fn a_hundred_thousand_assets_cost_the_same_as_forty() {
        // "WHEN a project contains hundreds of thousands of assets THEN browsing, searching, and
        // selecting SHALL remain responsive." The mechanism, as arithmetic: the window over a
        // hundred thousand rows is the same size as the window over a hundred.
        let small = Window::of(100, panel(0.0));
        let large = Window::of(100_000, panel(0.0));
        assert_eq!(small.count, large.count);
        assert!(
            large.count < 64,
            "a 600-pixel panel built {} rows, which is not virtualisation",
            large.count
        );
    }

    #[test]
    fn building_rows_touches_only_what_is_visible() {
        // The claim expressed as work rather than as time, because work is what a loaded continuous
        // integration machine still measures correctly.
        let assets: Vec<usize> = (0..100_000).collect();
        let mut built = 0_usize;
        let window = Window::of(assets.len(), panel(20_000.0));
        for _ in window.slice(&assets) {
            built += 1;
        }
        assert_eq!(built, window.count);
        assert!(built <= 40, "built {built} rows out of a hundred thousand");
    }

    #[test]
    fn the_window_follows_the_scroll_and_stays_inside_the_list() {
        let window = Window::of(1_000, panel(400.0));
        assert_eq!(window.first, 20 - OVERSCAN);
        assert!(window.range().end <= 1_000);

        let at_the_end = Window::of(1_000, panel(content_height(1_000, 20.0)));
        assert!(
            at_the_end.range().end <= 1_000,
            "scrolling past the end must not build rows that do not exist"
        );
    }

    #[test]
    fn an_empty_list_builds_nothing_and_a_collapsed_panel_builds_nothing() {
        assert_eq!(Window::of(0, panel(0.0)).count, 0);
        assert_eq!(Window::of(1_000, Viewport::new(0.0, 0.0, 20.0)).count, 0);
        assert_eq!(Window::of(1_000, Viewport::new(0.0, 600.0, 0.0)).count, 0);
    }

    #[test]
    fn the_slice_is_safe_when_the_list_shrank_under_the_window() {
        // A row was deleted between the window being computed and the panel being drawn, which is a
        // frame apart and happens.
        let window = Window::of(1_000, panel(19_000.0));
        let shrunk: Vec<usize> = (0..10).collect();
        assert!(window.slice(&shrunk).is_empty());
    }
}
