//! Viewport chrome is overlay, not toolbar — and the default workspace the chrome sits in.
//!
//! `editor-visual-language` states the constraint and the reason in one line: "Every pixel of
//! vertical chrome removed from around the viewport is viewport, which is the point." So the
//! viewport's controls are [`Overlay`]s *inside* it, and there is no type in this crate that could
//! express a second toolbar above it — [`Region::Toolbar`] exists once, in the application header,
//! and the composition below is fixed data rather than something a panel can add a row to.
//!
//! The two properties worth testing are both about area:
//!
//! * adding an overlay does not shrink the rendered viewport, because an overlay is drawn over it
//!   ([`viewport_height`] takes no overlay count);
//! * the viewport is the largest single region of the default workspace ([`Composition::default`]).
//!
//! --- CAPTURE ---------------------------------------------------------------------------------------
//!
//! Every overlay declares itself excluded from a clean capture. `editor-viewport-and-gizmos` owns
//! the capture path; what belongs here is that the *appearance* layer knows an overlay is chrome and
//! not content, so a screenshot intended to represent the shipping frame does not have a frame-rate
//! counter in the corner of it.

use crate::colour::Surface;

/// One control presented over the viewport rather than around it.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum Overlay {
    /// Perspective or orthographic.
    Projection,
    /// Lit, unlit, wireframe, and the rest of the view modes.
    RenderMode,
    /// What is drawn: the show flags.
    ShowFlags,
    /// How fast the camera flies.
    CameraSpeed,
    /// Snapping increments and whether snapping is on.
    Snapping,
    /// Move, rotate, scale, universal.
    TransformMode,
    /// Fill the window with this viewport.
    Maximise,
    /// The debug visualisation selector.
    DebugVisualisation,
    /// The ambient frame-cost readout. See [`Overlay::is_performance`].
    Performance,
    /// The view-orientation widget. Chrome, and not a manipulator; see [`crate::orientation`].
    Orientation,
}

impl Overlay {
    /// Every overlay, which is the whole set: an eleventh would be a specification change.
    pub const ALL: [Overlay; 10] = [
        Overlay::Projection,
        Overlay::RenderMode,
        Overlay::ShowFlags,
        Overlay::CameraSpeed,
        Overlay::Snapping,
        Overlay::TransformMode,
        Overlay::Maximise,
        Overlay::DebugVisualisation,
        Overlay::Performance,
        Overlay::Orientation,
    ];

    /// The label, in the engine's vocabulary.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            Overlay::Projection => "Perspective",
            Overlay::RenderMode => "Lit",
            Overlay::ShowFlags => "Show",
            Overlay::CameraSpeed => "Camera",
            Overlay::Snapping => "Snap",
            Overlay::TransformMode => "Transform",
            Overlay::Maximise => "Maximise",
            Overlay::DebugVisualisation => "Debug",
            Overlay::Performance => "Frame",
            Overlay::Orientation => "Orientation",
        }
    }

    /// Where it sits, so that the shipped defaults do not obstruct the content being judged.
    #[must_use]
    pub const fn corner(self) -> Corner {
        match self {
            Overlay::Projection
            | Overlay::RenderMode
            | Overlay::ShowFlags
            | Overlay::DebugVisualisation => Corner::TopLeft,
            Overlay::Orientation | Overlay::Maximise => Corner::TopRight,
            Overlay::Performance => Corner::BottomLeft,
            Overlay::CameraSpeed | Overlay::Snapping | Overlay::TransformMode => {
                Corner::BottomRight
            }
        }
    }

    /// Whether this is the ambient performance overlay.
    ///
    /// "The overlay answers 'is this frame affordable'; the profiler answers 'why'." The predicate
    /// exists so that [`crate::rules::check_performance_overlay`] can hold the line against the
    /// per-subsystem breakdown that is always about to be added to it.
    #[must_use]
    pub const fn is_performance(self) -> bool {
        matches!(self, Overlay::Performance)
    }
}

/// Which corner of the viewport an overlay sits in.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum Corner {
    /// Upper left.
    TopLeft,
    /// Upper right.
    TopRight,
    /// Lower left.
    BottomLeft,
    /// Lower right.
    BottomRight,
}

/// The viewport's chrome: which overlays are shown, each individually toggleable.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Chrome {
    hidden: Vec<Overlay>,
}

impl Default for Chrome {
    fn default() -> Self {
        Self::new()
    }
}

impl Chrome {
    /// Every overlay shown.
    #[must_use]
    pub const fn new() -> Self {
        Self { hidden: Vec::new() }
    }

    /// Whether an overlay is shown.
    #[must_use]
    pub fn shows(&self, overlay: Overlay) -> bool {
        !self.hidden.contains(&overlay)
    }

    /// Show or hide one overlay. "Overlays SHALL be individually toggleable."
    pub fn set(&mut self, overlay: Overlay, shown: bool) {
        if shown {
            self.hidden.retain(|held| *held != overlay);
        } else if !self.hidden.contains(&overlay) {
            self.hidden.push(overlay);
        }
    }

    /// The overlays shown, in a stable order.
    #[must_use]
    pub fn shown(&self) -> Vec<Overlay> {
        Overlay::ALL
            .into_iter()
            .filter(|overlay| self.shows(*overlay))
            .collect()
    }

    /// The overlays a clean capture draws. None of them.
    ///
    /// The method exists rather than the caller knowing, because "excluded from any image intended
    /// to represent the shipping frame" is a rule the capture path must not have to remember.
    #[must_use]
    pub fn shown_for_capture(&self) -> Vec<Overlay> {
        Vec::new()
    }
}

/// The height of the rendered viewport within a region of `available` logical pixels.
///
/// It takes no overlay count, and that is the whole implementation of "WHEN viewport controls are
/// added THEN the viewport's rendered area SHALL NOT shrink": there is no parameter through which an
/// overlay could take a pixel from it.
#[must_use]
pub const fn viewport_height(available: f32) -> f32 {
    available
}

/// A region of the default workspace.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum Region {
    /// The Cyberdyne mark, menus, project and scene name, platform, live status, account.
    Header,
    /// Selection mode, play controls, build, transform tools, snapping, viewport options.
    Toolbar,
    /// Scene hierarchy and world outliner, with permanent search.
    LeftUpper,
    /// Content browser.
    LeftLower,
    /// The viewport, with overlaid chrome.
    Centre,
    /// The active specialised editor: script graph, animation, materials, sequencing.
    CentreLower,
    /// The inspector.
    Right,
    /// Diagnostics: console, profiler, tasks.
    RightLower,
    /// Output log, find in files, command input, save and source-control state.
    Footer,
}

impl Region {
    /// Every region, in the order the specification's table lists them.
    pub const ALL: [Region; 9] = [
        Region::Header,
        Region::Toolbar,
        Region::LeftUpper,
        Region::LeftLower,
        Region::Centre,
        Region::CentreLower,
        Region::Right,
        Region::RightLower,
        Region::Footer,
    ];

    /// What the region holds, in the engine's own vocabulary.
    #[must_use]
    pub const fn contents(self) -> &'static str {
        match self {
            Region::Header => "mark, menus, project and scene, platform, live status, account",
            Region::Toolbar => "selection mode, play controls, build, transform tools, snapping",
            Region::LeftUpper => "scene hierarchy and world outliner, with permanent search",
            Region::LeftLower => "content browser",
            Region::Centre => "viewport, with overlaid chrome",
            Region::CentreLower => "the active specialised editor",
            Region::Right => "inspector",
            Region::RightLower => "diagnostics: console, profiler, tasks",
            Region::Footer => "output log, find in files, command input, save and source control",
        }
    }

    /// Which surface the region is painted on.
    #[must_use]
    pub const fn surface(self) -> Surface {
        match self {
            Region::Header | Region::Toolbar | Region::Footer => Surface::Window,
            Region::Centre => Surface::Sunken,
            _ => Surface::Panel,
        }
    }
}

/// The proportion of the window a region occupies in the default workspace.
///
/// Fractions of the window's width and height rather than pixels, because the requirement is about
/// proportion — "the viewport receives the largest single region" — and a pixel figure would be a
/// statement about one display.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Extent {
    /// Fraction of the window's width.
    pub width: f32,
    /// Fraction of the window's height.
    pub height: f32,
}

impl Extent {
    /// The fraction of the window's area.
    #[must_use]
    pub fn area(self) -> f32 {
        self.width * self.height
    }
}

/// The default workspace: which region is where, and how much of the window each gets.
///
/// "Users SHALL be able to rearrange all of it ... This requirement fixes the **default**, because
/// the default is what a new user learns and what a screenshot teaches." Rearranging is
/// `editor-ui-ux`'s docking model; this is the arrangement it starts from.
#[derive(Clone, PartialEq, Debug)]
pub struct Composition {
    regions: Vec<(Region, Extent)>,
}

impl Default for Composition {
    /// The arrangement the specification's table fixes, viewport-first.
    fn default() -> Self {
        let extent = |width: f32, height: f32| Extent { width, height };
        Self {
            regions: vec![
                (Region::Header, extent(1.0, 0.04)),
                (Region::Toolbar, extent(1.0, 0.04)),
                (Region::LeftUpper, extent(0.18, 0.50)),
                (Region::LeftLower, extent(0.18, 0.38)),
                (Region::Centre, extent(0.60, 0.62)),
                (Region::CentreLower, extent(0.60, 0.26)),
                (Region::Right, extent(0.22, 0.55)),
                (Region::RightLower, extent(0.22, 0.33)),
                (Region::Footer, extent(1.0, 0.04)),
            ],
        }
    }
}

impl Composition {
    /// Every region and its extent, in the table's order.
    #[must_use]
    pub fn regions(&self) -> &[(Region, Extent)] {
        &self.regions
    }

    /// One region's extent.
    #[must_use]
    pub fn extent(&self, region: Region) -> Option<Extent> {
        self.regions
            .iter()
            .find(|(candidate, _)| *candidate == region)
            .map(|(_, extent)| *extent)
    }

    /// The region with the largest share of the window.
    #[must_use]
    pub fn largest(&self) -> Region {
        self.regions
            .iter()
            .max_by(|first, second| {
                first
                    .1
                    .area()
                    .partial_cmp(&second.1.area())
                    .unwrap_or(std::cmp::Ordering::Equal)
            })
            .map_or(Region::Centre, |(region, _)| *region)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_viewport_dominates_the_default_layout() {
        // "WHEN the editor is opened with a default workspace on a typical display THEN the
        // viewport SHALL occupy the largest single region."
        let composition = Composition::default();
        assert_eq!(composition.largest(), Region::Centre);

        let viewport = composition.extent(Region::Centre).unwrap().area();
        for (region, extent) in composition.regions() {
            if *region != Region::Centre {
                assert!(
                    extent.area() < viewport,
                    "{region:?} takes {:.3} of the window against the viewport's {viewport:.3}",
                    extent.area()
                );
            }
        }
    }

    #[test]
    fn adding_viewport_controls_costs_no_viewport_height() {
        // "WHEN viewport controls are added THEN the viewport's rendered area SHALL NOT shrink."
        // There is no overlay count to pass, which is the point: a second toolbar cannot be
        // expressed, so it cannot be added by accident.
        let mut chrome = Chrome::new();
        let height = viewport_height(900.0);
        for overlay in Overlay::ALL {
            chrome.set(overlay, true);
            assert!((viewport_height(900.0) - height).abs() < f32::EPSILON);
        }
        assert_eq!(chrome.shown().len(), Overlay::ALL.len());
    }

    #[test]
    fn every_overlay_is_individually_toggleable_and_none_survives_a_clean_capture() {
        let mut chrome = Chrome::new();
        chrome.set(Overlay::Performance, false);
        assert!(!chrome.shows(Overlay::Performance));
        assert!(chrome.shows(Overlay::Snapping), "one at a time");
        chrome.set(Overlay::Performance, true);
        assert!(chrome.shows(Overlay::Performance));

        assert!(
            chrome.shown_for_capture().is_empty(),
            "chrome is excluded from an image meant to represent the shipping frame"
        );
    }

    #[test]
    fn the_default_overlays_keep_out_of_each_others_corners() {
        // Not a specification requirement, an implementation of one: "SHALL default to a position
        // that does not obstruct scene content". Four corners, and no corner carrying so much that
        // it reaches the middle.
        let mut per_corner = [0_usize; 4];
        for overlay in Overlay::ALL {
            per_corner[overlay.corner() as usize] += 1;
        }
        for count in per_corner {
            assert!(count <= 4, "a corner with {count} overlays is a toolbar");
        }
    }

    #[test]
    fn the_composition_covers_every_region_exactly_once() {
        let composition = Composition::default();
        for region in Region::ALL {
            assert!(
                composition.extent(region).is_some(),
                "{region:?} has no place in the default workspace"
            );
        }
        assert_eq!(composition.regions().len(), Region::ALL.len());
    }
}
