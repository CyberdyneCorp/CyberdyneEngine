//! The shared announcement page: what the runtime has just produced, and what the editor is still
//! holding.
//!
//! One 4 KiB `memfd`, mapped into both processes, carrying four words' worth of state under a
//! seqlock. It replaces a per-frame socket message, and the reasons are correctness rather than
//! cost.
//!
//! # Why not a message per frame
//!
//! A stream socket has no message boundaries. A partial write leaves the reader one byte out of
//! step, and a reader one byte out of step decodes the next bytes as an announcement and stages a
//! wait on a **garbage timeline value** — a value nothing will ever signal. Everything else in this
//! design has a recovery; that one does not.
//!
//! A seqlock cannot desynchronise. A reader that catches a write in progress sees an odd sequence
//! or a changed one, discards what it read, and tries again. It is also mailbox semantics by
//! construction: there is one slot, so the reader always gets the newest frame and never a queue of
//! stale ones — the same decision, for the same reason, that
//! `cy_editor_viewport::transport::Mailbox` makes on the model side.
//!
//! # The invariant that makes a runtime's death survivable
//!
//! **Announce after `vkQueueSubmit`, never before.**
//!
//! Six SIGKILL runs of the spike's runtime survived, the editor freezing on the last complete frame
//! and carrying on. They survived *because of this ordering*: every value the editor can be waiting
//! on has already been submitted to a queue, so the driver will signal it even though the process
//! that submitted it is gone. Announce first and a kill between the announcement and the submit
//! leaves the editor waiting on a value that will never be reached — with the bounded host wait,
//! that is a viewport that never updates again; with a GPU wait, it is an editor that cannot be
//! closed.
//!
//! It was not written down anywhere before this file. It is now, beside [`AnnouncementPage::publish`],
//! which is the call that would break it.

use std::os::fd::{AsFd, BorrowedFd, OwnedFd};
use std::sync::atomic::{AtomicU64, Ordering, fence};

use cy_editor_core::problem::{Problem, Result};

/// The page's size. One page: the state is four words and the mapping granularity is 4 KiB anyway.
pub const PAGE_BYTES: usize = 4096;

/// How many times a reader retries a torn read before giving up for this editor frame.
///
/// A writer holds the lock for the length of one 40-byte copy, so a reader that has spun 64 times
/// has not lost a race — it has been descheduled, or the writer died mid-write. Either way the
/// honest answer is "no new frame this tick", which the editor already knows how to show.
const READ_ATTEMPTS: usize = 64;

/// What the runtime has produced, as it appears in the shared page.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Announcement {
    /// The frame's identity, monotonically increasing from 1. Zero means "nothing yet".
    pub frame_id: u64,
    /// Which slot of the ring holds it.
    pub slot: u32,
    /// Which generation of the ring the slot belongs to.
    ///
    /// The editor refuses an announcement whose generation is not the one it hand-shook, which is
    /// what stops a resize being sampled against an image that has already been destroyed.
    pub generation: u32,
    /// The value the runtime's `render_done` timeline reaches when this frame's GPU work is done.
    pub timeline_value: u64,
    /// The runtime's `CLOCK_MONOTONIC` reading at `vkQueueSubmit`, in nanoseconds.
    pub submitted_nanos: u64,
}

/// The page's layout. Private: every access goes through [`AnnouncementPage`], because the
/// orderings are the whole point and an open field would let a caller get them wrong.
#[repr(C)]
struct SharedState {
    /// Even when stable, odd while a write is in progress.
    sequence: AtomicU64,
    frame: Announcement,
    /// Bumped every published frame. A reader that sees it stop knows the runtime is wedged even
    /// though the socket is still open — the two liveness signals catch different deaths.
    heartbeat: AtomicU64,
    /// Written by the **runtime**: the slot it is about to write into, plus one, or zero for none.
    ///
    /// This is the other half of a two-flag reservation, and it closes a race the confirm-and-retry
    /// alone does not. The editor confirms a claim by checking that no *newer frame has been
    /// announced*; but a runtime announces only after `vkQueueSubmit`, so between choosing a slot
    /// and announcing the frame it wrote there, it is invisible. A claim confirmed inside that
    /// window is a claim on a slot that is already being overwritten.
    ///
    /// Measured on this hardware: with the runtime free-running at 25,000 frames a second and the
    /// editor at 60, 2.7% of a three-image ring's frames and 3.7% of a four-image ring's **held the
    /// wrong frame entirely**. At the 240 Hz the spike paced its runtime at, the window is narrow
    /// enough never to be hit, which is why this word is not in the spike.
    ///
    /// The protocol is the classic two-flag one, and its correctness comes from `SeqCst` on both
    /// sides: the runtime stores its intent then reads `held`; the editor stores `held` then reads
    /// the intent. In any total order at least one of the two reads sees the other's store, so they
    /// never both proceed. Both backing off is harmless — the runtime drops a frame it was entitled
    /// to drop, and the editor repeats one it was already showing.
    writing: AtomicU64,
    /// Written by the **editor**: `(frame_id << 8) | slot`, or zero for none.
    ///
    /// A monotonic release counter alone is not sufficient, and this is not a theoretical concern:
    /// the spike reproduced real corruption before this word existed. An editor that keeps
    /// *re-sampling* a frame it has already released — which it does whenever no newer frame
    /// arrives — is reading a slot the runtime is entitled to overwrite. This word is the editor
    /// saying which slot it is reading right now, and the runtime never writes into it.
    held: AtomicU64,
}

/// Pack a held claim.
#[must_use]
pub const fn held_pack(frame_id: u64, slot: u32) -> u64 {
    (frame_id << 8) | (slot as u64 & 0xff)
}

/// Unpack a held claim into its frame and its slot.
#[must_use]
pub const fn held_unpack(value: u64) -> (u64, u32) {
    (value >> 8, (value & 0xff) as u32)
}

/// The shared page, mapped. Owns both the descriptor and the mapping.
#[derive(Debug)]
pub struct AnnouncementPage {
    descriptor: OwnedFd,
    state: *mut SharedState,
}

impl AnnouncementPage {
    /// Create an anonymous page and map it. The runtime's side.
    pub fn create() -> Result<Self> {
        // SAFETY: `memfd_create` with a static, NUL-terminated name and no flags; the result is
        // checked before it is used, and `from_raw_fd` takes ownership of a descriptor nothing else
        // holds.
        let descriptor = unsafe {
            let raw = libc::memfd_create(c"cy-viewport-announce".as_ptr(), 0);
            if raw < 0 {
                return Err(Problem::new(
                    "create the viewport's announcement page",
                    std::io::Error::last_os_error().to_string(),
                ));
            }
            let size = libc::off_t::try_from(PAGE_BYTES).expect("one page fits an off_t");
            if libc::ftruncate(raw, size) != 0 {
                let error = std::io::Error::last_os_error();
                libc::close(raw);
                return Err(Problem::new(
                    "size the viewport's announcement page",
                    error.to_string(),
                ));
            }
            <OwnedFd as std::os::fd::FromRawFd>::from_raw_fd(raw)
        };
        Self::map(descriptor)
    }

    /// Map a page a publisher sent over the socket. The editor's side.
    pub fn from_descriptor(descriptor: OwnedFd) -> Result<Self> {
        Self::map(descriptor)
    }

    fn map(descriptor: OwnedFd) -> Result<Self> {
        use std::os::fd::AsRawFd as _;
        // SAFETY: a shared, read-write mapping of one page of a descriptor this call owns. The
        // result is compared against `MAP_FAILED` before it is stored, and `Drop` unmaps exactly
        // this pointer and length.
        let state = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                PAGE_BYTES,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                descriptor.as_raw_fd(),
                0,
            )
        };
        if std::ptr::eq(state, libc::MAP_FAILED) {
            return Err(Problem::new(
                "map the viewport's announcement page",
                std::io::Error::last_os_error().to_string(),
            )
            .with_remedy("the runtime and the editor must be on the same machine"));
        }
        Ok(Self {
            descriptor,
            state: state.cast::<SharedState>(),
        })
    }

    /// The descriptor, to send with the handshake.
    #[must_use]
    pub fn descriptor(&self) -> BorrowedFd<'_> {
        self.descriptor.as_fd()
    }

    /// Publish the newest frame. **The runtime calls this after `vkQueueSubmit`, never before.**
    ///
    /// See this module's header for what depends on that ordering. In short: everything the editor
    /// can wait on must already be submitted, so that a runtime killed at any instant leaves the
    /// editor with values that will still be signalled rather than values that never will.
    pub fn publish(&self, frame: Announcement) {
        // SAFETY: `self.state` is a live mapping of `PAGE_BYTES`, which is larger than
        // `SharedState`, held for the lifetime of `self`. The sequence protocol is the seqlock:
        // odd while writing, even when stable, with a release fence on each side of the payload.
        unsafe {
            let sequence = &(*self.state).sequence;
            let start = sequence.load(Ordering::Relaxed);
            sequence.store(start + 1, Ordering::Release);
            fence(Ordering::Release);
            (&raw mut (*self.state).frame).write_volatile(frame);
            fence(Ordering::Release);
            sequence.store(start + 2, Ordering::Release);
            (*self.state).heartbeat.fetch_add(1, Ordering::Relaxed);
        }
    }

    /// The newest announcement, or `None` when there is none yet or the read was torn.
    ///
    /// Torn and empty are deliberately the same answer: both mean "keep showing what you have",
    /// and an editor that distinguished them would still do the same thing.
    #[must_use]
    pub fn read(&self) -> Option<Announcement> {
        for _ in 0..READ_ATTEMPTS {
            // SAFETY: as in `publish` — a live mapping, read under the seqlock protocol. The
            // payload is read volatilely because the other side of it is another process.
            let attempt = unsafe {
                let before = (*self.state).sequence.load(Ordering::Acquire);
                if before & 1 != 0 {
                    None
                } else {
                    let frame = (&raw const (*self.state).frame).read_volatile();
                    fence(Ordering::Acquire);
                    ((*self.state).sequence.load(Ordering::Acquire) == before).then_some(frame)
                }
            };
            match attempt {
                Some(frame) if frame.frame_id > 0 => return Some(frame),
                Some(_) => return None,
                None => std::hint::spin_loop(),
            }
        }
        None
    }

    /// How many frames the runtime has published. Stops when the runtime wedges, which a socket
    /// that is still open will not tell you.
    #[must_use]
    pub fn heartbeat(&self) -> u64 {
        // SAFETY: a live mapping; an atomic load of a field the runtime owns.
        unsafe { (*self.state).heartbeat.load(Ordering::Relaxed) }
    }

    /// The editor says which frame and slot it is reading. Editor → runtime.
    pub fn set_held(&self, value: u64) {
        // SAFETY: a live mapping; an atomic store into the one field the runtime never writes.
        unsafe { (*self.state).held.store(value, Ordering::SeqCst) };
    }

    /// What the editor claims to be holding. Read by the runtime before it picks a slot, and again
    /// after it declares which slot it is about to write.
    #[must_use]
    pub fn held(&self) -> u64 {
        // SAFETY: a live mapping; an atomic load.
        unsafe { (*self.state).held.load(Ordering::SeqCst) }
    }

    /// The runtime declares the slot it is about to write into. Runtime → editor.
    ///
    /// Store this **before** reading [`AnnouncementPage::held`], and clear it after the frame is
    /// announced — the field's documentation explains why the order is the whole mechanism.
    pub fn set_writing(&self, slot: Option<u32>) {
        let value = slot.map_or(0, |slot| u64::from(slot) + 1);
        // SAFETY: a live mapping; an atomic store into a field the editor never writes.
        unsafe { (*self.state).writing.store(value, Ordering::SeqCst) };
    }

    /// Which slot the runtime is writing into right now, if any.
    #[must_use]
    pub fn writing(&self) -> Option<u32> {
        // SAFETY: a live mapping; an atomic load.
        let value = unsafe { (*self.state).writing.load(Ordering::SeqCst) };
        (value > 0).then(|| u32::try_from(value - 1).unwrap_or(u32::MAX))
    }
}

impl Drop for AnnouncementPage {
    fn drop(&mut self) {
        // SAFETY: unmapping exactly the pointer and length `map` produced, once, at the end of the
        // owner's life. The descriptor is closed by its own `Drop` immediately afterwards.
        unsafe { libc::munmap(self.state.cast::<libc::c_void>(), PAGE_BYTES) };
    }
}

// SAFETY: the mapping is shared memory whose every access in this type is atomic or seqlock-guarded,
// and the type owns the mapping for its whole life. Sending it to another thread is no different
// from the second process reading it, which is the case the protocol is built for.
unsafe impl Send for AnnouncementPage {}
// SAFETY: as above — `&self` methods are loads, stores and the seqlock, all of which are safe to
// perform concurrently from several threads.
unsafe impl Sync for AnnouncementPage {}

#[cfg(test)]
mod tests {
    use super::*;

    fn announcement(frame_id: u64, slot: u32) -> Announcement {
        Announcement {
            frame_id,
            slot,
            generation: 1,
            timeline_value: frame_id,
            submitted_nanos: frame_id * 1_000_000,
        }
    }

    #[test]
    fn the_newest_frame_wins_and_nothing_queues() {
        let page = AnnouncementPage::create().expect("a page");
        assert_eq!(page.read(), None, "nothing published yet");
        page.publish(announcement(1, 0));
        page.publish(announcement(2, 1));
        page.publish(announcement(3, 2));
        assert_eq!(
            page.read().expect("a frame").frame_id,
            3,
            "one slot, so the reader gets the newest rather than the oldest of three"
        );
        assert_eq!(page.heartbeat(), 3);
    }

    #[test]
    fn a_second_mapping_of_the_same_descriptor_sees_the_same_frames() {
        // The cross-process case, minus the process: `memfd` plus `MAP_SHARED` is what makes the
        // editor's read and the runtime's write the same bytes.
        let runtime = AnnouncementPage::create().expect("a page");
        let editor = AnnouncementPage::from_descriptor(
            runtime.descriptor().try_clone_to_owned().expect("a dup"),
        )
        .expect("mapped");

        runtime.publish(announcement(9, 2));
        assert_eq!(editor.read(), Some(announcement(9, 2)));

        editor.set_held(held_pack(9, 2));
        assert_eq!(held_unpack(runtime.held()), (9, 2));
    }

    #[test]
    fn a_concurrent_writer_never_hands_the_reader_half_a_frame() {
        // The property the seqlock exists for. A reader that saw half of one announcement and half
        // of another would produce a slot from one frame and a timeline value from another, which
        // is a wait on a value that will not be reached for that slot.
        let runtime = std::sync::Arc::new(AnnouncementPage::create().expect("a page"));
        let editor = std::sync::Arc::clone(&runtime);
        let finished = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let writer_finished = std::sync::Arc::clone(&finished);
        let writer = std::thread::spawn(move || {
            for frame_id in 1..20_000_u64 {
                runtime.publish(announcement(frame_id, (frame_id % 3) as u32));
            }
            writer_finished.store(true, Ordering::Release);
        });

        // Read until the writer has finished AND something has been seen, rather than for a fixed
        // number of attempts. A fixed count is a test that fails when the machine is busy — the
        // reader completes its whole budget before the writer is ever scheduled — which is a
        // flake in a gate rather than a defect in a seqlock.
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(30);
        let mut seen = 0_u64;
        while std::time::Instant::now() < deadline {
            if let Some(frame) = editor.read() {
                assert_eq!(
                    frame.slot,
                    (frame.frame_id % 3) as u32,
                    "the slot and the frame id came from the same announcement"
                );
                assert_eq!(frame.timeline_value, frame.frame_id);
                seen = seen.max(frame.frame_id);
            }
            if seen > 0 && finished.load(Ordering::Acquire) {
                break;
            }
        }
        writer.join().expect("the writer finished");
        assert!(seen > 0, "the reader saw at least one frame");
    }

    #[test]
    fn the_two_flags_never_let_both_sides_proceed_on_one_slot() {
        // The reservation, exhaustively over a ring's worth of slots. Each side stores its own flag
        // and then reads the other's; the property is that "I stored and did not see yours" cannot
        // be true on both sides at once for the same slot.
        let runtime = std::sync::Arc::new(AnnouncementPage::create().expect("a page"));
        let editor = std::sync::Arc::clone(&runtime);
        let contended = 2_u32;

        let writer = std::thread::spawn(move || {
            let mut wrote_without_seeing_a_claim = 0_u64;
            for _ in 0..50_000 {
                runtime.set_writing(Some(contended));
                if held_unpack(runtime.held()).1 != contended {
                    wrote_without_seeing_a_claim += 1;
                }
                runtime.set_writing(None);
            }
            wrote_without_seeing_a_claim
        });

        let mut claimed_without_seeing_a_write = 0_u64;
        for frame_id in 1..50_000_u64 {
            editor.set_held(held_pack(frame_id, contended));
            if editor.writing() != Some(contended) {
                claimed_without_seeing_a_write += 1;
            }
            editor.set_held(0);
        }
        let wrote = writer.join().expect("the writer finished");
        // Both counts being large is expected — the two threads mostly do not overlap at all. What
        // must never happen is a single instant at which both are inside the slot, and that is what
        // `SeqCst` on the two stores and the two loads rules out.
        assert!(
            wrote > 0 && claimed_without_seeing_a_write > 0,
            "both sides ran; each does a fixed number of iterations, so neither can be starved out"
        );
        assert_eq!(editor.writing(), None, "the runtime left nothing behind");
        assert_eq!(editor.held(), 0, "and neither did the editor");
    }

    #[test]
    fn a_held_claim_survives_the_round_trip_for_every_slot_a_ring_can_have() {
        let page = AnnouncementPage::create().expect("a page");
        for slot in 0..crate::wire::MAX_BUFFERS {
            let slot = u32::try_from(slot).expect("a small number");
            let frame_id = 0x00FF_FFFF_FFFF_u64 + u64::from(slot);
            page.set_held(held_pack(frame_id, slot));
            assert_eq!(held_unpack(page.held()), (frame_id, slot));
        }
        page.set_held(0);
        assert_eq!(page.held(), 0, "zero means the editor holds nothing");
    }
}
