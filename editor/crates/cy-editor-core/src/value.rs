//! The value vocabulary shared by the engine, a document's fields, and a command's parameters.
//!
//! One enum, three consumers, and that is the point. `CyVar` is what crosses the C ABI, a
//! document's field holds one of these, and a command parameter is typed with one of these — so a
//! property edit made by a person, by a script and by an agent produce the same operation with the
//! same payload, which is what `editor-agent-interface` means by "an agent's move is a human's
//! move".
//!
//! --- WHY IT MIRRORS `CyVarType` AND DOES NOT EXTEND IT ---------------------------------------------
//!
//! Every variant here has a `CyVarType` it maps to. A variant with no ABI counterpart would be a
//! value the editor could hold and could never send, and the first place that shows up is a
//! property the inspector can edit and the runtime never receives. The mapping is checked in
//! `cy-editor-sdk`, which is where the two vocabularies meet.
//!
//! The eight narrow integer types the ABI gained at 1.1 collapse onto [`Value::Int`] here, exactly
//! as they collapse onto Swift's `.i64`: the payload is always 64 bits and the storage width is a
//! property of the *field*, enforced on the write by the engine's range check. An editor-side
//! `I8(i8)` would be a second place that decision is made, and the two would eventually disagree.

use std::fmt;

/// A value a field can hold, a command can take, or the engine can return.
#[derive(Clone, PartialEq, Debug, Default)]
pub enum Value {
    /// No value. The ABI's `CY_VAR_NIL`.
    #[default]
    Nil,
    /// A boolean.
    Bool(bool),
    /// An integer. Every ABI integer width arrives here; the field's declared width is what
    /// constrains a write, and the engine is what enforces it.
    Int(i64),
    /// A 32-bit float, which is what almost every engine field is.
    Float(f32),
    /// A 64-bit float.
    Double(f64),
    /// Two floats.
    Vec2([f32; 2]),
    /// Three floats: a position, a direction, a colour without alpha.
    Vec3([f32; 3]),
    /// Four floats.
    Vec4([f32; 4]),
    /// A quaternion, in the ABI's `x, y, z, w` order.
    Quat([f32; 4]),
    /// UTF-8 text, owned. Owned rather than borrowed because a value outlives the engine call that
    /// produced it — it goes into a transaction, and a transaction outlives everything.
    Text(String),
    /// An opaque byte buffer, owned for the same reason.
    Bytes(Vec<u8>),
    /// A reference to an entity, carried as the engine's own identifier.
    Entity(u64),
}

impl Value {
    /// The name of this value's kind, for a diagnostic or a machine-readable parameter description.
    #[must_use]
    pub const fn kind(&self) -> ValueKind {
        match self {
            Value::Nil => ValueKind::Nil,
            Value::Bool(_) => ValueKind::Bool,
            Value::Int(_) => ValueKind::Int,
            Value::Float(_) => ValueKind::Float,
            Value::Double(_) => ValueKind::Double,
            Value::Vec2(_) => ValueKind::Vec2,
            Value::Vec3(_) => ValueKind::Vec3,
            Value::Vec4(_) => ValueKind::Vec4,
            Value::Quat(_) => ValueKind::Quat,
            Value::Text(_) => ValueKind::Text,
            Value::Bytes(_) => ValueKind::Bytes,
            Value::Entity(_) => ValueKind::Entity,
        }
    }

    /// The integer, when this is one.
    #[must_use]
    pub const fn as_int(&self) -> Option<i64> {
        match self {
            Value::Int(value) => Some(*value),
            _ => None,
        }
    }

    /// The 32-bit float, when this is one.
    #[must_use]
    pub const fn as_float(&self) -> Option<f32> {
        match self {
            Value::Float(value) => Some(*value),
            _ => None,
        }
    }

    /// The three floats, when this is a `Vec3`.
    #[must_use]
    pub const fn as_vec3(&self) -> Option<[f32; 3]> {
        match self {
            Value::Vec3(value) => Some(*value),
            _ => None,
        }
    }

    /// The text, when this is text.
    #[must_use]
    pub fn as_text(&self) -> Option<&str> {
        match self {
            Value::Text(value) => Some(value),
            _ => None,
        }
    }
}

impl fmt::Display for Value {
    /// A form a person reads in an inspector and an agent reads in a tool result.
    ///
    /// Floats are printed with the shortest representation that round-trips, which is Rust's
    /// default, because a property panel that showed `0.30000001` for a value the user typed as
    /// `0.3` would be reporting the format rather than the value.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Value::Nil => write!(f, "nil"),
            Value::Bool(value) => write!(f, "{value}"),
            Value::Int(value) => write!(f, "{value}"),
            Value::Float(value) => write!(f, "{value}"),
            Value::Double(value) => write!(f, "{value}"),
            Value::Vec2([x, y]) => write!(f, "({x}, {y})"),
            Value::Vec3([x, y, z]) => write!(f, "({x}, {y}, {z})"),
            Value::Vec4([x, y, z, w]) | Value::Quat([x, y, z, w]) => {
                write!(f, "({x}, {y}, {z}, {w})")
            }
            Value::Text(value) => write!(f, "{value}"),
            Value::Bytes(value) => write!(f, "<{} bytes>", value.len()),
            Value::Entity(value) => write!(f, "entity#{value}"),
        }
    }
}

/// A value's kind without its payload: what a parameter or a field declares it accepts.
///
/// `editor-agent-interface` requires command metadata rich enough for machine invocation, "typed
/// parameters with their meaning". This is the type half; the meaning is a sentence the command
/// carries beside it.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum ValueKind {
    /// No value.
    Nil,
    /// A boolean.
    Bool,
    /// An integer of any ABI width.
    Int,
    /// A 32-bit float.
    Float,
    /// A 64-bit float.
    Double,
    /// Two floats.
    Vec2,
    /// Three floats.
    Vec3,
    /// Four floats.
    Vec4,
    /// A quaternion.
    Quat,
    /// UTF-8 text.
    Text,
    /// An opaque byte buffer.
    Bytes,
    /// An entity reference.
    Entity,
}

impl ValueKind {
    /// The name a caller sees in a parameter description or a schema listing.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            ValueKind::Nil => "nil",
            ValueKind::Bool => "bool",
            ValueKind::Int => "int",
            ValueKind::Float => "float",
            ValueKind::Double => "double",
            ValueKind::Vec2 => "vec2",
            ValueKind::Vec3 => "vec3",
            ValueKind::Vec4 => "vec4",
            ValueKind::Quat => "quat",
            ValueKind::Text => "text",
            ValueKind::Bytes => "bytes",
            ValueKind::Entity => "entity",
        }
    }

    /// A value of this kind with no information in it, for a parameter that was not supplied.
    #[must_use]
    pub fn empty(self) -> Value {
        match self {
            ValueKind::Nil => Value::Nil,
            ValueKind::Bool => Value::Bool(false),
            ValueKind::Int => Value::Int(0),
            ValueKind::Float => Value::Float(0.0),
            ValueKind::Double => Value::Double(0.0),
            ValueKind::Vec2 => Value::Vec2([0.0; 2]),
            ValueKind::Vec3 => Value::Vec3([0.0; 3]),
            ValueKind::Vec4 => Value::Vec4([0.0; 4]),
            ValueKind::Quat => Value::Quat([0.0, 0.0, 0.0, 1.0]),
            ValueKind::Text => Value::Text(String::new()),
            ValueKind::Bytes => Value::Bytes(Vec::new()),
            ValueKind::Entity => Value::Entity(0),
        }
    }
}

impl fmt::Display for ValueKind {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.name())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_kind_round_trips_through_an_empty_value() {
        let kinds = [
            ValueKind::Nil,
            ValueKind::Bool,
            ValueKind::Int,
            ValueKind::Float,
            ValueKind::Double,
            ValueKind::Vec2,
            ValueKind::Vec3,
            ValueKind::Vec4,
            ValueKind::Quat,
            ValueKind::Text,
            ValueKind::Bytes,
            ValueKind::Entity,
        ];
        for kind in kinds {
            assert_eq!(kind.empty().kind(), kind, "{kind} does not round-trip");
        }
    }

    #[test]
    fn an_identity_quaternion_is_the_empty_quaternion() {
        // Not zero. A zero quaternion is not a rotation, and an inspector that offered one as the
        // default for an unset field would be offering an invalid value.
        assert_eq!(ValueKind::Quat.empty(), Value::Quat([0.0, 0.0, 0.0, 1.0]));
    }
}
