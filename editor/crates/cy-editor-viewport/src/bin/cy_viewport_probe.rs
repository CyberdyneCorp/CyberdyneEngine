//! The frame-age probe: a stand-in runtime that publishes viewport frames at a fixed rate.
//!
//! Task 4.1's measurement needs a **real second process**, because the thing being measured is what
//! happens when a producer and a consumer run on independent clocks with a boundary between them.
//! Simulating that inside one process with two threads measures the scheduler; measuring it across a
//! pipe measures the transport.
//!
//! It writes framed [`cy_editor_viewport::transport::PresentedFrame`] encodings to standard output,
//! using `cy_editor_protocol`'s framing — the same length-and-checksum header the control path uses,
//! so the probe is not a second wire format.
//!
//! ```text
//!   cy-viewport-probe --frames 90 --interval-micros 16667 --payload-bytes 65536
//! ```
//!
//! `produced_micros` is taken from the **wall clock** rather than from a monotonic instant, because
//! the two processes have to agree about it and `Instant`'s origin is per process. Over the seconds a
//! measurement runs, the difference between the two clocks on Linux is not something this measures;
//! it is recorded here so the number is read for what it is.
//!
//! Not shipped, not linked into the editor: a fixture, in the same sense as
//! `cy-editor-testhost`'s `cy-runtime-stub`.

use std::io::Write;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use cy_editor_protocol::{FrameId, write_frame};
use cy_editor_viewport::state::ViewState;
use cy_editor_viewport::transport::{FrameImage, PresentedFrame};

fn main() {
    let settings = Settings::from_arguments(std::env::args().skip(1));
    let stdout = std::io::stdout();
    let mut out = stdout.lock();

    let payload = vec![0x5A_u8; settings.payload_bytes];
    let state = ViewState::new();
    let start = SystemTime::now();

    for index in 1..=settings.frames {
        // Pace against the START rather than against the previous frame, so a slow write does not
        // push every later frame late — the same reason a fixed-timestep loop accumulates from an
        // epoch rather than adding a delta each tick.
        let due = Duration::from_micros(settings.interval_micros * u64::from(index - 1));
        if let Some(remaining) = start
            .elapsed()
            .ok()
            .and_then(|elapsed| due.checked_sub(elapsed))
        {
            std::thread::sleep(remaining);
        }

        let image = if settings.payload_bytes == 0 {
            FrameImage::SharedTexture {
                handle: 0x00C0_FFEE,
                bytes: 1920 * 1080 * 4,
            }
        } else {
            FrameImage::Encoded(payload.clone())
        };
        let frame = PresentedFrame::new(
            FrameId::from_raw(u64::from(index)),
            state.clone(),
            image,
            wall_clock_micros(),
        );
        if write_frame(&mut out, &frame.encode()).is_err() {
            // The consumer went away. That is a normal end for a fixture, not a failure.
            return;
        }
    }
    let _ = out.flush();
}

/// Microseconds since the epoch, which is the one clock two processes share.
fn wall_clock_micros() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |since| {
            u64::try_from(since.as_micros()).unwrap_or(u64::MAX)
        })
}

struct Settings {
    frames: u32,
    interval_micros: u64,
    payload_bytes: usize,
}

impl Settings {
    /// Parse `--name value` pairs, ignoring anything unrecognised.
    ///
    /// Deliberately forgiving: this is a fixture invoked by one test, and a strict parser here would
    /// turn a typo in a measurement into an unexplained empty result.
    fn from_arguments(arguments: impl Iterator<Item = String>) -> Self {
        let mut settings = Self {
            frames: 90,
            interval_micros: 16_667,
            payload_bytes: 0,
        };
        let collected: Vec<String> = arguments.collect();
        for pair in collected.windows(2) {
            let value = pair[1].as_str();
            match pair[0].as_str() {
                "--frames" => settings.frames = value.parse().unwrap_or(settings.frames),
                "--interval-micros" => {
                    settings.interval_micros = value.parse().unwrap_or(settings.interval_micros);
                }
                "--payload-bytes" => {
                    settings.payload_bytes = value.parse().unwrap_or(settings.payload_bytes);
                }
                _ => {}
            }
        }
        settings
    }
}
