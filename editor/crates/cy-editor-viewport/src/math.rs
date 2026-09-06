//! The small amount of geometry the editor needs to express *intent*.
//!
//! --- WHY THERE IS ANY MATHEMATICS IN THE EDITOR AT ALL ---------------------------------------------
//!
//! `editor-viewport-and-gizmos` divides the work in one sentence: "The editor decides what should be
//! shown. The renderer decides how it is drawn." Reading that table carefully says what the editor
//! must be able to compute and what it must not:
//!
//! | Editor | Renderer |
//! |---|---|
//! | Camera pose, projection, and view state | Render graph, culling, LOD, materials, lighting |
//! | Gizmo **intent** and manipulation state | Gizmo **geometry**, depth behaviour, screen-constant sizing |
//! | Snapping rules | Selection outlines, highlights, overlays |
//!
//! Turning a cursor's motion into a translation along an axis is manipulation state, and it is the
//! editor's. It needs a ray through a pixel, a plane intersection and a quaternion — and that is the
//! whole of this module. There is no matrix multiplication on a frame path here, nothing is drawn,
//! and nothing here decides what a pixel looks like.
//!
//! **Picking is deliberately not in this list.** The editor sends a PIXEL and the identifier of the
//! frame it was clicked on; the runtime builds the ray from that frame's own view state and resolves
//! it against the draw list (`src/servers/render/picking.h`). An editor that built its own pick ray
//! would be one line away from resolving it too, and "editor-side picking that does not match what
//! the engine rendered" is a named forbidden pattern.
//!
//! --- CONVENTIONS, WHICH ARE THE ENGINE'S ------------------------------------------------------------
//!
//! Right-handed, Y up, a camera looking down its local −Z, column-vector convention (`m * v`), and
//! reversed-Z projections where 1 is the near plane. They match `src/core/math/` and
//! `src/servers/render/model.h` because a second convention is a second set of sign errors, and
//! `tests/rays_agree_with_the_engine.rs` pins the agreement to numbers rather than to this comment.
//!
//! --- WHERE THIS SHOULD EVENTUALLY LIVE --------------------------------------------------------------
//!
//! `cy-editor-core`, at layer 0, the moment a second crate needs it. It is here because exactly one
//! does, and moving a type down a layer speculatively is how a foundation crate acquires things
//! nobody uses.

use std::ops::{Add, Mul, Neg, Sub};

/// Below this a length is treated as zero. Squared lengths are compared against its square.
///
/// The same 1e-8 the engine uses for `kSmallLength`, so a direction the engine would refuse to
/// normalise is one this module refuses too.
pub const SMALL_LENGTH: f32 = 1e-8;

/// A point or a direction in three dimensions.
#[derive(Clone, Copy, PartialEq, Debug, Default)]
pub struct Vec3 {
    /// The first lane.
    pub x: f32,
    /// The second lane.
    pub y: f32,
    /// The third lane.
    pub z: f32,
}

impl Vec3 {
    /// The origin, and the zero direction.
    pub const ZERO: Self = Self::new(0.0, 0.0, 0.0);
    /// The engine's right.
    pub const X: Self = Self::new(1.0, 0.0, 0.0);
    /// The engine's up.
    pub const Y: Self = Self::new(0.0, 1.0, 0.0);
    /// The engine's backward; a camera looks down its negative.
    pub const Z: Self = Self::new(0.0, 0.0, 1.0);

    /// A vector with these lanes.
    #[must_use]
    pub const fn new(x: f32, y: f32, z: f32) -> Self {
        Self { x, y, z }
    }

    /// The same three numbers a `Value::Vec3` carries, in the same order.
    #[must_use]
    pub const fn from_array(lanes: [f32; 3]) -> Self {
        Self::new(lanes[0], lanes[1], lanes[2])
    }

    /// The three numbers, for a `Value::Vec3`.
    #[must_use]
    pub const fn to_array(self) -> [f32; 3] {
        [self.x, self.y, self.z]
    }

    /// The dot product.
    #[must_use]
    pub fn dot(self, other: Self) -> f32 {
        self.x
            .mul_add(other.x, self.y.mul_add(other.y, self.z * other.z))
    }

    /// The cross product.
    #[must_use]
    pub fn cross(self, other: Self) -> Self {
        Self::new(
            self.y.mul_add(other.z, -(self.z * other.y)),
            self.z.mul_add(other.x, -(self.x * other.z)),
            self.x.mul_add(other.y, -(self.y * other.x)),
        )
    }

    /// The squared length. Compared against [`SMALL_LENGTH`] squared to avoid a root.
    #[must_use]
    pub fn length_squared(self) -> f32 {
        self.dot(self)
    }

    /// The length.
    #[must_use]
    pub fn length(self) -> f32 {
        self.length_squared().sqrt()
    }

    /// The unit vector in this direction, or `fallback` when there is no direction to return.
    ///
    /// There is no infallible `normalize`, on purpose. Normalising a zero vector has no right answer
    /// and every wrong one is a NaN that spreads silently through a drag; making the caller supply
    /// what it wants instead is one word at each call site and no NaN anywhere.
    #[must_use]
    pub fn normalized_or(self, fallback: Self) -> Self {
        let length_squared = self.length_squared();
        if length_squared <= SMALL_LENGTH * SMALL_LENGTH {
            return fallback;
        }
        let inverse = 1.0 / length_squared.sqrt();
        self * inverse
    }

    /// Each lane multiplied by the matching lane of `other`.
    #[must_use]
    pub fn component_mul(self, other: Self) -> Self {
        Self::new(self.x * other.x, self.y * other.y, self.z * other.z)
    }

    /// Whether every lane is within `tolerance` of `other`'s.
    #[must_use]
    pub fn nearly_equals(self, other: Self, tolerance: f32) -> bool {
        (self.x - other.x).abs() <= tolerance
            && (self.y - other.y).abs() <= tolerance
            && (self.z - other.z).abs() <= tolerance
    }
}

impl Add for Vec3 {
    type Output = Self;
    fn add(self, other: Self) -> Self {
        Self::new(self.x + other.x, self.y + other.y, self.z + other.z)
    }
}

impl Sub for Vec3 {
    type Output = Self;
    fn sub(self, other: Self) -> Self {
        Self::new(self.x - other.x, self.y - other.y, self.z - other.z)
    }
}

impl Mul<f32> for Vec3 {
    type Output = Self;
    fn mul(self, scalar: f32) -> Self {
        Self::new(self.x * scalar, self.y * scalar, self.z * scalar)
    }
}

impl Neg for Vec3 {
    type Output = Self;
    fn neg(self) -> Self {
        Self::new(-self.x, -self.y, -self.z)
    }
}

/// A rotation, in the ABI's `x, y, z, w` order.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Quat {
    /// The i lane.
    pub x: f32,
    /// The j lane.
    pub y: f32,
    /// The k lane.
    pub z: f32,
    /// The real lane.
    pub w: f32,
}

impl Default for Quat {
    fn default() -> Self {
        Self::IDENTITY
    }
}

impl Quat {
    /// No rotation.
    pub const IDENTITY: Self = Self {
        x: 0.0,
        y: 0.0,
        z: 0.0,
        w: 1.0,
    };

    /// The four numbers a `Value::Quat` carries, in the same order.
    #[must_use]
    pub const fn from_array(lanes: [f32; 4]) -> Self {
        Self {
            x: lanes[0],
            y: lanes[1],
            z: lanes[2],
            w: lanes[3],
        }
    }

    /// The four numbers, for a `Value::Quat`.
    #[must_use]
    pub const fn to_array(self) -> [f32; 4] {
        [self.x, self.y, self.z, self.w]
    }

    /// A rotation of `radians` about `axis`. `axis` need not be unit length.
    #[must_use]
    pub fn from_axis_angle(axis: Vec3, radians: f32) -> Self {
        let axis = axis.normalized_or(Vec3::Y);
        let (sin, cos) = (radians * 0.5).sin_cos();
        Self {
            x: axis.x * sin,
            y: axis.y * sin,
            z: axis.z * sin,
            w: cos,
        }
    }

    /// The rotation that undoes this one.
    #[must_use]
    pub fn inverse(self) -> Self {
        Self {
            x: -self.x,
            y: -self.y,
            z: -self.z,
            w: self.w,
        }
    }

    /// `self` after `other`: the rotation that applies `other` first, then this one.
    ///
    /// Named `after` rather than `mul` because a quaternion product is not commutative and the
    /// reading order is the thing that gets it wrong: `world_turn.after(current)` turns about a
    /// world axis and `current.after(local_turn)` turns about the object's own. A method called
    /// `mul` reads as neither.
    #[must_use]
    pub fn after(self, other: Self) -> Self {
        Self {
            x: self.w * other.x + self.x * other.w + self.y * other.z - self.z * other.y,
            y: self.w * other.y - self.x * other.z + self.y * other.w + self.z * other.x,
            z: self.w * other.z + self.x * other.y - self.y * other.x + self.z * other.w,
            w: self.w * other.w - self.x * other.x - self.y * other.y - self.z * other.z,
        }
    }

    /// Rotate a vector.
    #[must_use]
    pub fn rotate(self, vector: Vec3) -> Vec3 {
        let imaginary = Vec3::new(self.x, self.y, self.z);
        let first = imaginary.cross(vector);
        let second = imaginary.cross(first);
        vector + (first * self.w + second) * 2.0
    }

    /// The unit quaternion nearest this one, or the identity when it has no length.
    ///
    /// A drag composes rotations every frame, and a composition of two unit quaternions is only
    /// nearly unit. Renormalising once per update is what keeps a long drag from slowly scaling the
    /// object it is rotating — a defect that looks like a physics bug and is arithmetic.
    #[must_use]
    pub fn normalized(self) -> Self {
        let length_squared = self.w.mul_add(
            self.w,
            self.x
                .mul_add(self.x, self.y.mul_add(self.y, self.z * self.z)),
        );
        if length_squared <= SMALL_LENGTH {
            return Self::IDENTITY;
        }
        let inverse = 1.0 / length_squared.sqrt();
        Self {
            x: self.x * inverse,
            y: self.y * inverse,
            z: self.z * inverse,
            w: self.w * inverse,
        }
    }
}

/// An axis-aligned box, for focusing and framing.
///
/// Held as a minimum and a maximum rather than a centre and an extent because that is the form the
/// engine reports bounds in, and converting once at the boundary is cheaper than converting at every
/// use of a field somebody has to remember is half-width.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Bounds {
    /// The lower corner.
    pub min: Vec3,
    /// The upper corner.
    pub max: Vec3,
}

impl Bounds {
    /// A box enclosing one point, which is what focusing on an object with no extent gets.
    #[must_use]
    pub const fn point(point: Vec3) -> Self {
        Self {
            min: point,
            max: point,
        }
    }

    /// A box from a centre and a half-extent.
    #[must_use]
    pub fn from_center_extents(center: Vec3, extents: Vec3) -> Self {
        Self {
            min: center - extents,
            max: center + extents,
        }
    }

    /// The middle.
    #[must_use]
    pub fn center(self) -> Vec3 {
        (self.min + self.max) * 0.5
    }

    /// The radius of the sphere that encloses the box. What a focus distance is computed from, so
    /// that framing does not depend on which way the box is turned relative to the camera.
    #[must_use]
    pub fn radius(self) -> f32 {
        (self.max - self.min).length() * 0.5
    }

    /// The box enclosing both.
    #[must_use]
    pub fn union(self, other: Self) -> Self {
        Self {
            min: Vec3::new(
                self.min.x.min(other.min.x),
                self.min.y.min(other.min.y),
                self.min.z.min(other.min.z),
            ),
            max: Vec3::new(
                self.max.x.max(other.max.x),
                self.max.y.max(other.max.y),
                self.max.z.max(other.max.z),
            ),
        }
    }
}

/// A ray in world space. `direction` is unit length wherever this module produces one.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Ray {
    /// Where it starts.
    pub origin: Vec3,
    /// Which way it goes.
    pub direction: Vec3,
}

impl Ray {
    /// The point `distance` along the ray.
    #[must_use]
    pub fn at(self, distance: f32) -> Vec3 {
        self.origin + self.direction * distance
    }

    /// Where the ray meets the plane through `point` with normal `normal`, or `None` when it is
    /// parallel to it.
    ///
    /// Rejects a hit behind the origin as well as a parallel ray. A drag that grabbed a plane
    /// behind the camera would jump the object to the other side of the world in one frame, which
    /// is the single most alarming failure a gizmo has.
    #[must_use]
    pub fn intersect_plane(self, point: Vec3, normal: Vec3) -> Option<Vec3> {
        let denominator = self.direction.dot(normal);
        if denominator.abs() <= 1e-6 {
            return None;
        }
        let distance = (point - self.origin).dot(normal) / denominator;
        if distance <= 0.0 {
            return None;
        }
        Some(self.at(distance))
    }

    /// The parameter along the line through `point` in direction `axis` that is closest to this ray.
    ///
    /// The classic closest-approach of two lines: the point returned is `point + axis * t`. `None`
    /// when the ray and the axis are parallel, where every point on the axis is equally close and
    /// there is no answer to give.
    ///
    /// THE SIGN OF THIS IS THE WHOLE OF AN AXIS DRAG, and it was wrong once. With `r` from the ray's
    /// origin to the axis's point, `b` the cosine between the two directions, and the two directions
    /// unit length, the parameter is `(b * (r·direction) - (r·axis)) / (1 - b²)` — and the negation
    /// of that is also a plausible-looking expression that is symmetric about the perpendicular case,
    /// so a test that drags through the origin passes with either. `tests` below therefore checks a
    /// case whose answer is a specific non-zero number.
    #[must_use]
    pub fn closest_parameter_on_axis(self, point: Vec3, axis: Vec3) -> Option<f32> {
        let axis = axis.normalized_or(Vec3::X);
        let between = point - self.origin;
        let axis_dot_direction = axis.dot(self.direction);
        let determinant = 1.0 - axis_dot_direction * axis_dot_direction;
        if determinant.abs() <= 1e-6 {
            return None;
        }
        let along_axis = between.dot(axis);
        let along_ray = between.dot(self.direction);
        Some((axis_dot_direction * along_ray - along_axis) / determinant)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rotating_by_an_axis_angle_turns_a_vector_the_way_the_engine_does() {
        let quarter = Quat::from_axis_angle(Vec3::Y, std::f32::consts::FRAC_PI_2);
        let turned = quarter.rotate(Vec3::X);
        // A quarter turn about +Y takes +X to −Z, which is the right-handed convention the engine
        // uses and the one a gizmo's arrow has to agree with.
        assert!(turned.nearly_equals(-Vec3::Z, 1e-5), "{turned:?}");
    }

    #[test]
    fn a_rotation_and_its_inverse_compose_to_nothing() {
        let rotation = Quat::from_axis_angle(Vec3::new(1.0, 2.0, 3.0), 0.7);
        let round_trip = rotation.after(rotation.inverse()).normalized();
        assert!((round_trip.w.abs() - 1.0).abs() < 1e-5, "{round_trip:?}");
    }

    #[test]
    fn normalising_a_zero_vector_returns_the_fallback_rather_than_a_nan() {
        let fallback = Vec3::Z;
        assert_eq!(Vec3::ZERO.normalized_or(fallback), fallback);
        assert!(!Vec3::ZERO.normalized_or(fallback).x.is_nan());
    }

    #[test]
    fn a_ray_parallel_to_a_plane_reports_no_hit_instead_of_an_infinity() {
        let ray = Ray {
            origin: Vec3::new(0.0, 1.0, 0.0),
            direction: Vec3::X,
        };
        assert!(ray.intersect_plane(Vec3::ZERO, Vec3::Y).is_none());
        // And a plane it does meet, in front of it.
        let hit = ray.intersect_plane(Vec3::new(5.0, 0.0, 0.0), Vec3::X);
        assert!(
            hit.expect("a hit in front")
                .nearly_equals(Vec3::new(5.0, 1.0, 0.0), 1e-5)
        );
    }

    #[test]
    fn a_plane_behind_the_ray_is_not_a_hit() {
        let ray = Ray {
            origin: Vec3::ZERO,
            direction: -Vec3::Z,
        };
        assert!(
            ray.intersect_plane(Vec3::new(0.0, 0.0, 10.0), Vec3::Z)
                .is_none()
        );
    }

    #[test]
    fn the_parameter_on_an_axis_has_the_sign_a_drag_expects() {
        // REGRESSION. This expression was written with its sign inverted, and every symmetric test
        // passed: dragging through the perpendicular case gives zero either way. The defect showed
        // up as an axis gizmo that moved the object the wrong way, and it was found by an
        // end-to-end test rather than by this file — which is why the case below has a specific
        // non-zero answer.
        //
        // A ray from (0, 0, 20) heading down and to the right crosses the world X axis at x = 5;
        // measured from a pivot at x = 2, that is +3.
        let direction = Vec3::new(5.0, 0.0, -20.0).normalized_or(-Vec3::Z);
        let ray = Ray {
            origin: Vec3::new(0.0, 0.0, 20.0),
            direction,
        };
        let parameter = ray
            .closest_parameter_on_axis(Vec3::new(2.0, 0.0, 0.0), Vec3::X)
            .expect("not parallel");
        assert!((parameter - 3.0).abs() < 1e-3, "{parameter}");

        // And the point it names is on the axis where the ray crosses it.
        let landed = Vec3::new(2.0, 0.0, 0.0) + Vec3::X * parameter;
        assert!(
            landed.nearly_equals(Vec3::new(5.0, 0.0, 0.0), 1e-3),
            "{landed:?}"
        );
    }

    #[test]
    fn the_closest_point_on_an_axis_is_the_perpendicular_foot() {
        let ray = Ray {
            origin: Vec3::new(0.0, 5.0, 0.0),
            direction: -Vec3::Y,
        };
        let parameter = ray
            .closest_parameter_on_axis(Vec3::ZERO, Vec3::X)
            .expect("not parallel");
        assert!(parameter.abs() < 1e-5, "{parameter}");

        // Parallel: there is no closest point, and saying so is better than picking one.
        let along = Ray {
            origin: Vec3::new(0.0, 1.0, 0.0),
            direction: Vec3::X,
        };
        assert!(
            along
                .closest_parameter_on_axis(Vec3::ZERO, Vec3::X)
                .is_none()
        );
    }
}
