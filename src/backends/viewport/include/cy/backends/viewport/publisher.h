#pragma once
// The viewport transport's PIXEL half, on the engine's side: the engine's rendered frames, in
// memory the editor imports rather than copies. M7 task 5b.1.
//
// ================================================================================================
// WHAT WAS MISSING, IN ONE PARAGRAPH
// ================================================================================================
//
// M6 delivered the CONTROL half and said so in its own header:
// `src/servers/render/viewport_transport.h` carries the frame's identity, the view state it was
// rendered with, its pacing and its degradation, and states that "the bytes … are the business of
// the module that owns a device". Nothing under `src/` owned one, so the only thing on the far end
// of the editor's transport was `cy-viewport-publisher` — a Vulkan fixture in the editor's own
// Cargo workspace that clears an image to a colour and moves a white bar. Six milestones of editor
// work and the viewport had never shown anything the engine drew.
//
// This module is that device-owning module. It is the module that turns
// `editor-viewport-and-gizmos`' "the viewport SHALL obtain its image through an abstract transport"
// from a shape into a picture.
//
// ================================================================================================
// THE WIRE IS THE EDITOR'S, AND THE EDITOR DOES NOT CHANGE
// ================================================================================================
//
// `editor/crates/cy-editor-viewport-transport/` is the consumer, and it is tested, measured and
// SIGKILL-proven against `cy_editor_viewport_transport::publisher` — the reference publisher, whose
// module comment says: "The engine's render server will grow a publisher of its own against this
// same wire format; this file is what it has to match." So this file MATCHES it rather than
// negotiating with it:
//
//   * a Unix socket, one message at connection carrying the `Handshake` and every descriptor:
//     `buffer_count` dma-bufs in slot order, then `render_done` and `release` as OPAQUE_FD timeline
//     semaphores, then the announcement page's `memfd`;
//   * a 4 KiB shared page holding the newest announcement under a seqlock, plus a heartbeat and the
//     two-flag slot reservation;
//   * `R8G8B8A8_UNORM` with an explicit DRM format modifier the driver chose — **not** linear,
//     which this hardware does not offer for a renderable image;
//   * every image handed over in `SHADER_READ_ONLY_OPTIMAL`.
//
// If the editor had to change to accept these frames, the wire would have been re-negotiated rather
// than implemented, and the six SIGKILL runs behind the editor's side would have to be re-earned.
//
// ================================================================================================
// THE ORDERING THAT MAKES A KILLED ENGINE SURVIVABLE
// ================================================================================================
//
// **Announce after `vkQueueSubmit`, never before.** Every value the editor can be waiting on is
// already on a queue by the time the editor can see the frame that names it, so the driver signals
// it even if this process is destroyed in the next instant. Announce first and a kill in between
// leaves the editor waiting on a value nothing will ever reach — with the bounded host wait, a
// viewport that never updates again. `publish()` is the only function that could break it and the
// note is beside the call.
//
// ================================================================================================
// WHY THE FRAME ARRIVES AS HOST PIXELS, AND WHAT THAT COSTS
// ================================================================================================
//
// STATED HERE RATHER THAN DISCOVERED. This publisher owns its own Vulkan device, and the engine's
// renderer draws on the RHI's. So a frame reaches the editor as: the engine renders on the GPU,
// reads the colour target back into host memory (the path `samples/03-first-light --capture` and
// `render.golden` already use), and this module uploads those bytes into the shared image. The
// pixels are the engine's rendered world; the transport to the editor is genuinely zero-copy; the
// engine-to-publisher step is not.
//
// The reason is a boundary rather than an oversight: creating an image whose memory is exportable
// as a dma-buf needs `VK_EXT_image_drm_format_modifier` and `VK_KHR_external_memory_fd` at image
// creation, and `cy::rhi::Device` has no way to ask for either — `TextureDescription` has no
// "shareable" and there is no accessor for the underlying `VkImage`. Making it zero-copy end to end
// is one flag on that description and one export entry point on the Vulkan backend, both of them in
// `src/backends/rhi/vulkan/`. At 1280x720 the copy is 3.7 MB each way; `Publisher::statistics`
// reports what it costs so the decision to fix it is made against a number.
//
// ================================================================================================
// WHAT IS DELIBERATELY NOT HERE
// ================================================================================================
//
// THE RENDERING. This module never draws. It is handed finished pixels, and a module that also
// decided what was in them would be a second renderer.
//
// THE EDITOR PROTOCOL. Picking, gizmo intent and transactions travel on a different socket, because
// they are control and this is an image. `samples/05b-editor-window/runtime/` owns that half.
//
// LINUX. dma-buf, `memfd` and `SCM_RIGHTS` are Linux, and so is the editor's side. On every other
// platform `create()` refuses with a sentence saying so rather than compiling to something that
// cannot work.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/ownership.h>

namespace cy::viewport {

/// How many images the ring may hold. Four, matching `cy_editor_viewport_transport::wire`.
inline constexpr u32 kMaxBuffers = 4;
/// The fewest that pipeline. Below this the engine and the editor contend for one image.
inline constexpr u32 kMinimumBuffers = 3;

struct PublisherOptions {
    /// Where the editor connects. `CY_VIEWPORT_SOCKET` is what the editor reads, so a host normally
    /// passes the same value it puts in that variable.
    const char* socket_path = "/tmp/cy-viewport.sock";
    u32 width = 1280;
    u32 height = 720;
    /// 1 to `kMaxBuffers`. Four is preferred; three is the minimum that pipelines, and one and two
    /// are accepted and reported on, because a measurement wants them.
    u32 buffers = kMaxBuffers;
    /// A substring of the adapter's name, so a machine with two GPUs can be told which. Empty means
    /// the first one — and it must be **the same device the editor picked**, because a dma-buf
    /// crossing two GPUs is an import the driver refuses.
    const char* adapter = "";
    /// Enable `VK_LAYER_KHRONOS_validation` when the layer is installed.
    ///
    /// ON BY DEFAULT, and absent rather than fatal: a machine with no validation layer runs without
    /// one and says so. External-memory and DRM-modifier images are exactly the corner of Vulkan
    /// where a wrong usage flag or a missing dedicated allocation produces a working-looking image
    /// on one driver and a refused import on the next, and the layer is what says which.
    bool validation = true;
};

/// Where this frame's pixels go: a host-visible staging surface, this frame's alone.
///
/// `row_pitch_bytes` is the staging buffer's, not the shared image's. The image is tiled by its DRM
/// modifier and the copy is the driver's; a caller that wrote at the image's pitch would be writing
/// at a pitch it cannot know and does not need.
struct FrameStaging {
    u8* pixels = nullptr;
    u32 width = 0;
    u32 height = 0;
    u32 row_pitch_bytes = 0;
    /// Which ring slot this frame will be published into. For a log; nothing else needs it.
    u32 slot = 0;

    [[nodiscard]] bool valid() const noexcept { return pixels != nullptr; }
};

/// What the publisher has done, for a report that is a number rather than an impression.
struct PublisherStatistics {
    u64 published = 0;
    /// Frames given up because every image was spoken for. **The editor must never throttle the
    /// engine**, so a full ring is this frame's problem and not the editor's.
    u64 dropped_full_ring = 0;
    /// Frames given up because the editor claimed the slot that had been chosen, in the window
    /// between choosing it and declaring it. Separate from the above because they mean different
    /// things: a full ring is normal, and this should be rare.
    u64 vetoed = 0;
    /// Editors that connected. More than one means the editor was restarted against a live engine,
    /// which is a case the ring has to survive.
    u64 connections = 0;
    /// Microseconds spent inside `publish()`, summed. The cost of the host copy this module's
    /// header explains, so that removing it is decided against a measurement.
    u64 upload_micros = 0;
};

/// The engine's half of the viewport transport.
///
/// NOT THREAD-SAFE. One thread renders, stages and publishes, exactly as `cy::render::
/// ViewportTransport` is one thread's. A host that renders on one thread and publishes on another
/// passes the frame between them through its own queue.
///
/// A concrete class over an opaque implementation rather than an abstract interface, because there
/// is exactly one implementation and there is no second one to write: the transport's other end is
/// a specific Linux mechanism, and a second "publisher" would be a second wire format. The pointer
/// keeps every Vulkan type out of this header, which is what
/// `tools/layercheck/layercheck.py`'s `gpuapi` check requires of everything above `src/backends/`.
class Publisher {
public:
    ~Publisher();

    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;

    /// Create a publisher, its ring and its socket.
    ///
    /// Fails, naming the missing piece, when there is no Vulkan loader, no device that can export a
    /// single-plane DRM-modifier image, or no socket to bind. Every one of those is a machine this
    /// artefact cannot run on, and each gets its own sentence: "no GPU" and "this GPU cannot export
    /// a dma-buf" send a reader to two different places.
    [[nodiscard]] static Expected<UniquePtr<Publisher>, Error> create(
        const PublisherOptions& options) noexcept;

    /// Accept a waiting editor, or notice that the one we had has gone. Called once per frame.
    ///
    /// A disconnected editor gets the whole ring back immediately. Without that, a three-image ring
    /// is exhausted within three frames by claims nobody will ever release, and the engine stops
    /// rendering because something else stopped watching.
    void service() noexcept;

    /// Whether an editor is connected right now.
    [[nodiscard]] bool has_client() const noexcept;

    /// Reserve an image for the next frame and hand back where to write it.
    ///
    /// An INVALID staging surface means every image is spoken for and this frame is dropped, which
    /// is the engine's right and never the editor's problem. It is not an error and is not reported
    /// as one; `statistics().dropped_full_ring` counts it.
    [[nodiscard]] FrameStaging begin_frame() noexcept;

    /// Upload the staged pixels, submit, and announce the frame. Returns its identity.
    ///
    /// Refused when `begin_frame` did not hand out a slot, because publishing without one would
    /// announce an image nothing wrote.
    [[nodiscard]] Expected<u64, Error> publish() noexcept;

    /// The name of the adapter this publisher is on. The editor prints its own; when a dma-buf
    /// import fails, the two lines beside each other are the diagnosis.
    [[nodiscard]] const char* adapter_name() const noexcept;

    /// The DRM format modifier the driver chose, for the same reason.
    [[nodiscard]] u64 modifier() const noexcept;

    [[nodiscard]] const PublisherStatistics& statistics() const noexcept;

    /// The implementation this object owns, forward-declared so no Vulkan type reaches this
    /// header. Public only because `make_unique` constructs a `Publisher`, and the only caller
    /// that can produce an `Impl*` is `create`, in the one translation unit where `Impl` is a
    /// complete type.
    struct Impl;
    explicit Publisher(Impl* impl) noexcept : impl_(impl) {}

private:
    Impl* impl_ = nullptr;
};

/// What is wrong with a ring of this size, or null when there is nothing to say.
///
/// Measured by M5.5's synchronisation spike: one image wedges, two throttle the engine to the
/// editor's refresh rate and cost a whole editor frame of latency, three is where pipelining
/// starts, four removes the last stalls. Said out loud rather than silently accepted, so a
/// two-image ring does not look like a working one.
[[nodiscard]] const char* ring_advisory(u32 buffers) noexcept;

}  // namespace cy::viewport
