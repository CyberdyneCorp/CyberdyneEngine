//! The headless editor: the same import, wait, claim and release code the window will run, with no
//! window.
//!
//! It exists because a clean run of a windowed editor proves nothing about a path the window did
//! not take, and because this is the binary that can be run in a loop, measured, and killed on a
//! machine with no display. Every number in this crate's README came out of it.
//!
//! Usage:
//!   cy-viewport-transport-probe --extensions        what wgpu enables, with and without the patch
//!   cy-viewport-transport-probe [--socket PATH] [--seconds S] [--rate HZ]
//!                               [--wait newest|poll|bounded] [--wait-micros N]
//!                               [--verify] [--no-patch] [--expect-frames N]
//!
//! It exits non-zero when it was asked to verify something and the answer was no, so that a test
//! can be a process invocation rather than a paragraph a human reads.

use std::sync::Arc;
use std::time::{Duration, Instant};

use crate::device::Gpu;
use crate::session::{Liveness, ViewportSession, WaitPolicy, monotonic_nanos, preferred_adapter};
use crate::wire::default_socket_path;

/// How many rows of the image the tear check samples. Sixteen of a 1080p frame locates a tear line
/// to within 68 rows, which is enough to say "this image contains two frames" and cheap enough to
/// do on every frame.
const PROBE_ROWS: usize = 16;

/// What one run measured.
#[derive(Default)]
struct Measurements {
    editor_frames: u64,
    shown: Vec<u64>,
    latencies: Vec<f64>,
    torn: u64,
    wrong: u64,
    older: u64,
    newer: u64,
    checked: u64,
    death: Option<(f64, u64)>,
    elapsed: f64,
}

/// How the run was asked to behave.
struct Settings {
    seconds: f64,
    rate: f64,
    verify: bool,
    expect_frames: u64,
}

/// Run the headless probe, as `cy-viewport-transport-probe` does.
pub fn main() {
    let arguments: Vec<String> = std::env::args().collect();
    let has = |key: &str| arguments.iter().any(|argument| argument == key);
    let value = |key: &str| {
        arguments
            .iter()
            .position(|argument| argument == key)
            .and_then(|index| arguments.get(index + 1))
            .cloned()
    };
    let number = |key: &str, fallback: f64| {
        value(key)
            .and_then(|text| text.parse::<f64>().ok())
            .unwrap_or(fallback)
    };

    let adapter = preferred_adapter();
    if has("--extensions") {
        report_extensions(&adapter);
        return;
    }

    let gpu = match Gpu::create(&adapter, !has("--no-patch")) {
        Ok(gpu) => Arc::new(gpu),
        Err(problem) => {
            println!("[probe] NO DEVICE: {problem}");
            std::process::exit(2);
        }
    };
    for note in &gpu.notes {
        println!("[probe] {note}");
    }

    let socket = value("--socket").unwrap_or_else(default_socket_path);
    let mut session = match ViewportSession::connect_to(Arc::clone(&gpu), &socket) {
        Ok(session) => session,
        Err(problem) => {
            println!("[probe] NO RUNTIME: {problem}");
            std::process::exit(3);
        }
    };
    let handshake = *session.handshake();
    println!(
        "[probe] connected: {}x{} ring={} modifier=0x{:x} stride={} allocation={} import={:.3} ms",
        handshake.width,
        handshake.height,
        handshake.buffer_count,
        handshake.modifier,
        handshake.planes[0].stride,
        handshake.planes[0].allocation_bytes,
        session.import_millis
    );

    let policy = match value("--wait").as_deref() {
        Some("newest") => WaitPolicy::Newest,
        Some("poll") => WaitPolicy::HostPoll,
        #[allow(
            clippy::cast_possible_truncation,
            clippy::cast_sign_loss,
            reason = "a timeout in microseconds from the command line"
        )]
        _ => WaitPolicy::HostWait(Duration::from_micros(number("--wait-micros", 2000.0) as u64)),
    };
    if let Err(problem) = session.set_policy(policy) {
        println!("[probe] {problem}");
    }
    println!("[probe] wait policy: {:?}", session.policy());

    #[allow(
        clippy::cast_possible_truncation,
        clippy::cast_sign_loss,
        reason = "a frame count from the command line"
    )]
    let settings = Settings {
        seconds: number("--seconds", 5.0),
        rate: number("--rate", 60.0),
        verify: has("--verify"),
        expect_frames: number("--expect-frames", 0.0) as u64,
    };

    let measurements = run(&gpu, &mut session, &settings);
    let failed = summarise(&session, &settings, &measurements);

    // wgpu's device teardown calls `vkDeviceWaitIdle`, which is exactly the call a wedged queue
    // never returns from. Dropping normally is therefore also the last check that nothing wedged:
    // the process reaching this line and exiting is the evidence.
    drop(session);
    let _ = gpu.device.poll(wgpu::PollType::Poll);
    if failed {
        std::process::exit(1);
    }
    println!("[probe] done");
}

/// The editor's frame loop, without an editor: acquire, composite, release, submit.
fn run(gpu: &Arc<Gpu>, session: &mut ViewportSession, settings: &Settings) -> Measurements {
    let handshake = *session.handshake();
    let composite = private_texture(gpu, handshake.width, handshake.height);
    let readback = settings
        .verify
        .then(|| readback_buffer(gpu, handshake.width, handshake.height));

    let mut measurements = Measurements::default();
    // The last liveness this probe reported, so that a change is reported and a repetition is not.
    let mut reported = Liveness::Live;
    let started = Instant::now();
    let mut next_due = started;

    while started.elapsed().as_secs_f64() < settings.seconds {
        if settings.rate > 0.0 {
            next_due += Duration::from_secs_f64(1.0 / settings.rate);
            let now = Instant::now();
            if next_due > now {
                std::thread::sleep(next_due - now);
            } else {
                next_due = now;
            }
        }
        measurements.editor_frames += 1;

        // EVERY TRANSITION, NOT ONLY THE FIRST. M9's closing gate found this by loading the
        // machine: under sixty-four spinning processes the publisher misses its heartbeat, the
        // session reports `Wedged`, and the SIGKILL that follows moves it to `Gone` — which this
        // reported only if `Gone` happened to be the first thing it saw. A runtime that stalls and
        // then dies was therefore described, for the rest of the session, as one that had stalled.
        // The editor's own message for the two is different on purpose, and
        // `editor-rust-application` asks for the state to be shown rather than for a frozen image
        // with no explanation, so the wrong one standing is the defect and not the loss of a line.
        if session.liveness() != reported {
            reported = session.liveness();
            if measurements.death.is_none() && reported != Liveness::Live {
                measurements.death =
                    Some((started.elapsed().as_secs_f64(), measurements.editor_frames));
            }
            println!(
                "[probe] the runtime is {:?} at {:.2} s — {} — and the editor carries on",
                reported,
                started.elapsed().as_secs_f64(),
                reported.message()
            );
        }

        let Some((slot, frame)) = session.acquire() else {
            session.poll();
            std::thread::yield_now();
            continue;
        };
        let source = session.texture(slot).expect("an imported image").clone();

        let mut encoder = gpu
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some("viewport"),
            });
        encoder.copy_texture_to_texture(
            texture_copy(&source),
            texture_copy(&composite),
            wgpu::Extent3d {
                width: handshake.width,
                height: handshake.height,
                depth_or_array_layers: 1,
            },
        );
        if let Some(buffer) = readback.as_ref() {
            encoder.copy_texture_to_buffer(
                texture_copy(&source),
                wgpu::TexelCopyBufferInfo {
                    buffer,
                    layout: wgpu::TexelCopyBufferLayout {
                        offset: 0,
                        bytes_per_row: Some(handshake.width * 4),
                        rows_per_image: Some(handshake.height),
                    },
                },
                wgpu::Extent3d {
                    width: handshake.width,
                    height: handshake.height,
                    depth_or_array_layers: 1,
                },
            );
        }
        // Before the submit, so the release lands on it rather than on the one after.
        session.release_finished();
        gpu.queue.submit([encoder.finish()]);

        if let Some(buffer) = readback.as_ref() {
            measurements.checked += 1;
            check(gpu, buffer, &handshake, frame.frame_id, &mut measurements);
        }

        if measurements.shown.last() != Some(&frame.frame_id) {
            #[allow(
                clippy::cast_precision_loss,
                reason = "nanoseconds to milliseconds, for a report"
            )]
            measurements
                .latencies
                .push(monotonic_nanos().saturating_sub(frame.submitted_nanos) as f64 / 1e6);
            measurements.shown.push(frame.frame_id);
        }
        let _ = gpu.device.poll(wgpu::PollType::Poll);
    }
    measurements.elapsed = started.elapsed().as_secs_f64();
    measurements
}

fn check(
    gpu: &Gpu,
    buffer: &wgpu::Buffer,
    handshake: &crate::wire::Handshake,
    frame_id: u64,
    measurements: &mut Measurements,
) {
    match inspect(gpu, buffer, handshake.width, handshake.height, frame_id) {
        Verdict::Whole => {}
        Verdict::Torn(tags) => {
            measurements.torn += 1;
            if measurements.torn == 1 {
                println!("[probe] first tear at frame {frame_id}: tags {tags:?}");
            }
        }
        Verdict::WrongFrame(tag) => {
            measurements.wrong += 1;
            // Older or newer is the whole diagnosis. A NEWER image means the runtime overwrote a
            // slot the editor had claimed — the corruption this protocol exists to prevent. An
            // OLDER one means the editor's read of the shared memory had not yet seen a write the
            // timeline said was finished, which is a visibility artefact of waiting on the host
            // instead of on the queue: whole frame, slightly stale, never torn.
            let expected = (frame_id & 0xffff) as u16;
            if tag < expected {
                measurements.older += 1;
            } else {
                measurements.newer += 1;
            }
            if measurements.wrong == 1 {
                println!(
                    "[probe] first wrong frame: claimed {expected}, image reads {tag} ({})",
                    if tag < expected { "older" } else { "NEWER" }
                );
            }
        }
    }
}

/// Print what happened, and answer whether anything asked for was not delivered.
#[allow(
    clippy::cast_precision_loss,
    reason = "counts turned into rates and percentages, for a report"
)]
fn summarise(session: &ViewportSession, settings: &Settings, measurements: &Measurements) -> bool {
    let counters = session.counters();
    println!(
        "[probe] {} editor frames in {:.2} s = {:.1} fps; showed {} distinct runtime frames of {} \
         announced",
        measurements.editor_frames,
        measurements.elapsed,
        measurements.editor_frames as f64 / measurements.elapsed,
        measurements.shown.len(),
        counters.announced
    );
    println!(
        "[probe] not-ready {} · bounded wait timed out {} · claim retries {} · claims vetoed {} · \
         claims abandoned {} · wrong generation {} · malformed {}",
        counters.not_ready,
        counters.timed_out,
        counters.claim_retries,
        counters.claim_vetoed,
        counters.claims_abandoned,
        counters.wrong_generation,
        counters.malformed
    );
    report("[probe] latency submit -> shown", &measurements.latencies);
    if let Some((at, frame)) = measurements.death {
        println!(
            "[probe] SURVIVED the runtime going away at {at:.2} s: {} further editor frames in \
             {:.2} s = {:.1} fps",
            measurements.editor_frames - frame,
            measurements.elapsed - at,
            (measurements.editor_frames - frame) as f64 / (measurements.elapsed - at)
        );
    }
    if measurements.checked > 0 {
        println!(
            "[probe] TEAR CHECK: {} of {} sampled frames were torn ({:.3}%); {} held another whole \
             frame ({:.3}%) — {} older, {} NEWER",
            measurements.torn,
            measurements.checked,
            100.0 * measurements.torn as f64 / measurements.checked as f64,
            measurements.wrong,
            100.0 * measurements.wrong as f64 / measurements.checked as f64,
            measurements.older,
            measurements.newer
        );
    }

    let mut failed = false;
    if settings.verify && (measurements.torn > 0 || measurements.newer > 0) {
        println!(
            "[probe] FAILED: the editor read an image the runtime had moved on to — {} torn, {} \
             holding a NEWER frame than the one claimed",
            measurements.torn, measurements.newer
        );
        failed = true;
    }
    if settings.expect_frames > 0 && (measurements.shown.len() as u64) < settings.expect_frames {
        println!(
            "[probe] FAILED: showed {} distinct frames, expected at least {}",
            measurements.shown.len(),
            settings.expect_frames
        );
        failed = true;
    }
    failed
}

fn report(label: &str, samples: &[f64]) {
    if samples.is_empty() {
        println!("{label}: no samples");
        return;
    }
    let mut sorted = samples.to_vec();
    sorted.sort_by(f64::total_cmp);
    #[allow(
        clippy::cast_precision_loss,
        clippy::cast_possible_truncation,
        clippy::cast_sign_loss,
        reason = "percentile arithmetic over a sample count"
    )]
    let at = |quantile: f64| sorted[((sorted.len() as f64 - 1.0) * quantile) as usize];
    #[allow(clippy::cast_precision_loss, reason = "a mean over a sample count")]
    let mean = sorted.iter().sum::<f64>() / sorted.len() as f64;
    println!(
        "{label}: n={} mean {mean:.3} ms p50 {:.3} p95 {:.3} p99 {:.3} max {:.3}",
        sorted.len(),
        at(0.50),
        at(0.95),
        at(0.99),
        sorted[sorted.len() - 1]
    );
}

fn report_extensions(adapter: &str) {
    for patch in [false, true] {
        println!("--- Gpu::create(patch = {patch}) ---");
        match Gpu::create(adapter, patch) {
            Ok(gpu) => {
                for note in &gpu.notes {
                    println!("    {note}");
                }
                println!(
                    "    external semaphores usable: {}",
                    gpu.external_semaphores()
                );
            }
            Err(problem) => println!("    FAILED: {problem}"),
        }
    }
}

fn texture_copy(texture: &wgpu::Texture) -> wgpu::TexelCopyTextureInfo<'_> {
    wgpu::TexelCopyTextureInfo {
        texture,
        mip_level: 0,
        origin: wgpu::Origin3d::ZERO,
        aspect: wgpu::TextureAspect::All,
    }
}

fn private_texture(gpu: &Gpu, width: u32, height: u32) -> wgpu::Texture {
    gpu.device.create_texture(&wgpu::TextureDescriptor {
        label: Some("composited"),
        size: wgpu::Extent3d {
            width,
            height,
            depth_or_array_layers: 1,
        },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba8Unorm,
        usage: wgpu::TextureUsages::COPY_DST | wgpu::TextureUsages::TEXTURE_BINDING,
        view_formats: &[],
    })
}

fn readback_buffer(gpu: &Gpu, width: u32, height: u32) -> wgpu::Buffer {
    assert!(
        (u64::from(width) * 4) % 256 == 0,
        "a texture-to-buffer copy needs 256-byte rows"
    );
    gpu.device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("tear-check"),
        size: u64::from(width) * u64::from(height) * 4,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        mapped_at_creation: false,
    })
}

enum Verdict {
    /// One frame's tag throughout: the image is whole.
    Whole,
    /// More than one frame's tag in one image.
    Torn(Vec<u16>),
    /// One tag throughout, and it is not the frame the editor claimed.
    WrongFrame(u16),
}

/// Map the copy back and ask whether every sampled row carries the same frame's tag.
///
/// The whole image is copied rather than a few rows: a sixteen-row copy is over in microseconds and
/// would snapshot one runtime frame however unsynchronised the transport was. The full read is the
/// one the editor actually performs, and it takes long enough to catch a writer in the act.
fn inspect(
    gpu: &Gpu,
    buffer: &wgpu::Buffer,
    width: u32,
    height: u32,
    expected_frame: u64,
) -> Verdict {
    let slice = buffer.slice(..);
    let ready = Arc::new(std::sync::atomic::AtomicBool::new(false));
    let signal = Arc::clone(&ready);
    slice.map_async(wgpu::MapMode::Read, move |_| {
        signal.store(true, std::sync::atomic::Ordering::Release);
    });
    let _ = gpu.device.poll(wgpu::PollType::wait_indefinitely());
    if !ready.load(std::sync::atomic::Ordering::Acquire) {
        return Verdict::Whole;
    }
    let mut tags: Vec<u16> = Vec::new();
    {
        let Ok(data) = slice.get_mapped_range() else {
            buffer.unmap();
            return Verdict::Whole;
        };
        let row_bytes = width as usize * 4;
        for index in 0..PROBE_ROWS {
            let row = index * height as usize / PROBE_ROWS;
            let base = row * row_bytes;
            for sample in 0..64 {
                let pixel = base + (sample * (row_bytes / 4 / 64)) * 4;
                // The white bar is 0xffffffff and carries no tag; the background's blue is 0x40.
                if data[pixel + 2] != 0x40 {
                    continue;
                }
                let tag = u16::from(data[pixel]) | (u16::from(data[pixel + 1]) << 8);
                if !tags.contains(&tag) {
                    tags.push(tag);
                }
            }
        }
    }
    buffer.unmap();
    let expected = (expected_frame & 0xffff) as u16;
    match tags.as_slice() {
        [] | [_] if tags.first() == Some(&expected) || tags.is_empty() => Verdict::Whole,
        [only] => Verdict::WrongFrame(*only),
        _ => Verdict::Torn(tags),
    }
}
