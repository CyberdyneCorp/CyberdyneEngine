//! The handshake, and how descriptors cross the process boundary.
//!
//! # What the socket carries, and what it deliberately does not
//!
//! One message, once, at connection: the [`Handshake`] and every file descriptor the editor needs.
//! After that the socket carries **nothing but liveness** — its EOF is how the editor learns the
//! runtime died. Per-frame state goes in the shared page ([`crate::announce`]) instead.
//!
//! That is not a micro-optimisation. A stream socket has no message boundaries, so a partial write
//! desynchronises the reader; a desynchronised reader takes the next bytes as a frame announcement
//! and stages a wait on a **garbage timeline value**, which is the one failure in this design that
//! nothing recovers from. A seqlock over one page cannot desynchronise: a torn read is detected and
//! retried, and the reader always sees the newest frame rather than a queue of stale ones.
//!
//! # The descriptors
//!
//! `SCM_RIGHTS` on a unix socket, all in the one message:
//!
//! | count | what |
//! |---|---|
//! | `buffer_count` | the ring's dma-buf descriptors, one per image, in slot order |
//! | 2 | the `render_done` and `release` timeline semaphores, exported as `OPAQUE_FD` |
//! | 1 | the `memfd` holding the shared announcement page |
//!
//! A descriptor NUMBER is meaningless in another process, which is why none of them appears in the
//! [`Handshake`] itself: the numbers in this struct describe the memory, and the kernel moves the
//! descriptors alongside it.

use std::os::fd::{AsRawFd, BorrowedFd, FromRawFd, OwnedFd, RawFd};

use cy_editor_core::problem::{Problem, Result};

/// The most images a ring may hold. Four, because four is where the spike's last stalls disappear
/// and a fifth image is 2.1 MB spent on nothing measurable.
pub const MAX_BUFFERS: usize = 4;

/// `CYVP`, so a connection to something that is not a viewport publisher fails at the first message
/// rather than as a nonsensical image size.
pub const PROTOCOL_MAGIC: u32 = 0x4359_5650;

/// The wire format's version. Bumped when this file's layout changes; a mismatch is refused with a
/// sentence naming both sides, because "the runtime is newer than the editor" is the common cause
/// and is fixed by rebuilding them together.
pub const PROTOCOL_VERSION: u32 = 1;

/// DRM fourcc for a Vulkan `R8G8B8A8_UNORM` image: `DRM_FORMAT_ABGR8888`.
pub const FOURCC_ABGR8888: u32 = fourcc(b'A', b'B', b'2', b'4');

const fn fourcc(a: u8, b: u8, c: u8, d: u8) -> u32 {
    (a as u32) | ((b as u32) << 8) | ((c as u32) << 16) | ((d as u32) << 24)
}

/// Where the editor looks for a publisher when nothing says otherwise.
///
/// `CY_VIEWPORT_SOCKET` overrides it, which is what lets two runs — a test's and a developer's —
/// share a machine without sharing a socket.
#[must_use]
pub fn default_socket_path() -> String {
    std::env::var("CY_VIEWPORT_SOCKET").unwrap_or_else(|_| "/tmp/cy-viewport.sock".to_string())
}

/// One image's memory layout, as the runtime's allocator reported it.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PlaneDescription {
    /// Bytes between the starts of two rows. Not `width * 4`: a modifier'd image is tiled, and the
    /// driver's row pitch is the only correct answer.
    pub stride: u64,
    /// Where the plane starts inside the allocation.
    pub offset: u64,
    /// **The allocation's size**, from `vkGetImageMemoryRequirements`, not `width * height * 4`.
    ///
    /// The two differ by whatever the modifier's tiling and alignment need, and the first spike
    /// that assumed they were equal imported an allocation the driver considered too small.
    pub allocation_bytes: u64,
}

/// Everything the editor needs to reconstruct the runtime's ring, sent once with the descriptors.
///
/// `#[repr(C)]` and integers only: it is written to the socket as bytes, and a layout that depended
/// on the compiler would be a protocol that depended on the compiler.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Handshake {
    /// [`PROTOCOL_MAGIC`].
    pub magic: u32,
    /// [`PROTOCOL_VERSION`].
    pub version: u32,
    /// The image's width in pixels.
    pub width: u32,
    /// The image's height in pixels.
    pub height: u32,
    /// The DRM fourcc of the pixel format, e.g. [`FOURCC_ABGR8888`].
    pub fourcc: u32,
    /// How many images the ring holds; how many dma-buf descriptors accompany this message.
    pub buffer_count: u32,
    /// Which generation of the ring this is.
    ///
    /// Bumped by the runtime whenever the ring is destroyed and rebuilt — a resize, a device loss,
    /// a reconnection. The editor refuses an announcement from a generation it did not hand-shake,
    /// which is what stops a frame arriving for an image that no longer exists.
    pub generation: u32,
    /// Non-zero when the runtime exported timeline semaphores; zero when it did not, which the
    /// editor treats as "show the newest image and synchronise nothing", never as an error.
    pub has_timelines: u32,
    /// The DRM format modifier the driver actually chose. `DRM_FORMAT_MOD_LINEAR` is **not**
    /// available on this hardware, so a transport that assumed linear would not run at all here.
    pub modifier: u64,
    /// One entry per image, in slot order. Entries past `buffer_count` are zero.
    pub planes: [PlaneDescription; MAX_BUFFERS],
}

impl Default for Handshake {
    fn default() -> Self {
        Self {
            magic: PROTOCOL_MAGIC,
            version: PROTOCOL_VERSION,
            width: 0,
            height: 0,
            fourcc: FOURCC_ABGR8888,
            buffer_count: 0,
            generation: 1,
            has_timelines: 1,
            modifier: 0,
            planes: [PlaneDescription::default(); MAX_BUFFERS],
        }
    }
}

impl Handshake {
    /// How many bytes the struct occupies on the wire.
    pub const BYTES: usize = std::mem::size_of::<Self>();

    /// The struct's bytes, for `sendmsg`.
    #[must_use]
    pub fn as_bytes(&self) -> &[u8] {
        // SAFETY: `Self` is `#[repr(C)]` and every field is an integer or an array of integers, so
        // it has no padding that carries meaning, no pointers and no `Drop`. Reading it as bytes is
        // exactly what the protocol says it is.
        unsafe { std::slice::from_raw_parts(std::ptr::from_ref(self).cast::<u8>(), Self::BYTES) }
    }

    /// Read a handshake a publisher sent, refusing one this build cannot honour.
    pub fn from_bytes(bytes: &[u8]) -> Result<Self> {
        if bytes.len() < Self::BYTES {
            return Err(Problem::new(
                "read a viewport handshake",
                format!(
                    "it is {} bytes; a handshake is {}",
                    bytes.len(),
                    Self::BYTES
                ),
            ));
        }
        let mut handshake = Self::default();
        // SAFETY: `Self` is `#[repr(C)]` and integers only, the destination is a live `Self`, and
        // the source has been checked to be at least as long. Every bit pattern is a valid value of
        // every field, so there is no invalid state to construct — the validation below is about
        // whether the values make sense, not whether they are readable.
        unsafe {
            std::ptr::copy_nonoverlapping(
                bytes.as_ptr(),
                std::ptr::from_mut(&mut handshake).cast::<u8>(),
                Self::BYTES,
            );
        }
        handshake.validate()?;
        Ok(handshake)
    }

    /// Refuse a handshake whose numbers cannot describe a ring.
    ///
    /// Each of these has a distinct cause, so each gets its own sentence rather than one
    /// "malformed handshake" that leaves a reader guessing which half is wrong.
    pub fn validate(self) -> Result<Self> {
        if self.magic != PROTOCOL_MAGIC {
            return Err(Problem::new(
                "read a viewport handshake",
                "the first four bytes are not this protocol's",
            )
            .with_remedy("check that CY_VIEWPORT_SOCKET names a viewport publisher"));
        }
        if self.version != PROTOCOL_VERSION {
            return Err(Problem::new(
                "read a viewport handshake",
                format!(
                    "it is version {}; this editor speaks version {PROTOCOL_VERSION}",
                    self.version
                ),
            )
            .with_remedy("rebuild the runtime and the editor together"));
        }
        let count = self.buffer_count as usize;
        if count == 0 || count > MAX_BUFFERS {
            return Err(Problem::new(
                "read a viewport handshake",
                format!("it announces {count} images; a ring holds 1 to {MAX_BUFFERS}"),
            ));
        }
        if self.width == 0 || self.height == 0 {
            return Err(Problem::new(
                "read a viewport handshake",
                format!("it announces a {}x{} image", self.width, self.height),
            ));
        }
        for (slot, plane) in self.planes.iter().take(count).enumerate() {
            if plane.stride < u64::from(self.width) || plane.allocation_bytes == 0 {
                return Err(Problem::new(
                    "read a viewport handshake",
                    format!(
                        "slot {slot} has stride {} and allocation {} bytes, which cannot hold a \
                         {}x{} image",
                        plane.stride, plane.allocation_bytes, self.width, self.height
                    ),
                ));
            }
        }
        Ok(self)
    }

    /// How many descriptors accompany a handshake of this shape: the ring, the two timelines when
    /// there are any, and the announcement page.
    #[must_use]
    pub fn expected_descriptors(&self) -> usize {
        self.buffer_count as usize + usize::from(self.has_timelines != 0) * 2 + 1
    }
}

/// Send a payload and a set of descriptors in one message.
///
/// # Errors
/// Whatever `sendmsg` reports, with the count that was attempted.
#[allow(
    clippy::cast_ptr_alignment,
    reason = "CMSG_DATA points at the kernel's control buffer, which the cmsg macros align for \
              exactly this; the descriptors are copied rather than dereferenced in place"
)]
pub fn send_descriptors(socket: RawFd, descriptors: &[RawFd], payload: &[u8]) -> Result<()> {
    const CONTROL_CAPACITY: usize = 256;
    let count = descriptors.len();
    if count == 0 || count > 16 {
        return Err(Problem::new(
            "send viewport descriptors",
            format!("{count} descriptors; the message carries 1 to 16"),
        ));
    }
    // SAFETY: `msghdr` is zeroed and then every field this call reads is set; `iov` and `control`
    // outlive the `sendmsg`; the control buffer is 256 bytes and holds at most 16 descriptors
    // (`CMSG_SPACE(64)` = 80). The pointer arithmetic is the libc `CMSG_*` macros, used as the
    // kernel documents them.
    let sent = unsafe {
        let mut iov = libc::iovec {
            iov_base: payload.as_ptr().cast::<libc::c_void>().cast_mut(),
            iov_len: payload.len(),
        };
        let mut control = [0_u8; CONTROL_CAPACITY];
        let byte_len =
            u32::try_from(std::mem::size_of_val(descriptors)).expect("at most sixteen descriptors");
        let mut message: libc::msghdr = std::mem::zeroed();
        message.msg_iov = &raw mut iov;
        message.msg_iovlen = 1;
        message.msg_control = control.as_mut_ptr().cast::<libc::c_void>();
        message.msg_controllen = libc::CMSG_SPACE(byte_len) as _;
        let header = libc::CMSG_FIRSTHDR(&raw const message);
        (*header).cmsg_level = libc::SOL_SOCKET;
        (*header).cmsg_type = libc::SCM_RIGHTS;
        (*header).cmsg_len = libc::CMSG_LEN(byte_len) as _;
        std::ptr::copy_nonoverlapping(
            descriptors.as_ptr(),
            libc::CMSG_DATA(header).cast::<RawFd>(),
            count,
        );
        libc::sendmsg(socket, &raw const message, 0)
    };
    if sent < 0 {
        return Err(Problem::new(
            "send viewport descriptors",
            std::io::Error::last_os_error().to_string(),
        ));
    }
    Ok(())
}

/// Receive a payload and the descriptors that came with it.
///
/// The descriptors are returned owned: dropping them closes them, which is what keeps a refused
/// handshake from leaking a ring's worth of dma-bufs.
///
/// # Errors
/// Whatever `recvmsg` reports.
#[allow(
    clippy::cast_ptr_alignment,
    reason = "CMSG_DATA points at the kernel's control buffer; the descriptors are read with \
              `read_unaligned`, which needs no alignment guarantee"
)]
pub fn receive_descriptors(socket: RawFd, buffer: &mut [u8]) -> Result<(usize, Vec<OwnedFd>)> {
    const CONTROL_CAPACITY: usize = 256;
    let mut owned = Vec::new();
    // SAFETY: as above — a zeroed `msghdr` with every field this call reads set, buffers that
    // outlive the call, and the libc `CMSG_*` macros used as documented. The descriptors the kernel
    // wrote into the control buffer are taken ownership of exactly once.
    let read = unsafe {
        let mut iov = libc::iovec {
            iov_base: buffer.as_mut_ptr().cast::<libc::c_void>(),
            iov_len: buffer.len(),
        };
        let mut control = [0_u8; CONTROL_CAPACITY];
        let mut message: libc::msghdr = std::mem::zeroed();
        message.msg_iov = &raw mut iov;
        message.msg_iovlen = 1;
        message.msg_control = control.as_mut_ptr().cast::<libc::c_void>();
        message.msg_controllen = CONTROL_CAPACITY as _;
        let read = libc::recvmsg(socket, &raw mut message, 0);
        if read < 0 {
            return Err(Problem::new(
                "receive viewport descriptors",
                std::io::Error::last_os_error().to_string(),
            ));
        }
        let header = libc::CMSG_FIRSTHDR(&raw const message);
        if !header.is_null() && (*header).cmsg_type == libc::SCM_RIGHTS {
            let payload_len = (*header).cmsg_len as usize - libc::CMSG_LEN(0) as usize;
            let count = payload_len / std::mem::size_of::<RawFd>();
            let data = libc::CMSG_DATA(header).cast::<RawFd>();
            for index in 0..count {
                owned.push(OwnedFd::from_raw_fd(data.add(index).read_unaligned()));
            }
        }
        read
    };
    Ok((usize::try_from(read).unwrap_or(0), owned))
}

/// Whether a descriptor is still open at the other end.
///
/// A closed socket is the only reliable "the runtime is gone" signal, and it is checked by reading
/// rather than by writing: a write to a dead socket raises `SIGPIPE`, and a publisher that died
/// while the editor was drawing must not take the editor with it.
#[must_use]
pub fn peer_closed(socket: BorrowedFd<'_>) -> bool {
    let mut byte = [0_u8; 1];
    // SAFETY: a non-blocking read of one byte into a live stack buffer on a descriptor the caller
    // owns for the duration of the borrow.
    let read = unsafe {
        libc::recv(
            socket.as_raw_fd(),
            byte.as_mut_ptr().cast::<libc::c_void>(),
            1,
            libc::MSG_DONTWAIT,
        )
    };
    read == 0
}

#[cfg(test)]
mod tests {
    use std::os::fd::AsFd;

    use super::*;

    fn a_handshake() -> Handshake {
        let mut handshake = Handshake {
            width: 1920,
            height: 1080,
            buffer_count: 3,
            generation: 7,
            modifier: 0x0300_0000_0000_0006,
            ..Handshake::default()
        };
        for plane in handshake.planes.iter_mut().take(3) {
            *plane = PlaneDescription {
                stride: 7680,
                offset: 0,
                allocation_bytes: 8_355_840,
            };
        }
        handshake
    }

    #[test]
    fn a_handshake_round_trips_as_bytes() {
        let original = a_handshake();
        let decoded = Handshake::from_bytes(original.as_bytes()).expect("our own bytes");
        assert_eq!(decoded, original);
        assert_eq!(
            decoded.expected_descriptors(),
            6,
            "three images, two timelines and the announcement page"
        );
    }

    #[test]
    fn an_allocation_smaller_than_the_image_is_refused_rather_than_imported() {
        // The failure the first spike hit: `width * height * 4` is not the allocation size, and an
        // import against the smaller number is rejected by the driver in a way that reads as a
        // driver defect rather than as a protocol defect.
        let mut handshake = a_handshake();
        handshake.planes[1].allocation_bytes = 0;
        let problem = handshake.validate().expect_err("refused");
        assert!(problem.to_string().contains("slot 1"), "{problem}");
    }

    #[test]
    fn a_publisher_from_another_protocol_is_refused_with_a_remedy() {
        let mut handshake = a_handshake();
        handshake.magic = 0xDEAD_BEEF;
        let problem = handshake.validate().expect_err("refused");
        assert!(problem.remedy.is_some(), "{problem}");

        let mut newer = a_handshake();
        newer.version = PROTOCOL_VERSION + 1;
        let problem = newer.validate().expect_err("refused");
        assert!(
            problem.to_string().contains("rebuild") || problem.remedy.is_some(),
            "{problem}"
        );
    }

    #[test]
    fn a_ring_of_zero_or_of_five_is_not_a_ring() {
        let mut none = a_handshake();
        none.buffer_count = 0;
        assert!(none.validate().is_err());
        let mut too_many = a_handshake();
        too_many.buffer_count = 5;
        assert!(too_many.validate().is_err());
    }

    #[test]
    fn descriptors_and_a_payload_cross_a_socket_together() {
        let (left, right) = std::os::unix::net::UnixStream::pair().expect("a socket pair");
        let page = crate::announce::AnnouncementPage::create().expect("a shared page");
        let handshake = a_handshake();
        send_descriptors(
            left.as_raw_fd(),
            &[page.descriptor().as_raw_fd()],
            handshake.as_bytes(),
        )
        .expect("sent");

        let mut buffer = vec![0_u8; 1024];
        let (read, descriptors) =
            receive_descriptors(right.as_raw_fd(), &mut buffer).expect("received");
        assert_eq!(read, Handshake::BYTES);
        assert_eq!(descriptors.len(), 1);
        assert_eq!(
            Handshake::from_bytes(&buffer).expect("the same handshake"),
            handshake
        );
    }

    #[test]
    fn the_editor_learns_the_runtime_died_from_the_sockets_eof() {
        let (left, right) = std::os::unix::net::UnixStream::pair().expect("a socket pair");
        assert!(!peer_closed(right.as_fd()), "both ends are open");
        drop(left);
        assert!(
            peer_closed(right.as_fd()),
            "EOF is how the editor learns the runtime is gone"
        );
    }
}
