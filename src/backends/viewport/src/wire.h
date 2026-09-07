#pragma once
// The viewport transport's wire, on the engine's side: the handshake, the shared announcement page,
// and the ring's slot rule. M7 task 5b.1.
//
// NONE OF THIS FILE NAMES VULKAN, and that is deliberate: everything here is a decision or a
// layout, and every one of them can be checked on a machine with no GPU. `unit.viewport_publisher`
// does exactly that — the seqlock, the two-flag reservation, the slot rule and the refusals are
// tested without a device, and only `publisher.cpp` needs one.
//
// It is the C++ counterpart of `cy_editor_viewport_transport::{wire, announce, ring}`, and it is a
// counterpart rather than a shared definition because the two ends are two languages. What keeps
// them in step is that the editor's side is the one that was measured and SIGKILL-proven, so this
// side matches it rather than the other way round, and `integration.viewport_publisher_wire`
// compares the layouts against the numbers the editor's `#[repr(C)]` produces.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <atomic>

namespace cy::viewport {

/// `CYVP`, so a connection to something that is not a viewport publisher fails at the first message
/// rather than as a nonsensical image size.
inline constexpr u32 kProtocolMagic = 0x4359'5650;

/// The wire format's version. A mismatch is refused naming both sides.
inline constexpr u32 kProtocolVersion = 1;

/// DRM fourcc for a Vulkan `R8G8B8A8_UNORM` image: `DRM_FORMAT_ABGR8888`.
inline constexpr u32 kFourccAbgr8888 = static_cast<u32>('A') | (static_cast<u32>('B') << 8) |
                                       (static_cast<u32>('2') << 16) |
                                       (static_cast<u32>('4') << 24);

/// One image's memory layout, as the driver's allocator reported it.
struct PlaneDescription {
    /// Bytes between the starts of two rows. **Not `width * 4`**: a modifier'd image is tiled and
    /// the driver's row pitch is the only correct answer.
    u64 stride = 0;
    u64 offset = 0;
    /// The ALLOCATION's size, from `vkGetImageMemoryRequirements`. The first spike that assumed it
    /// was `width * height * 4` imported an allocation the driver considered too small.
    u64 allocation_bytes = 0;
};

/// Everything the editor needs to reconstruct the ring, sent once with the descriptors.
///
/// Integers only and standard layout: it is written to the socket as bytes, and a layout that
/// depended on the compiler would be a protocol that depended on the compiler.
struct Handshake {
    u32 magic = kProtocolMagic;
    u32 version = kProtocolVersion;
    u32 width = 0;
    u32 height = 0;
    u32 fourcc = kFourccAbgr8888;
    u32 buffer_count = 0;
    /// Bumped whenever the ring is destroyed and rebuilt. The editor refuses an announcement from a
    /// generation it did not hand-shake, which is what stops a frame arriving for an image that no
    /// longer exists.
    u32 generation = 1;
    u32 has_timelines = 1;
    /// The modifier the driver actually chose. `DRM_FORMAT_MOD_LINEAR` is not available for a
    /// renderable image on this hardware, so a transport that assumed linear would not run at all.
    u64 modifier = 0;
    PlaneDescription planes[4]{};

    /// Refuse a handshake whose numbers cannot describe a ring. Each cause gets its own sentence.
    [[nodiscard]] Status validate() const noexcept;

    /// How many descriptors accompany a handshake of this shape.
    [[nodiscard]] u32 expected_descriptors() const noexcept;
};

static_assert(sizeof(PlaneDescription) == 24,
              "the editor's #[repr(C)] PlaneDescription is 24 bytes");
static_assert(sizeof(Handshake) == 136, "the editor's #[repr(C)] Handshake is 136 bytes");
static_assert(alignof(Handshake) == 8);

// --- The shared announcement page ---------------------------------------------------------------
//
// One 4 KiB `memfd`, mapped into both processes, carrying the newest frame under a seqlock.
//
// WHY NOT A MESSAGE PER FRAME. A stream socket has no message boundaries. A partial write leaves
// the reader one byte out of step, and a reader one byte out of step decodes the next bytes as an
// announcement and stages a wait on a GARBAGE TIMELINE VALUE — a value nothing will ever signal.
// Everything else in this design has a recovery; that one does not. A seqlock cannot desynchronise:
// a torn read is detected and retried, and the reader always sees the newest frame.

inline constexpr usize kPageBytes = 4096;

/// What the engine has produced, as it appears in the shared page.
struct Announcement {
    u64 frame_id = 0;
    u32 slot = 0;
    u32 generation = 0;
    /// The value `render_done` reaches when this frame's GPU work is done.
    u64 timeline_value = 0;
    /// `CLOCK_MONOTONIC` at `vkQueueSubmit`, in nanoseconds.
    u64 submitted_nanos = 0;
};

static_assert(sizeof(Announcement) == 32, "the editor's #[repr(C)] Announcement is 32 bytes");
static_assert(std::atomic<u64>::is_always_lock_free,
              "a shared page between two processes cannot use a lock-based atomic");

/// The page's layout. Private to `AnnouncementPage`, because the orderings are the whole point and
/// an open field would let a caller get them wrong.
struct SharedState {
    /// Even when stable, odd while a write is in progress.
    std::atomic<u64> sequence;
    /// The announcement, as four atomic words rather than as a struct.
    ///
    /// **A SEQLOCK'S PAYLOAD HAS TO BE ATOMIC, or it is a data race by the memory model's own
    /// definition** — two threads touching the same bytes with at least one writing, which is
    /// exactly what a seqlock does on purpose. Written as a plain struct and copied, it is
    /// undefined behaviour that happens to work, and `-fsanitize=thread` says so; written as
    /// relaxed atomics it is defined, and the sequence around it is what makes a torn read
    /// detectable rather than a torn OBJECT.
    ///
    /// The words are `frame_id`, `slot | generation << 32`, `timeline_value`, `submitted_nanos` —
    /// which is `Announcement`'s own layout, little-endian, so the editor's `#[repr(C)]` struct
    /// reads exactly these bytes and neither side knows the other spelled them differently.
    std::atomic<u64> words[4];
    /// Bumped every published frame. A reader that sees it stop knows the engine is wedged even
    /// though the socket is still open — the two liveness signals catch different deaths.
    std::atomic<u64> heartbeat;
    /// Written by the ENGINE: the slot it is about to write into, plus one, or zero for none.
    std::atomic<u64> writing;
    /// Written by the EDITOR: `(frame_id << 8) | slot`, or zero for none.
    std::atomic<u64> held;
};

static_assert(sizeof(SharedState) == 64, "the editor's #[repr(C)] SharedState is 64 bytes");
static_assert(sizeof(SharedState) <= kPageBytes);

[[nodiscard]] constexpr u64 held_pack(u64 frame_id, u32 slot) noexcept {
    return (frame_id << 8) | (static_cast<u64>(slot) & 0xFFULL);
}

[[nodiscard]] constexpr u64 held_frame(u64 packed) noexcept {
    return packed >> 8;
}
[[nodiscard]] constexpr u32 held_slot(u64 packed) noexcept {
    return static_cast<u32>(packed & 0xFFULL);
}

/// The shared page, mapped. Owns both the descriptor and the mapping.
class AnnouncementPage {
public:
    AnnouncementPage() = default;
    ~AnnouncementPage();

    AnnouncementPage(const AnnouncementPage&) = delete;
    AnnouncementPage& operator=(const AnnouncementPage&) = delete;

    /// Create an anonymous page and map it. The engine's side.
    [[nodiscard]] Status create() noexcept;

    /// The descriptor, to send with the handshake. −1 when there is none.
    [[nodiscard]] int descriptor() const noexcept { return descriptor_; }
    [[nodiscard]] bool mapped() const noexcept { return state_ != nullptr; }

    /// Publish the newest frame. **Called after `vkQueueSubmit`, never before.** See
    /// `publisher.h`'s header for what depends on that ordering.
    void publish(const Announcement& frame) noexcept;

    /// The engine declares the slot it is about to write into. Store this BEFORE reading `held`,
    /// and clear it after the frame is announced.
    ///
    /// The two-flag reservation: the engine stores its intent then reads `held`; the editor stores
    /// `held` then reads the intent. In any total order at least one of the two reads sees the
    /// other's store, so they never both proceed. Both backing off is harmless. `SeqCst` on all
    /// four accesses is what makes that argument valid.
    void set_writing(bool writing, u32 slot) noexcept;

    /// What the editor claims to be holding.
    [[nodiscard]] u64 held() const noexcept;

    /// How many frames have been published. For a test, and for a liveness report.
    [[nodiscard]] u64 heartbeat() const noexcept;

    /// The newest announcement as a reader would see it, under the same seqlock. Only a test reads
    /// its own page; the editor is the reader this exists for.
    [[nodiscard]] bool read(Announcement& out) const noexcept;

    /// Set the editor's claim. **Tests only** — in a session this word is the editor's, and the
    /// engine never writes it.
    void set_held_for_test(u64 packed) noexcept;

private:
    int descriptor_ = -1;
    SharedState* state_ = nullptr;
};

// --- The ring -----------------------------------------------------------------------------------

/// One image of the ring, as the engine sees it.
struct RingSlot {
    u64 last_frame = 0;
    /// When non-zero, the editor was observed holding this slot for that frame, and the engine must
    /// wait for the release timeline to reach it before writing here again.
    u64 needs_release = 0;
    /// Whether anything has ever been drawn here. Decides `UNDEFINED` against
    /// `SHADER_READ_ONLY_OPTIMAL` as a barrier's source layout.
    bool initialised = false;
};

/// Which image the next frame may be drawn into, and what happens when the answer is "none".
///
/// Pure decision-making with no Vulkan in it, so that the two properties that cost the most to get
/// wrong are testable on a machine with no GPU at all. Ported from
/// `cy_editor_viewport_transport::ring`, whose measurements chose the sizes.
class Ring {
public:
    [[nodiscard]] Status resize(u32 count) noexcept;

    [[nodiscard]] u32 size() const noexcept { return count_; }
    [[nodiscard]] u32 generation() const noexcept { return generation_; }
    [[nodiscard]] const RingSlot& slot(u32 index) const noexcept { return slots_[index]; }

    /// Take the editor's claim out of the shared page.
    ///
    /// A claim only pins a slot when it names the frame that slot actually holds: a stale claim —
    /// the editor holding a frame that has since been overwritten — must not pin the ring for ever.
    void observe_held(u64 packed) noexcept;

    /// The image the next frame may be drawn into, given how far the editor's release timeline has
    /// advanced, or `kNoSlot` when every image is spoken for.
    ///
    /// A slot may be written when it is not the one just published (the editor may be about to
    /// latch it), not the one the editor says it is holding, and either never published or already
    /// released. The OLDEST eligible slot wins, so the editor's chance of latching a frame before
    /// it is overwritten is as large as the ring allows.
    [[nodiscard]] u32 pick(u64 release_value) const noexcept;

    /// Record that a frame has been submitted into a slot and is about to be announced.
    void record_published(u32 index, u64 frame_id) noexcept;

    /// The release value this slot's next write must wait for, or zero for none.
    [[nodiscard]] u64 release_requirement(u32 index) const noexcept;

    /// The editor is gone: reclaim the whole ring. Without this an engine whose editor died keeps
    /// every slot the editor was holding, and stops rendering because something else stopped
    /// watching — the inversion the whole design is against.
    void reclaim() noexcept;

    static constexpr u32 kNoSlot = 0xFFFF'FFFFU;

private:
    RingSlot slots_[4]{};
    u32 count_ = 0;
    u32 published_ = kNoSlot;
    u32 held_slot_ = kNoSlot;
    u32 generation_ = 1;
};

/// Whether an editor's claim names this slot. Asked AFTER declaring the slot, and the frame is
/// dropped when the answer is yes — see `AnnouncementPage::set_writing` for why that second look is
/// not redundant with `observe_held`.
[[nodiscard]] bool claim_names(u64 packed, u32 slot) noexcept;

// --- The socket ---------------------------------------------------------------------------------

/// Send a payload and a set of descriptors in one `sendmsg`, over `SCM_RIGHTS`.
[[nodiscard]] Status send_descriptors(int socket, const int* descriptors, u32 count,
                                      const void* payload, usize payload_bytes) noexcept;

/// Whether the peer closed. Checked by READING rather than by writing: a write to a dead socket
/// raises `SIGPIPE`, and an editor that died while the engine was drawing must not take the engine
/// with it.
[[nodiscard]] bool peer_closed(int socket) noexcept;

/// The engine's `CLOCK_MONOTONIC`, in nanoseconds. The same clock the announcement carries and the
/// editor subtracts from.
[[nodiscard]] u64 monotonic_nanos() noexcept;

}  // namespace cy::viewport
