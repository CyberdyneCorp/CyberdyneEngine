#include <cy/rendering/temporal/framework.h>

#include <cy/core/math/scalar.h>

#include <cmath>
#include <cstring>

namespace cy::rendering {
namespace {

/// A hash over the projection's sixteen floats. Any change to the projection changes it, and the
/// framework is the only thing that computes it — so two consumers cannot disagree about whether
/// the projection changed, which is the disagreement the single framework exists to prevent.
[[nodiscard]] u64 projection_key_of(const Mat4& matrix) noexcept {
    u64 hash = 1469598103934665603ULL;
    for (const Vec4& column : matrix.columns) {
        const f32 components[4] = {column.x, column.y, column.z, column.w};
        for (const f32 value : components) {
            u32 bits = 0;
            // Bit-copied rather than cast: two different floats must not collide because one
            // rounded to the other's integer part.
            std::memcpy(&bits, &value, sizeof(bits));
            hash = (hash ^ bits) * 1099511628211ULL;
        }
    }
    return hash;
}

[[nodiscard]] u32 scaled(u32 extent, f32 scale) noexcept {
    const f32 value = static_cast<f32>(extent) * math::max(scale, 0.01F);
    // Rounded rather than truncated, and at least one: a history resource with a zero dimension is
    // a texture nothing can allocate, and a half-resolution buffer of an odd width has to round the
    // same way every frame or it reallocates on alternate frames forever.
    return math::max(static_cast<u32>(std::lround(value)), 1U);
}

}  // namespace

const char* temporal_invalidation_name(TemporalInvalidation cause) noexcept {
    switch (cause) {
        case TemporalInvalidation::None:
            return "None";
        case TemporalInvalidation::CameraCut:
            return "CameraCut";
        case TemporalInvalidation::Teleport:
            return "Teleport";
        case TemporalInvalidation::ProjectionChange:
            return "ProjectionChange";
        case TemporalInvalidation::ResolutionChange:
            return "ResolutionChange";
        case TemporalInvalidation::SceneLoad:
            return "SceneLoad";
        case TemporalInvalidation::Explicit:
            return "Explicit";
        case TemporalInvalidation::Count:
            break;
    }
    return "Unknown";
}

TemporalFramework::TemporalFramework(Allocator& allocator) noexcept
    : consumers_(allocator), histories_(allocator) {}

Status TemporalFramework::initialize(const TemporalConfig& config) noexcept {
    config_ = config;
    jitter_.configure(config.jitter);
    consumers_.clear();
    histories_.clear();
    stats_ = TemporalStatistics{};
    frame_ = 0;
    has_previous_ = false;
    return ok();
}

Expected<ConsumerId, Error> TemporalFramework::register_consumer(const char* name,
                                                                 bool needs_jitter) noexcept {
    Consumer consumer;
    consumer.name = name == nullptr ? "" : name;
    consumer.needs_jitter = needs_jitter;
    if (Status pushed = consumers_.push_back(consumer); !pushed) {
        return make_unexpected(pushed.error());
    }
    return ConsumerId{static_cast<u32>(consumers_.size() - 1)};
}

Expected<HistoryId, Error> TemporalFramework::declare_history(
    ConsumerId consumer, const HistoryDeclaration& declaration) noexcept {
    if (!consumer.valid() || consumer.value >= consumers_.size()) {
        // A history the framework does not know the owner of would be missing from the memory
        // report and from the invalidation broadcast, which is exactly the state this refusal
        // exists to make unreachable.
        return fail(ErrorCode::InvalidArgument,
                    "temporal: a history must be declared by a registered consumer");
    }
    if (declaration.frames == 0) {
        return fail(ErrorCode::InvalidArgument, "temporal: a history retains at least one frame");
    }
    HistoryResource resource;
    resource.id = HistoryId{static_cast<u32>(histories_.size())};
    resource.consumer = consumer.value;
    resource.declaration = declaration;
    resource.width = scaled(view_.width, declaration.resolution_scale);
    resource.height = scaled(view_.height, declaration.resolution_scale);
    resource.valid = false;
    if (Status pushed = histories_.push_back(resource); !pushed) {
        return make_unexpected(pushed.error());
    }
    stats_.history_bytes += histories_.back().bytes();
    return histories_.back().id;
}

void TemporalFramework::signal_cut(TemporalInvalidation cause) noexcept {
    // Recorded and applied at the next `begin_frame()`, so that a cut signalled halfway through a
    // frame's work reaches every consumer on the same frame boundary rather than some of them.
    pending_ = cause;
}

void TemporalFramework::begin_frame(const TemporalView& view) noexcept {
    ++frame_;
    previous_view_ = view_;
    view_ = view;
    invalidated_this_frame_ = false;
    stats_.classification.reset();

    const u64 key = projection_key_of(view.projection);
    const bool first = !has_previous_;
    const bool resized =
        !first && (previous_view_.width != view.width || previous_view_.height != view.height);
    const bool projection_changed = !first && !resized && key != projection_key_;
    const f32 travelled =
        first ? 0.0F : length(view.camera_position - previous_view_.camera_position);
    const bool teleported = !first && travelled > config_.teleport_distance;

    TemporalInvalidation cause = TemporalInvalidation::None;
    if (pending_ != TemporalInvalidation::None) {
        cause = pending_;
    } else if (resized) {
        cause = TemporalInvalidation::ResolutionChange;
    } else if (teleported) {
        cause = TemporalInvalidation::Teleport;
    } else if (projection_changed) {
        cause = TemporalInvalidation::ProjectionChange;
    }
    pending_ = TemporalInvalidation::None;
    projection_key_ = key;
    has_previous_ = true;

    if (resized) {
        resize_histories();
    }
    if (cause != TemporalInvalidation::None) {
        invalidate_all(cause);
    }

    // Jitter is advanced ONCE, here, and only when a registered consumer needs it.
    bool wanted = false;
    for (const Consumer& consumer : consumers_.span()) {
        wanted = wanted || consumer.needs_jitter;
    }
    jitter_.advance(wanted);
}

void TemporalFramework::invalidate_all(TemporalInvalidation cause) noexcept {
    for (HistoryResource& resource : histories_.span()) {
        resource.valid = false;
    }
    invalidated_this_frame_ = true;
    ++stats_.invalidations[static_cast<usize>(cause)];
    stats_.last_cause = cause;
    stats_.last_invalidated_frame = frame_;
}

void TemporalFramework::resize_histories() noexcept {
    stats_.history_bytes = 0;
    for (HistoryResource& resource : histories_.span()) {
        resource.width = scaled(view_.width, resource.declaration.resolution_scale);
        resource.height = scaled(view_.height, resource.declaration.resolution_scale);
        stats_.history_bytes += resource.bytes();
    }
}

void TemporalFramework::pin_jitter(u32 index) noexcept {
    jitter_.pin(index);
}

void TemporalFramework::unpin_jitter() noexcept {
    jitter_.unpin();
}

Mat4 TemporalFramework::jittered_view_projection() const noexcept {
    const Vec2 offset = jitter_.ndc_offset(view_.width, view_.height);
    Mat4 jittered = view_.view_projection();
    // A translation in CLIP space, applied on the left: row0 += offset.x * row3 and
    // row1 += offset.y * row3. In column-major storage that is one line per column, and doing it to
    // every column rather than only the translation one is what makes it correct for a projection
    // whose perspective divide lives in column two.
    for (Vec4& column : jittered.columns) {
        column.x += offset.x * column.w;
        column.y += offset.y * column.w;
    }
    return jittered;
}

const HistoryResource* TemporalFramework::history(HistoryId id) const noexcept {
    if (!id.valid() || id.value >= histories_.size()) {
        return nullptr;
    }
    return &histories_.span()[id.value];
}

void TemporalFramework::mark_history_written(HistoryId id) noexcept {
    if (!id.valid() || id.value >= histories_.size()) {
        return;
    }
    HistoryResource& resource = histories_.span()[id.value];
    resource.valid = true;
    resource.produced_with.width = resource.width;
    resource.produced_with.height = resource.height;
    resource.produced_with.frame = frame_;
    resource.produced_with.projection_key = projection_key_;
}

SurfaceMotion TemporalFramework::surface_motion(Vec3 current_world,
                                                Vec3 previous_world) const noexcept {
    SurfaceMotionInputs inputs;
    inputs.current_world = current_world;
    inputs.previous_world = previous_world;
    inputs.current_view_projection = view_.view_projection();
    // Frame one has no previous view. Using the current one makes every surface's motion exactly
    // zero, which is the correct answer for a frame whose history is invalid anyway.
    inputs.previous_view_projection =
        frame_ > 1 ? previous_view_.view_projection() : view_.view_projection();
    return derive_surface_motion(inputs);
}

ReprojectionResult TemporalFramework::classify(HistoryId id, Vec2 current_uv,
                                               const SurfaceMotion& motion, f32 current_depth,
                                               f32 history_depth) noexcept {
    const HistoryResource* resource = history(id);
    ReprojectionInputs inputs;
    inputs.current_uv = current_uv;
    inputs.motion = motion.motion;
    inputs.representable = motion.representable;
    inputs.current_depth = current_depth;
    inputs.history_depth = history_depth;
    inputs.depth_tolerance = config_.depth_tolerance;
    // The invalidation reaches the consumer AS a classification. There is no second flag to check
    // and therefore no second flag to forget.
    inputs.history_valid = resource != nullptr && resource->valid;
    const ReprojectionResult result = classify_history(inputs);
    stats_.classification.record(result.state);
    return result;
}

u64 TemporalFramework::history_bytes(ConsumerId consumer) const noexcept {
    u64 bytes = 0;
    for (const HistoryResource& resource : histories_.span()) {
        if (resource.consumer == consumer.value) {
            bytes += resource.bytes();
        }
    }
    return bytes;
}

}  // namespace cy::rendering
