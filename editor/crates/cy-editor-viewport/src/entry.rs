//! Per-axis numeric entry: position, rotation and scale, typed rather than dragged. Task 2.4.6.
//!
//! `docs/design/images/transform-gizmo.png` has a **Numeric Input** panel with nine fields — three
//! rows of X, Y and Z — and one sentence under it: "Type values for precise transformation." The
//! capability spec says the same thing twice over: "numeric entry SHALL be available for **every**
//! manipulation, and SHALL accept expressions and units", and "manipulation SHALL show numeric
//! feedback of the delta and the resulting value".
//!
//! --- WHY THIS IS THE SAME PATH AS A DRAG, AND NOT A SHORTCUT --------------------------------------
//!
//! A typed value is a manipulation. It produces **one transaction**, it is recorded with the same
//! `SetField` operations, it undoes identically, and it goes to the runtime through the same echo. If
//! it did not, the editor would have two ways to move an object and only one of them would be right —
//! and the wrong one would be the one used for precision work, where being wrong matters most.
//!
//! So this module writes through [`crate::gizmo`]'s own binding and does nothing else. It does not
//! know about handles, rays, cameras or pixels, because a typed value does not need one — which is
//! also why numeric entry keeps working when the runtime is not attached and no gizmo is drawn.
//!
//! --- ROTATION IS SHOWN IN DEGREES AND STORED AS A QUATERNION ---------------------------------------
//!
//! Three Euler angles are what a person can type and a quaternion is what the document holds, so the
//! conversion happens here, in one place, in a fixed order (X, then Y, then Z, applied intrinsically —
//! the order every tool this one will be compared against uses). Two consequences worth stating,
//! because both surprise people:
//!
//! * **The angles shown are not stored.** A rotation has many Euler representations and the one
//!   displayed is the canonical one for the stored quaternion. Typing 370° into a field that then
//!   reads 10° is not a defect.
//! * **Typing into one field rewrites all three.** [`apply`] reads the other two axes back out of the
//!   quaternion, which is the only way to change one angle without inventing values for the others.
//!
//! --- WHAT AN EMPTY SELECTION AND A MIXED ONE DO ----------------------------------------------------
//!
//! Several objects with different positions have no single value to show, and a field that showed the
//! first one's would silently overwrite the rest on the next keystroke. [`read`] answers `Mixed` and
//! the interface draws a field with no value in it, which is what `editor-ui-ux`'s multi-selection
//! rule requires — and typing into a mixed field is still legal, because "make them all 0" is a
//! reasonable thing to ask for.

use cy_editor_core::Actor;
use cy_editor_core::ids::NodeId;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::Value;
use cy_editor_documents::Document;

use crate::gizmo::{Transform3, TransformBinding};
use crate::math::{Quat, Vec3};
use crate::snapping::{Quantity, evaluate};

/// Which row of the numeric panel: the reference's Position, Rotation and Scale.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Row {
    /// Where it is, in metres.
    Position,
    /// Which way it faces, in degrees.
    Rotation,
    /// How big it is, as a factor.
    Scale,
}

impl Row {
    /// All three, in the order the panel draws them.
    pub const ALL: [Row; 3] = [Row::Position, Row::Rotation, Row::Scale];

    /// The label the panel shows.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            Row::Position => "Position",
            Row::Rotation => "Rotation",
            Row::Scale => "Scale",
        }
    }

    /// What this row's fields accept, which decides which units and suffixes parse.
    #[must_use]
    pub const fn quantity(self) -> Quantity {
        match self {
            Row::Position => Quantity::Length,
            Row::Rotation => Quantity::Angle,
            Row::Scale => Quantity::Factor,
        }
    }
}

/// What a field shows.
#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Field {
    /// One value, because every selected object agrees.
    Value(f32),
    /// The selected objects disagree. The field is drawn empty and typing into it sets them all.
    Mixed,
    /// Nothing is selected, or nothing selected carries a transform.
    Nothing,
}

impl Field {
    /// The number to put in the field, or nothing.
    #[must_use]
    pub const fn number(self) -> Option<f32> {
        match self {
            Field::Value(value) => Some(value),
            Field::Mixed | Field::Nothing => None,
        }
    }

    /// The text a field shows: a number, "—" for a mixed selection, empty for none.
    #[must_use]
    pub fn text(self, row: Row) -> String {
        match self {
            Field::Value(value) => match row {
                Row::Rotation => format!("{value:.2}"),
                _ => format!("{value:.3}"),
            },
            Field::Mixed => "—".to_string(),
            Field::Nothing => String::new(),
        }
    }
}

/// The nine fields, as the panel shows them.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Fields {
    /// Position X, Y, Z, in metres.
    pub position: [Field; 3],
    /// Rotation X, Y, Z, in degrees.
    pub rotation: [Field; 3],
    /// Scale X, Y, Z, as factors.
    pub scale: [Field; 3],
}

impl Fields {
    /// Nothing selected.
    #[must_use]
    pub const fn nothing() -> Self {
        Self {
            position: [Field::Nothing; 3],
            rotation: [Field::Nothing; 3],
            scale: [Field::Nothing; 3],
        }
    }

    /// One row of three.
    #[must_use]
    pub const fn row(&self, row: Row) -> [Field; 3] {
        match row {
            Row::Position => self.position,
            Row::Rotation => self.rotation,
            Row::Scale => self.scale,
        }
    }
}

/// What the panel shows for a selection.
#[must_use]
pub fn read(document: &Document, binding: TransformBinding, nodes: &[NodeId]) -> Fields {
    let transforms: Vec<Transform3> = nodes
        .iter()
        .filter_map(|node| transform_of(document, binding, *node))
        .collect();
    let Some(first) = transforms.first().copied() else {
        return Fields::nothing();
    };
    let first_angles = euler_degrees(first.rotation);
    let mut fields = Fields {
        position: lanes(first.translation),
        rotation: lanes(Vec3::from_array(first_angles)),
        scale: lanes(first.scale),
    };
    for transform in &transforms[1..] {
        let angles = euler_degrees(transform.rotation);
        let translation = transform.translation.to_array();
        let scale = transform.scale.to_array();
        for (axis, angle) in angles.into_iter().enumerate() {
            mix(&mut fields.position[axis], translation[axis]);
            mix(&mut fields.rotation[axis], angle);
            mix(&mut fields.scale[axis], scale[axis]);
        }
    }
    fields
}

/// Type a value into one field, for every selected object, as one transaction.
///
/// The text goes through [`crate::snapping::evaluate`], so `"2m + 30cm"`, `"45°"` and `"(1+2)*3"` are
/// all accepted and a suffix from the wrong quantity is refused with what to do about it.
///
/// Returns how many objects changed. Zero is not an error: setting a field to the value it already
/// holds is a legitimate thing to do and produces no history entry, because the document drops a
/// transaction whose operations changed nothing.
pub fn apply(
    document: &mut Document,
    binding: TransformBinding,
    nodes: &[NodeId],
    row: Row,
    axis: usize,
    text: &str,
    actor: Actor,
) -> Result<usize> {
    if axis > 2 {
        return Err(
            Problem::new("set a transform field", format!("there is no axis {axis}"))
                .with_remedy("the axes are 0, 1 and 2 — X, Y and Z"),
        );
    }
    let value = evaluate(text, row.quantity())?;
    apply_value(document, binding, nodes, row, axis, value, actor)
}

/// Set one field for every selected object, from a value that is already a number.
///
/// The half of [`apply`] below the parser, exposed because a caller that already holds a number
/// should not have to render it as text for [`evaluate`] to read back: the round trip is a precision
/// hazard and it makes the unit a matter of formatting rather than of type. The unit is the base one
/// — metres, radians, a factor — which is what every other manipulation in this crate speaks.
pub fn apply_value(
    document: &mut Document,
    binding: TransformBinding,
    nodes: &[NodeId],
    row: Row,
    axis: usize,
    value: f32,
    actor: Actor,
) -> Result<usize> {
    if axis > 2 {
        return Err(
            Problem::new("set a transform field", format!("there is no axis {axis}"))
                .with_remedy("the axes are 0, 1 and 2 — X, Y and Z"),
        );
    }
    if row == Row::Scale && value == 0.0 {
        return Err(Problem::new(
            "set a scale",
            "a scale of zero collapses the object and cannot be undone by scaling it back",
        )
        .with_remedy("enter a small number instead, or use the object's visibility to hide it"));
    }

    let targets: Vec<(NodeId, Transform3)> = nodes
        .iter()
        .filter_map(|node| {
            transform_of(document, binding, *node).map(|transform| (*node, transform))
        })
        .collect();
    if targets.is_empty() {
        return Err(Problem::new(
            "set a transform field",
            "nothing selected carries a transform",
        )
        .with_remedy("select an object with a transform component"));
    }

    let label = format!("Set {} {}", row.label(), ["X", "Y", "Z"][axis]);
    document.begin(label, actor);
    let mut changed = 0;
    for (node, transform) in targets {
        let updated = with_field(transform, row, axis, value);
        match write(document, node, binding, transform, updated) {
            Ok(touched) => changed += usize::from(touched),
            Err(problem) => {
                document.cancel()?;
                return Err(problem);
            }
        }
    }
    document.commit()?;
    Ok(changed)
}

/// The transform with one lane replaced.
fn with_field(transform: Transform3, row: Row, axis: usize, value: f32) -> Transform3 {
    match row {
        Row::Position => Transform3 {
            translation: replaced(transform.translation, axis, value),
            ..transform
        },
        Row::Scale => Transform3 {
            scale: replaced(transform.scale, axis, value),
            ..transform
        },
        Row::Rotation => {
            let mut angles = euler_degrees(transform.rotation);
            angles[axis] = value.to_degrees();
            Transform3 {
                rotation: from_euler_degrees(angles),
                ..transform
            }
        }
    }
}

/// Write only what moved, for the reason `crate::gizmo::write_transform` gives: a transaction whose
/// operations all address the same field is one operation after compaction.
fn write(
    document: &mut Document,
    node: NodeId,
    binding: TransformBinding,
    before: Transform3,
    after: Transform3,
) -> Result<bool> {
    let mut touched = false;
    if after.translation != before.translation {
        document.set_field(
            node,
            binding.component,
            binding.translation,
            Value::Vec3(after.translation.to_array()),
        )?;
        touched = true;
    }
    // Compared as bits, because "the rotation changed" is an exact question here: the value was
    // computed from the field the user typed and either it is the one already stored or it is not.
    if after.rotation.to_array().map(f32::to_bits) != before.rotation.to_array().map(f32::to_bits) {
        document.set_field(
            node,
            binding.component,
            binding.rotation,
            Value::Quat(after.rotation.to_array()),
        )?;
        touched = true;
    }
    if after.scale != before.scale {
        document.set_field(
            node,
            binding.component,
            binding.scale,
            Value::Vec3(after.scale.to_array()),
        )?;
        touched = true;
    }
    Ok(touched)
}

fn replaced(vector: Vec3, axis: usize, value: f32) -> Vec3 {
    let mut lanes = vector.to_array();
    lanes[axis] = value;
    Vec3::from_array(lanes)
}

fn lanes(vector: Vec3) -> [Field; 3] {
    vector.to_array().map(Field::Value)
}

/// Two objects that disagree on a lane make that field mixed, and it stays mixed.
fn mix(field: &mut Field, value: f32) {
    if let Field::Value(existing) = *field
        && existing.to_bits() != value.to_bits()
    {
        *field = Field::Mixed;
    }
}

fn transform_of(
    document: &Document,
    binding: TransformBinding,
    node: NodeId,
) -> Option<Transform3> {
    let content = document.content();
    let translation = content.field(node, binding.component, binding.translation)?;
    let rotation = content.field(node, binding.component, binding.rotation)?;
    let scale = content.field(node, binding.component, binding.scale)?;
    match (translation, rotation, scale) {
        (Value::Vec3(translation), Value::Quat(rotation), Value::Vec3(scale)) => Some(Transform3 {
            translation: Vec3::from_array(*translation),
            rotation: Quat::from_array(*rotation),
            scale: Vec3::from_array(*scale),
        }),
        _ => None,
    }
}

/// The rotation as three degrees, in X-then-Y-then-Z intrinsic order.
///
/// The pole — a Y angle of ±90°, where X and Z become the same rotation — is resolved by putting the
/// whole turn on X and none on Z, which is the convention every tool uses. Without it the angles come
/// out as a NaN and the inspector shows three empty fields on an object that is merely pointing
/// straight up.
#[must_use]
pub fn euler_degrees(rotation: Quat) -> [f32; 3] {
    let Quat { x, y, z, w } = rotation.normalized();
    let sine_pitch = 2.0 * (w * y - z * x);
    if sine_pitch.abs() >= 0.999_999 {
        let pitch = sine_pitch.signum() * std::f32::consts::FRAC_PI_2;
        let roll = 2.0 * z.atan2(w);
        return [roll.to_degrees(), pitch.to_degrees(), 0.0];
    }
    let roll = (2.0 * (w * x + y * z)).atan2(1.0 - 2.0 * (x * x + y * y));
    let pitch = sine_pitch.asin();
    let yaw = (2.0 * (w * z + x * y)).atan2(1.0 - 2.0 * (y * y + z * z));
    [roll.to_degrees(), pitch.to_degrees(), yaw.to_degrees()]
}

/// Three degrees back into a rotation, in the same order [`euler_degrees`] reads them.
#[must_use]
pub fn from_euler_degrees(degrees: [f32; 3]) -> Quat {
    let x = Quat::from_axis_angle(Vec3::X, degrees[0].to_radians());
    let y = Quat::from_axis_angle(Vec3::Y, degrees[1].to_radians());
    let z = Quat::from_axis_angle(Vec3::Z, degrees[2].to_radians());
    z.after(y).after(x).normalized()
}

#[cfg(test)]
mod tests {
    use cy_editor_core::value::ValueKind;

    use super::*;

    struct Fixture {
        document: Document,
        binding: TransformBinding,
        nodes: Vec<NodeId>,
    }

    fn fixture(count: usize) -> Fixture {
        let mut document = Document::new("worlds/city.cyworld");
        let component = document.schema_mut().declare_type("Transform", false);
        let translation = document
            .schema_mut()
            .declare_field(component, "translation", ValueKind::Vec3, "where it is")
            .expect("a fresh schema");
        let rotation = document
            .schema_mut()
            .declare_field(component, "rotation", ValueKind::Quat, "which way it faces")
            .expect("a fresh schema");
        let scale = document
            .schema_mut()
            .declare_field(component, "scale", ValueKind::Vec3, "how big it is")
            .expect("a fresh schema");
        let binding = TransformBinding {
            component,
            translation,
            rotation,
            scale,
        };
        let nodes = document
            .with_transaction("Populate", Actor::human("designer"), |document| {
                let mut nodes = Vec::new();
                for index in 0..count {
                    let node = document.create_node(None)?;
                    #[allow(clippy::cast_precision_loss, reason = "a test with three nodes")]
                    let offset = index as f32;
                    document.add_component(
                        node,
                        component,
                        vec![
                            (translation, Value::Vec3([1.0 + offset, 2.0, 3.0])),
                            (rotation, Value::Quat(Quat::IDENTITY.to_array())),
                            (scale, Value::Vec3([1.0, 1.0, 1.0])),
                        ],
                    )?;
                    nodes.push(node);
                }
                Ok(nodes)
            })
            .expect("a populated document");
        Fixture {
            document,
            binding,
            nodes,
        }
    }

    #[test]
    fn a_typed_value_is_one_transaction_and_undoes_to_the_exact_original_bits() {
        let Fixture {
            mut document,
            binding,
            nodes,
        } = fixture(1);
        let entries = document.history().entries().len();
        let before = transform_of(&document, binding, nodes[0]).expect("a transform");

        let changed = apply(
            &mut document,
            binding,
            &nodes,
            Row::Position,
            1,
            "4.25",
            Actor::human("designer"),
        )
        .expect("it applies");
        assert_eq!(changed, 1);
        assert_eq!(document.history().entries().len(), entries + 1);
        assert_eq!(
            transform_of(&document, binding, nodes[0])
                .expect("a transform")
                .translation
                .y
                .to_bits(),
            4.25_f32.to_bits()
        );

        document.undo().expect("undo").expect("an entry");
        assert_eq!(
            transform_of(&document, binding, nodes[0])
                .expect("a transform")
                .translation
                .to_array()
                .map(f32::to_bits),
            before.translation.to_array().map(f32::to_bits)
        );
    }

    #[test]
    fn a_field_accepts_expressions_and_units_and_refuses_the_wrong_suffix_with_a_remedy() {
        let Fixture {
            mut document,
            binding,
            nodes,
        } = fixture(1);
        apply(
            &mut document,
            binding,
            &nodes,
            Row::Position,
            0,
            "2m + 30cm",
            Actor::human("designer"),
        )
        .expect("an expression with units");
        let position = transform_of(&document, binding, nodes[0])
            .expect("a transform")
            .translation;
        assert!((position.x - 2.3).abs() < 1e-5, "{position:?}");

        let problem = apply(
            &mut document,
            binding,
            &nodes,
            Row::Rotation,
            0,
            "3m",
            Actor::human("designer"),
        )
        .expect_err("metres are not an angle");
        assert!(problem.remedy.is_some(), "{problem}");
        assert!(
            !document.is_transaction_open(),
            "a refused value left a transaction open"
        );
    }

    #[test]
    fn several_objects_that_disagree_show_a_mixed_field_and_typing_sets_them_all() {
        let Fixture {
            mut document,
            binding,
            nodes,
        } = fixture(3);
        let fields = read(&document, binding, &nodes);
        assert_eq!(fields.position[0], Field::Mixed, "they differ in X");
        assert_eq!(fields.position[1], Field::Value(2.0), "they agree in Y");
        assert_eq!(fields.position[0].text(Row::Position), "—");

        let changed = apply(
            &mut document,
            binding,
            &nodes,
            Row::Position,
            0,
            "7",
            Actor::human("designer"),
        )
        .expect("it applies to all three");
        assert_eq!(changed, 3);
        assert_eq!(
            read(&document, binding, &nodes).position[0],
            Field::Value(7.0)
        );
        assert_eq!(
            document
                .history()
                .entries()
                .last()
                .expect("an entry")
                .operations
                .len(),
            3,
            "one transaction, three objects"
        );
    }

    #[test]
    fn an_empty_selection_shows_nothing_rather_than_zeros() {
        let Fixture {
            document, binding, ..
        } = fixture(1);
        let fields = read(&document, binding, &[]);
        assert_eq!(fields.position[0], Field::Nothing);
        assert_eq!(fields.position[0].text(Row::Position), "");
        assert_eq!(fields.rotation[2].number(), None);
    }

    #[test]
    fn a_rotation_survives_the_trip_through_three_degree_fields() {
        for angles in [
            [0.0, 0.0, 0.0],
            [30.0, 0.0, 0.0],
            [0.0, 45.0, 0.0],
            [0.0, 0.0, -90.0],
            [15.0, 25.0, 35.0],
            [-120.0, 40.0, 170.0],
        ] {
            let rotation = from_euler_degrees(angles);
            let read_back = euler_degrees(rotation);
            let again = from_euler_degrees(read_back);
            // The angles are not required to be the same triple — a rotation has several — but the
            // rotation they produce is.
            let difference = (0..4)
                .map(|lane| (rotation.to_array()[lane] - again.to_array()[lane]).abs())
                .fold(0.0_f32, f32::max);
            assert!(
                difference < 1e-4,
                "{angles:?} read back as {read_back:?}, a different rotation"
            );
        }
    }

    #[test]
    fn straight_up_is_a_number_rather_than_a_nan() {
        // The pole. Without the special case the inspector shows three empty fields on an object
        // that is merely pointing at the sky.
        let up = from_euler_degrees([0.0, 90.0, 0.0]);
        let angles = euler_degrees(up);
        assert!(
            angles.iter().all(|angle| angle.is_finite()),
            "{angles:?} is not a set of angles"
        );
        assert!((angles[1].abs() - 90.0).abs() < 1e-2, "{angles:?}");
    }

    #[test]
    fn a_scale_of_zero_is_refused_because_it_cannot_be_scaled_back() {
        let Fixture {
            mut document,
            binding,
            nodes,
        } = fixture(1);
        let problem = apply(
            &mut document,
            binding,
            &nodes,
            Row::Scale,
            2,
            "0",
            Actor::human("designer"),
        )
        .expect_err("zero collapses the object");
        assert!(problem.remedy.is_some(), "{problem}");
    }

    #[test]
    fn typing_the_value_a_field_already_holds_records_no_history_entry() {
        let Fixture {
            mut document,
            binding,
            nodes,
        } = fixture(1);
        let entries = document.history().entries().len();
        let changed = apply(
            &mut document,
            binding,
            &nodes,
            Row::Position,
            1,
            "2",
            Actor::human("designer"),
        )
        .expect("it applies");
        assert_eq!(changed, 0, "nothing moved");
        assert_eq!(
            document.history().entries().len(),
            entries,
            "a move that moved nothing entered the history"
        );
    }
}
