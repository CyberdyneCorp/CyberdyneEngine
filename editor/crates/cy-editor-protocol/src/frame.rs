//! Framing: how one message is found in a byte stream.
//!
//! A four-byte length, a four-byte checksum, then the payload. The same shape the journal uses, and
//! for the same reason: a stream that stops mid-message must be *detected* rather than decoded, and
//! a length that can be read before the payload is what makes that possible.
//!
//! The checksum is not for corruption over a Unix domain socket, where there is none. It is for the
//! case a transport is later a network or a pipe shared with something else, and for the case a
//! runtime crashed mid-write — which is exactly the scenario `editor-rust-application` requires the
//! editor to survive, and the one this milestone's artefact tests.

use std::io::{Read, Write};

use cy_editor_core::problem::{Problem, Result};

/// The identifier a request and its echo share.
///
/// The spike's finding 2 in one type: a gizmo is drawn from locally predicted state and reconciled
/// against the runtime's authoritative echo, keyed by this. `editor-viewport-and-gizmos` already
/// requires the viewport transport to carry the frame's view state and identifiers, so this is that
/// identifier rather than a second one invented here.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug, Default)]
pub struct FrameId(u64);

impl FrameId {
    /// The frame with this number.
    #[must_use]
    pub const fn from_raw(raw: u64) -> Self {
        Self(raw)
    }

    /// The number.
    #[must_use]
    pub const fn as_u64(self) -> u64 {
        self.0
    }

    /// The next frame.
    #[must_use]
    pub const fn next(self) -> Self {
        Self(self.0 + 1)
    }
}

/// The largest message this protocol accepts, in bytes.
///
/// A bound rather than a limit anyone should reach: control messages are tens of bytes. It exists so
/// that a length field read from a stream that is *not* this protocol cannot make the reader
/// allocate four gigabytes before discovering that.
pub const MAX_FRAME_BYTES: usize = 16 * 1024 * 1024;

/// Write one framed message.
pub fn write_frame(writer: &mut impl Write, payload: &[u8]) -> Result<()> {
    if payload.len() > MAX_FRAME_BYTES {
        return Err(Problem::new(
            "send a message",
            format!(
                "it is {} bytes and the limit is {MAX_FRAME_BYTES}",
                payload.len()
            ),
        ));
    }
    let length = u32::try_from(payload.len()).expect("checked against MAX_FRAME_BYTES above");
    let mut header = Vec::with_capacity(8 + payload.len());
    header.extend_from_slice(&length.to_le_bytes());
    header.extend_from_slice(&checksum(payload).to_le_bytes());
    header.extend_from_slice(payload);
    writer
        .write_all(&header)
        .and_then(|()| writer.flush())
        .map_err(|error| Problem::new("send a message", error.to_string()))
}

/// Read one framed message, or `None` when the peer closed the connection cleanly.
///
/// A clean close and a crash look the same from here — the stream ends — and the difference is a
/// question for the session, which knows whether it had asked the runtime to stop.
pub fn read_frame(reader: &mut impl Read) -> Result<Option<Vec<u8>>> {
    let mut header = [0_u8; 8];
    if !read_exact_or_eof(reader, &mut header)? {
        return Ok(None);
    }
    let length = u32::from_le_bytes(header[..4].try_into().expect("four bytes")) as usize;
    let expected = u32::from_le_bytes(header[4..].try_into().expect("four bytes"));
    if length > MAX_FRAME_BYTES {
        return Err(Problem::new(
            "read a message",
            format!("its declared length is {length}, past the {MAX_FRAME_BYTES} limit"),
        )
        .with_remedy("the peer is not speaking this protocol, or the stream is out of step"));
    }
    let mut payload = vec![0_u8; length];
    if !read_exact_or_eof(reader, &mut payload)? {
        return Err(Problem::new(
            "read a message",
            "the connection ended part way through a message",
        )
        .with_remedy("the peer stopped mid-write; treat the session as lost"));
    }
    if checksum(&payload) != expected {
        return Err(
            Problem::new("read a message", "its checksum does not match")
                .with_remedy("the stream is out of step; the session cannot continue"),
        );
    }
    Ok(Some(payload))
}

/// Fill `buffer`, reporting whether it was filled.
///
/// `Ok(false)` is an end of stream — clean when nothing had been read, torn when something had. The
/// caller distinguishes the two, because only the caller knows whether a header or a payload was
/// being read and therefore which of the two it is.
fn read_exact_or_eof(reader: &mut impl Read, buffer: &mut [u8]) -> Result<bool> {
    let mut filled = 0;
    while filled < buffer.len() {
        match reader.read(&mut buffer[filled..]) {
            Ok(0) => return Ok(false),
            Ok(count) => filled += count,
            Err(error) if error.kind() == std::io::ErrorKind::Interrupted => {}
            Err(error) => return Err(Problem::new("read from the connection", error.to_string())),
        }
    }
    Ok(true)
}

/// FNV-1a over 32 bits. See the module note for what it is and is not for.
fn checksum(bytes: &[u8]) -> u32 {
    let mut hash = 0x811c_9dc5_u32;
    for byte in bytes {
        hash ^= u32::from(*byte);
        hash = hash.wrapping_mul(0x0100_0193);
    }
    hash
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_frame_round_trips() {
        let mut buffer = Vec::new();
        write_frame(&mut buffer, b"hello").unwrap();
        write_frame(&mut buffer, b"world").unwrap();

        let mut reader = buffer.as_slice();
        assert_eq!(read_frame(&mut reader).unwrap().unwrap(), b"hello");
        assert_eq!(read_frame(&mut reader).unwrap().unwrap(), b"world");
        assert_eq!(
            read_frame(&mut reader).unwrap(),
            None,
            "a clean end of stream"
        );
    }

    #[test]
    fn a_stream_that_stops_mid_message_is_detected() {
        let mut buffer = Vec::new();
        write_frame(&mut buffer, b"a message long enough to truncate").unwrap();
        buffer.truncate(buffer.len() - 5);

        let mut reader = buffer.as_slice();
        let problem = read_frame(&mut reader).unwrap_err();
        assert!(
            problem
                .remedy
                .as_deref()
                .unwrap()
                .contains("session as lost"),
            "{problem}"
        );
    }

    #[test]
    fn a_corrupted_payload_is_refused_rather_than_decoded() {
        let mut buffer = Vec::new();
        write_frame(&mut buffer, b"payload").unwrap();
        let last = buffer.len() - 1;
        buffer[last] ^= 0xff;

        let mut reader = buffer.as_slice();
        assert!(read_frame(&mut reader).is_err());
    }

    #[test]
    fn an_absurd_length_is_refused_before_it_is_allocated() {
        let mut buffer = Vec::new();
        buffer.extend_from_slice(&u32::MAX.to_le_bytes());
        buffer.extend_from_slice(&0_u32.to_le_bytes());

        let mut reader = buffer.as_slice();
        let problem = read_frame(&mut reader).unwrap_err();
        assert!(problem.because.contains("past the"), "{problem}");
    }
}
