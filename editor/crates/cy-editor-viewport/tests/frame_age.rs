//! Task 4.1's measurement: **how old is the image a designer is dragging against?**
//!
//! --- WHY THIS EXISTS ---------------------------------------------------------------------------------
//!
//! The milestone's control-path spike measured a gizmo drag's round trip and closed with the half it
//! had not measured, in as many words:
//!
//! > NOT MEASURED. The viewport transport. This spike measured the control path only. If the
//! > shared-texture or encoded-stream path is two or three frames behind, a drag will feel laggy
//! > however fast the transform applies. That belongs to task 4.1 and should be measured before
//! > panels are built on it.
//!
//! This is that measurement. It runs a **real second process** (`cy-viewport-probe`) producing frames
//! at 60 Hz over a pipe, and consumes them at a slower, deliberately uneven rate — because that is
//! what an editor actually is: a free-running interface thread that sometimes takes 40 ms to lay out
//! a panel. Measuring two threads in one process would measure the scheduler; measuring across a
//! process boundary measures the transport.
//!
//! --- WHAT IT FOUND, AND THE DECISION IT SETTLED ------------------------------------------------------
//!
//! **The queueing discipline dominates everything else about the transport.** A queue between a
//! producer and a slower consumer does not smooth anything: it accumulates, without bound, and never
//! recovers — while every individual frame looks healthy and throughput looks perfect. A single-slot
//! mailbox stays within about one producer interval and reports the frames it dropped.
//!
//! **The second finding is a warning rather than a reassurance.** A 64 KB encoded frame costs about
//! half a millisecond more than a zero-copy shared texture — genuinely second order, in either Cargo
//! profile. A full **uncompressed** 1080p frame is a different thing: at 8.3 MB per frame and 60 Hz
//! that is half a gigabyte a second through the boundary, and it is the only case where the profile
//! matters at all — p50 105.9 ms unoptimised, 27.9 ms optimised, against the shared texture's 9 ms in
//! both. Neither number is one to build a viewport on. So the third transport is an *encoded* stream
//! in this crate's vocabulary and in the engine's, and an uncompressed copy is not a transport anyone
//! should build — which is a decision this measurement settles rather than a preference, and the same
//! decision in both profiles, which is what makes it a decision rather than an artefact.
//!
//! That first finding is why `cy_editor_viewport::transport::Mailbox` holds one frame, and why there
//! is no queue anywhere in this crate. The numbers this run produced are recorded in the crate's
//! README.
//!
//! --- READING THE OUTPUT ------------------------------------------------------------------------------
//!
//! Each case prints a human line and a `RESULT` line, the same shape the latency spike used, so a
//! run's numbers can be recovered from a log without re-running it. Run it with
//! `cargo test -p cy-editor-viewport --test frame_age -- --nocapture`.

use std::collections::VecDeque;
use std::io::Read;
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use cy_editor_protocol::read_frame;
use cy_editor_viewport::transport::{Mailbox, PresentedFrame};

/// Which discipline the consumer reads under.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Discipline {
    /// One slot; the newest frame wins and the rest are counted. What the crate implements.
    Mailbox,
    /// Every frame kept in order. The obvious implementation, and the one this measures to reject.
    Queue,
}

impl Discipline {
    const fn name(self) -> &'static str {
        match self {
            Discipline::Mailbox => "mailbox",
            Discipline::Queue => "queue",
        }
    }
}

/// One case's settings.
struct Case {
    label: &'static str,
    discipline: Discipline,
    frames: u32,
    /// The producer's interval. 16,667 µs is 60 Hz.
    produce_micros: u64,
    /// The consumer's interval, deliberately slower than the producer's.
    consume_micros: u64,
    payload_bytes: usize,
}

/// What a case measured.
struct Measurement {
    label: &'static str,
    discipline: Discipline,
    samples: Vec<u64>,
    transferred_bytes: u64,
    superseded: u64,
}

impl Measurement {
    fn percentile(&self, fraction: f64) -> u64 {
        if self.samples.is_empty() {
            return 0;
        }
        let mut sorted = self.samples.clone();
        sorted.sort_unstable();
        #[allow(
            clippy::cast_precision_loss,
            clippy::cast_possible_truncation,
            clippy::cast_sign_loss,
            reason = "a sample count far below 2^24, and an index that is clamped below"
        )]
        let index = ((sorted.len() as f64 - 1.0) * fraction).round() as usize;
        sorted[index.min(sorted.len() - 1)]
    }

    fn report(&self) {
        let p50 = self.percentile(0.5);
        let p99 = self.percentile(0.99);
        let worst = self.samples.iter().copied().max().unwrap_or(0);
        println!(
            "{:<28} {:<8} n={:<4} p50={:>8.3} ms  p99={:>8.3} ms  max={:>8.3} ms  \
             dropped={:<5} moved={} MB",
            self.label,
            self.discipline.name(),
            self.samples.len(),
            millis(p50),
            millis(p99),
            millis(worst),
            self.superseded,
            self.transferred_bytes / (1024 * 1024),
        );
        println!(
            "RESULT {{\"case\":\"{}\",\"discipline\":\"{}\",\"samples\":{},\"p50_ms\":{:.3},\
             \"p99_ms\":{:.3},\"max_ms\":{:.3},\"dropped\":{},\"transferred_bytes\":{}}}",
            self.label,
            self.discipline.name(),
            self.samples.len(),
            millis(p50),
            millis(p99),
            millis(worst),
            self.superseded,
            self.transferred_bytes,
        );
    }
}

#[allow(
    clippy::cast_precision_loss,
    reason = "a microsecond count below 2^24 for any real result"
)]
fn millis(micros: u64) -> f64 {
    micros as f64 / 1000.0
}

/// Microseconds since the epoch: the one clock the probe and this process share. See the probe's
/// module note for why it is the wall clock rather than an `Instant`.
fn wall_clock_micros() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |since| {
            u64::try_from(since.as_micros()).unwrap_or(u64::MAX)
        })
}

/// Start the probe.
fn spawn_probe(case: &Case) -> Child {
    Command::new(env!("CARGO_BIN_EXE_cy-viewport-probe"))
        .arg("--frames")
        .arg(case.frames.to_string())
        .arg("--interval-micros")
        .arg(case.produce_micros.to_string())
        .arg("--payload-bytes")
        .arg(case.payload_bytes.to_string())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .expect("the probe binary is built alongside this test")
}

/// The shared slot the reader thread delivers into, in whichever discipline is being measured.
enum Sink {
    Mailbox(Mailbox),
    Queue(Arc<Mutex<VecDeque<PresentedFrame>>>),
}

impl Sink {
    fn new(discipline: Discipline) -> Self {
        match discipline {
            Discipline::Mailbox => Sink::Mailbox(Mailbox::new()),
            Discipline::Queue => Sink::Queue(Arc::new(Mutex::new(VecDeque::new()))),
        }
    }

    fn clone_handle(&self) -> Self {
        match self {
            Sink::Mailbox(mailbox) => Sink::Mailbox(mailbox.clone()),
            Sink::Queue(queue) => Sink::Queue(Arc::clone(queue)),
        }
    }

    fn deliver(&self, frame: PresentedFrame) {
        match self {
            Sink::Mailbox(mailbox) => mailbox.publish(frame),
            Sink::Queue(queue) => queue
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner)
                .push_back(frame),
        }
    }

    fn take(&self) -> Option<PresentedFrame> {
        match self {
            Sink::Mailbox(mailbox) => mailbox.take(),
            Sink::Queue(queue) => queue
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner)
                .pop_front(),
        }
    }

    fn superseded(&self) -> u64 {
        match self {
            Sink::Mailbox(mailbox) => mailbox.superseded(),
            Sink::Queue(_) => 0,
        }
    }
}

/// Run one case: produce in another process, consume here, and measure the age of what is consumed.
fn measure(case: &Case) -> Measurement {
    let mut child = spawn_probe(case);
    let stdout = child.stdout.take().expect("the probe's output was piped");

    let sink = Sink::new(case.discipline);
    let reader_sink = sink.clone_handle();
    let finished = Arc::new(AtomicBool::new(false));
    let transferred = Arc::new(AtomicU64::new(0));

    let reader_finished = Arc::clone(&finished);
    let reader_transferred = Arc::clone(&transferred);
    let reader = std::thread::spawn(move || {
        let mut stream = stdout;
        read_all(&mut stream, &reader_sink, &reader_transferred);
        reader_finished.store(true, Ordering::Release);
    });

    let samples = consume(case, &sink, &finished);

    reader.join().expect("the reader thread does not panic");
    let _ = child.wait();

    Measurement {
        label: case.label,
        discipline: case.discipline,
        samples,
        transferred_bytes: transferred.load(Ordering::Acquire),
        superseded: sink.superseded(),
    }
}

/// Read every framed frame the probe writes until the stream ends.
fn read_all(stream: &mut impl Read, sink: &Sink, transferred: &AtomicU64) {
    loop {
        match read_frame(stream) {
            Ok(Some(payload)) => {
                transferred.fetch_add(payload.len() as u64, Ordering::Relaxed);
                match PresentedFrame::decode(&payload) {
                    Ok(frame) => sink.deliver(frame),
                    Err(problem) => panic!("the probe wrote something undecodable: {problem}"),
                }
            }
            Ok(None) => return,
            Err(problem) => panic!("reading from the probe: {problem}"),
        }
    }
}

/// The editor's side: a free-running loop that is slower than the producer and occasionally stalls.
fn consume(case: &Case, sink: &Sink, finished: &AtomicBool) -> Vec<u64> {
    let mut samples = Vec::new();
    let started = Instant::now();
    let mut iteration: u32 = 0;

    // Bounded three ways so a defect in the probe cannot hang a test suite: by wall clock, by
    // iteration count, and by the producer having finished with nothing left to take.
    while started.elapsed() < Duration::from_secs(20) && iteration < case.frames * 4 {
        iteration += 1;
        std::thread::sleep(Duration::from_micros(case.consume_micros));
        // Every twelfth iteration, the interface thread does something expensive. This is what an
        // editor is: a panel relayout, a file dialogue, a garbage-collected script. Without it the
        // measurement is of a consumer that never falls behind, which is the case that cannot
        // distinguish the two disciplines.
        if iteration.is_multiple_of(12) {
            std::thread::sleep(Duration::from_millis(40));
        }
        if let Some(frame) = sink.take() {
            samples.push(wall_clock_micros().saturating_sub(frame.produced_micros));
            continue;
        }
        if finished.load(Ordering::Acquire) {
            return samples;
        }
    }
    samples
}

#[test]
fn a_queue_accumulates_lag_without_bound_and_a_mailbox_does_not() {
    // THE FINDING THIS WHOLE FILE EXISTS FOR. Both cases produce at 60 Hz and consume at 40 Hz with
    // a stall every twelfth iteration — the same producer, the same consumer, the same boundary. The
    // only difference is what happens to a frame that arrives while the consumer is busy.
    let queued = measure(&Case {
        label: "60 Hz -> 40 Hz, no payload",
        discipline: Discipline::Queue,
        frames: 90,
        produce_micros: 16_667,
        consume_micros: 25_000,
        payload_bytes: 0,
    });
    let mailboxed = measure(&Case {
        label: "60 Hz -> 40 Hz, no payload",
        discipline: Discipline::Mailbox,
        frames: 90,
        produce_micros: 16_667,
        consume_micros: 25_000,
        payload_bytes: 0,
    });

    println!("\n-- task 4.1: the age of the image the user is dragging against --");
    queued.report();
    mailboxed.report();

    assert!(
        !mailboxed.samples.is_empty() && !queued.samples.is_empty(),
        "the probe produced nothing; the measurement says nothing"
    );

    let queued_p50 = queued.percentile(0.5);
    let mailboxed_p50 = mailboxed.percentile(0.5);
    assert!(
        queued_p50 > mailboxed_p50 * 3,
        "a queue is supposed to accumulate and it did not: queue p50 {queued_p50} µs, mailbox p50 \
         {mailboxed_p50} µs. Either the consumer kept up (raise the stall) or a queue was quietly \
         introduced."
    );
    assert!(
        mailboxed.superseded > 0,
        "a mailbox that dropped nothing means the consumer never fell behind, and this case is \
         then measuring nothing"
    );
    // The absolute number is machine-dependent and the ratio above is the finding, but a mailbox
    // that let the image get half a second old would be a defect whatever the machine.
    assert!(
        mailboxed.percentile(0.99) < 500_000,
        "the mailbox let the image reach {} µs old",
        mailboxed.percentile(0.99)
    );
}

#[test]
fn an_encoded_frame_costs_a_millisecond_and_an_uncompressed_one_costs_a_hundred() {
    // The other half of the question task 4.1 asks: does a transport that copies pixels behave
    // differently in kind from one that shares a handle?
    //
    // A COMPRESSED one does not — about a millisecond, which is smaller than one stalled interface
    // frame. An UNCOMPRESSED one does: half a gigabyte a second through the boundary put the p50 age
    // an order of magnitude higher, which is the difference between a drag that feels attached to
    // the cursor and one that does not. The third case is here so that number exists rather than
    // being guessed at the next time somebody proposes shipping raw pixels.
    let shared = measure(&Case {
        label: "shared texture (no copy)",
        discipline: Discipline::Mailbox,
        frames: 90,
        produce_micros: 16_667,
        consume_micros: 20_000,
        payload_bytes: 0,
    });
    let encoded = measure(&Case {
        label: "encoded stream (64 KB)",
        discipline: Discipline::Mailbox,
        frames: 90,
        produce_micros: 16_667,
        consume_micros: 20_000,
        payload_bytes: 64 * 1024,
    });
    // A full uncompressed 1080p frame at 60 Hz: half a gigabyte a second across the boundary.
    let uncompressed = measure(&Case {
        label: "uncompressed 1080p (8.3 MB)",
        discipline: Discipline::Mailbox,
        frames: 45,
        produce_micros: 16_667,
        consume_micros: 20_000,
        payload_bytes: 1920 * 1080 * 4,
    });

    println!("\n-- task 4.1: what the payload costs --");
    shared.report();
    encoded.report();
    uncompressed.report();

    assert!(!shared.samples.is_empty());
    assert!(!encoded.samples.is_empty());
    assert!(!uncompressed.samples.is_empty());
    assert!(
        shared.transferred_bytes < encoded.transferred_bytes,
        "a shared texture is supposed to move fewer bytes than an encoded stream: {} vs {}",
        shared.transferred_bytes,
        encoded.transferred_bytes
    );

    // A compressed frame is close to a shared one. Three times is far looser than the millisecond
    // the run actually measures, because this is a floor on the finding rather than a benchmark.
    assert!(
        encoded.percentile(0.5) < shared.percentile(0.5).max(1_000) * 3,
        "an encoded frame cost {} µs against a shared texture's {} µs; that is no longer a \
         second-order effect and the recommendation would change",
        encoded.percentile(0.5),
        shared.percentile(0.5)
    );

    // And the uncompressed case is measured, not endorsed. A second is the line past which the
    // finding stops being "prefer a shared texture" and becomes "this transport does not work".
    assert!(
        uncompressed.percentile(0.99) < 1_000_000,
        "an uncompressed 1080p transport reached {} µs",
        uncompressed.percentile(0.99)
    );
}
