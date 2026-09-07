#pragma once
// Receiver-driven page marking, compaction, update classes and staleness priority. Task 8.1.
//
// `virtual-shadows` — "Receiver-driven page marking" and "Update classes and staleness".
//
// ================================================================================================
// THE INTERFACE TAKES RECEIVER SAMPLES, NOT OBJECTS, AND THAT IS THE REQUIREMENT
// ================================================================================================
//
// "There SHALL NOT be a CPU loop over objects and lights to determine shadow work." A CPU function
// that took a scene and a light list would be exactly that loop, so `PageRequestSet::mark()` takes
// one receiver sample — a world position and the texel density the pixel needs — which is what a
// compute shader has when it reads the depth buffer. The device-side pass is one dispatch over the
// depth buffer calling the same arithmetic; this class is its CPU half, and it is what lets the
// marking rules be tested without a device.
//
// The compaction is the other half of the requirement — "the marks are compacted into a unique page
// set". Duplicate marks are the normal case: a thousand pixels of one wall land in one page. So
// `mark()` is cheap and lossy about order, and `compact()` produces the unique set, SORTED BY THE
// PACKED PAGE ID so that two runs over the same frame produce the same list in the same order. That
// determinism is not cosmetic: the render order of dirty pages decides which ones fit inside a
// budget, and a set that reordered itself between runs would make a shadow budget irreproducible.
//
// ================================================================================================
// A HARD CAP ON PAGES PER FRAME IS THE THING THIS FILE EXISTS NOT TO HAVE
// ================================================================================================
//
// "The budget controller SHALL spend its allocation on the pages where staleness would be visible,
// rather than enforcing a fixed cap on pages per frame — a hard cap turns a camera cut into a
// stall, while prioritised staleness degrades gracefully."
//
// `select_pages_to_render()` is therefore ordered by `staleness_priority()` and stops when the
// allocation is spent — and a `Critical` page is never allowed to go stale, so it sorts above
// everything and is taken even when the allocation is exhausted. A cut that dirties two thousand
// pages produces several frames of refresh with stale pages used meanwhile, which is the scenario.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/shadows/address_space.h>

namespace cy::rendering {

/// `virtual-shadows`' four update classes, in the order it names them. Ordered finest-deadline
/// first, so a smaller enumerator is a more urgent page and the ordering is usable directly.
enum class UpdateClass : u8 {
    Critical = 0,
    Dynamic,
    Normal,
    Background,
    Count,
};

[[nodiscard]] const char* update_class_name(UpdateClass update_class) noexcept;

/// How many frames a page of this class may remain stale. `Critical` answers zero — "content whose
/// shadow must be exact SHALL be classified critical and SHALL NOT be allowed to go stale".
[[nodiscard]] u32 max_stale_frames(UpdateClass update_class) noexcept;

/// One marked page and what the marking learned about it.
struct PageRequest {
    VirtualPage page;
    /// How many receiver samples landed in it. A page one pixel needs and a page a wall needs are
    /// not equally worth rendering.
    u32 marks = 0;
    /// The largest receiver importance that marked it, in [0,1]. Carried through to the budget.
    f32 importance = 0.0F;
};

/// One receiver sample: what a compute shader has after reading one pixel of the depth buffer.
struct ReceiverSample {
    Vec3 world_position{0.0F, 0.0F, 0.0F};
    /// The world edge length this pixel's shadow lookup would like a texel to be. Derived on the
    /// GPU from the pixel's depth and the projection; the number that makes page resolution
    /// selection a measurement rather than a distance ramp.
    f32 texel_world_size = 0.0F;
    /// The view's own priority times the receiver's importance. A reflection probe contributes
    /// demand at lower weight than the main camera — "Multi-view page sharing".
    f32 importance = 1.0F;
};

/// The mark-and-compact set. One per frame, shared across every light and every view: two views
/// that need the same page mark the same id and compaction merges them, which is the multi-view
/// requirement satisfied by construction rather than by a merge step.
///
/// Not thread-safe. The device-side pass is a dispatch; this is the frame thread's model of it.
class PageRequestSet {
public:
    explicit PageRequestSet(Allocator& allocator) noexcept;

    [[nodiscard]] Status initialize(u32 capacity) noexcept;

    void begin_frame() noexcept;

    /// Project one receiver sample into one light's space and mark the page it lands in.
    ///
    /// `levels` is that light's address spaces, finest first. The level is chosen as the finest one
    /// whose texel is no finer than the sample asked for, which is the whole of "page resolution
    /// SHALL be selected from the receiver's projected shadow texel density" — a distant pixel and
    /// a near one mark different levels of the same light without anything else being consulted.
    ///
    /// Silently does nothing when the sample falls outside every level, which is the common case
    /// for a spot light and half the pixels on the screen.
    void mark(Span<const ShadowAddressSpace> levels, const ReceiverSample& sample) noexcept;

    /// Mark a page directly. Used by invalidation and by tests; the same de-duplication applies.
    void mark_page(VirtualPage page, f32 importance) noexcept;

    /// Produce the unique set, ordered by packed page id. Idempotent within a frame.
    [[nodiscard]] Status compact() noexcept;

    [[nodiscard]] Span<const PageRequest> pages() const noexcept { return unique_.span(); }

    /// Marks submitted this frame, before de-duplication. The ratio against `pages().size()` is the
    /// number that says whether marking is doing useful work.
    [[nodiscard]] u32 marks() const noexcept { return marks_; }

    /// Marks dropped because the set was full. Non-zero means the capacity is too small for the
    /// scene, and it is reported rather than hidden.
    [[nodiscard]] u32 overflow() const noexcept { return overflow_; }

private:
    Array<PageRequest> raw_;
    Array<PageRequest> unique_;
    u32 capacity_ = 0;
    u32 marks_ = 0;
    u32 overflow_ = 0;
    bool compacted_ = false;
};

/// What the budget knows about one dirty page when it decides whether to render it this frame.
struct StalePage {
    VirtualPage page;
    UpdateClass update_class = UpdateClass::Normal;
    /// Frames since the page was last rendered.
    u32 age = 0;
    /// Largest receiver importance that marked it this frame, in [0,1].
    f32 importance = 0.0F;
    /// How fast the content in the page is moving, normalised to [0,1] by the caller. Motion makes
    /// staleness visible sooner.
    f32 motion = 0.0F;
    /// Milliseconds the page cost the last time it was rendered. Zero for a page never rendered.
    f32 last_cost_ms = 0.0F;
};

/// How visible this page's staleness is. Larger is more urgent. A `Critical` page answers
/// `kCriticalPriority` or above, so the selection below cannot leave one behind.
[[nodiscard]] f32 staleness_priority(const StalePage& page) noexcept;

inline constexpr f32 kCriticalPriority = 1.0e6F;

/// Order `pages` by priority and take them until `allocation_ms` is spent, leaving the rest stale.
/// Critical pages are taken regardless, and `overspend_ms` reports by how much they took the frame
/// past its allocation — reported rather than silently absorbed, because a scene whose critical
/// pages alone exceed the allocation is a content problem the budget cannot fix.
///
/// `pages` is reordered in place. `out` receives the selected pages in render order.
struct PageSelection {
    u32 selected = 0;
    u32 deferred = 0;
    f32 spend_ms = 0.0F;
    f32 overspend_ms = 0.0F;
};

[[nodiscard]] PageSelection select_pages_to_render(Span<StalePage> pages, f32 allocation_ms,
                                                   f32 default_cost_ms, VirtualPage* out,
                                                   u32 out_capacity) noexcept;

}  // namespace cy::rendering
