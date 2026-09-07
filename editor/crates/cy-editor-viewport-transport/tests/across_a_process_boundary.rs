//! The transport, run rather than described: two processes, one ring of dma-bufs, and the two
//! properties that cost the most to get wrong.
//!
//! Both tests spawn the reference publisher and the headless probe as real programs, because that
//! is the only configuration in which any of this is true. A fixture in this process would share an
//! address space, a device and a scheduler with the thing it is testing, and would pass over every
//! defect the design exists to prevent.
//!
//! --- SKIPPING, LOUDLY -----------------------------------------------------------------------------
//!
//! They need a Vulkan device that can export dma-bufs and timeline semaphores, which continuous
//! integration does not have. Where there is none they print why and pass, in the manner
//! `tests/render/` established — a skipped test that prints nothing is a test nobody notices has
//! stopped running.

// The transport is Linux's; on the other two platforms there is nothing to run and this file
// compiles to an empty test binary rather than a failure.
#![cfg(target_os = "linux")]

use std::io::Read as _;
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

const PUBLISHER: &str = env!("CARGO_BIN_EXE_cy-viewport-publisher");
const PROBE: &str = env!("CARGO_BIN_EXE_cy-viewport-transport-probe");

/// A publisher, and the socket it listens on — or `None` when this machine cannot run one.
#[allow(
    clippy::zombie_processes,
    reason = "a returned child is killed and waited on by `finish`; the two paths that return \
              None have already reaped or killed it"
)]
fn publisher(socket: &str, extra: &[&str]) -> Option<Child> {
    let _ = std::fs::remove_file(socket);
    let mut command = Command::new(PUBLISHER);
    command
        .args(["--socket", socket])
        .args(extra)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    let mut child = command.spawn().expect("the publisher binary exists");

    // Wait for it to bind, or to die trying.
    let deadline = Instant::now() + Duration::from_secs(20);
    while Instant::now() < deadline {
        if std::path::Path::new(socket).exists() {
            return Some(child);
        }
        if let Some(status) = child.try_wait().expect("the child is waitable") {
            let mut errors = String::new();
            if let Some(stream) = child.stderr.as_mut() {
                let _ = stream.read_to_string(&mut errors);
            }
            eprintln!(
                "SKIPPED: the reference publisher could not start ({status}). This test needs a \
                 Vulkan device that exports dma-bufs and timeline semaphores.\n{errors}"
            );
            return None;
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    let _ = child.kill();
    eprintln!("SKIPPED: the reference publisher never bound {socket}");
    None
}

fn finish(mut child: Child) {
    let _ = child.kill();
    let _ = child.wait();
}

#[test]
fn the_editor_reads_whole_frames_while_the_runtime_writes_the_others() {
    // The property: with timeline semaphores, a claimed slot and a released one, the editor never
    // samples an image the runtime is still writing. `--heavy` widens the runtime's write window
    // with extra full-image clears carrying the previous frame's tag, so that a failure would show
    // up as two tags in one image rather than as nothing at all.
    let socket = "/tmp/cy-viewport-test-whole.sock";
    let Some(child) = publisher(
        socket,
        &["--buffers", "4", "--seconds", "20", "--heavy", "4"],
    ) else {
        return;
    };

    let output = Command::new(PROBE)
        .args(["--socket", socket])
        .args(["--seconds", "4", "--rate", "60", "--verify"])
        .args(["--expect-frames", "60"])
        .output()
        .expect("the probe runs");
    finish(child);

    let report = String::from_utf8_lossy(&output.stdout);
    assert!(
        output.status.success(),
        "the editor read a torn or wrong frame:\n{report}{}",
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(
        report.contains("TEAR CHECK: 0 of"),
        "the tear check must have run and found nothing:\n{report}"
    );
    assert!(
        report.contains("VK_KHR_external_semaphore_fd enabled on this device: true"),
        "the editor's device must have the extension wgpu does not enable:\n{report}"
    );
}

#[test]
fn the_editor_samples_the_runtimes_image_in_a_layout_the_validation_layer_agrees_with() {
    // A REGRESSION TEST for a defect that was invisible without the layer on: the imported image was
    // declared to wgpu as already being in `SHADER_READ_ONLY_OPTIMAL`, so no barrier was emitted and
    // every sample read an image whose layout on *this* device was still `UNDEFINED`. Fourteen
    // seconds of the window produced 2,759 validation errors and a picture that looked perfect.
    //
    // The check is the layer's own verdict, because that is the only thing that can see it. Where
    // the layer is not installed the test says so and passes, in the same manner as the two beside
    // it — a machine with no validation layer is a machine that cannot answer the question.
    //
    // **It is a guard rather than a reproduction, and the difference is worth stating.** Reverting
    // the fix and running this configuration produced the error 2 times out of 2 in the *shipping*
    // profile and 0 out of 2 in `dev`: the probe has to sample a slot before the runtime has
    // written it, and an unoptimised probe is too slow to manage it. The window, which samples four
    // slots for as long as it is open, produced 2,759 of them. So a regression will be caught here
    // in the shipping profile's run and by anybody who opens the editor with the layer on, and this
    // test passing in `dev` alone proves nothing.
    let socket = "/tmp/cy-viewport-test-layout.sock";
    // The rate is the reproducing one: a runtime that is already free-running when the editor
    // attaches is what puts a never-sampled slot in front of the first draw.
    let Some(child) = publisher(
        socket,
        &["--buffers", "4", "--rate", "240", "--seconds", "20"],
    ) else {
        return;
    };

    let output = Command::new(PROBE)
        .args(["--socket", socket])
        .args(["--seconds", "2", "--rate", "60", "--verify"])
        .env("VK_LOADER_LAYERS_ENABLE", "VK_LAYER_KHRONOS_validation")
        .output()
        .expect("the probe runs");
    finish(child);

    let report = String::from_utf8_lossy(&output.stdout);
    let complaints = String::from_utf8_lossy(&output.stderr);
    let whole = format!("{report}{complaints}");
    if !whole.contains("VK_KHR_external_semaphore_fd enabled on this device: true") {
        eprintln!("SKIPPED: the probe could not open a device that imports dma-bufs.\n{whole}");
        return;
    }
    if !whole.to_lowercase().contains("validation") && !output.status.success() {
        eprintln!("SKIPPED: the validation layer is not installed on this machine.");
        return;
    }
    assert!(
        !whole.contains("InvalidImageLayout"),
        "the editor sampled the runtime's image in the wrong layout:\n{whole}"
    );
    assert!(
        !whole.contains("Validation Error"),
        "the validation layer objected to how the editor reads the runtime's ring:\n{whole}"
    );
    assert!(
        output.status.success(),
        "the probe failed with the validation layer on:\n{whole}"
    );
}

#[test]
fn the_editor_survives_the_runtime_being_killed() {
    // `editor-rust-application`: a runtime failure must not terminate the editor. SIGKILL is the
    // hardest version — no unwinding, no cleanup, no chance to say goodbye — and it is survivable
    // only because the publisher announces AFTER `vkQueueSubmit`, so every value the editor can be
    // waiting on is already on a queue and will still be signalled.
    let socket = "/tmp/cy-viewport-test-kill.sock";
    let Some(mut child) = publisher(
        socket,
        &["--buffers", "4", "--seconds", "30", "--heavy", "8"],
    ) else {
        return;
    };

    let probe = Command::new(PROBE)
        .args(["--socket", socket])
        .args(["--seconds", "6", "--rate", "60"])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("the probe runs");

    std::thread::sleep(Duration::from_secs(2));
    child.kill().expect("SIGKILL");
    let _ = child.wait();

    let output = probe.wait_with_output().expect("the probe finished");
    let report = String::from_utf8_lossy(&output.stdout);
    assert!(
        output.status.success(),
        "the editor did not survive its runtime being killed:\n{report}{}",
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(
        report.contains("SURVIVED the runtime going away"),
        "the editor must keep drawing after the runtime dies:\n{report}"
    );
    assert!(
        report.contains("The runtime is no longer running"),
        "and it must say so rather than showing a frozen image with no explanation:\n{report}"
    );
    // The probe returning at all is the other half of the check: wgpu's teardown calls
    // `vkDeviceWaitIdle`, which is exactly the call that never returns from a wedged queue. A
    // process that exits zero has an unwedged one.
}
