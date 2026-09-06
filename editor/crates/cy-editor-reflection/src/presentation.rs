//! What reflection says about *presenting* a field: units, ranges, ordering, disclosure, validation.
//!
//! `editor-ui-ux`: "Reflection metadata SHALL drive presentation: ranges, units, tooltips,
//! categories, ordering, conditional visibility, read-only state, and validation."
//!
//! --- WHAT THE ABI CAN AND CANNOT TELL US TODAY, STATED PLAINLY --------------------------------------
//!
//! At ABI 1.1 a `CyFieldDesc` carries four things: the field's `CyVarType`, its byte offset, its
//! byte size, and its name. That is enough to *edit* a reflected engine type — which is the M5
//! requirement — and it is not enough to present one well: the engine's own `reflect::TypeInfo`
//! carries attributes such as `Unit(Metres)` and a range, and none of them cross the boundary. So a
//! runtime type arrives here with [`Presentation::default`] on every field, and the metadata that
//! would refine it comes from one of two places:
//!
//!   * a **document's** schema, which is the editor's own and can carry whatever it likes; or
//!   * a registered [`crate::overrides::Overrides`] entry, which is the same mechanism
//!     `editor-ui-ux` requires for custom property editors — "Custom property editors and custom
//!     whole-type editors SHALL be registrable to override the generated presentation, and SHALL be
//!     usable by plugins."
//!
//! That is a real limit and it is written here rather than worked around, because the shape of the
//! workaround — deriving a unit from a field's *name* — is how an editor comes to believe that a
//! field called `angle` is in degrees when the engine stores radians.

use cy_editor_core::ids::FieldId;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::Value;

/// The unit a numeric field is in, when something said.
///
/// Shown beside the value rather than folded into it: "Units shown on numeric fields" is a
/// discoverability requirement, and a field that silently converted would make the number in the
/// inspector disagree with the number in the engine.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub enum Unit {
    /// Metres.
    Metres,
    /// Degrees.
    Degrees,
    /// Radians.
    Radians,
    /// Seconds.
    Seconds,
    /// Milliseconds.
    Milliseconds,
    /// Kilograms.
    Kilograms,
    /// A fraction from zero to one.
    Normalised,
    /// A percentage.
    Percent,
    /// Logical pixels.
    Pixels,
}

impl Unit {
    /// The suffix drawn after the number.
    #[must_use]
    pub const fn suffix(self) -> &'static str {
        match self {
            Unit::Metres => "m",
            Unit::Degrees => "°",
            Unit::Radians => "rad",
            Unit::Seconds => "s",
            Unit::Milliseconds => "ms",
            Unit::Kilograms => "kg",
            Unit::Normalised => "",
            Unit::Percent => "%",
            Unit::Pixels => "px",
        }
    }
}

/// The values a numeric field accepts.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Range {
    /// The smallest accepted value.
    pub minimum: f64,
    /// The largest.
    pub maximum: f64,
}

impl Range {
    /// A range.
    #[must_use]
    pub const fn new(minimum: f64, maximum: f64) -> Self {
        Self { minimum, maximum }
    }

    /// Whether a value is inside it. Non-numeric values are outside no range.
    #[must_use]
    pub fn admits(self, value: &Value) -> bool {
        match numeric(value) {
            Some(number) => number >= self.minimum && number <= self.maximum,
            None => true,
        }
    }
}

/// The numeric content of a value, for range checking.
#[expect(
    clippy::cast_precision_loss,
    reason = "a range's bounds are doubles, so an integer beyond 2^53 is compared at the nearest \
              representable one; the alternative is refusing to range-check large integers at all"
)]
fn numeric(value: &Value) -> Option<f64> {
    match value {
        Value::Int(number) => Some(*number as f64),
        Value::Float(number) => Some(f64::from(*number)),
        Value::Double(number) => Some(*number),
        _ => None,
    }
}

/// Whether a field is shown by default or is advanced detail.
///
/// `editor-visual-language`: "An object's inspector SHALL open showing the properties that are
/// ordinarily edited, with advanced detail **collapsed but present**." Two states rather than
/// visible/hidden, because hidden detail is undiscoverable and the requirement is explicit that
/// advanced sections are "visibly collapsed rather than hidden".
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug, Default)]
pub enum Disclosure {
    /// Shown when the inspector opens.
    #[default]
    Default,
    /// Present, collapsed, and discoverable.
    Advanced,
}

/// A condition under which a field is shown at all.
///
/// "conditional visibility" — a field that only makes sense when another is set a certain way. The
/// condition addresses the other field by **identity** rather than by name, for the reason the
/// document schema gives: a rename must not silently disconnect it.
#[derive(Clone, PartialEq, Debug)]
pub struct VisibleWhen {
    /// The field the condition reads.
    pub field: FieldId,
    /// The value it must hold.
    pub equals: Value,
}

/// Everything reflection says about how a field is presented.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct Presentation {
    /// The unit, when one is known.
    pub unit: Option<Unit>,
    /// The accepted range, when one is declared.
    pub range: Option<Range>,
    /// A sentence explaining what the field affects, for a tooltip and for an agent.
    pub tooltip: String,
    /// The section it is grouped under. Empty means the type's own section.
    pub category: String,
    /// Where it sits within its section. Declaration order by default.
    pub order: u32,
    /// Whether the editor may write it.
    pub writable: bool,
    /// Whether it opens visible or collapsed.
    pub disclosure: Disclosure,
    /// The condition under which it is shown at all.
    pub visible_when: Option<VisibleWhen>,
    /// A registered control that replaces the one the field's kind implies.
    ///
    /// A *name*, resolved by whatever draws — the toolkit is an implementation detail, so a control
    /// cannot be a value here. See [`crate::overrides::PropertyOverride::with_editor`].
    pub editor: Option<String>,
    /// The value a "reset to default" restores, when the type declares one.
    ///
    /// "Every setting SHALL be able to explain what it affects and **what its default is**, and
    /// SHALL show whether it differs from the default."
    pub default: Option<Value>,
}

impl Presentation {
    /// The presentation of a writable field at a given position, with nothing else known.
    ///
    /// What a runtime type gets today; see the module note.
    #[must_use]
    pub fn at(order: u32) -> Self {
        Self {
            order,
            writable: true,
            ..Self::default()
        }
    }

    /// Refuse a value this field cannot hold, saying what would be accepted.
    ///
    /// This is the "validation" half of the requirement, and it runs *before* a transaction is
    /// opened: an inspector that recorded an out-of-range write and then reported it would have put
    /// an invalid value in the undo history.
    pub fn validate(&self, name: &str, value: &Value) -> Result<()> {
        if !self.writable {
            return Err(Problem::new(
                format!("set {name}"),
                "the field is read-only in this build",
            )
            .with_remedy(
                "read-only fields are derived or engine-owned; change what produces the value \
                 rather than the value",
            ));
        }
        if let Some(range) = self.range
            && !range.admits(value)
        {
            return Err(Problem::new(
                format!("set {name} to {value}"),
                format!("the field accepts {} to {}", range.minimum, range.maximum),
            )
            .with_remedy(format!(
                "supply a value between {} and {}{}",
                range.minimum,
                range.maximum,
                self.unit
                    .map_or(String::new(), |unit| format!(" {}", unit.suffix()))
            )));
        }
        Ok(())
    }

    /// Whether a value differs from the declared default, so the interface can mark it modified.
    #[must_use]
    pub fn is_modified(&self, value: &Value) -> bool {
        self.default
            .as_ref()
            .is_some_and(|default| default != value)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_out_of_range_value_is_refused_before_a_transaction_opens() {
        let presentation = Presentation {
            range: Some(Range::new(0.0, 1.0)),
            unit: Some(Unit::Normalised),
            ..Presentation::at(0)
        };
        presentation
            .validate("roughness", &Value::Float(0.5))
            .unwrap();
        let problem = presentation
            .validate("roughness", &Value::Float(4.0))
            .unwrap_err();
        assert!(problem.because.contains("0 to 1"), "{problem}");
        assert!(problem.remedy.is_some());
    }

    #[test]
    fn a_read_only_field_says_why_rather_than_being_merely_greyed() {
        // "WHEN a command is disabled THEN the reason SHALL be reportable rather than the control
        // being merely greyed" — the same rule, one level down, at the field.
        let presentation = Presentation {
            writable: false,
            ..Presentation::at(0)
        };
        let problem = presentation
            .validate("bounds", &Value::Vec3([1.0; 3]))
            .unwrap_err();
        assert!(problem.because.contains("read-only"), "{problem}");
    }

    #[test]
    fn a_range_says_nothing_about_a_value_that_is_not_a_number() {
        let range = Range::new(0.0, 1.0);
        assert!(range.admits(&Value::Text("anything".into())));
        assert!(range.admits(&Value::Int(1)));
        assert!(!range.admits(&Value::Int(2)));
    }

    #[test]
    fn a_modified_setting_is_visible_as_modified() {
        let presentation = Presentation {
            default: Some(Value::Float(1.0)),
            ..Presentation::at(0)
        };
        assert!(!presentation.is_modified(&Value::Float(1.0)));
        assert!(presentation.is_modified(&Value::Float(2.0)));
    }

    #[test]
    fn a_field_with_no_declared_default_is_never_reported_as_modified() {
        // Rather than reported as modified always, which is what a `Value::Nil` default would do
        // and would put a reset affordance on every row in the inspector.
        assert!(!Presentation::at(0).is_modified(&Value::Float(7.0)));
    }
}
