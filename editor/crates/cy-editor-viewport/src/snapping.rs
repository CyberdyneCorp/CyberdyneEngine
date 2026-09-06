//! Snapping and precision: increments, units, and the numeric entry that accepts an expression.
//!
//! `editor-viewport-and-gizmos` — "Snapping and precision":
//!
//! > The editor SHALL support grid snapping, angle snapping, scale snapping, vertex and surface
//! > snapping, pivot snapping, and alignment to another object's transform. Snapping increments SHALL
//! > be **configurable, unit-aware, and toggleable transiently with a modifier**. Numeric entry SHALL
//! > be available for every manipulation, and SHALL **accept expressions and units**. Precision SHALL
//! > be maintained at large world coordinates.
//!
//! --- SNAPPING ROUNDS THE RESULT, NOT THE MOVEMENT -------------------------------------------------
//!
//! Every function here takes an absolute value and returns an absolute value on the grid. None of
//! them takes a delta. That is the difference between an object that lands on the grid and one that
//! lands a constant offset from it — the second is what you get by snapping the movement, and it is
//! invisible until somebody notices two objects snapped to the "same" grid do not line up.
//!
//! It also gives idempotence for free, which is the property the requirement's scenario is really
//! about: `snap(snap(x)) == snap(x)`, exactly, so a drag that jitters by a hundredth of a millimetre
//! produces the same snapped value every frame and the object does not shimmer between two positions
//! at the far edge of the world. `tests::snapping_is_exact_and_idempotent_far_from_the_origin` pins
//! that at a million metres out, where a single-precision float's spacing is about 0.0625.
//!
//! --- VERTEX AND SURFACE SNAPPING NEED THE RUNTIME, AND SAY SO --------------------------------------
//!
//! Grid, angle and scale snapping are arithmetic and are done here. Vertex and surface snapping need
//! to know where the geometry is, and the editor does not — that is the same question picking asks,
//! and it is answered the same way: the runtime resolves it against what it drew. [`SnapTarget`] is
//! what comes back, and [`SnapSettings::modes`] says which of the two are on so a viewport knows
//! whether to ask at all. An editor that kept its own copy of the geometry to snap against would be
//! a second renderer with extra steps.

use std::f32::consts::PI;

use cy_editor_core::problem::{Problem, Result};

use crate::math::{Quat, Vec3};

/// The units a value can be entered in.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Unit {
    /// The engine's base length unit.
    Metres,
    /// A hundredth of a metre.
    Centimetres,
    /// A thousandth of a metre.
    Millimetres,
    /// A thousand metres.
    Kilometres,
    /// The unit a person types an angle in.
    Degrees,
    /// The unit the engine holds an angle in.
    Radians,
    /// A hundredth, for a scale factor.
    Percent,
}

impl Unit {
    /// How many base units one of these is. Metres for a length, radians for an angle, a plain
    /// factor for a scale.
    ///
    /// Metres and radians both answer one, and the arms are kept separate rather than merged
    /// because they are the base units of two different quantities. Merging them would read as a
    /// coincidence being exploited, and the day somebody changes the angle base unit they would
    /// change the length one with it.
    #[allow(
        clippy::match_same_arms,
        reason = "see the note above: two quantities, one number"
    )]
    #[must_use]
    pub fn in_base_units(self) -> f32 {
        match self {
            Unit::Metres => 1.0,
            Unit::Centimetres => 0.01,
            Unit::Millimetres => 0.001,
            Unit::Kilometres => 1000.0,
            Unit::Degrees => PI / 180.0,
            Unit::Radians => 1.0,
            Unit::Percent => 0.01,
        }
    }

    /// The suffix a user types.
    #[must_use]
    pub const fn suffix(self) -> &'static str {
        match self {
            Unit::Metres => "m",
            Unit::Centimetres => "cm",
            Unit::Millimetres => "mm",
            Unit::Kilometres => "km",
            Unit::Degrees => "deg",
            Unit::Radians => "rad",
            Unit::Percent => "%",
        }
    }

    /// What quantity this unit measures. A length suffix on an angle field is a mistake worth
    /// reporting rather than silently converting.
    #[must_use]
    pub const fn quantity(self) -> Quantity {
        match self {
            Unit::Metres | Unit::Centimetres | Unit::Millimetres | Unit::Kilometres => {
                Quantity::Length
            }
            Unit::Degrees | Unit::Radians => Quantity::Angle,
            Unit::Percent => Quantity::Factor,
        }
    }
}

/// What is being measured, which decides both the default unit and which suffixes are legal.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Quantity {
    /// A distance. Entered in metres unless a suffix says otherwise.
    Length,
    /// A rotation. Entered in **degrees** unless a suffix says otherwise, because that is what a
    /// person means by "45" and the engine's radians are an implementation detail of the engine.
    Angle,
    /// A scale factor. Entered as a plain number; `%` divides by a hundred.
    Factor,
}

impl Quantity {
    /// The unit a bare number is in.
    #[must_use]
    pub const fn default_unit(self) -> Unit {
        match self {
            Quantity::Length => Unit::Metres,
            Quantity::Angle => Unit::Degrees,
            Quantity::Factor => Unit::Percent,
        }
    }

    /// Every suffix this quantity accepts, longest first so that `cm` is matched before `m`.
    #[must_use]
    pub fn units(self) -> &'static [Unit] {
        match self {
            Quantity::Length => &[
                Unit::Millimetres,
                Unit::Centimetres,
                Unit::Kilometres,
                Unit::Metres,
            ],
            Quantity::Angle => &[Unit::Degrees, Unit::Radians],
            Quantity::Factor => &[Unit::Percent],
        }
    }
}

/// Which kinds of snapping are on.
///
/// Five independent toggles, which is what the requirement lists and what a settings panel shows.
/// A bitfield would be shorter and would make `modes.vertex` into `modes.contains(Snap::VERTEX)`,
/// which is worse at every call site for no benefit at this size.
#[allow(
    clippy::struct_excessive_bools,
    reason = "five independent user settings, not a state machine"
)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct SnapModes {
    /// Positions land on the grid.
    pub grid: bool,
    /// Rotations land on the angle increment.
    pub angle: bool,
    /// Scales land on the scale increment.
    pub scale: bool,
    /// Positions land on another object's vertex. Needs the runtime.
    pub vertex: bool,
    /// Positions land on another object's surface. Needs the runtime.
    pub surface: bool,
}

/// Increments and what they apply to.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct SnapSettings {
    /// The grid increment, in metres.
    pub grid: f32,
    /// The angle increment, in radians.
    pub angle: f32,
    /// The scale increment, as a factor.
    pub scale: f32,
    /// Which kinds are on.
    pub modes: SnapModes,
    /// The unit the grid increment is shown and entered in. Snapping is done in metres whatever
    /// this says; it exists so a field that reads "10" means ten centimetres to a user who works in
    /// centimetres and ten metres to one who does not.
    pub grid_display_unit: Unit,
}

impl Default for SnapSettings {
    fn default() -> Self {
        Self {
            grid: 0.25,
            angle: PI / 12.0,
            scale: 0.1,
            modes: SnapModes {
                grid: true,
                angle: true,
                scale: true,
                vertex: false,
                surface: false,
            },
            grid_display_unit: Unit::Metres,
        }
    }
}

impl SnapSettings {
    /// Whether snapping applies, given whether the transient modifier is held.
    ///
    /// The modifier **inverts** rather than enables. That is what every editor does and it is worth
    /// stating why: a user who works with snapping on needs a way to place something off the grid,
    /// and a user who works with it off needs a way to place something on it. One key, both needs,
    /// and no mode to remember.
    #[must_use]
    pub const fn active(enabled: bool, modifier_held: bool) -> bool {
        enabled != modifier_held
    }

    /// A position on the grid, or the position unchanged when grid snapping is not in force.
    #[must_use]
    pub fn position(&self, position: Vec3, modifier_held: bool) -> Vec3 {
        if !Self::active(self.modes.grid, modifier_held) {
            return position;
        }
        Vec3::new(
            snap_to(position.x, self.grid),
            snap_to(position.y, self.grid),
            snap_to(position.z, self.grid),
        )
    }

    /// An angle on the increment, in radians.
    #[must_use]
    pub fn radians(&self, radians: f32, modifier_held: bool) -> f32 {
        if !Self::active(self.modes.angle, modifier_held) {
            return radians;
        }
        snap_to(radians, self.angle)
    }

    /// A scale on the increment.
    ///
    /// Clamped away from zero: a snapped scale of exactly nothing collapses the object to a point
    /// it cannot be dragged back out of, because every subsequent multiplication of zero is zero.
    #[must_use]
    pub fn scale_factor(&self, scale: Vec3, modifier_held: bool) -> Vec3 {
        if !Self::active(self.modes.scale, modifier_held) {
            return scale;
        }
        Vec3::new(
            snap_scale_lane(scale.x, self.scale),
            snap_scale_lane(scale.y, self.scale),
            snap_scale_lane(scale.z, self.scale),
        )
    }

    /// Whether the viewport should ask the runtime for something to snap to.
    #[must_use]
    pub const fn needs_runtime_targets(&self) -> bool {
        self.modes.vertex || self.modes.surface
    }
}

/// Round to the nearest multiple of `increment`.
///
/// `f32::round` ties away from zero, which makes the function symmetric about the origin — the
/// alternative, rounding half to even, would put a value exactly between two grid lines on a
/// different line depending on which line it was, and a user placing objects on a wall would find
/// every other one offset.
fn snap_to(value: f32, increment: f32) -> f32 {
    // `is_finite` first, so a NaN increment takes this branch rather than falling through the
    // comparison below — every comparison against a NaN is false, including `<= 0.0`.
    if !increment.is_finite() || increment <= 0.0 || !value.is_finite() {
        return value;
    }
    (value / increment).round() * increment
}

fn snap_scale_lane(value: f32, increment: f32) -> f32 {
    let snapped = snap_to(value, increment);
    if snapped.abs() < increment * 0.5 {
        return increment.copysign(if value < 0.0 { -1.0 } else { 1.0 });
    }
    snapped
}

/// Something in the world the runtime found to snap to.
///
/// Returned by the runtime, because only the runtime knows where the geometry is — the same argument
/// picking makes. The editor decides whether to ask and what to do with the answer.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct SnapTarget {
    /// Where to put the dragged object's pivot.
    pub position: Vec3,
    /// The surface normal there, for a surface snap that also orients. Zero when the target is a
    /// vertex, which has no normal of its own.
    pub normal: Vec3,
    /// What was snapped to, so the editor can refuse to snap an object to itself.
    pub identity: u64,
}

/// Where a transform's snapping and alignment operate from.
///
/// "pivot snapping, and alignment to another object's transform" — both are answered by replacing
/// part of a transform with part of another one, so they are one function with a mask.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Alignment {
    /// Take the other object's position.
    pub position: bool,
    /// Take its rotation.
    pub rotation: bool,
    /// Take its scale.
    pub scale: bool,
}

impl Alignment {
    /// Take everything: "align to that object".
    pub const ALL: Self = Self {
        position: true,
        rotation: true,
        scale: true,
    };

    /// Take only the orientation, which is what "match rotation" means and is the common case.
    pub const ROTATION_ONLY: Self = Self {
        position: false,
        rotation: true,
        scale: false,
    };

    /// Apply the alignment: `subject` with `target`'s selected parts.
    #[must_use]
    pub fn apply(
        self,
        subject: (Vec3, Quat, Vec3),
        target: (Vec3, Quat, Vec3),
    ) -> (Vec3, Quat, Vec3) {
        (
            if self.position { target.0 } else { subject.0 },
            if self.rotation { target.1 } else { subject.1 },
            if self.scale { target.2 } else { subject.2 },
        )
    }
}

/// Evaluate what a user typed into a numeric field.
///
/// Accepts `+ - * /`, parentheses, decimal numbers, and a unit suffix on any number. The answer is
/// always in base units — metres, radians, or a plain factor — because that is what the rest of the
/// editor holds.
///
/// ```text
///   "2m + 30cm"   -> 2.3         (length)
///   "45"          -> 0.7853982   (angle: a bare number is degrees)
///   "90deg / 2"   -> 0.7853982   (angle: the 2 is a plain number, not two degrees)
///   "150%"        -> 1.5         (factor)
///   "(1 + 2) * 3" -> 9.0         (length, in metres)
/// ```
///
/// --- WHERE THE DEFAULT UNIT IS APPLIED, WHICH IS THE ONLY SUBTLE PART ---------------------------
///
/// **Once per product, and only when nothing in that product carried a unit already.** "90deg / 2"
/// divides by the number two rather than by two degrees, and "45" is forty-five degrees. The naive
/// rule — every bare number takes the default unit — gets the first of those wrong by a factor of
/// fifty-seven, and it gets it wrong silently: `90deg / 2` evaluates to forty-five *radians* and
/// lands in a transaction looking like a plausible number.
///
/// A parenthesised group counts as already carrying a unit, because its contents applied the rule
/// themselves. Without that, "(45) * 2" in an angle field would take the degree factor twice.
///
/// A suffix from the wrong quantity is refused with what to do about it, rather than converted —
/// "3m" in a rotation field is a mistake, and a value silently interpreted as three radians is a
/// mistake that lands in a transaction.
pub fn evaluate(input: &str, quantity: Quantity) -> Result<f32> {
    let mut parser = Parser {
        rest: input.trim(),
        quantity,
    };
    let value = parser.expression()?;
    parser.skip_spaces();
    if !parser.rest.is_empty() {
        return Err(Problem::new(
            format!("read \"{input}\""),
            format!(
                "\"{}\" is not something this field understands",
                parser.rest
            ),
        )
        .with_remedy("enter a number, or an expression such as \"2m + 30cm\""));
    }
    if !value.is_finite() {
        return Err(Problem::new(
            format!("read \"{input}\""),
            "the expression does not evaluate to a number",
        )
        .with_remedy("check for a division by zero"));
    }
    Ok(value)
}

/// A recursive-descent parser over the tiny grammar above.
///
/// It is a parser rather than a `str::parse` because the requirement says "expressions and units",
/// and because the alternative — accepting a bare number and telling the user to do the arithmetic —
/// is what makes people place objects with a calculator open beside the editor.
/// A value and whether anything in it carried an explicit unit.
#[derive(Clone, Copy, Debug)]
struct Dimensioned {
    value: f32,
    dimensioned: bool,
}

struct Parser<'a> {
    rest: &'a str,
    quantity: Quantity,
}

impl Parser<'_> {
    fn skip_spaces(&mut self) {
        self.rest = self.rest.trim_start();
    }

    /// `expression := term (('+' | '-') term)*`
    ///
    /// Every term is in base units by the time it gets here, so the additions are additions.
    fn expression(&mut self) -> Result<f32> {
        let mut value = self.term()?.value;
        loop {
            self.skip_spaces();
            let Some(operator) = self.take_one_of(&['+', '-']) else {
                return Ok(value);
            };
            let right = self.term()?.value;
            value = if operator == '+' {
                value + right
            } else {
                value - right
            };
        }
    }

    /// `term := factor (('*' | '/') factor)*`
    ///
    /// The default unit is applied HERE, once, and only when no factor of the product carried one.
    /// See `evaluate`'s note for the "90deg / 2" case that makes this the only workable place.
    fn term(&mut self) -> Result<Dimensioned> {
        let mut term = self.factor()?;
        loop {
            self.skip_spaces();
            let Some(operator) = self.take_one_of(&['*', '/']) else {
                break;
            };
            let right = self.factor()?;
            term.value = if operator == '*' {
                term.value * right.value
            } else {
                term.value / right.value
            };
            term.dimensioned |= right.dimensioned;
        }
        if !term.dimensioned {
            term.value *= self.quantity.default_unit().in_base_units();
            term.dimensioned = true;
        }
        Ok(term)
    }

    /// `factor := '-'? ( '(' expression ')' | number unit? )`
    fn factor(&mut self) -> Result<Dimensioned> {
        self.skip_spaces();
        if self.take_one_of(&['-']).is_some() {
            let inner = self.factor()?;
            return Ok(Dimensioned {
                value: -inner.value,
                dimensioned: inner.dimensioned,
            });
        }
        if self.take_one_of(&['(']).is_some() {
            let value = self.expression()?;
            self.skip_spaces();
            if self.take_one_of(&[')']).is_none() {
                return Err(
                    Problem::new("read an expression", "a bracket was not closed")
                        .with_remedy("add the missing \")\""),
                );
            }
            // A group has already applied the rule to its own terms, so it counts as carrying a
            // unit. Otherwise "(45) * 2" in an angle field would take the degree factor twice.
            return Ok(Dimensioned {
                value,
                dimensioned: true,
            });
        }
        self.number()
    }

    fn number(&mut self) -> Result<Dimensioned> {
        let digits = self
            .rest
            .find(|character: char| !character.is_ascii_digit() && character != '.')
            .unwrap_or(self.rest.len());
        let (text, rest) = self.rest.split_at(digits);
        let value: f32 = text.parse().map_err(|_| {
            Problem::new(
                "read a number",
                format!("\"{}\" does not start with one", self.rest),
            )
            .with_remedy("enter a number, or an expression such as \"2m + 30cm\"")
        })?;
        self.rest = rest;
        match self.unit()? {
            Some(unit) => Ok(Dimensioned {
                value: value * unit.in_base_units(),
                dimensioned: true,
            }),
            None => Ok(Dimensioned {
                value,
                dimensioned: false,
            }),
        }
    }

    /// The suffix on the number just read, or `None` when it carried one.
    fn unit(&mut self) -> Result<Option<Unit>> {
        for unit in self.quantity.units() {
            if let Some(rest) = self.rest.strip_prefix(unit.suffix()) {
                self.rest = rest;
                return Ok(Some(*unit));
            }
        }
        self.reject_foreign_suffix()?;
        Ok(None)
    }

    /// Refuse a suffix that belongs to a different quantity, rather than ignoring it.
    ///
    /// Ignoring it is what a `parse` would do, and the result is "3m" typed into a rotation field
    /// becoming three radians — a hundred and seventy-two degrees, in a transaction, silently.
    fn reject_foreign_suffix(&self) -> Result<()> {
        for quantity in [Quantity::Length, Quantity::Angle, Quantity::Factor] {
            if quantity == self.quantity {
                continue;
            }
            for unit in quantity.units() {
                if self.rest.starts_with(unit.suffix()) {
                    return Err(Problem::new(
                        format!("read \"{}\"", unit.suffix()),
                        format!(
                            "this field is a {:?} and that is not a {:?} unit",
                            self.quantity, self.quantity
                        ),
                    )
                    .with_remedy(format!(
                        "use one of: {}",
                        self.quantity
                            .units()
                            .iter()
                            .map(|unit| unit.suffix())
                            .collect::<Vec<_>>()
                            .join(", ")
                    )));
                }
            }
        }
        Ok(())
    }

    fn take_one_of(&mut self, candidates: &[char]) -> Option<char> {
        let first = self.rest.chars().next()?;
        if !candidates.contains(&first) {
            return None;
        }
        self.rest = &self.rest[first.len_utf8()..];
        Some(first)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_modifier_inverts_snapping_rather_than_enabling_it() {
        assert!(SnapSettings::active(true, false), "on, no modifier");
        assert!(!SnapSettings::active(true, true), "on, modifier held: off");
        assert!(SnapSettings::active(false, true), "off, modifier held: on");
        assert!(!SnapSettings::active(false, false));
    }

    #[test]
    fn snapping_is_exact_and_idempotent_far_from_the_origin() {
        // "WHEN an object is manipulated far from the world origin THEN manipulation SHALL remain
        // precise and free of visible jitter."
        //
        // A million metres out, a single-precision float's spacing is about 0.0625, so a 0.25 grid
        // is representable and the snapped value must be EXACT. Idempotence is what "free of jitter"
        // means in practice: a drag that wobbles by a thousandth of a millimetre must produce the
        // same snapped position every frame rather than flicking between two.
        let settings = SnapSettings::default();
        let far = Vec3::new(1_000_000.3, 12.4, -999_999.6);
        let once = settings.position(far, false);
        let twice = settings.position(once, false);
        assert_eq!(
            once.to_array().map(f32::to_bits),
            twice.to_array().map(f32::to_bits),
            "snapping is not idempotent: {once:?} then {twice:?}"
        );
        for lane in once.to_array() {
            let multiples = lane / settings.grid;
            assert!(
                (multiples - multiples.round()).abs() < 1e-3,
                "{lane} is not a multiple of the grid"
            );
        }
    }

    #[test]
    fn snapping_rounds_the_result_rather_than_the_movement() {
        let settings = SnapSettings::default();
        // Two objects at different offsets snap to the SAME grid line, which is only true because
        // the absolute position is what is rounded.
        let first = settings.position(Vec3::new(4.13, 0.0, 0.0), false);
        let second = settings.position(Vec3::new(4.16, 0.0, 0.0), false);
        assert_eq!(first.x.to_bits(), second.x.to_bits());
        assert!((first.x - 4.25).abs() < 1e-5, "{first:?}");
    }

    #[test]
    fn an_angle_snaps_to_the_increment_and_a_scale_never_snaps_to_nothing() {
        let settings = SnapSettings::default();
        let snapped = settings.radians(0.30, false);
        assert!((snapped - PI / 12.0).abs() < 1e-5, "{snapped}");

        // A scale rounded to zero is an object that cannot be dragged back, because every later
        // multiplication of zero is zero.
        let tiny = settings.scale_factor(Vec3::new(0.01, 0.01, 0.01), false);
        for lane in tiny.to_array() {
            assert!(lane.abs() >= settings.scale * 0.99, "{tiny:?}");
        }
    }

    #[test]
    fn a_zero_increment_is_a_no_op_rather_than_a_division_by_zero() {
        let settings = SnapSettings {
            grid: 0.0,
            ..SnapSettings::default()
        };
        let position = Vec3::new(1.234, 5.678, -9.0);
        assert_eq!(settings.position(position, false), position);
    }

    #[test]
    fn alignment_takes_the_parts_it_is_asked_for() {
        let subject = (
            Vec3::new(1.0, 2.0, 3.0),
            Quat::IDENTITY,
            Vec3::new(1.0, 1.0, 1.0),
        );
        let target = (
            Vec3::new(9.0, 9.0, 9.0),
            Quat::from_axis_angle(Vec3::Y, 1.0),
            Vec3::new(2.0, 2.0, 2.0),
        );
        let all = Alignment::ALL.apply(subject, target);
        assert_eq!(all.0, target.0);
        assert_eq!(all.2, target.2);

        let rotation_only = Alignment::ROTATION_ONLY.apply(subject, target);
        assert_eq!(rotation_only.0, subject.0, "the position is left alone");
        assert_eq!(rotation_only.1, target.1);
        assert_eq!(rotation_only.2, subject.2);
    }

    #[test]
    fn numeric_entry_accepts_expressions_and_units() {
        assert!((evaluate("2m + 30cm", Quantity::Length).unwrap() - 2.3).abs() < 1e-5);
        assert!((evaluate("  1.5 ", Quantity::Length).unwrap() - 1.5).abs() < 1e-6);
        assert!((evaluate("(1 + 2) * 3", Quantity::Length).unwrap() - 9.0).abs() < 1e-5);
        assert!((evaluate("-2 * 3", Quantity::Length).unwrap() + 6.0).abs() < 1e-5);
        assert!((evaluate("1000mm", Quantity::Length).unwrap() - 1.0).abs() < 1e-6);
        assert!((evaluate("2km / 4", Quantity::Length).unwrap() - 500.0).abs() < 1e-3);
    }

    #[test]
    fn a_bare_angle_is_degrees_because_that_is_what_a_person_means_by_forty_five() {
        let forty_five = evaluate("45", Quantity::Angle).unwrap();
        assert!((forty_five - PI / 4.0).abs() < 1e-5, "{forty_five}");
        let same = evaluate("90deg / 2", Quantity::Angle).unwrap();
        assert!((same - PI / 4.0).abs() < 1e-5, "{same}");
        let radians = evaluate("1rad", Quantity::Angle).unwrap();
        assert!((radians - 1.0).abs() < 1e-6);
        // A group has applied the rule to its own terms, so it does not take the factor twice.
        let grouped = evaluate("(45) * 2", Quantity::Angle).unwrap();
        assert!((grouped - PI / 2.0).abs() < 1e-5, "{grouped}");
    }

    #[test]
    fn a_percentage_is_a_factor_and_a_bare_number_in_that_field_is_too() {
        assert!((evaluate("150%", Quantity::Factor).unwrap() - 1.5).abs() < 1e-5);
        // A scale field's bare number is a percentage, which is what "200" means beside a "%".
        assert!((evaluate("200", Quantity::Factor).unwrap() - 2.0).abs() < 1e-5);
    }

    #[test]
    fn a_unit_from_the_wrong_quantity_is_refused_rather_than_converted() {
        // "3m" in a rotation field silently becoming three radians is a mistake that reaches a
        // transaction. It is refused, with the units that would work.
        let problem = evaluate("3m", Quantity::Angle).expect_err("a length in an angle field");
        assert!(
            problem.remedy.as_deref().unwrap().contains("deg"),
            "{problem}"
        );
    }

    #[test]
    fn nonsense_is_refused_with_something_to_do_about_it() {
        for input in ["", "abc", "2 +", "(1 + 2", "1 2"] {
            let problem = evaluate(input, Quantity::Length)
                .expect_err(&format!("\"{input}\" should not evaluate"));
            assert!(problem.remedy.is_some(), "{input}: {problem}");
        }
        let divided = evaluate("1 / 0", Quantity::Length).expect_err("a division by zero");
        assert!(divided.because.contains("does not evaluate"), "{divided}");
    }

    #[test]
    fn vertex_and_surface_snapping_are_declared_as_needing_the_runtime() {
        let mut settings = SnapSettings::default();
        assert!(!settings.needs_runtime_targets());
        settings.modes.vertex = true;
        assert!(settings.needs_runtime_targets());
    }
}
