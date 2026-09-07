//! Moving the camera: orbit, pan, zoom, fly, focus, frame all, and view-axis snapping.
//!
//! `editor-viewport-and-gizmos` — "Navigation":
//!
//! > The viewport SHALL provide standard navigation — orbit, pan, zoom, fly, focus on selection,
//! > frame all, and view-axis snapping — with **speed adaptive to the distance to the point of
//! > interest**. Navigation SHALL be **bindable and preset-selectable**, including presets matching
//! > existing engines. Navigation SHALL remain smooth and predictable while the world is streaming,
//! > and SHALL not be blocked by loading.
//!
//! --- THE PIVOT DISTANCE IS THE WHOLE OF "ADAPTIVE" -----------------------------------------------------
//!
//! Every speed here is derived from one number: how far the camera is from the point it is orbiting.
//! Pan moves the world by exactly the number of world units a pixel covers **at the pivot's depth**,
//! zoom is a proportion of the distance rather than an amount of it, and fly scales with it. So the
//! same gesture moves an object across the screen whether the camera is a metre away or a kilometre,
//! which is what "adaptive" means to a person using it.
//!
//! Fixed speeds are the obvious implementation and they produce the two failures everybody has met:
//! a camera that cannot reach a distant object, and one that shoots past a near one. Both are the
//! same defect measured at two scales.
//!
//! --- NAVIGATION CANNOT BE BLOCKED, BECAUSE THERE IS NOTHING TO BLOCK ON --------------------------------
//!
//! > **WHEN** the user flies into unloaded regions **THEN** navigation SHALL remain responsive while
//! > content loads progressively.
//!
//! Every function in this module takes a [`ViewState`] and returns nothing fallible. There is no
//! runtime handle, no session, no transport and no `Result` — a camera move cannot wait on streaming
//! because it has nothing to wait on, and the requirement is a property of the signatures rather than
//! a promise about scheduling. The image the camera is moving over may be stale; that is the
//! transport's business and it is reported there.
//!
//! --- BINDINGS ARE DATA -----------------------------------------------------------------------------------
//!
//! [`Bindings`] is a list of (button, modifiers) → gesture rows, and the four presets are four such
//! lists. Rebinding is editing a row. There is no branch anywhere below on which preset is active,
//! which is what keeps "preset-selectable" from meaning "three code paths, two of which are tested".

use crate::math::{Bounds, Quat, Vec3};
use crate::state::{Projection, ViewState};

/// A pointer button, named rather than numbered so a binding table reads.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Button {
    /// The primary button.
    Left,
    /// The wheel button.
    Middle,
    /// The secondary button.
    Right,
    /// The wheel itself, which binds like a button and drives a zoom.
    Wheel,
}

/// Held modifier keys.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct Modifiers {
    /// Shift.
    pub shift: bool,
    /// Control.
    pub control: bool,
    /// Alt.
    pub alt: bool,
}

impl Modifiers {
    /// Nothing held.
    pub const NONE: Self = Self {
        shift: false,
        control: false,
        alt: false,
    };

    /// Only alt.
    pub const ALT: Self = Self {
        shift: false,
        control: false,
        alt: true,
    };

    /// Only shift.
    pub const SHIFT: Self = Self {
        shift: true,
        control: false,
        alt: false,
    };
}

/// What a drag does.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Gesture {
    /// Turn about the pivot.
    Orbit,
    /// Slide the world under the cursor.
    Pan,
    /// Approach or retreat from the pivot.
    Zoom,
    /// Move the camera itself, first-person.
    Fly,
}

/// A named set of bindings.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum NavigationPreset {
    /// This editor's own: middle orbits, shift-middle pans, wheel zooms, right flies.
    #[default]
    Cyberdyne,
    /// Alt with the three buttons, as Maya has bound them for thirty years.
    Maya,
    /// Middle orbits, shift-middle pans, wheel zooms — no modifier for the common case.
    Blender,
    /// Right-drag flies, both buttons pan. The game-engine convention.
    Unreal,
}

impl NavigationPreset {
    /// A name for a settings panel.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            NavigationPreset::Cyberdyne => "Cyberdyne",
            NavigationPreset::Maya => "Maya",
            NavigationPreset::Blender => "Blender",
            NavigationPreset::Unreal => "Unreal",
        }
    }

    /// Every preset, for a settings panel and for the test that checks each one is complete.
    #[must_use]
    pub const fn all() -> [Self; 4] {
        [
            NavigationPreset::Cyberdyne,
            NavigationPreset::Maya,
            NavigationPreset::Blender,
            NavigationPreset::Unreal,
        ]
    }
}

/// One row of a binding table.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Binding {
    /// The button held.
    pub button: Button,
    /// The modifiers held with it.
    pub modifiers: Modifiers,
    /// What the drag does.
    pub gesture: Gesture,
}

/// What a drag with a given button and modifiers does.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Bindings {
    preset: NavigationPreset,
    rows: Vec<Binding>,
}

impl Default for Bindings {
    fn default() -> Self {
        Self::preset(NavigationPreset::default())
    }
}

impl Bindings {
    /// The bindings a named preset defines.
    #[must_use]
    pub fn preset(preset: NavigationPreset) -> Self {
        let rows = match preset {
            NavigationPreset::Cyberdyne => vec![
                row(Button::Middle, Modifiers::NONE, Gesture::Orbit),
                row(Button::Middle, Modifiers::SHIFT, Gesture::Pan),
                row(Button::Wheel, Modifiers::NONE, Gesture::Zoom),
                row(Button::Right, Modifiers::NONE, Gesture::Fly),
            ],
            NavigationPreset::Maya => vec![
                row(Button::Left, Modifiers::ALT, Gesture::Orbit),
                row(Button::Middle, Modifiers::ALT, Gesture::Pan),
                row(Button::Right, Modifiers::ALT, Gesture::Zoom),
                row(Button::Wheel, Modifiers::NONE, Gesture::Zoom),
            ],
            NavigationPreset::Blender => vec![
                row(Button::Middle, Modifiers::NONE, Gesture::Orbit),
                row(Button::Middle, Modifiers::SHIFT, Gesture::Pan),
                row(Button::Wheel, Modifiers::NONE, Gesture::Zoom),
                row(Button::Middle, Modifiers::ALT, Gesture::Fly),
            ],
            NavigationPreset::Unreal => vec![
                row(Button::Right, Modifiers::NONE, Gesture::Fly),
                row(Button::Left, Modifiers::NONE, Gesture::Orbit),
                row(Button::Middle, Modifiers::NONE, Gesture::Pan),
                row(Button::Wheel, Modifiers::NONE, Gesture::Zoom),
            ],
        };
        Self { preset, rows }
    }

    /// Which preset these started as. A rebound table keeps the name it was derived from, so a
    /// settings panel can say "Maya (modified)" rather than losing where the user started.
    #[must_use]
    pub const fn origin(&self) -> NavigationPreset {
        self.preset
    }

    /// The gesture a button and modifier combination performs, if any.
    #[must_use]
    pub fn gesture(&self, button: Button, modifiers: Modifiers) -> Option<Gesture> {
        self.rows
            .iter()
            .find(|binding| binding.button == button && binding.modifiers == modifiers)
            .map(|binding| binding.gesture)
    }

    /// Bind a combination, replacing whatever it did before.
    pub fn bind(&mut self, button: Button, modifiers: Modifiers, gesture: Gesture) {
        self.rows
            .retain(|binding| binding.button != button || binding.modifiers != modifiers);
        self.rows.push(row(button, modifiers, gesture));
    }

    /// Every row, for a settings panel.
    #[must_use]
    pub fn rows(&self) -> &[Binding] {
        &self.rows
    }
}

const fn row(button: Button, modifiers: Modifiers, gesture: Gesture) -> Binding {
    Binding {
        button,
        modifiers,
        gesture,
    }
}

/// The six directions a view-axis snap looks from.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ViewAxis {
    /// Looking along −Z.
    Front,
    /// Looking along +Z.
    Back,
    /// Looking along −X.
    Right,
    /// Looking along +X.
    Left,
    /// Looking along −Y.
    Top,
    /// Looking along +Y.
    Bottom,
}

impl ViewAxis {
    /// The direction the camera looks in this view.
    #[must_use]
    pub const fn forward(self) -> Vec3 {
        match self {
            ViewAxis::Front => Vec3::new(0.0, 0.0, -1.0),
            ViewAxis::Back => Vec3::new(0.0, 0.0, 1.0),
            ViewAxis::Right => Vec3::new(-1.0, 0.0, 0.0),
            ViewAxis::Left => Vec3::new(1.0, 0.0, 0.0),
            ViewAxis::Top => Vec3::new(0.0, -1.0, 0.0),
            ViewAxis::Bottom => Vec3::new(0.0, 1.0, 0.0),
        }
    }

    /// A name for a menu and for the orientation widget's labels.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            ViewAxis::Front => "Front",
            ViewAxis::Back => "Back",
            ViewAxis::Right => "Right",
            ViewAxis::Left => "Left",
            ViewAxis::Top => "Top",
            ViewAxis::Bottom => "Bottom",
        }
    }
}

/// Tunables a user changes and a preset does not.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct NavigationSettings {
    /// Radians the camera turns per pixel of orbit drag.
    pub orbit_radians_per_pixel: f32,
    /// Proportion of the pivot distance one wheel notch covers.
    pub zoom_per_notch: f32,
    /// Metres per second of fly, at a pivot distance of one metre. Scaled from there.
    pub fly_metres_per_second: f32,
    /// How near the pivot a zoom may bring the camera. Below this a zoom stops rather than passing
    /// through what it is looking at, which is the failure that loses a user their bearings.
    pub minimum_pivot_distance: f32,
    /// How much of the viewport a focused object fills, as a fraction of the vertical extent.
    pub focus_fill: f32,
}

impl Default for NavigationSettings {
    fn default() -> Self {
        Self {
            orbit_radians_per_pixel: 0.008,
            zoom_per_notch: 0.12,
            fly_metres_per_second: 1.0,
            minimum_pivot_distance: 0.05,
            focus_fill: 0.6,
        }
    }
}

/// The camera's navigator: the pivot it turns about, and the bindings that reach it.
///
/// Per viewport, and part of what a viewport persists. It is separate from [`ViewState`] because the
/// pivot distance is not part of an *observation* — two captures with the same camera and different
/// pivots are the same view — and `editor-viewport-and-gizmos` requires a captured view state to be
/// exactly what reproduces an observation.
#[derive(Clone, PartialEq, Debug)]
pub struct Navigator {
    /// How far ahead of the camera the point of interest is. Every speed derives from it.
    pivot_distance: f32,
    /// What a button does.
    pub bindings: Bindings,
    /// What the speeds are.
    pub settings: NavigationSettings,
}

impl Default for Navigator {
    fn default() -> Self {
        Self::new()
    }
}

impl Navigator {
    /// A navigator with the default bindings, pivoting ten metres ahead.
    #[must_use]
    pub fn new() -> Self {
        Self {
            pivot_distance: 10.0,
            bindings: Bindings::default(),
            settings: NavigationSettings::default(),
        }
    }

    /// How far the point of interest is.
    #[must_use]
    pub const fn pivot_distance(&self) -> f32 {
        self.pivot_distance
    }

    /// Move the point of interest, clamped to the minimum.
    pub fn set_pivot_distance(&mut self, distance: f32) {
        self.pivot_distance = distance.max(self.settings.minimum_pivot_distance);
    }

    /// Where the camera is looking, in world space.
    #[must_use]
    pub fn pivot(&self, state: &ViewState) -> Vec3 {
        state.camera.position + state.camera.forward() * self.pivot_distance
    }

    /// Turn about the pivot. `dx` and `dy` are pixels of cursor motion.
    ///
    /// Yaw is about the WORLD's up and pitch about the CAMERA's right, which is what keeps the
    /// horizon level through a long orbit. Yawing about the camera's own up accumulates roll, and
    /// the roll is invisible until the user notices the world is tilted and cannot say when it
    /// started.
    pub fn orbit(&self, state: &mut ViewState, dx: f32, dy: f32) {
        let pivot = self.pivot(state);
        let yaw = Quat::from_axis_angle(Vec3::Y, -dx * self.settings.orbit_radians_per_pixel);
        let pitched = self.pitch(state, dy);
        let rotation = yaw.after(pitched).normalized();
        state.camera.rotation = rotation;
        state.camera.position = pivot - state.camera.forward() * self.pivot_distance;
    }

    /// The camera's rotation after a pitch of `dy` pixels, refusing one that would tip past
    /// vertical.
    ///
    /// A camera that passes the pole flips the horizon, and the user's next orbit drag goes the
    /// wrong way. Stopping just short is what every editor does and it is worth saying why: the
    /// alternative — allowing it — is not a richer camera, it is a camera whose controls invert
    /// without warning.
    fn pitch(&self, state: &ViewState, dy: f32) -> Quat {
        let axis = state.camera.right();
        let pitch = Quat::from_axis_angle(axis, -dy * self.settings.orbit_radians_per_pixel);
        let candidate = pitch.after(state.camera.rotation).normalized();
        let forward = candidate.rotate(-Vec3::Z);
        if forward.dot(Vec3::Y).abs() > 0.999 {
            return state.camera.rotation;
        }
        candidate
    }

    /// Slide the world under the cursor. `dx` and `dy` are pixels.
    ///
    /// One pixel moves the world by exactly the world distance a pixel covers at the pivot's depth,
    /// so the point under the cursor stays under it — for a point at the pivot's distance, which is
    /// the one the user is looking at.
    pub fn pan(&self, state: &mut ViewState, dx: f32, dy: f32) {
        let per_pixel = self.world_units_per_pixel(state);
        let right = state.camera.right();
        let up = state.camera.up();
        state.camera.position =
            state.camera.position - right * (dx * per_pixel) + up * (dy * per_pixel);
    }

    /// World units one vertical pixel covers at the pivot's distance.
    fn world_units_per_pixel(&self, state: &ViewState) -> f32 {
        let height = f32::from(u16::try_from(state.viewport.height.max(1)).unwrap_or(u16::MAX));
        match state.projection {
            Projection::Perspective { fov_y } => {
                2.0 * (fov_y * 0.5).tan() * self.pivot_distance / height
            }
            Projection::Orthographic { height: extent } => extent / height,
        }
    }

    /// Approach or retreat from the pivot. `notches` is wheel detents; positive approaches.
    ///
    /// A PROPORTION of the distance rather than an amount of it. That is what makes the same notch
    /// useful at both scales, and it is also why the camera never quite reaches the pivot: the
    /// remaining distance is multiplied, so it approaches and stops at the minimum rather than
    /// passing through the object.
    pub fn zoom(&mut self, state: &mut ViewState, notches: f32) {
        let factor = (-notches * self.settings.zoom_per_notch).exp();
        let target = (self.pivot_distance * factor).max(self.settings.minimum_pivot_distance);
        let moved = self.pivot_distance - target;
        state.camera.position = state.camera.position + state.camera.forward() * moved;
        self.pivot_distance = target;
        if let Projection::Orthographic { height } = state.projection {
            // An orthographic camera has no perspective to approach through, so its zoom is the
            // extent it shows. Moving it without this would change nothing on screen, which reads
            // as a broken wheel.
            state.projection = Projection::Orthographic {
                height: (height * factor).max(self.settings.minimum_pivot_distance),
            };
        }
    }

    /// Move the camera itself. `direction` is in camera space — x right, y up, z backward — and
    /// `seconds` is how long the key was held.
    ///
    /// The pivot travels with the camera, so flying forward for a while and then orbiting turns
    /// about what is now in front rather than about where the camera used to be looking.
    pub fn fly(&self, state: &mut ViewState, direction: Vec3, seconds: f32) {
        let speed = self.settings.fly_metres_per_second * self.pivot_distance.max(0.1);
        let local = direction.normalized_or(Vec3::ZERO);
        let world = state.camera.rotation.rotate(local);
        state.camera.position = state.camera.position + world * (speed * seconds);
    }

    /// Put `bounds` in the middle of the view at a comfortable size, keeping the view direction.
    ///
    /// "Focus on selection" and "frame all" are the same operation over different bounds, so they
    /// are one function. Keeping the direction is what makes focus feel like a zoom rather than a
    /// teleport: the user's mental orientation survives it.
    pub fn focus(&mut self, state: &mut ViewState, bounds: Bounds) {
        let radius = bounds.radius().max(self.settings.minimum_pivot_distance);
        let distance = match state.projection {
            Projection::Perspective { fov_y } => {
                let half = (fov_y * 0.5 * self.settings.focus_fill).tan().max(1e-3);
                radius / half
            }
            Projection::Orthographic { .. } => {
                state.projection = Projection::Orthographic {
                    height: radius * 2.0 / self.settings.focus_fill,
                };
                self.pivot_distance.max(radius * 2.0)
            }
        };
        self.pivot_distance = distance.max(self.settings.minimum_pivot_distance);
        state.camera.position = bounds.center() - state.camera.forward() * self.pivot_distance;
    }

    /// Look along a principal axis, keeping the pivot and the distance.
    ///
    /// This is what the view-orientation widget drives. It moves the CAMERA and nothing else — see
    /// [`crate::overlay`] for why that distinction is a requirement rather than an implementation
    /// note.
    pub fn snap_to_axis(&self, state: &mut ViewState, axis: ViewAxis) {
        let pivot = self.pivot(state);
        state.camera.rotation = look_along(axis.forward());
        state.camera.position = pivot - state.camera.forward() * self.pivot_distance;
    }
}

/// The rotation of a camera looking in `forward` with the world's up.
///
/// Straight up and straight down have no unique answer — every roll about the view axis looks the
/// same — so the reference direction becomes −Z there. Without that, a top view is a division by a
/// zero-length cross product and the camera's rotation becomes NaN, which is a viewport that has
/// gone black for reasons nobody can see.
///
/// Public because [`crate::overlay::ViewPreset`] needs exactly this and a second implementation of
/// "look that way" is how two parts of a viewport come to disagree about which way is up.
#[must_use]
pub fn look_along(forward: Vec3) -> Quat {
    let forward = forward.normalized_or(-Vec3::Z);
    let reference = if forward.dot(Vec3::Y).abs() > 0.999 {
        -Vec3::Z
    } else {
        Vec3::Y
    };
    let right = reference.cross(-forward).normalized_or(Vec3::X);
    let up = (-forward).cross(right);
    quat_from_basis(right, up, -forward)
}

/// A rotation from three orthonormal columns, by Shepperd's method: pick the largest diagonal term
/// so the square root is never taken of something near zero.
fn quat_from_basis(right: Vec3, up: Vec3, backward: Vec3) -> Quat {
    let trace = right.x + up.y + backward.z;
    if trace > 0.0 {
        let scale = (trace + 1.0).sqrt() * 2.0;
        return Quat {
            x: (up.z - backward.y) / scale,
            y: (backward.x - right.z) / scale,
            z: (right.y - up.x) / scale,
            w: 0.25 * scale,
        }
        .normalized();
    }
    if right.x > up.y && right.x > backward.z {
        let scale = (1.0 + right.x - up.y - backward.z).sqrt() * 2.0;
        return Quat {
            x: 0.25 * scale,
            y: (up.x + right.y) / scale,
            z: (backward.x + right.z) / scale,
            w: (up.z - backward.y) / scale,
        }
        .normalized();
    }
    if up.y > backward.z {
        let scale = (1.0 + up.y - right.x - backward.z).sqrt() * 2.0;
        return Quat {
            x: (up.x + right.y) / scale,
            y: 0.25 * scale,
            z: (backward.y + up.z) / scale,
            w: (backward.x - right.z) / scale,
        }
        .normalized();
    }
    let scale = (1.0 + backward.z - right.x - up.y).sqrt() * 2.0;
    Quat {
        x: (backward.x + right.z) / scale,
        y: (backward.y + up.z) / scale,
        z: 0.25 * scale,
        w: (right.y - up.x) / scale,
    }
    .normalized()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn looking_at_origin() -> (Navigator, ViewState) {
        let mut navigator = Navigator::new();
        let mut state = ViewState::new();
        navigator.set_pivot_distance(10.0);
        state.camera.position = Vec3::new(0.0, 0.0, 10.0);
        (navigator, state)
    }

    #[test]
    fn orbiting_keeps_the_pivot_where_it_was() {
        let (navigator, mut state) = looking_at_origin();
        let pivot = navigator.pivot(&state);
        for _ in 0..40 {
            navigator.orbit(&mut state, 7.0, 3.0);
        }
        let after = navigator.pivot(&state);
        assert!(after.nearly_equals(pivot, 1e-2), "{after:?} vs {pivot:?}");
        // And the distance is unchanged: an orbit turns, it does not approach.
        let distance = (state.camera.position - pivot).length();
        assert!((distance - 10.0).abs() < 1e-2, "{distance}");
    }

    #[test]
    fn an_orbit_cannot_tip_past_vertical_and_flip_the_horizon() {
        let (navigator, mut state) = looking_at_origin();
        for _ in 0..500 {
            navigator.orbit(&mut state, 0.0, 20.0);
        }
        let forward = state.camera.forward();
        assert!(
            forward.dot(Vec3::Y).abs() <= 0.9995,
            "the camera passed the pole: {forward:?}"
        );
    }

    #[test]
    fn zoom_is_a_proportion_so_the_same_notch_suits_both_scales() {
        let (mut near, mut near_state) = looking_at_origin();
        near.set_pivot_distance(1.0);
        let before_near = near.pivot_distance();
        near.zoom(&mut near_state, 1.0);
        let near_step = before_near - near.pivot_distance();

        let (mut far, mut far_state) = looking_at_origin();
        far.set_pivot_distance(1000.0);
        let before_far = far.pivot_distance();
        far.zoom(&mut far_state, 1.0);
        let far_step = before_far - far.pivot_distance();

        assert!(
            far_step > near_step * 100.0,
            "a fixed step would make the far camera unusable: {near_step} vs {far_step}"
        );
    }

    #[test]
    fn zooming_in_forever_stops_short_of_the_thing_being_looked_at() {
        let (mut navigator, mut state) = looking_at_origin();
        for _ in 0..1000 {
            navigator.zoom(&mut state, 1.0);
        }
        assert!(navigator.pivot_distance() >= navigator.settings.minimum_pivot_distance);
        assert!(
            state.camera.position.z > 0.0,
            "the camera passed through what it was looking at: {:?}",
            state.camera.position
        );
    }

    #[test]
    fn panning_moves_the_world_by_what_a_pixel_covers_at_the_pivot() {
        let (navigator, mut state) = looking_at_origin();
        let before = state.camera.position;
        navigator.pan(&mut state, 100.0, 0.0);
        // 60 degree vertical field, 1080 pixels tall, pivot 10 m away: one pixel is
        // 2 * tan(30°) * 10 / 1080 ≈ 0.010692 m, so a hundred pixels is about 1.069 m.
        let moved = before.x - state.camera.position.x;
        assert!((moved - 1.069).abs() < 0.01, "{moved}");
    }

    #[test]
    fn focus_frames_the_bounds_without_turning_the_camera() {
        let (mut navigator, mut state) = looking_at_origin();
        let before = state.camera.rotation;
        let bounds =
            Bounds::from_center_extents(Vec3::new(50.0, 0.0, 0.0), Vec3::new(2.0, 2.0, 2.0));
        navigator.focus(&mut state, bounds);

        assert_eq!(
            state.camera.rotation, before,
            "focus is a zoom, not a teleport"
        );
        let pivot = navigator.pivot(&state);
        assert!(pivot.nearly_equals(bounds.center(), 1e-3), "{pivot:?}");
        assert!(navigator.pivot_distance() > bounds.radius());
    }

    #[test]
    fn a_view_axis_snap_looks_the_right_way_including_straight_down() {
        let (navigator, mut state) = looking_at_origin();
        for axis in [
            ViewAxis::Front,
            ViewAxis::Back,
            ViewAxis::Left,
            ViewAxis::Right,
            ViewAxis::Top,
            ViewAxis::Bottom,
        ] {
            let mut snapped = state.clone();
            navigator.snap_to_axis(&mut snapped, axis);
            let forward = snapped.camera.forward();
            assert!(
                forward.nearly_equals(axis.forward(), 1e-4),
                "{}: {forward:?}",
                axis.name()
            );
            assert!(!forward.x.is_nan(), "{} produced a NaN", axis.name());
        }
        navigator.snap_to_axis(&mut state, ViewAxis::Top);
        assert!(state.camera.position.y > 0.0, "top looks down from above");
    }

    #[test]
    fn every_preset_binds_every_gesture_and_they_are_not_all_the_same() {
        for preset in NavigationPreset::all() {
            let bindings = Bindings::preset(preset);
            for gesture in [Gesture::Orbit, Gesture::Pan, Gesture::Zoom] {
                assert!(
                    bindings.rows().iter().any(|row| row.gesture == gesture),
                    "{} does not bind {gesture:?}",
                    preset.name()
                );
            }
            assert_eq!(bindings.origin(), preset);
        }
        assert_ne!(
            Bindings::preset(NavigationPreset::Maya),
            Bindings::preset(NavigationPreset::Unreal)
        );
    }

    #[test]
    fn rebinding_replaces_a_row_rather_than_adding_a_second_meaning() {
        let mut bindings = Bindings::preset(NavigationPreset::Maya);
        let before = bindings.rows().len();
        assert_eq!(
            bindings.gesture(Button::Left, Modifiers::ALT),
            Some(Gesture::Orbit)
        );

        bindings.bind(Button::Left, Modifiers::ALT, Gesture::Pan);
        assert_eq!(
            bindings.gesture(Button::Left, Modifiers::ALT),
            Some(Gesture::Pan)
        );
        assert_eq!(bindings.rows().len(), before, "one row, not two");
        assert_eq!(bindings.gesture(Button::Left, Modifiers::NONE), None);
    }
}
