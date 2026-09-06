//! A small deterministic byte codec, shared by the journal and the live bridge.
//!
//! One encoding, two consumers, on purpose. `editor-documents-and-transactions` requires that "the
//! journal SHALL be the same operation stream used by diff, live editing, and any future
//! collaboration, rather than a separate representation" — so the bytes an autosave writes and the
//! bytes a hosted runtime receives are produced by the same code, and a change to one is a change to
//! both.
//!
//! --- WHY NOT A SERIALISATION CRATE ------------------------------------------------------------------
//!
//! Because what this needs is roughly a hundred lines, and because the two properties that matter
//! here are ones a general framework does not promise. It is **deterministic** — the same value
//! produces the same bytes on every host, which is what lets a journal be compared and a
//! reproduction be replayed — and it is **self-describing enough to refuse**, so a truncated file or
//! a frame from a newer editor is a named error rather than a plausible-looking wrong value.
//!
//! Little-endian throughout, because every platform this engine targets is, and because a codec that
//! swapped by host would produce a journal that only replays where it was written.
//!
//! --- WHAT IT DOES NOT DO ------------------------------------------------------------------------------
//!
//! No versioning of its own, no schema evolution, no compression. Those belong to the formats built
//! on it — the journal writes its own version number and refuses one it does not know — because a
//! codec that also owned migration would be the place two unrelated decisions met.

use crate::problem::{Problem, Result};
use crate::value::Value;

/// Append-only byte writer.
#[derive(Clone, Debug, Default)]
pub struct Writer {
    bytes: Vec<u8>,
}

impl Writer {
    /// An empty writer.
    #[must_use]
    pub const fn new() -> Self {
        Self { bytes: Vec::new() }
    }

    /// The bytes written so far.
    #[must_use]
    pub fn finish(self) -> Vec<u8> {
        self.bytes
    }

    /// How many bytes have been written. Used by the history budget, which counts payload bytes.
    #[must_use]
    pub fn len(&self) -> usize {
        self.bytes.len()
    }

    /// Whether nothing has been written.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }

    /// One byte.
    pub fn u8(&mut self, value: u8) {
        self.bytes.push(value);
    }

    /// A 32-bit unsigned integer.
    pub fn u32(&mut self, value: u32) {
        self.bytes.extend_from_slice(&value.to_le_bytes());
    }

    /// A 64-bit unsigned integer.
    pub fn u64(&mut self, value: u64) {
        self.bytes.extend_from_slice(&value.to_le_bytes());
    }

    /// A 128-bit unsigned integer, which is what an identity is.
    pub fn u128(&mut self, value: u128) {
        self.bytes.extend_from_slice(&value.to_le_bytes());
    }

    /// A 64-bit signed integer.
    pub fn i64(&mut self, value: i64) {
        self.bytes.extend_from_slice(&value.to_le_bytes());
    }

    /// A 32-bit float, by its bits — so a NaN round-trips as the same NaN and a journal compares.
    pub fn f32(&mut self, value: f32) {
        self.bytes.extend_from_slice(&value.to_bits().to_le_bytes());
    }

    /// A 64-bit float, by its bits.
    pub fn f64(&mut self, value: f64) {
        self.bytes.extend_from_slice(&value.to_bits().to_le_bytes());
    }

    /// A length-prefixed byte string.
    pub fn bytes(&mut self, value: &[u8]) {
        self.u32(u32::try_from(value.len()).unwrap_or(u32::MAX));
        self.bytes.extend_from_slice(value);
    }

    /// A length-prefixed UTF-8 string.
    pub fn text(&mut self, value: &str) {
        self.bytes(value.as_bytes());
    }

    /// A [`Value`], tagged by its kind.
    pub fn value(&mut self, value: &Value) {
        match value {
            Value::Nil => self.u8(0),
            Value::Bool(inner) => {
                self.u8(1);
                self.u8(u8::from(*inner));
            }
            Value::Int(inner) => {
                self.u8(2);
                self.i64(*inner);
            }
            Value::Float(inner) => {
                self.u8(3);
                self.f32(*inner);
            }
            Value::Double(inner) => {
                self.u8(4);
                self.f64(*inner);
            }
            Value::Vec2(lanes) => {
                self.u8(5);
                for lane in lanes {
                    self.f32(*lane);
                }
            }
            Value::Vec3(lanes) => {
                self.u8(6);
                for lane in lanes {
                    self.f32(*lane);
                }
            }
            Value::Vec4(lanes) => {
                self.u8(7);
                for lane in lanes {
                    self.f32(*lane);
                }
            }
            Value::Quat(lanes) => {
                self.u8(8);
                for lane in lanes {
                    self.f32(*lane);
                }
            }
            Value::Text(inner) => {
                self.u8(9);
                self.text(inner);
            }
            Value::Bytes(inner) => {
                self.u8(10);
                self.bytes(inner);
            }
            Value::Entity(inner) => {
                self.u8(11);
                self.u64(*inner);
            }
        }
    }
}

/// Byte reader that refuses rather than guessing.
#[derive(Clone, Debug)]
pub struct Reader<'bytes> {
    bytes: &'bytes [u8],
    at: usize,
}

impl<'bytes> Reader<'bytes> {
    /// A reader over `bytes`.
    #[must_use]
    pub const fn new(bytes: &'bytes [u8]) -> Self {
        Self { bytes, at: 0 }
    }

    /// How many bytes remain.
    #[must_use]
    pub const fn remaining(&self) -> usize {
        self.bytes.len() - self.at
    }

    /// Whether everything has been consumed.
    #[must_use]
    pub const fn is_empty(&self) -> bool {
        self.remaining() == 0
    }

    fn take(&mut self, count: usize) -> Result<&'bytes [u8]> {
        if self.remaining() < count {
            return Err(Problem::new(
                "decode a record",
                format!(
                    "it wants {count} more bytes and {} are left",
                    self.remaining()
                ),
            )
            .with_remedy("the record is truncated; the writer stopped part way"));
        }
        let slice = &self.bytes[self.at..self.at + count];
        self.at += count;
        Ok(slice)
    }

    /// One byte.
    pub fn u8(&mut self) -> Result<u8> {
        Ok(self.take(1)?[0])
    }

    /// A 32-bit unsigned integer.
    pub fn u32(&mut self) -> Result<u32> {
        let bytes = self.take(4)?;
        Ok(u32::from_le_bytes(bytes.try_into().expect("four bytes")))
    }

    /// A 64-bit unsigned integer.
    pub fn u64(&mut self) -> Result<u64> {
        let bytes = self.take(8)?;
        Ok(u64::from_le_bytes(bytes.try_into().expect("eight bytes")))
    }

    /// A 128-bit unsigned integer.
    pub fn u128(&mut self) -> Result<u128> {
        let bytes = self.take(16)?;
        Ok(u128::from_le_bytes(
            bytes.try_into().expect("sixteen bytes"),
        ))
    }

    /// A 64-bit signed integer.
    pub fn i64(&mut self) -> Result<i64> {
        let bytes = self.take(8)?;
        Ok(i64::from_le_bytes(bytes.try_into().expect("eight bytes")))
    }

    /// A 32-bit float.
    pub fn f32(&mut self) -> Result<f32> {
        let bytes = self.take(4)?;
        Ok(f32::from_bits(u32::from_le_bytes(
            bytes.try_into().expect("four bytes"),
        )))
    }

    /// A 64-bit float.
    pub fn f64(&mut self) -> Result<f64> {
        let bytes = self.take(8)?;
        Ok(f64::from_bits(u64::from_le_bytes(
            bytes.try_into().expect("eight bytes"),
        )))
    }

    /// A length-prefixed byte string.
    pub fn bytes(&mut self) -> Result<Vec<u8>> {
        let length = self.u32()? as usize;
        Ok(self.take(length)?.to_vec())
    }

    /// A length-prefixed UTF-8 string.
    pub fn text(&mut self) -> Result<String> {
        let bytes = self.bytes()?;
        String::from_utf8(bytes).map_err(|error| {
            Problem::new("decode a string", format!("it is not valid UTF-8: {error}"))
        })
    }

    /// A [`Value`].
    pub fn value(&mut self) -> Result<Value> {
        let tag = self.u8()?;
        let value = match tag {
            0 => Value::Nil,
            1 => Value::Bool(self.u8()? != 0),
            2 => Value::Int(self.i64()?),
            3 => Value::Float(self.f32()?),
            4 => Value::Double(self.f64()?),
            5 => Value::Vec2([self.f32()?, self.f32()?]),
            6 => Value::Vec3([self.f32()?, self.f32()?, self.f32()?]),
            7 => Value::Vec4([self.f32()?, self.f32()?, self.f32()?, self.f32()?]),
            8 => Value::Quat([self.f32()?, self.f32()?, self.f32()?, self.f32()?]),
            9 => Value::Text(self.text()?),
            10 => Value::Bytes(self.bytes()?),
            11 => Value::Entity(self.u64()?),
            other => {
                return Err(Problem::new(
                    "decode a value",
                    format!("tag {other} is not one this build knows"),
                )
                .with_remedy(
                    "the record was written by a newer editor; open it with that one, or discard it",
                ));
            }
        };
        Ok(value)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_value_round_trips_byte_for_byte() {
        let values = [
            Value::Nil,
            Value::Bool(true),
            Value::Int(i64::MIN),
            Value::Float(-0.0),
            Value::Double(f64::MAX),
            Value::Vec2([1.0, 2.0]),
            Value::Vec3([1.0, 2.0, 3.0]),
            Value::Vec4([1.0, 2.0, 3.0, 4.0]),
            Value::Quat([0.0, 0.0, 0.0, 1.0]),
            Value::Text("street lamp".into()),
            Value::Bytes(vec![0, 255, 7]),
            Value::Entity(u64::MAX),
        ];
        for value in &values {
            let mut writer = Writer::new();
            writer.value(value);
            let bytes = writer.finish();
            let mut reader = Reader::new(&bytes);
            assert_eq!(&reader.value().unwrap(), value);
            assert!(reader.is_empty(), "{value:?} left bytes behind");
        }
    }

    #[test]
    fn encoding_is_deterministic() {
        let value = Value::Vec3([1.5, -2.0, 0.25]);
        let encode = || {
            let mut writer = Writer::new();
            writer.value(&value);
            writer.finish()
        };
        assert_eq!(encode(), encode());
    }

    #[test]
    fn a_truncated_record_is_named_rather_than_guessed() {
        let mut writer = Writer::new();
        writer.value(&Value::Text("a long enough string".into()));
        let bytes = writer.finish();
        let mut reader = Reader::new(&bytes[..bytes.len() - 4]);
        let problem = reader.value().unwrap_err();
        assert!(
            problem.remedy.as_deref().unwrap().contains("truncated"),
            "{problem}"
        );
    }

    #[test]
    fn a_tag_from_a_newer_editor_says_so() {
        let bytes = [200_u8];
        let problem = Reader::new(&bytes).value().unwrap_err();
        assert!(
            problem.remedy.as_deref().unwrap().contains("newer editor"),
            "{problem}"
        );
    }
}
