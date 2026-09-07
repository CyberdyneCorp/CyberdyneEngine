//! JSON, in the subset JSON-RPC 2.0 carries.
//!
//! --- WHY THIS EXISTS RATHER THAN A DEPENDENCY -------------------------------------------------------
//!
//! The manifest states the argument in full. In short: this is the whole of what the protocol needs,
//! it is exercised by round-trip tests below, and it keeps a workspace that has spent five
//! milestones being buildable offline in a bare checkout that way.
//!
//! --- WHAT IT DELIBERATELY DOES NOT DO ---------------------------------------------------------------
//!
//! No streaming, no schema, no derive. Numbers are `f64` on the way in and are written back as
//! integers when they are integral, which is what a JSON-RPC identifier needs and is the only
//! numeric subtlety in the protocol. Depth is bounded, because a parser with unbounded recursion is
//! a stack overflow one malformed message away, and a transport reading from a pipe is exactly where
//! that message arrives.

use std::collections::BTreeMap;
use std::fmt::Write as _;

use cy_editor_core::problem::{Problem, Result};

/// How deeply a document may nest before this parser refuses it.
///
/// The deepest thing the protocol sends is a tool call's arguments inside a request, which is four.
/// Sixty-four is far above anything real and far below a stack overflow, and the refusal names the
/// limit so a caller that genuinely needs more knows what to ask for.
const MAXIMUM_DEPTH: usize = 64;

/// A JSON value.
#[derive(Clone, PartialEq, Debug, Default)]
pub enum Json {
    /// `null`.
    #[default]
    Null,
    /// `true` or `false`.
    Bool(bool),
    /// A number. JSON has one numeric type and so does this.
    Number(f64),
    /// A string.
    Text(String),
    /// An array.
    Array(Vec<Json>),
    /// An object. Ordered, so that two encodings of the same value are the same bytes — which is
    /// what lets a test compare them and a log be diffed.
    Object(BTreeMap<String, Json>),
}

impl Json {
    /// An object from its entries.
    #[must_use]
    pub fn object(entries: impl IntoIterator<Item = (&'static str, Json)>) -> Self {
        Json::Object(
            entries
                .into_iter()
                .map(|(key, value)| (key.to_string(), value))
                .collect(),
        )
    }

    /// A string value.
    #[must_use]
    pub fn text(value: impl Into<String>) -> Self {
        Json::Text(value.into())
    }

    /// One member of an object, or `Null` when there is none.
    ///
    /// `Null` rather than `Option`, because every caller here treats a missing member and an
    /// explicit null the same way and the two-step would only move the check.
    #[must_use]
    pub fn get(&self, key: &str) -> &Json {
        match self {
            Json::Object(members) => members.get(key).unwrap_or(&Json::Null),
            _ => &Json::Null,
        }
    }

    /// This value as text, when it is text.
    #[must_use]
    pub fn as_text(&self) -> Option<&str> {
        match self {
            Json::Text(value) => Some(value),
            _ => None,
        }
    }

    /// This value as a number, when it is one.
    #[must_use]
    pub const fn as_number(&self) -> Option<f64> {
        match self {
            Json::Number(value) => Some(*value),
            _ => None,
        }
    }

    /// This value's object members, when it is an object.
    #[must_use]
    pub const fn as_object(&self) -> Option<&BTreeMap<String, Json>> {
        match self {
            Json::Object(members) => Some(members),
            _ => None,
        }
    }

    /// Whether this is `null` or absent.
    #[must_use]
    pub const fn is_null(&self) -> bool {
        matches!(self, Json::Null)
    }

    /// Render it.
    #[must_use]
    pub fn render(&self) -> String {
        let mut out = String::new();
        self.write(&mut out);
        out
    }

    fn write(&self, out: &mut String) {
        match self {
            Json::Null => out.push_str("null"),
            Json::Bool(true) => out.push_str("true"),
            Json::Bool(false) => out.push_str("false"),
            Json::Number(value) => write_number(*value, out),
            Json::Text(value) => write_string(value, out),
            Json::Array(items) => {
                out.push('[');
                for (index, item) in items.iter().enumerate() {
                    if index != 0 {
                        out.push(',');
                    }
                    item.write(out);
                }
                out.push(']');
            }
            Json::Object(members) => {
                out.push('{');
                for (index, (key, value)) in members.iter().enumerate() {
                    if index != 0 {
                        out.push(',');
                    }
                    write_string(key, out);
                    out.push(':');
                    value.write(out);
                }
                out.push('}');
            }
        }
    }
}

/// An integral number is written without a decimal point.
///
/// JSON-RPC identifiers are integers and a peer that received `1.0` where it sent `1` would be
/// entitled to say the reply matched no request it made. A non-finite number is written as `null`,
/// because JSON has no spelling for one and inventing `NaN` produces a document nothing can parse.
#[allow(
    clippy::cast_possible_truncation,
    reason = "the branch is guarded on the value being integral and inside the range an f64 \
              represents exactly, which is the only case that reaches the cast"
)]
fn write_number(value: f64, out: &mut String) {
    if !value.is_finite() {
        out.push_str("null");
    } else if value.fract() == 0.0 && value.abs() < 9.007_199_254_740_992e15 {
        let _ = write!(out, "{}", value as i64);
    } else {
        let _ = write!(out, "{value}");
    }
}

fn write_string(value: &str, out: &mut String) {
    out.push('"');
    for character in value.chars() {
        match character {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            // Everything below a space has to be escaped, and the six with short forms are above.
            // \u{7f} is left alone deliberately: it is legal unescaped JSON and escaping it would
            // make this writer's output differ from every other for no benefit.
            control if control < ' ' => {
                let _ = write!(out, "\\u{:04x}", control as u32);
            }
            other => out.push(other),
        }
    }
    out.push('"');
}

/// Read one JSON document.
///
/// Trailing content is an error rather than ignored: a line that carries two documents is a framing
/// failure, and silently reading the first would desynchronise the stream in a way that is very hard
/// to diagnose from the other end.
pub fn parse(text: &str) -> Result<Json> {
    let mut parser = Parser {
        rest: text.as_bytes(),
        at: 0,
    };
    let value = parser.value(0)?;
    parser.spaces();
    if parser.at != parser.rest.len() {
        return Err(refuse(
            "read one JSON document",
            "there is more after the end of it",
        ));
    }
    Ok(value)
}

struct Parser<'a> {
    rest: &'a [u8],
    at: usize,
}

impl Parser<'_> {
    fn spaces(&mut self) {
        while self
            .rest
            .get(self.at)
            .is_some_and(|byte| matches!(byte, b' ' | b'\t' | b'\n' | b'\r'))
        {
            self.at += 1;
        }
    }

    fn peek(&self) -> Option<u8> {
        self.rest.get(self.at).copied()
    }

    fn expect(&mut self, literal: &str) -> Result<()> {
        if self.rest[self.at..].starts_with(literal.as_bytes()) {
            self.at += literal.len();
            return Ok(());
        }
        Err(refuse(
            format!("read {literal:?} at byte {}", self.at),
            "the document does not say that there",
        ))
    }

    fn value(&mut self, depth: usize) -> Result<Json> {
        if depth > MAXIMUM_DEPTH {
            return Err(Problem::new(
                "read a JSON document",
                format!("it nests more than {MAXIMUM_DEPTH} deep"),
            )
            .with_remedy(
                "send a flatter document; nothing this protocol carries needs that depth",
            ));
        }
        self.spaces();
        match self.peek() {
            None => Err(refuse("read a value", "the document ended")),
            Some(b'n') => self.expect("null").map(|()| Json::Null),
            Some(b't') => self.expect("true").map(|()| Json::Bool(true)),
            Some(b'f') => self.expect("false").map(|()| Json::Bool(false)),
            Some(b'"') => self.string().map(Json::Text),
            Some(b'[') => self.array(depth),
            Some(b'{') => self.object(depth),
            Some(_) => self.number(),
        }
    }

    fn array(&mut self, depth: usize) -> Result<Json> {
        self.at += 1;
        let mut items = Vec::new();
        self.spaces();
        if self.peek() == Some(b']') {
            self.at += 1;
            return Ok(Json::Array(items));
        }
        loop {
            items.push(self.value(depth + 1)?);
            self.spaces();
            match self.peek() {
                Some(b',') => self.at += 1,
                Some(b']') => {
                    self.at += 1;
                    return Ok(Json::Array(items));
                }
                _ => {
                    return Err(refuse(
                        "read an array",
                        "it has no comma and no closing bracket",
                    ));
                }
            }
        }
    }

    fn object(&mut self, depth: usize) -> Result<Json> {
        self.at += 1;
        let mut members = BTreeMap::new();
        self.spaces();
        if self.peek() == Some(b'}') {
            self.at += 1;
            return Ok(Json::Object(members));
        }
        loop {
            self.spaces();
            let key = self.string()?;
            self.spaces();
            if self.peek() != Some(b':') {
                return Err(refuse(
                    "read an object",
                    "a member has no colon after its name",
                ));
            }
            self.at += 1;
            members.insert(key, self.value(depth + 1)?);
            self.spaces();
            match self.peek() {
                Some(b',') => self.at += 1,
                Some(b'}') => {
                    self.at += 1;
                    return Ok(Json::Object(members));
                }
                _ => {
                    return Err(refuse(
                        "read an object",
                        "it has no comma and no closing brace",
                    ));
                }
            }
        }
    }

    fn string(&mut self) -> Result<String> {
        if self.peek() != Some(b'"') {
            return Err(refuse("read a string", "it does not begin with a quote"));
        }
        self.at += 1;
        let mut out = String::new();
        loop {
            let byte = self
                .peek()
                .ok_or_else(|| refuse("read a string", "the document ended inside it"))?;
            self.at += 1;
            match byte {
                b'"' => return Ok(out),
                b'\\' => {
                    let escape = self
                        .peek()
                        .ok_or_else(|| refuse("read an escape", "the document ended inside it"))?;
                    self.at += 1;
                    match escape {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\u{8}'),
                        b'f' => out.push('\u{c}'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => out.push(self.unicode()?),
                        other => {
                            return Err(refuse(
                                "read an escape",
                                format!("\\{} is not one JSON has", other as char),
                            ));
                        }
                    }
                }
                // The bytes of a multi-byte character are copied through as they arrive; the string
                // was &str to begin with, so the sequence is valid by construction.
                other => {
                    let start = self.at - 1;
                    let length = utf8_length(other);
                    self.at = start + length;
                    let slice = self.rest.get(start..self.at).ok_or_else(|| {
                        refuse("read a string", "it ends in the middle of a character")
                    })?;
                    out.push_str(
                        std::str::from_utf8(slice)
                            .map_err(|_| refuse("read a string", "it is not valid UTF-8"))?,
                    );
                }
            }
        }
    }

    /// One `\uXXXX` escape, pairing a surrogate with the one that follows it.
    ///
    /// Surrogate pairs are the case that is always forgotten, and the symptom is an emoji in a tool
    /// argument turning into two replacement characters — which looks like a font problem and is
    /// not.
    fn unicode(&mut self) -> Result<char> {
        let first = self.hex4()?;
        if !(0xd800..0xdc00).contains(&first) {
            return char::from_u32(first)
                .ok_or_else(|| refuse("read an escape", "it is not a character"));
        }
        self.expect("\\u")?;
        let second = self.hex4()?;
        if !(0xdc00..0xe000).contains(&second) {
            return Err(refuse(
                "read a surrogate pair",
                "the second half is not a low surrogate",
            ));
        }
        let combined = 0x1_0000 + ((first - 0xd800) << 10) + (second - 0xdc00);
        char::from_u32(combined).ok_or_else(|| refuse("read an escape", "it is not a character"))
    }

    fn hex4(&mut self) -> Result<u32> {
        let slice = self
            .rest
            .get(self.at..self.at + 4)
            .ok_or_else(|| refuse("read an escape", "it is shorter than four digits"))?;
        self.at += 4;
        let text = std::str::from_utf8(slice)
            .map_err(|_| refuse("read an escape", "its digits are not text"))?;
        u32::from_str_radix(text, 16)
            .map_err(|_| refuse("read an escape", format!("{text:?} is not four hex digits")))
    }

    fn number(&mut self) -> Result<Json> {
        let start = self.at;
        while self
            .peek()
            .is_some_and(|byte| matches!(byte, b'-' | b'+' | b'.' | b'e' | b'E' | b'0'..=b'9'))
        {
            self.at += 1;
        }
        let text = std::str::from_utf8(&self.rest[start..self.at])
            .map_err(|_| refuse("read a number", "its digits are not text"))?;
        text.parse::<f64>()
            .map(Json::Number)
            .map_err(|_| refuse("read a number", format!("{text:?} is not one")))
    }
}

/// How many bytes a UTF-8 character starting with this byte occupies.
const fn utf8_length(first: u8) -> usize {
    match first {
        0x00..=0x7f => 1,
        0xc0..=0xdf => 2,
        0xe0..=0xef => 3,
        _ => 4,
    }
}

fn refuse(what: impl Into<String>, because: impl Into<String>) -> Problem {
    Problem::new(what, because).with_remedy(
        "the message is not well-formed JSON; a transport that sends one document per line is what \
         this reads",
    )
}

/// Base64, for the one thing the protocol carries that is not text: an image.
///
/// The standard alphabet with padding, which is what the Model Context Protocol's image content
/// requires. Encoding only: nothing here ever has to read one back, and a decoder nobody calls is a
/// decoder nobody tests.
#[must_use]
pub fn base64(bytes: &[u8]) -> String {
    const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let triple = (u32::from(chunk[0]) << 16)
            | (chunk.get(1).map_or(0, |byte| u32::from(*byte)) << 8)
            | chunk.get(2).map_or(0, |byte| u32::from(*byte));
        out.push(ALPHABET[(triple >> 18) as usize & 63] as char);
        out.push(ALPHABET[(triple >> 12) as usize & 63] as char);
        out.push(if chunk.len() > 1 {
            ALPHABET[(triple >> 6) as usize & 63] as char
        } else {
            '='
        });
        out.push(if chunk.len() > 2 {
            ALPHABET[triple as usize & 63] as char
        } else {
            '='
        });
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_document_round_trips() {
        let text = r#"{"a":[1,2,3],"b":{"c":true,"d":null},"e":"hello"}"#;
        let value = parse(text).unwrap();
        assert_eq!(value.render(), text);
    }

    #[test]
    fn an_identifier_stays_an_integer() {
        // A peer that sent `"id": 3` and received `"id": 3.0` is entitled to say the reply matched
        // no request it made.
        assert_eq!(Json::Number(3.0).render(), "3");
        assert_eq!(Json::Number(-0.5).render(), "-0.5");
    }

    #[test]
    fn a_string_with_control_characters_and_quotes_survives() {
        let awkward = "he said \"no\"\n\tand \\ left\u{1}";
        let rendered = Json::text(awkward).render();
        assert_eq!(parse(&rendered).unwrap().as_text().unwrap(), awkward);
        assert!(rendered.contains("\\u0001"), "{rendered}");
    }

    #[test]
    fn a_surrogate_pair_becomes_the_character_it_names() {
        // The case that is always forgotten, and whose symptom looks like a font problem.
        let value = parse(r#""😀""#).unwrap();
        assert_eq!(value.as_text().unwrap(), "\u{1f600}");
    }

    #[test]
    fn a_multi_byte_character_survives_unescaped() {
        let value = parse("\"naïve — 日本語\"").unwrap();
        assert_eq!(value.as_text().unwrap(), "naïve — 日本語");
    }

    #[test]
    fn two_documents_on_one_line_are_refused_rather_than_half_read() {
        // Reading the first and ignoring the rest desynchronises the stream, and the failure that
        // follows names something else entirely.
        let refused = parse("{} {}").unwrap_err();
        assert!(refused.because.contains("more after the end"), "{refused}");
    }

    #[test]
    fn a_deeply_nested_document_is_refused_rather_than_overflowing_the_stack() {
        let deep = "[".repeat(MAXIMUM_DEPTH + 5) + &"]".repeat(MAXIMUM_DEPTH + 5);
        let refused = parse(&deep).unwrap_err();
        assert!(refused.because.contains("nests more than"), "{refused}");
    }

    #[test]
    fn base64_matches_the_standard_alphabet_including_its_padding() {
        assert_eq!(base64(b""), "");
        assert_eq!(base64(b"f"), "Zg==");
        assert_eq!(base64(b"fo"), "Zm8=");
        assert_eq!(base64(b"foo"), "Zm9v");
        assert_eq!(base64(b"foobar"), "Zm9vYmFy");
        assert_eq!(base64(&[0x89, b'P', b'N', b'G']), "iVBORw==");
    }

    #[test]
    fn a_missing_member_reads_as_null_rather_than_panicking() {
        let value = parse(r#"{"a":1}"#).unwrap();
        assert!(value.get("b").is_null());
        assert!(Json::Null.get("anything").is_null());
    }
}
