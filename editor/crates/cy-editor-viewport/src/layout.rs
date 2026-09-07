//! Where the gizmo's handles are on screen — **as the runtime drew them**. Task 2.4.
//!
//! --- WHY THE EDITOR DOES NOT WORK THIS OUT FOR ITSELF ----------------------------------------------
//!
//! `editor-viewport-and-gizmos` assigns "gizmo geometry generation, depth handling, and screen-constant
//! sizing" to the engine, and names "editor-side picking that does not match what the engine rendered"
//! as a forbidden pattern. Those two together decide this module's shape: the editor cannot compute
//! where the arrows are, because a second computation of the same geometry is a second answer, and the
//! moment it disagrees the user grabs one handle and drags another.
//!
//! So the runtime publishes the geometry it *actually drew*, in screen space, alongside the frame that
//! contains it — and the editor hit-tests **that**. A [`GizmoLayout`] carries the [`FrameId`] of the
//! frame it belongs to for the same reason a [`crate::picking::PickRequest`] does: a click lands two
//! frames after the pixels it was aimed at, and resolving it against a newer layout is the same defect
//! in a smaller place.
//!
//! What is here is therefore a **reader**, not a generator. There is no function in this module that
//! produces a handle's position, and there is deliberately nowhere to put one.
//!
//! --- THE ACQUISITION SLOP, AND WHY IT IS NOT THE ENGINE'S ------------------------------------------
//!
//! `editor-visual-language` requires handles to be "acquirable without precision — sized for confident
//! grabbing rather than for minimal footprint", and fixes the number: twelve logical pixels, scaled
//! with the interface. That is a property of the *pointer*, not of the drawing, so the engine draws a
//! handle at whatever size reads well and the editor accepts a click within [`ACQUISITION`] of it.
//! Making the drawn handle twelve pixels wide instead would have put a fat arrow on the screen to
//! solve a problem the screen does not have.
//!
//! --- CONSTANT SCREEN SIZE IS CHECKABLE FROM HERE, WHICH IS THE POINT -------------------------------
//!
//! "Drawn in world units a gizmo becomes unusable exactly when precision matters most, and the failure
//! is gradual enough that nobody files it" (`design.md` §5c). Gradual failures need a check rather than
//! an eye, and [`screen_constant`] is it: hand it the layouts a runtime published while the camera
//! pulled away, and it fails when the extent moved. The editor cannot fix a runtime that gets this
//! wrong, but it can refuse to be the reason nobody noticed.

use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_protocol::FrameId;

use crate::gizmo::{GizmoMode, Handle};

/// How far from a handle a click still counts, in logical pixels.
///
/// `cy_editor_visual::gizmo::MINIMUM_ACQUISITION` is the same number said as an appearance rule; this
/// is the pointer half of it. They are equal and a test in `cy-editor-shell` holds them equal — this
/// crate cannot name the visual crate, which is layer 1 for the interface rather than for the model.
pub const ACQUISITION: f32 = 12.0;

/// One handle, where the runtime put it.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct HandleSpot {
    /// Which handle.
    pub handle: Handle,
    /// Its centre in viewport pixels, with the origin at the viewport's top left.
    pub x: f32,
    /// The same, vertically.
    pub y: f32,
    /// The radius the runtime drew it at, in pixels. The hit test adds [`ACQUISITION`] to it.
    pub radius: f32,
    /// How near the camera it is, in view depth. Two overlapping handles are resolved in favour of
    /// the nearer one, which is what "depth handling is the engine's" leaves the editor to honour.
    pub depth: f32,
}

/// The gizmo the runtime drew into one frame.
#[derive(Clone, PartialEq, Debug)]
pub struct GizmoLayout {
    /// The frame this belongs to. A layout with no frame cannot be hit-tested against a click.
    pub frame: FrameId,
    /// Which gizmo was drawn — which may not be the mode that was asked for, when the universal
    /// gizmo degraded. The overlay says so; see `cy_editor_visual::gizmo::presentation`.
    pub mode: GizmoMode,
    /// The gizmo's centre in viewport pixels.
    pub centre: (f32, f32),
    /// How far the gizmo reaches from its centre, in pixels. **This is the number that must not vary
    /// with camera distance**; see [`screen_constant`].
    pub extent: f32,
    /// Every handle it drew.
    pub spots: Vec<HandleSpot>,
}

impl GizmoLayout {
    /// An empty layout for a frame: the runtime drew no gizmo, because nothing was selected.
    #[must_use]
    pub fn none(frame: FrameId) -> Self {
        Self {
            frame,
            mode: GizmoMode::Translate,
            centre: (0.0, 0.0),
            extent: 0.0,
            spots: Vec::new(),
        }
    }

    /// Whether the runtime drew anything at all.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.spots.is_empty()
    }

    /// The handle under a pixel, or `None`.
    ///
    /// Nearest first and then nearest to the camera, so that the centre's three concentric
    /// affordances — circle, cube, outer ring — stay separately targetable rather than the first one
    /// in the list winning every click.
    #[must_use]
    pub fn hit(&self, x: f32, y: f32) -> Option<Handle> {
        let mut best: Option<(f32, f32, Handle)> = None;
        for spot in &self.spots {
            let distance = ((spot.x - x).powi(2) + (spot.y - y).powi(2)).sqrt();
            if distance > spot.radius + ACQUISITION {
                continue;
            }
            let candidate = (distance, spot.depth, spot.handle);
            match best {
                Some((distance_so_far, depth_so_far, _))
                    if (distance_so_far, depth_so_far) <= (candidate.0, candidate.1) => {}
                _ => best = Some(candidate),
            }
        }
        best.map(|(_, _, handle)| handle)
    }

    /// Where one handle is, if it was drawn.
    #[must_use]
    pub fn spot(&self, handle: Handle) -> Option<HandleSpot> {
        self.spots
            .iter()
            .copied()
            .find(|spot| spot.handle == handle)
    }

    /// Encode, for the transport that carries it beside the frame.
    #[must_use]
    pub fn encode(&self) -> Vec<u8> {
        let mut writer = Writer::new();
        writer.u8(LAYOUT_VERSION);
        writer.u64(self.frame.as_u64());
        writer.u8(mode_code(self.mode));
        writer.f32(self.centre.0);
        writer.f32(self.centre.1);
        writer.f32(self.extent);
        writer.u32(u32::try_from(self.spots.len()).unwrap_or(u32::MAX));
        for spot in &self.spots {
            writer.u8(handle_code(spot.handle));
            writer.f32(spot.x);
            writer.f32(spot.y);
            writer.f32(spot.radius);
            writer.f32(spot.depth);
        }
        writer.finish()
    }

    /// Decode what a runtime published.
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        let mut reader = Reader::new(bytes);
        let version = reader.u8()?;
        if version != LAYOUT_VERSION {
            return Err(Problem::new(
                "read a gizmo layout",
                format!("it is version {version} and this editor reads version {LAYOUT_VERSION}"),
            )
            .with_remedy("run a runtime built from this revision"));
        }
        let frame = FrameId::from_raw(reader.u64()?);
        let mode = mode_of_code(reader.u8()?)?;
        let centre = (reader.f32()?, reader.f32()?);
        let extent = reader.f32()?;
        let count = reader.u32()?;
        let mut spots = Vec::new();
        for _ in 0..count {
            spots.push(HandleSpot {
                handle: handle_of_code(reader.u8()?)?,
                x: reader.f32()?,
                y: reader.f32()?,
                radius: reader.f32()?,
                depth: reader.f32()?,
            });
        }
        Ok(Self {
            frame,
            mode,
            centre,
            extent,
            spots,
        })
    }
}

/// The encoding's version. One byte, refused rather than guessed at.
const LAYOUT_VERSION: u8 = 1;

/// How far two extents may differ and still count as constant, as a fraction.
///
/// Not zero: a runtime that rounds a gizmo's size to whole pixels is doing the right thing, and a
/// check that failed over half a pixel would be turned off within a week. Two per cent is far tighter
/// than the failure this exists to catch — a world-space gizmo halves every time the camera doubles
/// its distance.
const EXTENT_TOLERANCE: f32 = 0.02;

/// Refuse a run of layouts whose gizmo changed size as the camera moved.
///
/// The check for task 2.4.2, from the side that can make it: the editor knows it asked for the same
/// gizmo on the same object, so any change in [`GizmoLayout::extent`] is the runtime sizing in world
/// units. The failure names both extents, because "the gizmo is the wrong size" is not a report
/// anybody can act on.
pub fn screen_constant(layouts: &[GizmoLayout]) -> Result<()> {
    let Some(first) = layouts.iter().find(|layout| !layout.is_empty()) else {
        return Ok(());
    };
    for layout in layouts.iter().filter(|layout| !layout.is_empty()) {
        let difference = (layout.extent - first.extent).abs();
        if difference > first.extent.abs().max(1.0) * EXTENT_TOLERANCE {
            return Err(Problem::new(
                "keep the gizmo at a constant screen size",
                format!(
                    "frame {} drew it {:.1} px across and frame {} drew it {:.1} px across",
                    first.frame.as_u64(),
                    first.extent,
                    layout.frame.as_u64(),
                    layout.extent
                ),
            )
            .with_remedy(
                "size the gizmo from the view rather than in world units; a gizmo drawn in world \
                 units becomes unusable exactly when precision matters most",
            ));
        }
    }
    Ok(())
}

const fn mode_code(mode: GizmoMode) -> u8 {
    match mode {
        GizmoMode::Translate => 0,
        GizmoMode::Rotate => 1,
        GizmoMode::Scale => 2,
        GizmoMode::Universal => 3,
    }
}

fn mode_of_code(code: u8) -> Result<GizmoMode> {
    GizmoMode::ALL
        .into_iter()
        .find(|mode| mode_code(*mode) == code)
        .ok_or_else(|| {
            Problem::new(
                "read a gizmo layout",
                format!("no gizmo mode has code {code}"),
            )
        })
}

/// The handle's code is its index in [`Handle::ALL`], which is the order the enumeration declares.
#[allow(
    clippy::cast_possible_truncation,
    reason = "there are fifteen handles and the list is a compile-time constant"
)]
fn handle_code(handle: Handle) -> u8 {
    Handle::ALL
        .iter()
        .position(|candidate| *candidate == handle)
        .unwrap_or(0) as u8
}

fn handle_of_code(code: u8) -> Result<Handle> {
    Handle::ALL
        .get(code as usize)
        .copied()
        .ok_or_else(|| Problem::new("read a gizmo layout", format!("no handle has code {code}")))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn spot(handle: Handle, x: f32, y: f32, radius: f32, depth: f32) -> HandleSpot {
        HandleSpot {
            handle,
            x,
            y,
            radius,
            depth,
        }
    }

    fn layout(frame: u64, extent: f32) -> GizmoLayout {
        GizmoLayout {
            frame: FrameId::from_raw(frame),
            mode: GizmoMode::Universal,
            centre: (960.0, 540.0),
            extent,
            spots: vec![
                spot(Handle::AxisX, 1060.0, 540.0, 6.0, 10.0),
                spot(Handle::AxisY, 960.0, 440.0, 6.0, 10.0),
                spot(Handle::PlaneXY, 1000.0, 500.0, 8.0, 10.0),
                spot(Handle::RingZ, 960.0, 640.0, 4.0, 12.0),
                spot(Handle::ScreenRing, 960.0, 540.0, 60.0, 20.0),
                spot(Handle::Uniform, 960.0, 540.0, 10.0, 9.0),
                spot(Handle::Screen, 960.0, 540.0, 18.0, 11.0),
            ],
        }
    }

    #[test]
    fn a_click_within_the_acquisition_radius_takes_the_handle() {
        let layout = layout(7, 120.0);
        assert_eq!(layout.hit(1060.0, 540.0), Some(Handle::AxisX));
        // Eleven pixels off, which is inside the slop and outside the drawn handle: "acquirable
        // without precision" is what that slop is.
        assert_eq!(layout.hit(1071.0, 540.0), Some(Handle::AxisX));
        // And far enough away is nothing at all, rather than the nearest thing.
        assert_eq!(layout.hit(1400.0, 900.0), None);
    }

    #[test]
    fn the_centres_three_affordances_stay_separately_targetable() {
        // They are concentric — circle, cube, outer ring — so the hit test cannot be "the first
        // match". Nearest, then nearest to the camera.
        let layout = layout(7, 120.0);
        assert_eq!(
            layout.hit(960.0, 540.0),
            Some(Handle::Uniform),
            "the centre cube is nearest the camera at the exact centre"
        );
        assert_eq!(
            layout.hit(960.0, 600.0),
            Some(Handle::ScreenRing),
            "the outer ring is reachable where the cube is not"
        );
    }

    #[test]
    fn an_empty_layout_hits_nothing_and_says_so() {
        let empty = GizmoLayout::none(FrameId::from_raw(3));
        assert!(empty.is_empty());
        assert_eq!(empty.hit(960.0, 540.0), None);
    }

    #[test]
    fn a_layout_survives_the_round_trip_the_transport_puts_it_through() {
        let original = layout(11, 96.0);
        let decoded = GizmoLayout::decode(&original.encode()).expect("it decodes");
        assert_eq!(decoded, original);
    }

    #[test]
    fn a_layout_from_another_version_is_refused_with_a_remedy() {
        let mut bytes = layout(11, 96.0).encode();
        bytes[0] = 9;
        let problem = GizmoLayout::decode(&bytes).expect_err("a version this editor cannot read");
        assert!(problem.remedy.is_some(), "{problem}");
    }

    #[test]
    fn a_gizmo_that_shrinks_with_camera_distance_is_refused_by_name() {
        // The check that makes task 2.4.2 checkable rather than a matter of eyesight. A world-space
        // gizmo halves as the camera doubles its distance; this is that, at four frames.
        let world_sized: Vec<GizmoLayout> = [120.0, 60.0, 30.0, 15.0]
            .into_iter()
            .enumerate()
            .map(|(index, extent)| layout(index as u64, extent))
            .collect();
        let problem = screen_constant(&world_sized).expect_err("it shrank");
        assert!(problem.because.contains("120.0"), "{problem}");
        assert!(problem.remedy.is_some(), "{problem}");

        let screen_sized: Vec<GizmoLayout> = (0..4).map(|index| layout(index, 120.0)).collect();
        screen_constant(&screen_sized).expect("constant across the run");

        // Rounding to whole pixels is not a violation, and a check that said it was would be turned
        // off rather than obeyed.
        let rounded = vec![layout(0, 120.0), layout(1, 121.0)];
        screen_constant(&rounded).expect("a pixel of rounding is not a defect");
    }

    #[test]
    fn every_handle_has_a_code_and_survives_it() {
        for handle in Handle::ALL {
            assert_eq!(
                handle_of_code(handle_code(handle)).expect("a known code"),
                handle
            );
        }
    }
}
