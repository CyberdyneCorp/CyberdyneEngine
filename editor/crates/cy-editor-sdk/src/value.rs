//! `CyVar` in, [`Value`] out. The one place the two vocabularies meet.
//!
//! `cy_abi.h` states the ownership rule once, at the table: "an entry is called on the thread that
//! called into the module, arguments are borrowed for the duration of the call, and a returned
//! `CyVar` is owned by the caller if and only if it carries `CY_VAR_FLAG_OWNED`". Both halves are
//! implemented here and nowhere else.
//!
//! Reading copies out and releases, so no editor value ever holds engine memory. Writing builds a
//! **borrowed** var over a Rust buffer that outlives the call — which is what the borrowed-argument
//! rule permits, and which is why writing a string does not need an engine handle to allocate one.

use cy_editor_core::Value;
use cy_editor_core::problem::{Problem, Result};

use crate::generated::enums::VarType;
use crate::generated::ffi;
use crate::generated::interface::Interface;

/// `CY_VAR_FLAG_OWNED`, from `cy_abi.h`.
///
/// Hand-written, because the ABI description carries declarations and not preprocessor constants.
/// It is one bit that has never moved and the ABI gate refuses a change to the enum beside it, but
/// this is nonetheless the one number in this crate that is a copy rather than a generation — said
/// here so that a reader knows to check it if a release ever misbehaves.
pub const VAR_FLAG_OWNED: u32 = 0x1;

/// A `CyVar` the SDK owns and will release exactly once.
///
/// Exists so that the release cannot be forgotten on an error path — every early return in
/// [`read_var`] would otherwise leak a heap payload, and `var_live_count` is precisely the counter
/// that would then climb through a session without anyone noticing until it mattered.
pub(crate) struct OwnedVar {
    var: ffi::CyVar,
    interface: Interface,
}

impl OwnedVar {
    /// Take ownership of a var the engine filled in.
    pub(crate) const fn new(interface: Interface, var: ffi::CyVar) -> Self {
        Self { var, interface }
    }

    /// The var, borrowed for reading.
    pub(crate) const fn get(&self) -> &ffi::CyVar {
        &self.var
    }
}

impl Drop for OwnedVar {
    fn drop(&mut self) {
        if self.var.flags & VAR_FLAG_OWNED == 0 {
            return;
        }
        if let Some(release) = self.interface.table().var_release {
            // SAFETY: the var was produced by this interface and is released exactly once — `Drop`
            // runs once, and `OwnedVar` is neither `Copy` nor `Clone`. `var_release` also clears
            // the value to nil, so even a hypothetical second call would be a no-op.
            unsafe { release(&raw mut self.var) };
        }
    }
}

/// Copy a `CyVar` out into an owned editor [`Value`].
///
/// Fails rather than guessing on a type this build has no name for, which is what an engine one
/// minor version ahead produces.
pub(crate) fn read_var(var: &ffi::CyVar) -> Result<Value> {
    let kind = VarType::from_raw(var.r#type).ok_or_else(|| {
        Problem::new(
            "read a field",
            format!(
                "the engine sent CyVarType {}, which this editor has no name for",
                var.r#type
            ),
        )
        .with_remedy("the runtime is newer than this editor; rebuild them from one revision")
    })?;

    // SAFETY, once for the whole match: reading a union member is sound exactly when the tag says
    // that member is the live one, and `var.r#type` is that tag. This is the only place in the
    // workspace that reads it, which is why the reasoning is written once here rather than repeated.
    let payload = &var.payload;
    let value = match kind {
        VarType::Nil => Value::Nil,
        VarType::Bool => Value::Bool(unsafe { payload.as_bool }),
        VarType::I64
        | VarType::I8
        | VarType::I16
        | VarType::I32
        | VarType::U8
        | VarType::U16
        | VarType::U32
        | VarType::U64 => Value::Int(unsafe { payload.as_i64 }),
        VarType::F32 => Value::Float(unsafe { payload.as_f32 }),
        VarType::F64 => Value::Double(unsafe { payload.as_f64 }),
        VarType::Vec2 => {
            let lanes = unsafe { payload.as_f32x4 };
            Value::Vec2([lanes[0], lanes[1]])
        }
        VarType::Vec3 => {
            let lanes = unsafe { payload.as_f32x4 };
            Value::Vec3([lanes[0], lanes[1], lanes[2]])
        }
        VarType::Vec4 => Value::Vec4(unsafe { payload.as_f32x4 }),
        VarType::Quat => Value::Quat(unsafe { payload.as_f32x4 }),
        VarType::Entity => Value::Entity(unsafe { payload.as_entity }),
        VarType::String => Value::Text(read_text(var)?),
        VarType::Bytes => Value::Bytes(read_bytes(var)),
    };
    Ok(value)
}

fn read_bytes(var: &ffi::CyVar) -> Vec<u8> {
    // SAFETY: the tag says this is a byte payload, so `as_bytes` is the live member. A null pointer
    // with a non-zero length would be an engine defect; treating null as empty is the reading that
    // cannot fault, and an empty buffer is what a caller does something sensible with.
    let data = unsafe { var.payload.as_bytes };
    let length = usize::try_from(var.length).unwrap_or(usize::MAX);
    if data.is_null() || length == 0 {
        return Vec::new();
    }
    // SAFETY: the ABI's contract is that `length` bytes are readable at `data` for the duration of
    // the caller's ownership of the var, which is at least this call.
    unsafe { std::slice::from_raw_parts(data.cast::<u8>(), length) }.to_vec()
}

fn read_text(var: &ffi::CyVar) -> Result<String> {
    let bytes = read_bytes(var);
    String::from_utf8(bytes).map_err(|error| {
        Problem::new(
            "read a text field",
            format!("the engine's bytes are not valid UTF-8: {error}"),
        )
        .with_remedy("the field holds bytes rather than text; read it as bytes")
    })
}

/// A `CyVar` borrowed over editor-owned memory, for the duration of one call.
///
/// The lifetime is the point: the var holds a pointer into `owner`, and the type system is what
/// stops it outliving the buffer. Nothing constructs one of these except [`with_var`].
pub(crate) struct BorrowedVar<'owner> {
    var: ffi::CyVar,
    marker: std::marker::PhantomData<&'owner [u8]>,
}

impl BorrowedVar<'_> {
    pub(crate) const fn as_ptr(&self) -> *const ffi::CyVar {
        &raw const self.var
    }
}

/// Build a `CyVar` for `value` and hand it to `use_it` for exactly the duration of one call.
///
/// A closure rather than a returned value, because a text or byte var points into a buffer that
/// must outlive the call and no longer. Returning one would make the caller responsible for
/// keeping the buffer alive, which is the mistake this shape removes.
pub(crate) fn with_var<R>(value: &Value, use_it: impl FnOnce(&BorrowedVar<'_>) -> R) -> R {
    let mut var = ffi::CyVar {
        r#type: VarType::Nil.as_raw(),
        flags: 0,
        length: 0,
        payload: ffi::CyVarPayload { as_i64: 0 },
    };
    match value {
        Value::Nil => {}
        Value::Bool(inner) => {
            var.r#type = VarType::Bool.as_raw();
            var.payload = ffi::CyVarPayload { as_bool: *inner };
        }
        Value::Int(inner) => {
            var.r#type = VarType::I64.as_raw();
            var.payload = ffi::CyVarPayload { as_i64: *inner };
        }
        Value::Float(inner) => {
            var.r#type = VarType::F32.as_raw();
            var.payload = ffi::CyVarPayload { as_f32: *inner };
        }
        Value::Double(inner) => {
            var.r#type = VarType::F64.as_raw();
            var.payload = ffi::CyVarPayload { as_f64: *inner };
        }
        Value::Vec2([x, y]) => {
            var.r#type = VarType::Vec2.as_raw();
            var.payload = ffi::CyVarPayload {
                as_f32x4: [*x, *y, 0.0, 0.0],
            };
        }
        Value::Vec3([x, y, z]) => {
            var.r#type = VarType::Vec3.as_raw();
            var.payload = ffi::CyVarPayload {
                as_f32x4: [*x, *y, *z, 0.0],
            };
        }
        Value::Vec4(lanes) => {
            var.r#type = VarType::Vec4.as_raw();
            var.payload = ffi::CyVarPayload { as_f32x4: *lanes };
        }
        Value::Quat(lanes) => {
            var.r#type = VarType::Quat.as_raw();
            var.payload = ffi::CyVarPayload { as_f32x4: *lanes };
        }
        Value::Entity(inner) => {
            var.r#type = VarType::Entity.as_raw();
            var.payload = ffi::CyVarPayload { as_entity: *inner };
        }
        Value::Text(text) => {
            var.r#type = VarType::String.as_raw();
            var.length = text.len() as u64;
            var.payload = ffi::CyVarPayload {
                as_bytes: text.as_ptr().cast::<std::ffi::c_void>(),
            };
            // The borrow ends when this call does; `text` outlives it because it is borrowed from
            // the caller's `value`.
            return use_it(&BorrowedVar {
                var,
                marker: std::marker::PhantomData,
            });
        }
        Value::Bytes(bytes) => {
            var.r#type = VarType::Bytes.as_raw();
            var.length = bytes.len() as u64;
            var.payload = ffi::CyVarPayload {
                as_bytes: bytes.as_ptr().cast::<std::ffi::c_void>(),
            };
            return use_it(&BorrowedVar {
                var,
                marker: std::marker::PhantomData,
            });
        }
    }
    use_it(&BorrowedVar {
        var,
        marker: std::marker::PhantomData,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Round-trip a value through the FFI representation without an engine.
    ///
    /// Not a substitute for the end-to-end test against a real interface table — that is
    /// `tests/embedded_host.rs` — but it is what pins the union tags, and it runs in microseconds.
    fn round_trip(value: &Value) -> Value {
        with_var(value, |borrowed| {
            // SAFETY: the borrowed var is live for the duration of this closure, which is exactly
            // the window the ABI's borrowed-argument rule describes.
            read_var(unsafe { &*borrowed.as_ptr() }).expect("a var this SDK built is readable")
        })
    }

    #[test]
    fn every_inline_value_round_trips_through_a_var() {
        let values = [
            Value::Nil,
            Value::Bool(true),
            Value::Int(-9_000_000_000),
            Value::Float(1.5),
            Value::Double(-2.25),
            Value::Vec2([1.0, 2.0]),
            Value::Vec3([1.0, 2.0, 3.0]),
            Value::Vec4([1.0, 2.0, 3.0, 4.0]),
            Value::Quat([0.0, 0.0, 0.0, 1.0]),
            Value::Entity(42),
        ];
        for value in values {
            assert_eq!(
                round_trip(&value),
                value,
                "{value:?} did not survive the round trip"
            );
        }
    }

    #[test]
    fn text_and_bytes_round_trip_without_an_engine_allocation() {
        assert_eq!(
            round_trip(&Value::Text("streetlight".into())),
            Value::Text("streetlight".into())
        );
        assert_eq!(
            round_trip(&Value::Bytes(vec![1, 2, 3])),
            Value::Bytes(vec![1, 2, 3])
        );
        assert_eq!(
            round_trip(&Value::Text(String::new())),
            Value::Text(String::new())
        );
    }

    #[test]
    fn an_unknown_var_type_is_reported_rather_than_guessed() {
        let var = ffi::CyVar {
            r#type: 9_999,
            flags: 0,
            length: 0,
            payload: ffi::CyVarPayload { as_i64: 0 },
        };
        let problem = read_var(&var).unwrap_err();
        assert!(problem.because.contains("9999"), "{}", problem.because);
        assert!(problem.remedy.is_some());
    }
}
