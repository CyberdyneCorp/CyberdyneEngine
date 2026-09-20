# `src/backends/viewport` — the engine's side of the editor's viewport

The pixel half of the viewport transport: the engine's rendered frames, in memory the editor
imports rather than copies. Linux uses dma-buf/Vulkan and macOS uses IOSurface/Metal.

## What was missing, in one paragraph

M6 delivered the *control* half and said so in its own header.
`src/servers/render/viewport_transport.h` carries the frame's identity, the view state it was
rendered with, its pacing and its degradation, and states that "the bytes … are the business of the
module that owns a device". Nothing under `src/` owned one, so the only thing on the far end of the
editor's transport was `cy-viewport-publisher` — a Vulkan fixture in the editor's own Cargo
workspace that clears an image to a colour and moves a white bar. Six milestones of editor work, and
the viewport had never shown anything the engine drew.

This module is that device-owning module.

## The wire is the editor's, and the editor did not change

`editor/crates/cy-editor-viewport-transport/` is the consumer. Both native implementations share
the handshake, announcement page, frame identities, generations, heartbeat and held/writing slot
ownership. Their native handles and completion mechanisms differ:

| | |
|---|---|
| socket | one Unix stream, one message at connection: the `Handshake` and every descriptor |
| Linux descriptors | `buffer_count` dma-bufs, two OPAQUE_FD timeline semaphores, then the announcement-page memfd |
| macOS descriptors | one shared-page descriptor; IOSurface IDs occupy the handshake plane handle fields |
| per frame | a 4 KiB shared page under a seqlock, plus a heartbeat and the two-flag slot reservation |
| format | `R8G8B8A8_UNORM`; Linux carries the DRM modifier and macOS carries IOSurface row/allocation sizes |
| completion | Linux announces timeline values; macOS announces only after `MTLCommandBuffer` completion |

Proven rather than asserted: `cy-viewport-transport-probe`, the editor's own headless consumer,
imports this publisher's ring and shows **292 distinct runtime frames of 292 announced**, zero
malformed and zero of the wrong generation, at 1280x720 with modifier `0x300000000606014`.

On an **Apple M3 Pro**, macOS 27.0, the Metal probe imported a four-IOSurface 960x540 ring in
11.169 ms on its first connection and 1.941 ms on reconnect. Two three-second consumers received
180 and 181 distinct nonzero frames with zero skipped frames and zero bounded-wait timeouts. The
runtime reported two connections, zero full-ring drops, and zero ownership vetoes.

## The ordering that makes a killed engine survivable

**Announce after `vkQueueSubmit`, never before.** Every value the editor can be waiting on is
already on a queue by the time the editor can see the frame that names it, so the driver signals it
even if this process is destroyed in the next instant. Announce first and a kill in between leaves
the editor waiting on a value nothing will ever reach — with the bounded host wait, a viewport that
never updates again. `Publisher::publish` is the only function that could break it and the note is
beside the call.

## The frame arrives as host pixels, and what that costs

**Stated rather than discovered.** This publisher owns its own Vulkan device and the engine's
renderer draws on the RHI's. So a frame reaches the editor as: the engine renders on the GPU, reads
the colour target back into host memory (the path `samples/03-first-light --capture` and
`render.golden` already use), and this module uploads those bytes into the shared image. The pixels
are the engine's rendered world; the transport to the editor is genuinely zero-copy; the
engine-to-publisher step is not.

The reason is a boundary rather than an oversight: creating an image whose memory is exportable as a
dma-buf needs `VK_EXT_image_drm_format_modifier` and `VK_KHR_external_memory_fd` **at image
creation**, and `cy::rhi::Device` has no way to ask for either — `TextureDescription` has no
"shareable" and there is no accessor for the underlying `VkImage`, deliberately, because a renderer
that could reach one could branch on a backend. Making it zero-copy end to end is one flag on that
description and one export entry point on the Vulkan backend, both in `src/backends/rhi/vulkan/`.

Measured at 1280x720, over a 35-second editor session: **136 ms of host copy across 1281 published
frames — 106 µs a frame**, against a 16.7 ms frame. `PublisherStatistics::upload_micros` is what
that number comes from, so the decision to remove it is made against a measurement.

## Two Vulkan devices in one process, and the one hazard in that

`src/backends/rhi/vulkan/` also uses volk, and it calls `volkLoadInstanceOnly` and `volkLoadDevice`
— which write volk's **process-wide** dispatch tables. This module creates a second instance and a
second device in the same process, so it calls neither: instance entry points are resolved by hand
into a private table and device ones into a `VolkDeviceTable` of its own. Calling `volkLoadDevice`
here would dispatch the *renderer's* calls through this module's device, which works by accident on
a driver whose device functions do not vary by device and does not on one with a layer chain.

## What is here

| file | what |
|---|---|
| `include/cy/backends/viewport/publisher.h` | the whole public surface, naming no Vulkan type |
| `src/wire.h` | the handshake, the announcement page and the ring's slot rule — **no Vulkan**, so every decision in them is testable on a machine with no GPU |
| `src/wire.cpp` | those, implemented |
| `src/publisher.cpp` | Linux Vulkan/dma-buf publisher |
| `src/publisher_metal.mm` | macOS Metal/IOSurface publisher |

## What is not

**The rendering.** This module never draws; it is handed finished pixels, and a module that also
decided what was in them would be a second renderer.

**The editor protocol.** Picking, gizmo intent and transactions travel on a different socket
(`src/runtime/editor_bridge/`), because they are control and this is an image. The two are joined
only by the frame identifier they both carry.

**Windows.** No NT-handle implementation exists yet, so the module is not declared there.

**A hardened macOS broker.** The current wgpu 30 integration can import an IOSurface ID without an
XPC object, so the publisher uses deprecated global IOSurface lookup. The socket is mode 0600 and
same-user, but the IDs remain discoverable outside that socket. Replace the lookup with an XPC/Mach
port handoff before treating the transport as a hostile multi-user boundary.

## Tests

| suite | what it holds |
|---|---|
| `unit.viewport_publisher` | the layouts against the editor's `#[repr(C)]` numbers, the handshake's refusals, the seqlock, the slot rule, the release gate, the reclaim |
| `integration.viewport_publisher` | the two contended protocols — a concurrent writer against a reader, and the two-flag reservation — at the iteration counts it takes to hit the races the editor's own spike measured at a few per cent |
| `smoke.editor_window` | the whole thing, against the real editor, on a real display |
