# `cy-editor-viewport-transport`

The engine's rendered image, in the editor's window, with no copy through the CPU — and the
synchronisation that makes reading it safe while another process is still drawing into the ring it
came from.

Tasks 1.0.1, 1.0.1b, 1.0.1c, 1.0.1d, 1.0.2, 1.0.2b and 1.0.2c of M5.5.

## What it is

Two programs and one ring of images. The runtime allocates three or four `VkImage`s with
DRM-format-modifier tiling, exports each as a dma-buf descriptor, exports two timeline semaphores,
creates a shared page, and hands all of it to the editor over a unix socket with `SCM_RIGHTS`. From
then on the socket carries **liveness only**: per-frame state lives in the shared page under a
seqlock, and the socket's EOF is how the editor learns the runtime died.

| file | what it holds |
|---|---|
| `wire.rs` | the handshake and the descriptor passing |
| `announce.rs` | the shared page: the seqlock, the heartbeat, and the two reservation flags |
| `device.rs` | a wgpu device with `VK_KHR_external_semaphore_fd` enabled, which wgpu will not do itself |
| `image.rs` | importing the dma-bufs and the timelines |
| `session.rs` | the editor's side: what to show this frame, and how to be sure it is safe |
| `ring.rs` | the runtime's side: which image the next frame may go into |
| `publisher.rs` | the reference publisher, as a real second process |
| `probe.rs` | the headless editor, so the window and the tests run the same code |

## Running it

```
just build-editor --profile release
build/<label>/editor/shipping/cy-viewport-publisher --buffers 4 --rate 240 &
build/<label>/editor/shipping/cy-viewport-transport-probe --seconds 5 --rate 60 --verify
```

`--extensions` reports what wgpu enables with and without the patch, and is the first thing to run
when the viewport shows nothing.

## The measurements

All on this project's reference machine — NVIDIA RTX 5060, Vulkan 1.4.312, Linux — in the
**shipping** profile, 1920×1080, four images, editor at 60 Hz. The publisher writes five full-image
clears per frame (`--heavy 4`), which is a genuinely slow write and therefore a wide, honest window
for a reader to be caught in.

### The device

```
wgpu enables by itself: VK_KHR_swapchain VK_KHR_swapchain_mutable_format VK_EXT_robustness2
                        VK_KHR_external_memory_fd VK_EXT_external_memory_dma_buf
                        VK_EXT_image_drm_format_modifier VK_EXT_memory_budget
missing without patching: VK_KHR_external_semaphore_fd
we added:                 VK_KHR_external_semaphore_fd
VK_KHR_external_semaphore_fd enabled on this device: true
```

The same binary run with `--no-patch` reports `false`, which is the control that makes "the patch is
what does it" a measurement rather than a claim.

### The image, and why `width * height * 4` is the wrong number

```
1920x1080  modifier 0x300000000606014  stride 7680  allocation 8847360 B
```

`1920 * 1080 * 4` is 8,294,400. The driver's allocation is **6.7% larger**, and importing against
the smaller number is refused in a way that reads as a driver defect. `DRM_FORMAT_MOD_LINEAR` is not
among this device's usable modifiers at all.

### Latency and freshness, at runtime rates an engine actually produces

| runtime | frames shown | latency mean | p95 | p99 | torn | wrong |
|---|---|---|---|---|---|---|
| 60 Hz | 300 of 300 | 11.4 ms | 11.5 | 13.5 | 0 | 0 |
| 240 Hz | 300 of 300 | 2.88 ms | 3.07 | 4.54 | 0 | 0 |
| 1000 Hz | 300 of 300 | 1.07 ms | 1.51 | 3.53 | 0 | 0 |
| free-running (≈20,000 fps) | 300 of 300 | 1.30 ms | 3.63 | 5.12 | 0 | 2–7% older |

Latency is `vkQueueSubmit` in the runtime to `queue.submit` in the editor, measured on
`CLOCK_MONOTONIC` in both processes.

**The last row is the honest one and is worth reading carefully.** With the runtime free-running at
twenty thousand frames a second — 300 times faster than any engine renders — 2% to 7% of editor
frames show a *whole frame that is a few frames older than the announcement claimed*. Never a torn
image, and never a **newer** one: the check distinguishes the two, and the newer count is zero in
every run. An older image means the editor's read of shared memory had not yet seen a write the
timeline said was finished, which is the visibility cost of waiting on the host instead of on the
queue. A newer or torn image would mean the runtime had overwritten a slot the editor had claimed,
which is the failure this protocol exists to prevent and which no run has produced.

### How many images, and what happens when there are not enough

Runtime free-running, editor at 60 Hz:

| images | runtime fps | dropped on a full ring | editor latency | what happened |
|---|---|---|---|---|
| 1 | **0.2** | 33,171,247 | 2,037 ms | wedges: two frames in nine seconds, and the editor reports the runtime wedged |
| 2 | 12,600 | 30,164,369 | 16.8 ms | throttled to lockstep: **a whole editor frame of latency**, exactly as the design predicted |
| 3 | 18,000 | 343,665 | 1.22 ms | pipelines |
| 4 | 20,700 | 0 | 1.30 ms | never blocked and never dropped |

Three is the minimum, four is preferred, and **the runtime drops rather than blocks** — the editor
must never throttle the runtime. `Ring::advisory` says out loud what a ring of one or two costs, so
a two-image ring cannot look like a working one.

### Surviving the runtime

SIGKILL to the publisher, mid-frame, with the editor attached:

```
[probe] the runtime is Gone at 3.55 s — The runtime is no longer running. This is the last frame
        it produced. — and the editor carries on
[probe] SURVIVED the runtime going away at 3.55 s: 324 further editor frames in 5.46 s = 59.4 fps
[probe] done
```

The editor keeps its frame rate, freezes on the last complete frame, says which of the two deaths it
was, and **exits cleanly** — the last part matters, because wgpu's teardown calls `vkDeviceWaitIdle`,
which is the call a wedged queue never returns from. The process exiting zero is the evidence that
nothing wedged. `tests/across_a_process_boundary.rs` is that run, automated.

## The five decisions, and where they are written down

Each is a measurement rather than a preference; `openspec/changes/implement-m5b-operable/design.md`
has the reasoning and the code has the invariant beside the line that would break it.

1. **Timeline semaphores over `OPAQUE_FD`**, imported into a device patched through
   `Adapter::open_with_callback`. `SYNC_FD` is binary-only. The capability check asks whether the
   **extension was enabled**, never whether a function pointer is non-null — ash installs a
   *panicking stub*, so the pointer test says yes without the extension and the call then aborts the
   process. (`device.rs`)
2. **A bounded host wait, never a wait on the editor's queue.** There is no policy variant that
   stages one, and the absence is deliberate: an unsatisfiable queue wait renders nothing, times out
   every later independent submission, and hangs shutdown forever. (`session.rs`)
3. **Three images minimum, four preferred, and the runtime drops on a full ring.** (`ring.rs`)
4. **Announce after `vkQueueSubmit`, never before.** Surviving a runtime SIGKILL depends entirely on
   it: every value the editor can wait on is already submitted and will signal even though the
   process is gone. (`announce.rs`, and the comment sits on the `publish` call in `publisher.rs`)
5. **Per-frame state in a shared page under a seqlock, and `held = (frame_id, slot)` flowing editor
   → runtime.** A partial socket write desynchronises the reader, which then waits on a garbage
   timeline value — the one failure here that has no recovery. (`announce.rs`)

### A sixth, found by running this rather than the spike

The spike's confirm-and-retry — claim a slot, then check that no newer frame has been announced —
has a window. A runtime announces only *after* `vkQueueSubmit`, so between choosing a slot and
announcing the frame it wrote there it is invisible, and a claim confirmed inside that window is a
claim on an image already being overwritten. At the 240 Hz the spike paced its runtime at, the
window is never hit; free-running at 25,000 fps it measured **2.7% of frames at three images and
3.7% at four holding the wrong frame**.

The fix is a two-flag reservation: the runtime stores the slot it is about to write and then reads
the editor's claim; the editor stores its claim and then reads the runtime's. Both `SeqCst`, so in
any total order at least one of them sees the other, and they never both proceed. Backing off is
free on both sides — the runtime drops a frame it was entitled to drop, and the editor repeats one
it was already showing. After it, no run has produced a newer-frame or torn read at any rate.

## What this crate deliberately is not

**Not a renderer.** It hands back a `wgpu::Texture` and a `PresentedFrame`; what draws them is the
one render crate, and `cy-editor-app/tests/containment.rs` fails if any crate but that one and this
one names a graphics API.

**Not portable, and honest about it.** dma-buf, `memfd` and `OPAQUE_FD` are Linux's. On macOS and
Windows the crate compiles to a constant, so the workspace still builds and tests on three
platforms, and the viewport treats "there is no transport" exactly as it treats a runtime that has
not started yet.

**Not the engine's publisher.** `publisher.rs` is a reference implementation: it draws a tagged test
pattern, not a scene. The engine's render server will publish against this same wire format, and
this file is the executable description of what it must do — including the ordering a paragraph
would state and a compiler would not check.
