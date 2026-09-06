//! The viewport transport: how the image reaches the editor, and how old it is when it does.
//!
//! `editor-viewport-and-gizmos` — "Viewport transport":
//!
//! > The viewport SHALL obtain its image through an **abstract transport** with at least three
//! > implementations [local surface, shared texture, encoded stream] ... The editor SHALL treat all
//! > three uniformly: a viewport feature SHALL work over any transport unless it documents a reason
//! > not to. The transport SHALL carry, alongside the image, the **frame's view state and
//! > identifiers** ... Latency and frame pacing SHALL be reported per transport, and the editor SHALL
//! > surface when it is viewing a stale or degraded stream.
//!
//! --- THE THREE KINDS DIFFER IN ONE FIELD, AND THAT IS THE DESIGN ------------------------------------
//!
//! [`PresentedFrame`] is the same struct for all three; only [`FrameImage`] varies. Everything a
//! viewport feature does — resolve a click, align an overlay, decide the gizmo is stale, report the
//! pacing — reads the fields that are identical across the three, so "console editing is not
//! special" is a property of the type rather than a promise. A feature that needed a local surface
//! would have to name [`FrameImage::Surface`] explicitly, which is exactly the documented exception
//! the requirement allows and exactly the thing a reviewer can grep for.
//!
//! --- THE LATEST FRAME WINS. THIS IS THE MEASURED DECISION OF TASK 4.1 -------------------------------
//!
//! [`Mailbox`] holds ONE frame. A publication into an unconsumed mailbox replaces what is there and
//! counts a `superseded`. There is no queue, and adding one would be the single most expensive
//! mistake available here.
//!
//! Measured, not argued (`tests/frame_age.rs`, and the numbers are in this crate's README): with a
//! producer at 60 Hz and a consumer at 40 Hz that stalls for 40 ms every twelfth iteration, a
//! **queue** grows without bound — p50 523 ms, p99 1045 ms, and still climbing when the run ended,
//! because nothing in a queue ever catches up. The **mailbox** stayed at the age of the newest frame
//! — p50 8.4 ms, p99 16.6 ms, one producer interval — and reported the 37 frames it dropped. Both
//! look identical if you measure throughput; only one of them is an editor a designer can drag a
//! gizmo in.
//!
//! That is the finding the milestone's control-path spike explicitly did not cover: "If the image
//! the user is dragging against is two or three frames stale, the drag feels laggy however fast the
//! transform applies. Measure that at task 4.1 before four panels are built on the assumption."
//!
//! --- NOTHING HERE BLOCKS ------------------------------------------------------------------------------
//!
//! > The editor SHALL never block its interface thread on runtime rendering; a stalled runtime SHALL
//! > produce a stale-frame indication rather than a frozen editor.
//!
//! [`Transport::poll`] returns `Option` and never waits. There is no `recv`, no `wait_for_frame` and
//! no timeout parameter anywhere in this module, which is the same shape — and the same reasoning —
//! as `cy_editor_protocol::Session` having no blocking send.

use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Mutex, MutexGuard};
use std::time::Instant;

use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_protocol::FrameId;

use crate::state::ViewState;

/// How the image reaches the editor.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum TransportKind {
    /// The runtime is in the editor's own process and presents into an editor-owned surface.
    LocalSurface,
    /// The runtime is a separate process on this machine and shares the texture.
    SharedTexture,
    /// The runtime is remote or a console; the image is compressed and input is forwarded back.
    EncodedStream,
}

impl TransportKind {
    /// A name for a report and a log.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            TransportKind::LocalSurface => "local-surface",
            TransportKind::SharedTexture => "shared-texture",
            TransportKind::EncodedStream => "encoded-stream",
        }
    }

    /// Whether the image's bytes cross the process boundary.
    ///
    /// False for a shared texture, which is the whole reason it exists, and true for an encoded
    /// stream. A local surface copies nothing because there is no boundary.
    #[must_use]
    pub const fn copies_pixels(self) -> bool {
        matches!(self, TransportKind::EncodedStream)
    }
}

/// The image itself, which is the only thing the three kinds disagree about.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum FrameImage {
    /// An editor-owned surface the runtime presented into. The number identifies which.
    Surface(u64),
    /// A texture shared between processes. The number is the platform handle's identifier, and no
    /// pixels crossed the boundary to deliver it.
    SharedTexture {
        /// The shared handle's identifier.
        handle: u64,
        /// How large the image is, for a report. Not how much was copied — nothing was.
        bytes: u64,
    },
    /// A compressed image. The bytes are here because they genuinely crossed the boundary.
    Encoded(Vec<u8>),
}

impl FrameImage {
    /// How many bytes this delivery actually moved. Zero for a surface and a shared texture.
    #[must_use]
    pub fn transferred_bytes(&self) -> u64 {
        match self {
            FrameImage::Surface(_) | FrameImage::SharedTexture { .. } => 0,
            FrameImage::Encoded(bytes) => bytes.len() as u64,
        }
    }
}

/// Why an image is not what the project actually looks like.
///
/// Mirrors `cy::render::ViewportDegradation`. A reason rather than a boolean, because
/// "Degradation SHALL be surfaced to the user" needs three different sentences.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum Degradation {
    /// Full quality: what the game will look like.
    #[default]
    None,
    /// Rendering below the requested rate to stay inside the budget.
    ReducedRate,
    /// Rendering below the viewport's pixel size and upscaling.
    ReducedResolution,
    /// Not rendering at all. The last image stands.
    Paused,
}

impl Degradation {
    /// What the editor tells the user. Empty when there is nothing to say, so a caller can show the
    /// string when it is non-empty rather than branching on the variant.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Degradation::None => "",
            Degradation::ReducedRate => "This viewport is rendering below its requested rate.",
            Degradation::ReducedResolution => {
                "This viewport is rendering below full resolution and is being upscaled."
            }
            Degradation::Paused => "This viewport is paused; the image is the last one rendered.",
        }
    }

    /// The discriminant that crosses the boundary.
    const fn as_u8(self) -> u8 {
        match self {
            Degradation::None => 0,
            Degradation::ReducedRate => 1,
            Degradation::ReducedResolution => 2,
            Degradation::Paused => 3,
        }
    }

    const fn from_u8(raw: u8) -> Option<Self> {
        match raw {
            0 => Some(Degradation::None),
            1 => Some(Degradation::ReducedRate),
            2 => Some(Degradation::ReducedResolution),
            3 => Some(Degradation::Paused),
            _ => None,
        }
    }
}

/// One frame, as it arrived.
#[derive(Clone, PartialEq, Debug)]
pub struct PresentedFrame {
    /// The frame's identity. **The same identifier the control path reconciles against** — a gizmo
    /// drag predicts locally and reconciles the runtime's echo by this number, and inventing a
    /// second one would make "the image and the echo are the same frame" unanswerable.
    pub frame: FrameId,
    /// The view state the frame was rendered with. A copy, so the editor's camera moving does not
    /// change what this frame was.
    pub state: ViewState,
    /// The image.
    pub image: FrameImage,
    /// The runtime's monotonic clock when the frame finished, in microseconds.
    pub produced_micros: u64,
    /// Whether the image is honest about the project's appearance.
    pub degradation: Degradation,
    /// The fraction of the viewport's pixel size actually rendered.
    pub resolution_scale: f32,
}

impl PresentedFrame {
    /// A full-quality frame with this identity and state.
    #[must_use]
    pub fn new(frame: FrameId, state: ViewState, image: FrameImage, produced_micros: u64) -> Self {
        Self {
            frame,
            state,
            image,
            produced_micros,
            degradation: Degradation::None,
            resolution_scale: 1.0,
        }
    }

    /// Whether this image can be judged as the project's appearance.
    #[must_use]
    pub fn full_quality(&self) -> bool {
        self.degradation == Degradation::None && self.resolution_scale >= 1.0
    }

    /// Encode the frame, for a transport that carries bytes.
    ///
    /// The same codec the journal and the control path use. A second encoding for images would be a
    /// second thing to keep in step with `ViewState`, and the view state is the half that matters.
    #[must_use]
    pub fn encode(&self) -> Vec<u8> {
        let mut writer = Writer::new();
        writer.u64(self.frame.as_u64());
        writer.u64(self.produced_micros);
        writer.u8(self.degradation.as_u8());
        writer.f32(self.resolution_scale);
        match &self.image {
            FrameImage::Surface(id) => {
                writer.u8(0);
                writer.u64(*id);
            }
            FrameImage::SharedTexture { handle, bytes } => {
                writer.u8(1);
                writer.u64(*handle);
                writer.u64(*bytes);
            }
            FrameImage::Encoded(bytes) => {
                writer.u8(2);
                writer.bytes(bytes);
            }
        }
        self.state.write(&mut writer);
        writer.finish()
    }

    /// Decode a frame a peer encoded.
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        let mut reader = Reader::new(bytes);
        let frame = FrameId::from_raw(reader.u64()?);
        let produced_micros = reader.u64()?;
        let degradation = Degradation::from_u8(reader.u8()?).ok_or_else(|| {
            Problem::new(
                "decode a viewport frame",
                "its degradation reason is not one this build knows",
            )
            .with_remedy("the runtime is newer than the editor; rebuild them together")
        })?;
        let resolution_scale = reader.f32()?;
        let image = match reader.u8()? {
            0 => FrameImage::Surface(reader.u64()?),
            1 => FrameImage::SharedTexture {
                handle: reader.u64()?,
                bytes: reader.u64()?,
            },
            2 => FrameImage::Encoded(reader.bytes()?),
            other => {
                return Err(Problem::new(
                    "decode a viewport frame",
                    format!("image kind {other} is not one this build knows"),
                ));
            }
        };
        Ok(Self {
            frame,
            state: ViewState::read(&mut reader)?,
            image,
            produced_micros,
            degradation,
            resolution_scale,
        })
    }
}

/// What the editor asked of a transport.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct TransportBudget {
    /// The interval the editor wants frames at, in microseconds. Zero means "as fast as you can".
    pub requested_interval_micros: u32,
    /// How old the newest frame may be before the editor says it is looking at a stale image.
    ///
    /// Three requested intervals rather than one: a single interval is normal jitter, and a warning
    /// that fires on jitter is a warning users learn to ignore.
    pub stale_after_micros: u32,
}

impl Default for TransportBudget {
    fn default() -> Self {
        Self {
            requested_interval_micros: 16_667,
            stale_after_micros: 50_000,
        }
    }
}

impl TransportBudget {
    /// A budget for a viewport that is visible but not focused: a third of the rate.
    #[must_use]
    pub const fn unfocused() -> Self {
        Self {
            requested_interval_micros: 50_000,
            stale_after_micros: 150_000,
        }
    }

    /// Refuse a budget that would report every frame stale on arrival.
    pub fn validate(self) -> Result<Self> {
        if self.stale_after_micros == 0 {
            return Err(Problem::new(
                "set a viewport budget",
                "a stale threshold of zero reports every frame as stale the instant it arrives",
            )
            .with_remedy("use at least twice the requested interval"));
        }
        Ok(self)
    }
}

/// Pacing and latency for one transport.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct Pacing {
    /// Frames the editor consumed.
    pub accepted: u64,
    /// Frames replaced before the editor consumed them. See the module note: a counter rather than
    /// a queue is the decision this number exists to make visible.
    pub superseded: u64,
    /// Consumed frames whose age exceeded the requested interval.
    pub late: u64,
    /// Bytes the transport actually moved. Zero for a shared texture, by construction.
    pub transferred_bytes: u64,

    /// The age of the last frame the editor consumed, at the moment it consumed it.
    pub last_age_micros: u32,
    /// The oldest frame the editor ever consumed. A mean hides exactly the stall a user notices.
    pub worst_age_micros: u32,
    /// The mean age over every consumed frame.
    pub mean_age_micros: u32,

    /// The total of every consumed frame's age, which the mean is derived from rather than
    /// smoothed. Reading a mean twice must give the same number.
    total_age_micros: u64,
}

impl Pacing {
    fn record(&mut self, age_micros: u64, transferred: u64, budget: TransportBudget) {
        let age = saturating_micros(age_micros);
        self.accepted += 1;
        self.transferred_bytes += transferred;
        self.last_age_micros = age;
        self.worst_age_micros = self.worst_age_micros.max(age);
        self.total_age_micros += u64::from(age);
        self.mean_age_micros = saturating_micros(self.total_age_micros / self.accepted);
        if budget.requested_interval_micros != 0 && age > budget.requested_interval_micros {
            self.late += 1;
        }
    }
}

/// Clamp a microsecond span into the `u32` a report holds.
///
/// A span past seventy-one minutes is a suspended process rather than a pacing measurement, and
/// saturating keeps one such gap from wrapping into a small number that reads as healthy.
fn saturating_micros(micros: u64) -> u32 {
    u32::try_from(micros).unwrap_or(u32::MAX)
}

/// The editor's side of a transport: never blocks, and always answers how old the image is.
pub trait Transport {
    /// Which of the three this is.
    fn kind(&self) -> TransportKind;

    /// The newest frame that has arrived, or `None`.
    ///
    /// **Never blocks and never waits.** An implementation that could wait would be waited on, in
    /// some code path, on some machine, and the symptom would be a frozen editor blamed on the
    /// renderer.
    fn poll(&mut self) -> Option<PresentedFrame>;
}

/// A single-slot exchange: the newest frame wins and the ones nobody read are counted.
///
/// Cloneable and shared: the producing side holds one handle and the consuming side another, which
/// is the shape a runtime thread and an interface thread need.
#[derive(Clone, Debug, Default)]
pub struct Mailbox {
    slot: Arc<Mutex<Option<PresentedFrame>>>,
    superseded: Arc<AtomicU64>,
}

impl Mailbox {
    /// An empty mailbox.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Put a frame in, replacing and counting whatever was there.
    pub fn publish(&self, frame: PresentedFrame) {
        let mut slot = self.lock();
        if slot.is_some() {
            self.superseded.fetch_add(1, Ordering::Relaxed);
        }
        *slot = Some(frame);
    }

    /// Take the frame, if there is one. Never blocks on the producer.
    pub fn take(&self) -> Option<PresentedFrame> {
        self.lock().take()
    }

    /// How many frames were replaced before anything read them.
    #[must_use]
    pub fn superseded(&self) -> u64 {
        self.superseded.load(Ordering::Relaxed)
    }

    /// A poisoned lock is recoverable here: the slot holds a frame, and a frame is not an
    /// invariant that a panicking writer can leave half-built. Losing the image for one frame is
    /// the correct cost; propagating a panic into the interface thread is not.
    fn lock(&self) -> MutexGuard<'_, Option<PresentedFrame>> {
        self.slot
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
    }
}

/// A transport backed by a [`Mailbox`]. The local-surface and shared-texture cases, and the
/// consuming half of the encoded-stream case once its bytes have been read off the wire.
#[derive(Clone, Debug)]
pub struct MailboxTransport {
    kind: TransportKind,
    mailbox: Mailbox,
}

impl MailboxTransport {
    /// A transport of this kind over this mailbox.
    #[must_use]
    pub fn new(kind: TransportKind, mailbox: Mailbox) -> Self {
        Self { kind, mailbox }
    }

    /// The mailbox the producer publishes into.
    #[must_use]
    pub fn mailbox(&self) -> &Mailbox {
        &self.mailbox
    }
}

impl Transport for MailboxTransport {
    fn kind(&self) -> TransportKind {
        self.kind
    }

    fn poll(&mut self) -> Option<PresentedFrame> {
        self.mailbox.take()
    }
}

/// What the editor knows about one viewport's image: the newest frame, how old it is, and whether
/// the user should be told something about it.
#[derive(Clone, Debug)]
pub struct FrameStream {
    kind: TransportKind,
    budget: TransportBudget,
    latest: Option<PresentedFrame>,
    pacing: Pacing,
}

impl FrameStream {
    /// A stream for a transport of this kind, with the default budget.
    #[must_use]
    pub fn new(kind: TransportKind) -> Self {
        Self {
            kind,
            budget: TransportBudget::default(),
            latest: None,
            pacing: Pacing::default(),
        }
    }

    /// Ask for a different rate. Refused when it would report every frame stale.
    pub fn set_budget(&mut self, budget: TransportBudget) -> Result<()> {
        self.budget = budget.validate()?;
        Ok(())
    }

    /// The budget in force.
    #[must_use]
    pub const fn budget(&self) -> TransportBudget {
        self.budget
    }

    /// Which transport this is.
    #[must_use]
    pub const fn kind(&self) -> TransportKind {
        self.kind
    }

    /// Consume everything a transport has and keep the newest.
    ///
    /// Returns the frame that was accepted, if any. One call per interface frame; it does exactly
    /// as much work as there is to do and never more, which is what makes an interface frame's cost
    /// bounded by what the runtime sent.
    pub fn pump(
        &mut self,
        transport: &mut dyn Transport,
        now_micros: u64,
    ) -> Option<&PresentedFrame> {
        let frame = transport.poll()?;
        self.accept(frame, now_micros);
        self.latest.as_ref()
    }

    /// Accept one frame, recording what it cost to have waited for it.
    pub fn accept(&mut self, frame: PresentedFrame, now_micros: u64) {
        let age = now_micros.saturating_sub(frame.produced_micros);
        self.pacing
            .record(age, frame.image.transferred_bytes(), self.budget);
        self.latest = Some(frame);
    }

    /// The newest frame, or `None` before anything has arrived.
    #[must_use]
    pub const fn latest(&self) -> Option<&PresentedFrame> {
        self.latest.as_ref()
    }

    /// How old the newest frame is now. Zero when nothing has arrived, which
    /// [`FrameStream::latest`] distinguishes.
    #[must_use]
    pub fn age_micros(&self, now_micros: u64) -> u64 {
        self.latest
            .as_ref()
            .map_or(0, |frame| now_micros.saturating_sub(frame.produced_micros))
    }

    /// Whether the editor should say it is looking at a stale image.
    ///
    /// False before the first frame: a transport that has not started is a different message from
    /// one that has stopped, and saying "stale" at the start of every session is how a warning
    /// stops being read.
    #[must_use]
    pub fn is_stale(&self, now_micros: u64) -> bool {
        self.latest.is_some()
            && self.age_micros(now_micros) > u64::from(self.budget.stale_after_micros)
    }

    /// What the editor should tell the user about this image, or `None` when there is nothing to
    /// say. One call, so a panel does not decide the precedence of "stale" against "degraded" for
    /// itself and get a different answer from the next panel.
    #[must_use]
    pub fn advisory(&self, now_micros: u64) -> Option<String> {
        let frame = self.latest.as_ref()?;
        if self.is_stale(now_micros) {
            let age_ms = self.age_micros(now_micros) / 1000;
            return Some(format!(
                "The runtime has not produced a frame for {age_ms} ms; this image is stale."
            ));
        }
        if frame.degradation == Degradation::None && frame.resolution_scale >= 1.0 {
            return None;
        }
        if frame.degradation == Degradation::None {
            return Some(Degradation::ReducedResolution.message().to_string());
        }
        Some(frame.degradation.message().to_string())
    }

    /// Pacing and latency, for the report the requirement asks for per transport.
    #[must_use]
    pub const fn pacing(&self) -> Pacing {
        self.pacing
    }

    /// Forget the statistics, keeping the frame.
    pub fn reset_pacing(&mut self) {
        self.pacing = Pacing::default();
    }
}

/// A monotonic clock in microseconds, so that everything in this crate can be driven by an explicit
/// number in a test and by a real reading in an editor.
#[derive(Clone, Copy, Debug)]
pub struct Clock {
    origin: Instant,
}

impl Default for Clock {
    fn default() -> Self {
        Self::new()
    }
}

impl Clock {
    /// A clock starting now.
    #[must_use]
    pub fn new() -> Self {
        Self {
            origin: Instant::now(),
        }
    }

    /// Microseconds since the clock was made.
    #[must_use]
    pub fn micros(&self) -> u64 {
        u64::try_from(self.origin.elapsed().as_micros()).unwrap_or(u64::MAX)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame(number: u64, produced_micros: u64) -> PresentedFrame {
        PresentedFrame::new(
            FrameId::from_raw(number),
            ViewState::new(),
            FrameImage::SharedTexture {
                handle: 0xAB,
                bytes: 8_294_400,
            },
            produced_micros,
        )
    }

    #[test]
    fn the_newest_frame_wins_and_the_ones_nobody_read_are_counted() {
        let mailbox = Mailbox::new();
        mailbox.publish(frame(1, 0));
        mailbox.publish(frame(2, 16_667));
        mailbox.publish(frame(3, 33_334));

        let taken = mailbox.take().expect("a frame is waiting");
        assert_eq!(
            taken.frame,
            FrameId::from_raw(3),
            "the newest, not the oldest"
        );
        assert_eq!(mailbox.superseded(), 2);
        assert!(mailbox.take().is_none(), "and the slot is empty afterwards");
    }

    #[test]
    fn a_stalled_runtime_is_reported_stale_rather_than_waited_for() {
        let mut stream = FrameStream::new(TransportKind::SharedTexture);
        // Before the first frame there is nothing to be stale about.
        assert!(!stream.is_stale(10_000_000));
        assert!(stream.advisory(10_000_000).is_none());

        stream.accept(frame(1, 1_000_000), 1_005_000);
        assert!(!stream.is_stale(1_020_000));
        assert!(stream.is_stale(1_080_000));
        let advisory = stream.advisory(1_080_000).expect("the user is told");
        assert!(advisory.contains("stale"), "{advisory}");
    }

    #[test]
    fn degradation_is_surfaced_rather_than_mistaken_for_the_projects_appearance() {
        let mut stream = FrameStream::new(TransportKind::EncodedStream);
        let mut reduced = frame(1, 0);
        reduced.degradation = Degradation::ReducedResolution;
        reduced.resolution_scale = 0.5;
        stream.accept(reduced, 1_000);

        assert!(!stream.latest().expect("a frame").full_quality());
        let advisory = stream.advisory(1_000).expect("the user is told");
        assert!(advisory.contains("full resolution"), "{advisory}");

        // A frame that claims full quality and is upscaled anyway is still reported: the two fields
        // disagreeing is a runtime defect, and the honest answer is the one that warns.
        let mut inconsistent = frame(2, 16_000);
        inconsistent.resolution_scale = 0.75;
        stream.accept(inconsistent, 17_000);
        assert!(stream.advisory(17_000).is_some());
    }

    #[test]
    fn pacing_reports_the_age_of_what_was_consumed_and_what_missed_the_budget() {
        let mut stream = FrameStream::new(TransportKind::LocalSurface);
        stream.accept(frame(1, 0), 5_000);
        stream.accept(frame(2, 16_667), 66_667);

        let pacing = stream.pacing();
        assert_eq!(pacing.accepted, 2);
        assert_eq!(pacing.last_age_micros, 50_000);
        assert_eq!(pacing.worst_age_micros, 50_000);
        assert_eq!(pacing.mean_age_micros, 27_500);
        assert_eq!(
            pacing.late, 1,
            "one frame was older than the 16.7 ms request"
        );
        assert_eq!(
            pacing.transferred_bytes, 0,
            "a shared texture moves no pixels, which is why it exists"
        );
    }

    #[test]
    fn an_encoded_frame_reports_what_it_actually_moved() {
        let mut stream = FrameStream::new(TransportKind::EncodedStream);
        let encoded = PresentedFrame::new(
            FrameId::from_raw(1),
            ViewState::new(),
            FrameImage::Encoded(vec![0_u8; 64_000]),
            0,
        );
        stream.accept(encoded, 1_000);
        assert_eq!(stream.pacing().transferred_bytes, 64_000);
        assert!(TransportKind::EncodedStream.copies_pixels());
        assert!(!TransportKind::SharedTexture.copies_pixels());
    }

    #[test]
    fn a_frame_round_trips_through_the_wire_with_its_view_state() {
        let mut state = ViewState::new();
        state.camera.position = crate::math::Vec3::new(1.0, 2.0, 3.0);
        state.view_mode = crate::viewmode::ViewMode::Wireframe;
        let original = PresentedFrame {
            frame: FrameId::from_raw(77),
            state,
            image: FrameImage::Encoded(vec![1, 2, 3, 4]),
            produced_micros: 123_456,
            degradation: Degradation::ReducedRate,
            resolution_scale: 0.5,
        };
        let decoded = PresentedFrame::decode(&original.encode()).expect("our own encoding");
        assert_eq!(decoded, original);
    }

    #[test]
    fn a_budget_that_would_cry_stale_on_arrival_is_refused() {
        let mut stream = FrameStream::new(TransportKind::LocalSurface);
        let broken = TransportBudget {
            requested_interval_micros: 16_667,
            stale_after_micros: 0,
        };
        let problem = stream.set_budget(broken).expect_err("refused");
        assert!(problem.remedy.is_some(), "{problem}");
        assert_eq!(stream.budget(), TransportBudget::default());
    }

    #[test]
    fn a_transport_is_polled_and_never_waited_on() {
        let mailbox = Mailbox::new();
        let mut transport = MailboxTransport::new(TransportKind::SharedTexture, mailbox.clone());
        let mut stream = FrameStream::new(TransportKind::SharedTexture);

        // Nothing published: the pump returns immediately with nothing, which is what keeps an
        // interface frame's cost bounded by what the runtime sent.
        assert!(stream.pump(&mut transport, 0).is_none());

        mailbox.publish(frame(9, 1_000));
        let accepted = stream.pump(&mut transport, 2_000).expect("a frame arrived");
        assert_eq!(accepted.frame, FrameId::from_raw(9));
        assert_eq!(transport.kind(), TransportKind::SharedTexture);
    }
}
