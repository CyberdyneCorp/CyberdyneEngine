#pragma once
// Barriers, and the interface that is the only way to record one. Tasks 2.2.1 and 2.2.4.
//
// --- THE INVARIANT, AS A TYPE ------------------------------------------------------------------
//
// `rhi-and-render-graph`: "The RHI's public recording API SHALL NOT expose barriers, image layout
// transitions, or queue ownership transfers." cy::rhi::CommandBuffer, which is what a pass records
// into, has no barrier method — not a deprecated one, not a documented-as-unsafe one, none. The
// calls that emit a barrier live on `BarrierRecorder`, and a `BarrierRecorder` is reachable only
// through `Device::barrier_recorder(GraphBarrierKey)`, whose key type cannot be constructed by
// anything except cy::rendering::GraphExecutor.
//
// The forward declaration of that class below is a NAME, not a dependency: this module neither
// includes nor links the render graph, and the layer checker sees no upward include because there
// is none. What the name buys is that the rule is in the type system, where it is checked by the
// compiler on every build, rather than in a comment that is checked by whoever reviews the
// thirtieth pass.
//
// A passkey is not proof on its own — someone determined enough can declare a class of that name.
// The second half is the source-level gate in tools/layercheck/layercheck.py (`barriers`), which
// fails the build when a barrier-emitting symbol appears outside src/backends/rhi/ and
// src/rendering/graph/. Task 2.2.4 asks for both, and asks that the gate be proved by introducing a
// violation and watching the build fail; tools/layercheck/fixtures/barrier-outside-graph/ is that
// violation, kept as a fixture so the proof does not have to be repeated by hand.
//
// --- WHAT A BARRIER BATCH IS -------------------------------------------------------------------
//
// A batch carries NO backend handles. `image_ids` and `buffer_ids` are graph resource ids, and the
// executor patches the real handles in at record time. That is what lets the derivation run with no
// device at all — the null backend and continuous integration execute the identical code path — and
// it is what makes two independent processes produce byte-identical plans.

#include <cy/backends/rhi/handles.h>
#include <cy/backends/rhi/types.h>
#include <cy/core/memory/array.h>

namespace cy::rendering {
// The one type permitted to ask a device for a barrier recorder. Declared, never included.
class GraphExecutor;
}  // namespace cy::rendering

namespace cy::rhi {

/// A resource's identity within one compiled graph. The graph owns the mapping to real handles;
/// a barrier batch carries only this, so derivation never touches a device.
using GraphResourceId = u32;
inline constexpr GraphResourceId kInvalidGraphResource = ~0U;

/// Which parts of an image a barrier applies to. Depth and colour are separate aspects because a
/// depth-stencil image transitions both together and a colour image has neither.
enum class ImageAspect : u8 {
    Color = 0,
    Depth,
    DepthStencil,
};

/// One image barrier, already coalesced into a rectangle of mips and layers.
///
/// `src_queue_family` and `dst_queue_family` are kQueueFamilyIgnored except on the two halves of an
/// ownership transfer, where they are equal on both halves and the release/acquire pair is ordered
/// by a semaphore rather than by the barriers themselves.
struct ImageBarrier {
    GraphResourceId resource = kInvalidGraphResource;
    Stage src_stage = Stage::None;
    AccessFlags src_access = AccessFlags::None;
    Stage dst_stage = Stage::None;
    AccessFlags dst_access = AccessFlags::None;
    ImageLayout old_layout = ImageLayout::Undefined;
    ImageLayout new_layout = ImageLayout::Undefined;
    u32 src_queue_family = kQueueFamilyIgnored;
    u32 dst_queue_family = kQueueFamilyIgnored;
    ImageAspect aspect = ImageAspect::Color;
    SubresourceRange range{};
    /// Patched by the executor immediately before recording. Null in a derived plan.
    TextureHandle texture;
};

struct BufferBarrier {
    GraphResourceId resource = kInvalidGraphResource;
    Stage src_stage = Stage::None;
    AccessFlags src_access = AccessFlags::None;
    Stage dst_stage = Stage::None;
    AccessFlags dst_access = AccessFlags::None;
    u32 src_queue_family = kQueueFamilyIgnored;
    u32 dst_queue_family = kQueueFamilyIgnored;
    u64 offset = 0;
    /// Zero means the whole buffer, resolved by the backend. Nothing derives a partial range yet.
    u64 size = 0;
    BufferHandle buffer;
};

/// A global barrier with no resource identity. Two things produce one: a hazard on a buffer whose
/// whole range is affected, and a memory-aliasing hazard — where the resources genuinely are two
/// different objects over the same bytes, so naming either of them would be misleading.
struct MemoryBarrier {
    Stage src_stage = Stage::None;
    AccessFlags src_access = AccessFlags::None;
    Stage dst_stage = Stage::None;
    AccessFlags dst_access = AccessFlags::None;
};

/// Everything that must be recorded at one point in a command buffer, in one call.
///
/// One call rather than three, because a backend submits them as a single dependency: splitting a
/// batch into three consecutive barriers is a correctness-preserving but measurably worse thing to
/// do, and doing it by accident is easy when the batch is three separate members recorded
/// separately.
struct BarrierBatch {
    Array<ImageBarrier> images;
    Array<BufferBarrier> buffers;
    Array<MemoryBarrier> memory;

    [[nodiscard]] bool empty() const noexcept {
        return images.empty() && buffers.empty() && memory.empty();
    }

    void clear() noexcept {
        images.clear();
        buffers.clear();
        memory.clear();
    }

    [[nodiscard]] usize count() const noexcept {
        return images.size() + buffers.size() + memory.size();
    }
};

/// The passkey. Its constructor is private and its only friend is the render graph's executor, so
/// no other translation unit can produce the argument `Device::barrier_recorder()` requires.
///
/// Non-copyable on purpose: a key obtained legitimately cannot be squirrelled away and handed to
/// somebody else, so "who may emit a barrier" stays a question with one answer.
class GraphBarrierKey {
public:
    GraphBarrierKey(const GraphBarrierKey&) = delete;
    GraphBarrierKey& operator=(const GraphBarrierKey&) = delete;
    GraphBarrierKey(GraphBarrierKey&&) = delete;
    GraphBarrierKey& operator=(GraphBarrierKey&&) = delete;
    ~GraphBarrierKey() = default;

private:
    GraphBarrierKey() = default;
    friend class ::cy::rendering::GraphExecutor;
};

/// The barrier-emitting interface. Implemented by every backend; obtained only with a key.
///
/// `record()` takes a whole batch and a command buffer. There is deliberately no "begin barrier /
/// add / end" shape: a partially built barrier is a state a caller can leave the recorder in, and
/// the only caller is a loop over a derived plan that always has the whole batch in hand.
class BarrierRecorder {
public:
    BarrierRecorder() = default;
    virtual ~BarrierRecorder() = default;

    BarrierRecorder(const BarrierRecorder&) = delete;
    BarrierRecorder& operator=(const BarrierRecorder&) = delete;
    BarrierRecorder(BarrierRecorder&&) = delete;
    BarrierRecorder& operator=(BarrierRecorder&&) = delete;

    /// Record `batch` into `command_buffer`. Every handle in the batch has already been patched.
    virtual void record_barriers(CommandBufferHandle command_buffer,
                                 const BarrierBatch& batch) noexcept = 0;

    /// How many batches and how many individual barriers this recorder has emitted since the
    /// device was created. `rhi-and-render-graph` requires barriers to be visible in the trace, and
    /// a test that asserts "this frame emitted exactly one ownership release" reads it from here.
    [[nodiscard]] virtual u64 recorded_batch_count() const noexcept = 0;
    [[nodiscard]] virtual u64 recorded_barrier_count() const noexcept = 0;
};

}  // namespace cy::rhi
