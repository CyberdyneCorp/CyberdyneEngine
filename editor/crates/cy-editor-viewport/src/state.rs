//! A viewport's view state: what is being looked at, from where, and how.
//!
//! `editor-viewport-and-gizmos` asks two things of this type, and the second is the one that makes
//! it a value rather than a bag of fields on a panel:
//!
//! > Camera state SHALL be per viewport, persisted per document, and restorable.
//!
//! > A viewport view state SHALL be **capturable and restorable**, including camera pose,
//! > projection, view mode, visibility filters, and time state, so that an observation can be
//! > reproduced exactly. A captured view state SHALL be attachable to a defect report and to the
//! > diagnostics trace.
//!
//! So [`ViewState`] carries all five, and [`ViewState::encode`]/[`ViewState::decode`] round-trip it
//! through the same codec the journal and the bridge use. "Attachable to a defect report" is then a
//! byte string rather than a feature: whatever can carry bytes can carry a view.
//!
//! --- WHAT IS NOT IN IT, AND WHY THAT MATTERS FOR REPRODUCTION ---------------------------------------
//!
//! Nothing derived. There is no matrix, no frustum and no cached ray — those are recomputed from the
//! five authored fields, which is what makes two captures compare equal when they describe the same
//! observation. A cached matrix would make a restored view state *nearly* equal to the original and
//! the difference would be invisible until a golden image disagreed.
//!
//! Nothing about the runtime either: no frame identifier, no transport, no staleness. A view state
//! is what the editor asked for; [`crate::transport::PresentedFrame`] is what came back, and it
//! carries its own copy of the state it was rendered with. Keeping the two apart is what makes "the
//! hit is resolved against the frame shown, not a newer one" expressible at all.

use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::problem::{Problem, Result};

use crate::math::{Quat, Ray, Vec3};
use crate::viewmode::ViewMode;

/// Where a viewport draws, in the target's pixels.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ViewportRect {
    /// Left edge.
    pub x: u32,
    /// Top edge.
    pub y: u32,
    /// Width, in pixels.
    pub width: u32,
    /// Height, in pixels.
    pub height: u32,
}

impl Default for ViewportRect {
    fn default() -> Self {
        Self {
            x: 0,
            y: 0,
            width: 1920,
            height: 1080,
        }
    }
}

impl ViewportRect {
    /// Width over height, with a zero height answered as 1 rather than as an infinity.
    #[must_use]
    pub fn aspect(self) -> f32 {
        if self.height == 0 {
            return 1.0;
        }
        pixels(self.width) / pixels(self.height)
    }

    /// How many pixels the viewport covers. What a budget divides by.
    #[must_use]
    pub const fn pixel_count(self) -> u64 {
        self.width as u64 * self.height as u64
    }
}

/// A pixel count as a float. One place, so the cast that clippy dislikes is justified once.
#[allow(
    clippy::cast_precision_loss,
    reason = "a viewport is far below 2^24 pixels wide"
)]
fn pixels(count: u32) -> f32 {
    count as f32
}

/// A projection's meaning, never its matrix.
///
/// The same semantic shape `src/servers/render/model.h` uses, and for the same reason: the renderer
/// builds the matrix, applying the engine's reversed-Z convention in exactly one place. An editor
/// that sent a matrix could send one with a conventional depth range and it would look almost right.
#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Projection {
    /// A perspective projection with a vertical field of view, in radians.
    Perspective {
        /// Vertical field of view, radians.
        fov_y: f32,
    },
    /// An orthographic projection with a vertical extent, in world units.
    Orthographic {
        /// Vertical extent, world units.
        height: f32,
    },
}

impl Default for Projection {
    fn default() -> Self {
        Self::Perspective {
            fov_y: std::f32::consts::FRAC_PI_3,
        }
    }
}

impl Projection {
    /// Whether this is the orthographic one. What a view-axis snap and a navigation mode branch on.
    #[must_use]
    pub const fn is_orthographic(self) -> bool {
        matches!(self, Projection::Orthographic { .. })
    }
}

/// The camera's pose: where it is and which way it faces.
///
/// No scale. A scaled camera is not a camera, and leaving the field out is cheaper than validating
/// it away.
#[derive(Clone, Copy, PartialEq, Debug, Default)]
pub struct CameraPose {
    /// Where the camera is, in world space.
    pub position: Vec3,
    /// Which way it faces. The camera looks down its local −Z.
    pub rotation: Quat,
}

impl CameraPose {
    /// The direction the camera looks.
    #[must_use]
    pub fn forward(self) -> Vec3 {
        self.rotation.rotate(-Vec3::Z)
    }

    /// The camera's right.
    #[must_use]
    pub fn right(self) -> Vec3 {
        self.rotation.rotate(Vec3::X)
    }

    /// The camera's up.
    #[must_use]
    pub fn up(self) -> Vec3 {
        self.rotation.rotate(Vec3::Y)
    }
}

/// What the viewport is allowed to show, beyond what the renderer decides to draw.
///
/// `editor-viewport-and-gizmos`: "Selection, visibility filters, isolation, view modes | Editor".
/// The engine is told the layer mask; isolation and the locked set are authoring state and are
/// resolved to a mask and an exclusion list before they cross the bridge.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct VisibilityFilter {
    /// The engine layers this viewport draws.
    pub layers: u32,
    /// When set, only these identities are shown — "isolation". Empty means "isolate nothing",
    /// which is different from isolating an empty set and is why this is an `Option`.
    pub isolated: Option<Vec<u64>>,
    /// Identities the user has locked. Not drawn differently; not selectable.
    pub locked: Vec<u64>,
}

impl Default for VisibilityFilter {
    fn default() -> Self {
        Self {
            layers: u32::MAX,
            isolated: None,
            locked: Vec::new(),
        }
    }
}

impl VisibilityFilter {
    /// Whether an identity is shown under this filter.
    #[must_use]
    pub fn shows(&self, identity: u64) -> bool {
        match &self.isolated {
            Some(only) => only.contains(&identity),
            None => true,
        }
    }

    /// Whether an identity may be selected: shown, and not locked.
    #[must_use]
    pub fn selectable(&self, identity: u64) -> bool {
        self.shows(identity) && !self.locked.contains(&identity)
    }
}

/// Where the world is in time, for a reproduction.
///
/// A view state that restored a camera and not a clock would reproduce a different frame of an
/// animated scene, which is the class of defect report that reads "it only happens sometimes".
#[derive(Clone, Copy, PartialEq, Debug, Default)]
pub struct TimeState {
    /// The world's time, in seconds.
    pub seconds: f64,
    /// Whether the world is advancing.
    pub paused: bool,
}

/// Everything needed to reproduce one observation.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct ViewState {
    /// Where the camera is and which way it faces.
    pub camera: CameraPose,
    /// How the world is projected.
    pub projection: Projection,
    /// Where the image goes, in pixels.
    pub viewport: ViewportRect,
    /// Which of the engine's debug views is requested.
    pub view_mode: ViewMode,
    /// What is shown and what is selectable.
    pub filter: VisibilityFilter,
    /// Where the world is in time.
    pub time: TimeState,
    /// Near clip distance, world units.
    pub near: f32,
    /// Far clip distance, world units. Zero means the engine's infinite far plane, which is the
    /// default and is what reversed-Z makes reasonable rather than a trick.
    pub far: f32,
}

impl ViewState {
    /// A perspective view at the origin looking down −Z, the way a new viewport opens.
    #[must_use]
    pub fn new() -> Self {
        Self {
            near: 0.1,
            far: 0.0,
            ..Self::default()
        }
    }

    /// The ray a pixel of this view aims, in world space.
    ///
    /// **For manipulation, never for picking.** A gizmo drag needs a ray to intersect with its own
    /// plane or axis, and that is editor-side intent. A pick sends the PIXEL and the frame it was
    /// clicked on and lets the runtime build the ray from the frame's own view state — see
    /// [`crate::picking`], and `src/servers/render/picking.h` for the reason.
    ///
    /// Pixels are measured from the viewport's top-left corner, which is the convention every
    /// windowing system reports a cursor in, and the flip to the projection's bottom-left origin
    /// happens here and nowhere else.
    #[must_use]
    pub fn ray_through_pixel(&self, pixel_x: f32, pixel_y: f32) -> Ray {
        let (ndc_x, ndc_y) = self.normalized_device(pixel_x, pixel_y);
        let aspect = self.viewport.aspect();
        match self.projection {
            Projection::Perspective { fov_y } => {
                let tan_half = (fov_y * 0.5).tan();
                let direction = Vec3::new(ndc_x * tan_half * aspect, ndc_y * tan_half, -1.0);
                Ray {
                    origin: self.camera.position,
                    direction: self
                        .camera
                        .rotation
                        .rotate(direction)
                        .normalized_or(self.camera.forward()),
                }
            }
            Projection::Orthographic { height } => {
                let half_height = height * 0.5;
                let offset = Vec3::new(ndc_x * half_height * aspect, ndc_y * half_height, 0.0);
                Ray {
                    origin: self.camera.position + self.camera.rotation.rotate(offset),
                    direction: self.camera.forward(),
                }
            }
        }
    }

    /// The normalised device coordinates of a pixel: −1 to 1, y up.
    fn normalized_device(&self, pixel_x: f32, pixel_y: f32) -> (f32, f32) {
        let width = pixels(self.viewport.width.max(1));
        let height = pixels(self.viewport.height.max(1));
        let x = (pixel_x - pixels(self.viewport.x)) / width;
        let y = (pixel_y - pixels(self.viewport.y)) / height;
        (x.mul_add(2.0, -1.0), y.mul_add(-2.0, 1.0))
    }

    /// The plane a free (screen-space) drag happens in: through `point`, facing the camera.
    #[must_use]
    pub fn facing_plane_normal(&self) -> Vec3 {
        -self.camera.forward()
    }

    /// Capture this state as bytes, for a defect report or the diagnostics trace.
    #[must_use]
    pub fn encode(&self) -> Vec<u8> {
        let mut writer = Writer::new();
        self.write(&mut writer);
        writer.finish()
    }

    /// Restore a captured state.
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        let mut reader = Reader::new(bytes);
        Self::read(&mut reader)
    }

    /// Append this state to an existing stream, for a caller writing a larger record.
    pub fn write(&self, writer: &mut Writer) {
        writer.u8(CAPTURE_VERSION);
        for lane in self.camera.position.to_array() {
            writer.f32(lane);
        }
        for lane in self.camera.rotation.to_array() {
            writer.f32(lane);
        }
        match self.projection {
            Projection::Perspective { fov_y } => {
                writer.u8(0);
                writer.f32(fov_y);
            }
            Projection::Orthographic { height } => {
                writer.u8(1);
                writer.f32(height);
            }
        }
        writer.u32(self.viewport.x);
        writer.u32(self.viewport.y);
        writer.u32(self.viewport.width);
        writer.u32(self.viewport.height);
        writer.u32(self.view_mode as u32);
        write_filter(writer, &self.filter);
        writer.f64(self.time.seconds);
        writer.u8(u8::from(self.time.paused));
        writer.f32(self.near);
        writer.f32(self.far);
    }

    /// Read a state a previous [`ViewState::write`] produced.
    pub fn read(reader: &mut Reader<'_>) -> Result<Self> {
        let version = reader.u8()?;
        if version != CAPTURE_VERSION {
            return Err(Problem::new(
                "restore a captured view",
                format!("it was written by capture format {version}, and this build writes {CAPTURE_VERSION}"),
            )
            .with_remedy("open it in the editor that wrote it, or capture the view again"));
        }
        let position = read_vec3(reader)?;
        let rotation =
            Quat::from_array([reader.f32()?, reader.f32()?, reader.f32()?, reader.f32()?]);
        let projection = match reader.u8()? {
            0 => Projection::Perspective {
                fov_y: reader.f32()?,
            },
            1 => Projection::Orthographic {
                height: reader.f32()?,
            },
            other => {
                return Err(Problem::new(
                    "restore a captured view",
                    format!("projection kind {other} is not one this build knows"),
                ));
            }
        };
        let viewport = ViewportRect {
            x: reader.u32()?,
            y: reader.u32()?,
            width: reader.u32()?,
            height: reader.u32()?,
        };
        let view_mode = ViewMode::from_index(reader.u32()?).ok_or_else(|| {
            Problem::new(
                "restore a captured view",
                "it names a debug view this build does not have",
            )
            .with_remedy("the capture is from a newer editor; the view restores without it")
        })?;
        let filter = read_filter(reader)?;
        let time = TimeState {
            seconds: reader.f64()?,
            paused: reader.u8()? != 0,
        };
        Ok(Self {
            camera: CameraPose { position, rotation },
            projection,
            viewport,
            view_mode,
            filter,
            time,
            near: reader.f32()?,
            far: reader.f32()?,
        })
    }
}

/// The capture format's version. Bumped when the layout changes, so that a restore refuses rather
/// than reading a camera pose out of a filter.
const CAPTURE_VERSION: u8 = 1;

fn write_filter(writer: &mut Writer, filter: &VisibilityFilter) {
    writer.u32(filter.layers);
    match &filter.isolated {
        Some(only) => {
            writer.u8(1);
            write_identities(writer, only);
        }
        None => writer.u8(0),
    }
    write_identities(writer, &filter.locked);
}

fn write_identities(writer: &mut Writer, identities: &[u64]) {
    writer.u32(u32::try_from(identities.len()).unwrap_or(u32::MAX));
    for identity in identities.iter().take(u32::MAX as usize) {
        writer.u64(*identity);
    }
}

fn read_filter(reader: &mut Reader<'_>) -> Result<VisibilityFilter> {
    let layers = reader.u32()?;
    let isolated = if reader.u8()? == 1 {
        Some(read_identities(reader)?)
    } else {
        None
    };
    Ok(VisibilityFilter {
        layers,
        isolated,
        locked: read_identities(reader)?,
    })
}

fn read_identities(reader: &mut Reader<'_>) -> Result<Vec<u64>> {
    let count = reader.u32()? as usize;
    let mut identities = Vec::new();
    for _ in 0..count {
        identities.push(reader.u64()?);
    }
    Ok(identities)
}

fn read_vec3(reader: &mut Reader<'_>) -> Result<Vec3> {
    Ok(Vec3::new(reader.f32()?, reader.f32()?, reader.f32()?))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn populated() -> ViewState {
        let mut state = ViewState::new();
        state.camera.position = Vec3::new(12.0, 3.5, -40.0);
        state.camera.rotation = Quat::from_axis_angle(Vec3::Y, 0.85);
        state.projection = Projection::Orthographic { height: 24.0 };
        state.viewport = ViewportRect {
            x: 8,
            y: 16,
            width: 1280,
            height: 720,
        };
        state.view_mode = ViewMode::Overdraw;
        state.filter.layers = 0b1011;
        state.filter.isolated = Some(vec![7, 9]);
        state.filter.locked = vec![3];
        state.time = TimeState {
            seconds: 12.25,
            paused: true,
        };
        state
    }

    #[test]
    fn a_captured_view_restores_to_the_same_observation() {
        // "WHEN a user reports a rendering problem from the viewport THEN the captured view state
        // SHALL restore the same view." Equality of the whole value is what "the same" means, and
        // it is only checkable because nothing derived is stored.
        let original = populated();
        let restored = ViewState::decode(&original.encode()).expect("a capture this build wrote");
        assert_eq!(restored, original);
    }

    #[test]
    fn a_capture_from_another_format_is_refused_with_something_to_do_about_it() {
        let mut bytes = populated().encode();
        bytes[0] = 99;
        let problem = ViewState::decode(&bytes).expect_err("a format this build does not know");
        assert!(problem.remedy.is_some(), "{problem}");
    }

    #[test]
    fn the_centre_pixel_aims_straight_down_the_camera_axis() {
        let mut state = ViewState::new();
        state.camera.rotation = Quat::from_axis_angle(Vec3::Y, 0.3);
        let ray = state.ray_through_pixel(960.0, 540.0);
        assert!(
            ray.direction.nearly_equals(state.camera.forward(), 1e-5),
            "{:?} vs {:?}",
            ray.direction,
            state.camera.forward()
        );
        assert_eq!(ray.origin, state.camera.position);
    }

    #[test]
    fn an_orthographic_ray_moves_its_origin_rather_than_its_direction() {
        let mut state = ViewState::new();
        state.projection = Projection::Orthographic { height: 10.0 };
        let centre = state.ray_through_pixel(960.0, 540.0);
        let corner = state.ray_through_pixel(1920.0, 540.0);
        assert!(corner.direction.nearly_equals(centre.direction, 1e-5));
        // Half the width to the right: the aspect is 16:9 and the height is 10.
        assert!((corner.origin.x - 8.888_889).abs() < 1e-3, "{corner:?}");
    }

    #[test]
    fn isolation_and_locking_are_different_questions() {
        let mut filter = VisibilityFilter::default();
        assert!(filter.shows(1) && filter.selectable(1));

        filter.locked = vec![1];
        assert!(filter.shows(1), "a locked object is still visible");
        assert!(!filter.selectable(1), "and is not selectable");

        filter.isolated = Some(vec![2]);
        assert!(!filter.shows(1));
        assert!(filter.shows(2) && filter.selectable(2));
    }
}
