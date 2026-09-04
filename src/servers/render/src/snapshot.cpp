// The snapshot's interpolation rule and the double-buffered exchange. See
// cy/servers/render/snapshot.h.

#include <cy/servers/render/snapshot.h>

#include <cy/core/math/scalar.h>

namespace cy::render {

Transform resolve_transform(const Transform& previous, const Transform& current, f32 alpha,
                            bool teleported) noexcept {
    if (teleported) {
        // A jump has no midpoint worth drawing: blending across it draws the object smeared over
        // the gap for one frame, which is exactly what a teleport looks like when nobody thought
        // about it.
        return current;
    }
    return interpolate(previous, current, math::saturate(alpha));
}

SnapshotBuffer::SnapshotBuffer(Allocator& allocator) noexcept
    : buffers_{RenderSnapshot(allocator), RenderSnapshot(allocator)} {}

RenderSnapshot& SnapshotBuffer::writable() noexcept {
    // CLEARED HERE AND NOT IN `publish()`, and the difference is a defect rather than a preference.
    //
    // Clearing at publish time empties the buffer that has *just stopped* being readable — which is
    // the one the renderer acquired at the start of the frame and is still holding. The runtime
    // runs N fixed ticks and then one variable-rate render (`src/runtime/simulation.h`), so a
    // publish mid-frame is the normal case, not an edge one, and the reader would have watched its
    // snapshot empty underneath it.
    //
    // Clearing when the next extraction actually asks for a buffer gives the reader the slack the
    // interface promises: a publish that happens mid-frame lands in the buffer the renderer is not
    // reading, and the buffer it IS reading survives until the extraction after that begins. Two
    // buffers give exactly one publish of slack; a reader that needs more than that needs a queue,
    // and the interface says so rather than pretending otherwise.
    RenderSnapshot& buffer = buffers_[writing_];
    buffer.clear();
    return buffer;
}

u64 SnapshotBuffer::publish() noexcept {
    // The written buffer becomes the readable one and the other becomes the scratch — untouched
    // until `writable()` is asked for it. See above.
    writing_ ^= 1U;
    has_published_ = true;
    return ++published_;
}

const RenderSnapshot* SnapshotBuffer::readable() const noexcept {
    if (!has_published_) {
        return nullptr;
    }
    return &buffers_[writing_ ^ 1U];
}

}  // namespace cy::render
