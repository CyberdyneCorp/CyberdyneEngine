//! The editor's session over one runtime's ring: what to show this frame, and how to be sure it is
//! safe to read.
//!
//! # The wait policy, and the variant that is deliberately absent
//!
//! There is no "wait on the editor's queue" here, and its absence is the design.
//!
//! `wgpu_hal`'s `Queue::add_wait_semaphore` will happily stage a wait on an imported timeline. It
//! is also **one bad value away from an editor that renders nothing and cannot be closed**, which
//! the spike measured rather than guessed: an unsatisfiable wait returns from `submit` in 0.19 ms
//! because the wait is on the GPU, every later *independent* submission then times out, and
//! shutdown hangs forever inside `vkDeviceWaitIdle`. `editor-rust-application` requires that a
//! runtime failure not terminate the editor; a wedged queue is worse than termination, because the
//! window is still there and does nothing.
//!
//! [`WaitPolicy::HostWait`] — `vkWaitSemaphores` with a 2 ms timeout, on the editor's own thread —
//! showed 97% of the newest frames at the same latency as the GPU wait's 100%. Three frames in 360
//! for an editor that cannot be wedged is not a close call, and making the fast-and-fatal variant
//! unrepresentable is what stops it being reintroduced by somebody optimising a frame time.
//!
//! # Two liveness signals, because processes die in two different ways
//!
//! The socket's EOF catches a process that is gone. The heartbeat in the shared page catches one
//! that is still running and no longer producing — a hung GPU, a deadlock, a debugger. The editor
//! shows the last complete frame and says which of the two happened.

use std::os::fd::{AsFd, AsRawFd};
use std::sync::Arc;
use std::time::{Duration, Instant};

use ash::vk;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_protocol::FrameId;
use cy_editor_viewport::state::ViewState;
use cy_editor_viewport::transport::{
    FrameImage, PresentedFrame, SharedImage, Transport, TransportKind,
};

use crate::announce::{Announcement, AnnouncementPage, held_pack};
use crate::device::Gpu;
use crate::image::{import_ring, import_timeline};
use crate::wire::{Handshake, default_socket_path, peer_closed, receive_descriptors};

/// The default bounded wait: 2 ms, measured as the point where a longer wait buys no more frames.
pub const DEFAULT_HOST_WAIT: Duration = Duration::from_millis(2);

/// How long the heartbeat may stand still before the editor calls the runtime wedged.
pub const HEARTBEAT_PATIENCE: Duration = Duration::from_millis(500);

/// How long a session waits, at teardown, for the submissions that name its imported semaphores.
///
/// Bounded like every other wait here: a runtime that died mid-frame can leave work that never
/// completes, and an editor that will not close is worse than one that closes untidily.
pub const TEARDOWN_PATIENCE: Duration = Duration::from_millis(250);

/// How many times the editor will re-aim its claim at a newer frame before repeating the one it
/// has.
///
/// Eight, because each attempt costs a store and a read of the shared page, and a runtime fast
/// enough to win eight in a row is fast enough that the frame we would settle on is stale by the
/// time we draw it.
pub const CLAIM_ATTEMPTS: usize = 8;

/// Which adapter the editor should open, from `CY_VIEWPORT_ADAPTER`.
///
/// An empty string means "the first one". Importing across two physical devices does not work, and
/// the driver's refusal does not say so, which is why a machine with two GPUs gets a way to say
/// which one the runtime is on.
#[must_use]
pub fn preferred_adapter() -> String {
    std::env::var("CY_VIEWPORT_ADAPTER").unwrap_or_default()
}

/// `CLOCK_MONOTONIC` in microseconds — the same clock the publisher stamps frames with.
///
/// The frame ages `cy_editor_viewport::transport::FrameStream` reports are differences between a
/// runtime's reading and an editor's, so they must be readings of the same clock. An `Instant`
/// origin per process would give two zeroes in two places and ages that mean nothing.
#[must_use]
pub fn monotonic_micros() -> u64 {
    monotonic_nanos() / 1_000
}

/// `CLOCK_MONOTONIC` in nanoseconds.
#[must_use]
pub fn monotonic_nanos() -> u64 {
    let mut time = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    // SAFETY: `clock_gettime` writes into a live, correctly typed stack value.
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &raw mut time) };
    let seconds = u64::try_from(time.tv_sec).unwrap_or(0);
    let nanos = u64::try_from(time.tv_nsec).unwrap_or(0);
    seconds * 1_000_000_000 + nanos
}

/// How the editor makes an image safe to read.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WaitPolicy {
    /// Show whatever the newest announcement names, synchronising nothing.
    ///
    /// What a runtime that exported no timelines gets, and the negative control the spike measured
    /// 99.9% corruption with. Never chosen while timelines are available.
    Newest,
    /// Ask whether the frame's value has already been reached; if it has not, keep the previous
    /// frame. Never blocks at all.
    HostPoll,
    /// Ask, and if it has not, wait on the editor's own thread for at most this long.
    ///
    /// The default, and the only policy that both shows the newest frame and cannot wedge anything.
    HostWait(Duration),
}

impl Default for WaitPolicy {
    fn default() -> Self {
        Self::HostWait(DEFAULT_HOST_WAIT)
    }
}

/// Why the editor is not showing a new frame.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Liveness {
    /// The runtime is producing frames.
    #[default]
    Live,
    /// The runtime's process is gone: the socket returned EOF. The last complete frame stands.
    Gone,
    /// The runtime is running and has not produced a frame for [`HEARTBEAT_PATIENCE`].
    Wedged,
}

impl Liveness {
    /// What the editor tells the user, or empty when there is nothing to say.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Liveness::Live => "",
            Liveness::Gone => {
                "The runtime is no longer running. This is the last frame it produced."
            }
            Liveness::Wedged => {
                "The runtime is running but has stopped producing frames. This image is stale."
            }
        }
    }
}

/// What the session has had to do, for a report and for a test.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SessionCounters {
    /// Announcements the editor saw.
    pub announced: u64,
    /// Announced frames the editor never showed, because a newer one arrived first.
    pub skipped: u64,
    /// Times the newest frame's GPU work was not finished when the editor looked.
    pub not_ready: u64,
    /// Times the bounded host wait expired and the previous frame stood.
    pub timed_out: u64,
    /// Times a claim had to be retried because the runtime moved on while it was being made.
    pub claim_retries: u64,
    /// Announcements refused because they named a generation of the ring that no longer exists.
    pub wrong_generation: u64,
    /// Announcements refused because they named a slot outside the ring.
    pub malformed: u64,
    /// Times the editor stood aside because the runtime was already writing the slot it wanted.
    pub claim_vetoed: u64,
    /// Editor frames that repeated their image because a claim could not be confirmed.
    ///
    /// Non-zero means the runtime is producing far faster than the editor consumes — thousands of
    /// frames a second against sixty. It is a rate to watch rather than a fault: the alternative to
    /// repeating a frame here is showing one the runtime is overwriting.
    pub claims_abandoned: u64,
}

/// One connection to one runtime's viewport ring.
pub struct ViewportSession {
    gpu: Arc<Gpu>,
    handshake: Handshake,
    textures: Vec<wgpu::Texture>,
    views: Vec<wgpu::TextureView>,
    render_done: Option<vk::Semaphore>,
    release: Option<vk::Semaphore>,
    stream: std::os::unix::net::UnixStream,
    page: AnnouncementPage,
    policy: WaitPolicy,
    view_state: ViewState,

    latest: Option<Announcement>,
    shown: Option<Announcement>,
    holding: Option<Announcement>,
    finished_with: Option<Announcement>,
    signalled_release: u64,

    liveness: Liveness,
    heartbeat: u64,
    heartbeat_at: Instant,
    counters: SessionCounters,
    /// How long the import took, which is the one cost a session pays that a frame does not.
    pub import_millis: f64,
}

impl ViewportSession {
    /// Connect to the publisher on the default socket.
    pub fn connect(gpu: Arc<Gpu>) -> Result<Self> {
        Self::connect_to(gpu, &default_socket_path())
    }

    /// Connect to a publisher at a named socket, import its ring, and be ready to show frames.
    pub fn connect_to(gpu: Arc<Gpu>, path: &str) -> Result<Self> {
        let stream = std::os::unix::net::UnixStream::connect(path).map_err(|error| {
            Problem::new(
                "connect to the runtime's viewport",
                format!("{path}: {error}"),
            )
            .with_remedy("start the runtime, or set CY_VIEWPORT_SOCKET to its socket")
        })?;
        Self::adopt(gpu, stream)
    }

    /// Take over a socket somebody else opened. The path a test and a spawned runtime share.
    pub fn adopt(gpu: Arc<Gpu>, stream: std::os::unix::net::UnixStream) -> Result<Self> {
        let started = Instant::now();
        let mut buffer = vec![0_u8; 1024];
        let (read, descriptors) = receive_descriptors(stream.as_raw_fd(), &mut buffer)?;
        if read < Handshake::BYTES {
            return Err(Problem::new(
                "read the runtime's viewport handshake",
                format!("{read} bytes arrived; a handshake is {}", Handshake::BYTES),
            ));
        }
        let handshake = Handshake::from_bytes(&buffer)?;
        let expected = handshake.expected_descriptors();
        if descriptors.len() != expected {
            return Err(Problem::new(
                "read the runtime's viewport handshake",
                format!(
                    "it describes {expected} descriptors and {} arrived",
                    descriptors.len()
                ),
            ));
        }

        let mut descriptors = descriptors;
        let page = AnnouncementPage::from_descriptor(
            descriptors.pop().expect("the announcement page is last"),
        )?;
        let (render_done, release) = if handshake.has_timelines != 0 {
            let release = import_timeline(&gpu, descriptors.pop().expect("the release timeline"))?;
            let render_done =
                import_timeline(&gpu, descriptors.pop().expect("the render-done timeline"))?;
            (Some(render_done), Some(release))
        } else {
            (None, None)
        };
        let textures = import_ring(&gpu, &handshake, descriptors)?;
        let views = textures
            .iter()
            .map(|texture| texture.create_view(&wgpu::TextureViewDescriptor::default()))
            .collect();

        stream
            .set_nonblocking(true)
            .map_err(|error| Problem::new("watch the runtime's socket", error.to_string()))?;

        Ok(Self {
            gpu,
            handshake,
            textures,
            views,
            render_done,
            release,
            stream,
            page,
            policy: if render_done.is_some() {
                WaitPolicy::default()
            } else {
                WaitPolicy::Newest
            },
            view_state: ViewState::new(),
            latest: None,
            shown: None,
            holding: None,
            finished_with: None,
            signalled_release: 0,
            liveness: Liveness::Live,
            heartbeat: 0,
            heartbeat_at: Instant::now(),
            counters: SessionCounters::default(),
            import_millis: started.elapsed().as_secs_f64() * 1e3,
        })
    }

    /// What the runtime said about its ring.
    #[must_use]
    pub fn handshake(&self) -> &Handshake {
        &self.handshake
    }

    /// The imported image in a slot.
    #[must_use]
    pub fn texture(&self, slot: usize) -> Option<&wgpu::Texture> {
        self.textures.get(slot)
    }

    /// A view of the imported image in a slot, which is what a render pass samples.
    #[must_use]
    pub fn view(&self, slot: usize) -> Option<&wgpu::TextureView> {
        self.views.get(slot)
    }

    /// The device the images were imported into.
    #[must_use]
    pub fn gpu(&self) -> &Arc<Gpu> {
        &self.gpu
    }

    /// The policy in force.
    #[must_use]
    pub fn policy(&self) -> WaitPolicy {
        self.policy
    }

    /// Choose a different policy. Refuses to synchronise against timelines that do not exist.
    pub fn set_policy(&mut self, policy: WaitPolicy) -> Result<()> {
        if self.render_done.is_none() && policy != WaitPolicy::Newest {
            return Err(Problem::new(
                "set the viewport's wait policy",
                "this runtime exported no timeline semaphores, so there is nothing to wait on",
            ));
        }
        self.policy = policy;
        Ok(())
    }

    /// The view state frames from this session are reported with.
    ///
    /// The editor sets what it asked the runtime to render. When the engine's render server grows
    /// a publisher of its own it will echo the state it actually rendered, and this becomes that
    /// echo — the requirement that a frame carry its own view state is about the click that lands
    /// two frames later, and only the runtime can answer it honestly.
    pub fn set_view_state(&mut self, state: ViewState) {
        self.view_state = state;
    }

    /// Whether the runtime is alive, gone, or wedged.
    #[must_use]
    pub fn liveness(&self) -> Liveness {
        self.liveness
    }

    /// What the session has had to do.
    #[must_use]
    pub fn counters(&self) -> SessionCounters {
        self.counters
    }

    /// The frame the editor is currently showing.
    #[must_use]
    pub fn shown(&self) -> Option<Announcement> {
        self.shown
    }

    /// Read the socket and the shared page: is the runtime alive, and is there a newer frame?
    ///
    /// Never blocks. There is no `recv`, no timeout and no wait anywhere in this method, for the
    /// same reason `cy_editor_viewport::transport::Transport::poll` has none: a method that could
    /// block would be blocked on, on some machine, and the symptom would be a frozen editor blamed
    /// on the renderer.
    pub fn poll(&mut self) {
        if self.liveness != Liveness::Gone && peer_closed(self.stream.as_fd()) {
            self.liveness = Liveness::Gone;
        }
        let heartbeat = self.page.heartbeat();
        if heartbeat != self.heartbeat {
            self.heartbeat = heartbeat;
            self.heartbeat_at = Instant::now();
            if self.liveness == Liveness::Wedged {
                self.liveness = Liveness::Live;
            }
        } else if self.liveness == Liveness::Live
            && self.heartbeat_at.elapsed() > HEARTBEAT_PATIENCE
        {
            self.liveness = Liveness::Wedged;
        }

        let Some(frame) = self.page.read() else {
            return;
        };
        if frame.generation != self.handshake.generation {
            // A frame for a ring that no longer exists. Sampling it would read an image that has
            // been destroyed, which is the failure the generation exists to prevent.
            self.counters.wrong_generation += 1;
            return;
        }
        if frame.slot >= self.handshake.buffer_count {
            // A slot outside the ring the handshake described. This editor will not have a publisher
            // it trusts — the engine's render server publishes over this same wire format, and a
            // defect there must be a counter here rather than an index into a Vec.
            self.counters.malformed += 1;
            return;
        }
        if self
            .latest
            .is_none_or(|previous| frame.frame_id > previous.frame_id)
        {
            if self
                .latest
                .is_some_and(|previous| Some(previous.frame_id) != self.shown.map(|s| s.frame_id))
            {
                self.counters.skipped += 1;
            }
            self.counters.announced += 1;
            self.latest = Some(frame);
        }
    }

    /// Decide which image to sample this editor frame, and make it safe to sample.
    ///
    /// Returns the slot and the frame it holds, or `None` before the first frame has arrived.
    /// Whatever it returns stays valid until the next call: the claim in the shared page is what
    /// keeps the runtime out of that slot.
    pub fn acquire(&mut self) -> Option<(usize, Announcement)> {
        self.poll();
        let Some(semaphore) = self.render_done else {
            let frame = self.latest.or(self.shown)?;
            self.shown = Some(frame);
            return Some((frame.slot as usize, frame));
        };
        let fresh = self
            .latest
            .filter(|frame| Some(frame.frame_id) != self.shown.map(|shown| shown.frame_id));
        let Some(frame) = fresh else {
            // Nothing new. Keep showing what we hold — and keep holding it, because a slot we let
            // go of is a slot the runtime may overwrite while we are still sampling it.
            return self.shown.map(|shown| (shown.slot as usize, shown));
        };

        if !self.await_frame(semaphore, frame) {
            return self.shown.map(|shown| (shown.slot as usize, shown));
        }
        let Some(frame) = self.claim(frame) else {
            // The claim never held. Keep what we have rather than sample a slot the runtime may
            // overwrite while we read it.
            return self.shown.map(|shown| (shown.slot as usize, shown));
        };
        // We are switching away from whatever we held, so we are finished with it — and the release
        // is signalled on the next submit rather than now, which is what makes it a promise the GPU
        // keeps rather than one the CPU makes early.
        self.finished_with = self.holding;
        self.holding = Some(frame);
        self.shown = Some(frame);
        Some((frame.slot as usize, frame))
    }

    /// Wait, as far as the policy allows, for a frame's GPU work to be finished. `false` means
    /// "not yet; keep the previous frame".
    fn await_frame(&mut self, semaphore: vk::Semaphore, frame: Announcement) -> bool {
        match self.policy {
            WaitPolicy::Newest => true,
            WaitPolicy::HostPoll | WaitPolicy::HostWait(_) => {
                if self.gpu.timeline_value(semaphore) >= frame.timeline_value {
                    return true;
                }
                self.counters.not_ready += 1;
                let WaitPolicy::HostWait(timeout) = self.policy else {
                    return false;
                };
                let nanos = u64::try_from(timeout.as_nanos()).unwrap_or(u64::MAX);
                if self
                    .gpu
                    .wait_timeline(semaphore, frame.timeline_value, nanos)
                {
                    return true;
                }
                self.counters.timed_out += 1;
                false
            }
        }
    }

    /// Publish a claim on a frame's slot, and check that the runtime has not moved on since.
    ///
    /// The confirm-and-retry is what makes the claim safe rather than merely polite: between
    /// reading the announcement and writing the claim, the runtime may already have chosen the same
    /// slot for its next frame. If it has moved on, its choice was made without seeing our claim,
    /// so we take the newer frame instead of arguing about the old one.
    ///
    /// # Giving up is the safe answer
    ///
    /// `None` after [`CLAIM_ATTEMPTS`] tries, which means "show the frame you already have".
    ///
    /// The spike's version returned the newest *unconfirmed* announcement instead, which is a claim
    /// that was never made: the editor would sample a slot the runtime had not agreed to leave
    /// alone. Measurement says this path is not, in fact, where the wrong frames came from —
    /// `claims_abandoned` reads zero in every run, including a runtime free-running at 25,000
    /// frames a second, because there is always a newer frame to re-aim at. It stays a refusal
    /// rather than a fallback because repeating a frame costs one frame of staleness on a viewport
    /// showing hundreds a second, and sampling an unclaimed slot costs correctness.
    ///
    /// What the wrong frames actually came from is the window this function's *other* check closes:
    /// see the veto below, and `announce.rs`'s `writing` field.
    fn claim(&mut self, frame: Announcement) -> Option<Announcement> {
        let mut candidate = frame;
        for _ in 0..CLAIM_ATTEMPTS {
            self.page
                .set_held(held_pack(candidate.frame_id, candidate.slot));
            // The other half of the reservation: our claim is published, so now look at what the
            // runtime says it is writing. Store-then-load on both sides, both `SeqCst`, is what
            // makes "we both proceeded" impossible rather than unlikely.
            if self.page.writing() == Some(candidate.slot) {
                self.counters.claim_vetoed += 1;
                self.poll();
                candidate = self.latest.unwrap_or(candidate);
                continue;
            }
            self.poll();
            match self.latest {
                Some(newest) if newest.frame_id == candidate.frame_id => return Some(candidate),
                Some(newest) => {
                    self.counters.claim_retries += 1;
                    candidate = newest;
                }
                None => return Some(candidate),
            }
        }
        self.counters.claims_abandoned += 1;
        self.restore_claim();
        None
    }

    /// Put the claim back on the frame the editor is still showing.
    ///
    /// A failed claim must not leave the shared page naming a slot the editor is not reading — and,
    /// more importantly, must not leave it naming *nothing*, because the frame still on screen is
    /// still being sampled every editor frame and the runtime would be entitled to overwrite it.
    fn restore_claim(&self) {
        let held = self
            .holding
            .map_or(0, |frame| held_pack(frame.frame_id, frame.slot));
        self.page.set_held(held);
    }

    /// Tell the runtime, on the GPU timeline, that the editor has finished with the frame it
    /// stopped showing. Call this immediately before the submit that samples the new one.
    ///
    /// Staged on the queue, so it lands on the next submit — which, because wgpu chains submissions
    /// with relay semaphores, is never earlier than the submit that did the sampling. Signalling
    /// from the CPU instead would tell the runtime the image was free while the editor's own read
    /// of it was still queued.
    pub fn release_finished(&mut self) {
        let (Some(semaphore), Some(frame)) = (self.release, self.finished_with.take()) else {
            return;
        };
        if frame.frame_id <= self.signalled_release {
            return;
        }
        // SAFETY: the queue is the Vulkan queue of the device the semaphore was imported into, and
        // the semaphore outlives the session. `as_hal` yields `None` on any other backend, in which
        // case nothing is staged and the runtime reclaims the slot by other means.
        if let Some(queue) = unsafe { self.gpu.queue.as_hal::<wgpu_hal::api::Vulkan>() } {
            queue.add_signal_semaphore(semaphore, Some(frame.frame_id));
            self.signalled_release = frame.frame_id;
        }
    }

    /// The frame as the viewport model sees it: identity, view state, and an image nobody copied.
    #[must_use]
    pub fn presented(&self, frame: Announcement) -> PresentedFrame {
        // `poll` refuses an announcement whose slot is outside the ring, so this index is inside
        // the array by the time a frame reaches here.
        let plane = self.handshake.planes[(frame.slot as usize).min(crate::wire::MAX_BUFFERS - 1)];
        PresentedFrame::new(
            FrameId::from_raw(frame.frame_id),
            self.view_state.clone(),
            FrameImage::SharedTexture {
                handle: u64::from(frame.slot),
                image: SharedImage {
                    width: self.handshake.width,
                    height: self.handshake.height,
                    fourcc: self.handshake.fourcc,
                    modifier: self.handshake.modifier,
                    stride: plane.stride,
                    offset: plane.offset,
                    allocation_bytes: plane.allocation_bytes,
                    slot: frame.slot,
                    buffer_count: self.handshake.buffer_count,
                    generation: frame.generation,
                    timeline_value: frame.timeline_value,
                },
            },
            frame.submitted_nanos / 1_000,
        )
    }
}

impl Drop for ViewportSession {
    fn drop(&mut self) {
        // Tell the runtime we hold nothing, so that a ring is not left short by an editor that
        // closed one viewport and kept working.
        self.page.set_held(0);
        if self.render_done.is_none() && self.release.is_none() {
            return;
        }
        // A release signal may be staged on the queue and not yet submitted, and earlier ones may
        // still be pending on the GPU. Destroying a semaphore either of those refers to is a use
        // after free with a driver on the other end of it, so: flush, then wait.
        //
        // The wait is BOUNDED, for the reason this module is built around — a wait that cannot time
        // out is how an editor becomes a window that will not close. If it expires we destroy
        // anyway, because the alternative is never closing, and by then the runtime is gone.
        let encoder = self
            .gpu
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some("viewport-session-teardown"),
            });
        self.gpu.queue.submit([encoder.finish()]);
        let _ = self.gpu.device.poll(wgpu::PollType::Wait {
            submission_index: None,
            timeout: Some(TEARDOWN_PATIENCE),
        });
        for semaphore in [self.render_done, self.release].into_iter().flatten() {
            // SAFETY: both semaphores were imported by `import_timeline` on this device and are
            // destroyed exactly once. Everything this session staged has been submitted by the
            // flush above, and the bounded wait above gave those submissions their chance to
            // finish.
            unsafe { self.gpu.ash_device.destroy_semaphore(semaphore, None) };
        }
    }
}

impl Transport for ViewportSession {
    fn kind(&self) -> TransportKind {
        TransportKind::SharedTexture
    }

    fn poll(&mut self) -> Option<PresentedFrame> {
        let (_, frame) = self.acquire()?;
        Some(self.presented(frame))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_default_policy_is_the_bounded_host_wait() {
        assert_eq!(
            WaitPolicy::default(),
            WaitPolicy::HostWait(DEFAULT_HOST_WAIT)
        );
        assert_eq!(DEFAULT_HOST_WAIT.as_millis(), 2);
    }

    #[test]
    fn there_is_no_queue_wait_variant_to_choose() {
        // A test that reads oddly and earns its place: the failure it stands for is an editor that
        // renders nothing and cannot be closed, and the way that failure comes back is somebody
        // adding a fourth variant to make a frame time better. Any such addition fails here, and
        // the reason is in this module's header.
        let policies = [
            WaitPolicy::Newest,
            WaitPolicy::HostPoll,
            WaitPolicy::HostWait(DEFAULT_HOST_WAIT),
        ];
        assert_eq!(
            std::mem::size_of_val(&policies) / std::mem::size_of::<WaitPolicy>(),
            3
        );
        for policy in policies {
            let name = format!("{policy:?}");
            assert!(
                !name.contains("Queue") && !name.contains("Gpu"),
                "no policy stages a wait on the editor's queue: {name}"
            );
        }
    }

    #[test]
    fn the_two_deaths_get_two_different_sentences() {
        assert!(Liveness::Live.message().is_empty());
        assert!(Liveness::Gone.message().contains("no longer running"));
        assert!(Liveness::Wedged.message().contains("stopped producing"));
        assert_ne!(Liveness::Gone.message(), Liveness::Wedged.message());
    }

    #[test]
    fn every_refusal_the_editor_can_make_has_a_counter_of_its_own() {
        // The editor never crashes on a bad announcement; it counts one and keeps the frame it has.
        // Four distinct refusals, four distinct counters — a single "errors" number would make
        // "the runtime is announcing slots that do not exist" indistinguishable from "the runtime
        // is faster than us", which need opposite responses.
        let counters = SessionCounters::default();
        assert_eq!(counters.wrong_generation, 0);
        assert_eq!(counters.malformed, 0);
        assert_eq!(counters.claim_vetoed, 0);
        assert_eq!(counters.claims_abandoned, 0);
        assert_eq!(counters.timed_out, 0);
    }

    #[test]
    fn both_sides_stamp_the_same_clock() {
        // The frame ages the viewport reports are a runtime's reading subtracted from an editor's,
        // so they must be readings of one clock. An `Instant` origin per process would give two
        // zeroes and an age of nonsense.
        let first = monotonic_micros();
        std::thread::sleep(Duration::from_millis(2));
        let second = monotonic_micros();
        assert!(second > first, "CLOCK_MONOTONIC advances");
        assert!(
            second - first < 1_000_000,
            "and it is the same epoch in both readings"
        );
    }
}
