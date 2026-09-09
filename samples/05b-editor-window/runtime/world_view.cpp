// The authored world this runtime renders. See world_view.h.

#include "world_view.h"

#include <cy/core/math/quat.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace cy::sample::editor_window {
namespace {

namespace ser = cy::scene::serialization;

/// Read a whole file into `out`. Reports `NotFound` rather than an empty world, because "the file
/// is not there" and "the world is empty" are two different things and only one of them is a
/// mistake.
[[nodiscard]] Status read_file(const char* path, Array<char>& out) noexcept {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return fail(ErrorCode::NotFound, "the world file could not be opened");
    }
    // THE LOOP ENDS ON THE STREAM'S OWN STATE, not on a short read. A `fread` that returns fewer
    // bytes than asked for is either the end of the file or an error, and only `feof` and `ferror`
    // say which; reading again after either is undefined, and treating an error as an end of file
    // is a world silently truncated to whatever arrived.
    Status status = ok();
    char buffer[4096];
    while (std::feof(file) == 0 && std::ferror(file) == 0) {
        const usize read = std::fread(buffer, 1, sizeof(buffer), file);
        if (read == 0) {
            break;
        }
        status = out.append(Span<const char>(buffer, read));
        if (!status) {
            break;
        }
    }
    const bool failed = std::ferror(file) != 0;
    (void)std::fclose(file);
    if (!status) {
        return status;
    }
    if (failed) {
        return fail(ErrorCode::Io, "the world file could not be read to its end");
    }
    return ok();
}

/// A colour that depends only on the node's identity, so the same world looks the same every run
/// and two objects a person created are told apart at a glance.
void colour_of(u64 identity, f32 out[3]) noexcept {
    // Three decorrelated bytes of the identity, lifted off black so nothing is unlit-looking.
    const u32 red = static_cast<u32>((identity >> 8U) & 0xFFU);
    const u32 green = static_cast<u32>((identity >> 24U) & 0xFFU);
    const u32 blue = static_cast<u32>((identity >> 40U) & 0xFFU);
    out[0] = 0.35F + (0.6F * (static_cast<f32>(red) / 255.0F));
    out[1] = 0.35F + (0.6F * (static_cast<f32>(green) / 255.0F));
    out[2] = 0.35F + (0.6F * (static_cast<f32>(blue) / 255.0F));
}

/// The yaw a quaternion describes, which is all M3's `Object` can carry.
///
/// Stated rather than silently dropped: an authored rotation with pitch or roll in it is rendered
/// as its yaw, because `first_light::Object` holds one angle. It is the renderer's limit, not the
/// world's — the world keeps the whole quaternion and writes it back out unchanged.
[[nodiscard]] f32 yaw_of(const Quat& rotation) noexcept {
    const f32 siny = 2.0F * ((rotation.w * rotation.y) + (rotation.z * rotation.x));
    const f32 cosy = 1.0F - (2.0F * ((rotation.x * rotation.x) + (rotation.y * rotation.y)));
    return std::atan2(siny, cosy);
}

/// The mean of a node's scale lanes, which is what a uniformly scaled box needs.
[[nodiscard]] f32 uniform_scale(const Vec3& scale) noexcept {
    return (scale.x + scale.y + scale.z) / 3.0F;
}

}  // namespace

Status WorldView::open(const char* directory, const char* asset_path,
                       const reflect::TypeRegistry& registry) noexcept {
    loaded_ = false;
    placed_.clear();

    char path[1024] = {};
    const bool rooted = directory != nullptr && directory[0] != '\0';
    (void)std::snprintf(path, sizeof(path), rooted ? "%s/%s" : "%s%s", rooted ? directory : "",
                        asset_path);

    Array<char> text(*allocator_);
    if (Status read = read_file(path, text); !read) {
        return read;
    }
    if (Status built = ser::build_authoring_schema(registry, schema_); !built) {
        return built;
    }
    const Expected<ser::WorldReadReport, Error> report =
        ser::read_world(std::string_view(text.data(), text.size()), asset_path, world_);
    if (!report) {
        return make_unexpected(report.error());
    }
    if (Expected<u32, Error> resolved = ser::resolve_against(world_, schema_); !resolved) {
        return make_unexpected(resolved.error());
    }
    loaded_ = true;
    return ok();
}

Status WorldView::apply(Span<const u8> bytes, ser::TransactionReport& out) noexcept {
    return ser::apply_transaction(world_, bytes, out);
}

u32 WorldView::present(first_light::Scene& scene) noexcept {
    placed_.clear();
    overflowed_ = 0;
    const Span<first_light::Object> objects = scene.objects_mutable();
    if (objects.size() < 2) {
        return 0;
    }
    // The box's index range, read out of the scene. Object 1 is the pillar, which is the unit box.
    box_first_ = objects[1].first_index;
    box_count_ = objects[1].index_count;

    u32 slot = 1;
    for (const ser::WorldNode& node : world_.nodes()) {
        if (!node.live) {
            continue;
        }
        Transform placement;
        if (!ser::transform_of(world_, node, placement)) {
            continue;  // a node with no placement is not something this renderer can draw
        }
        if (slot >= objects.size()) {
            overflowed_ += 1;
            continue;
        }
        first_light::Object& object = objects[slot];
        object.world_position[0] = static_cast<f64>(placement.translation.x);
        object.world_position[1] = static_cast<f64>(placement.translation.y);
        object.world_position[2] = static_cast<f64>(placement.translation.z);
        object.yaw_radians = yaw_of(placement.rotation);
        object.scale = uniform_scale(placement.scale);
        colour_of(node.identity, object.base_color);
        object.first_index = box_first_;
        object.index_count = box_count_;
        if (Status pushed = placed_.push_back(Placed{node.identity, slot}); !pushed) {
            return slot - 1;
        }
        slot += 1;
    }
    // Everything the world does not fill draws nothing. A zero index count is a draw of no
    // triangles, which is legal and free, and it is what keeps the object list a fixed length while
    // the world grows and shrinks.
    for (usize spare = slot; spare < objects.size(); ++spare) {
        objects[spare].index_count = 0;
    }
    return static_cast<u32>(placed_.size());
}

u32 WorldView::object_for(u64 identity) const noexcept {
    for (const Placed& entry : placed_) {
        if (entry.identity == identity) {
            return entry.object;
        }
    }
    return kNoObject;
}

u64 WorldView::identity_of(u32 object) const noexcept {
    for (const Placed& entry : placed_) {
        if (entry.object == object) {
            return entry.identity;
        }
    }
    return 0;
}

Status WorldView::publish(const first_light::Scene& scene, const first_light::Camera& camera,
                          Array<render::GpuInstance>& instances,
                          Array<render::DrawItem>& draws) const noexcept {
    instances.clear();
    draws.clear();
    const Span<const first_light::Object> objects = scene.objects();
    for (const Placed& entry : placed_) {
        if (entry.object >= objects.size()) {
            continue;
        }
        const first_light::Object& object = objects[entry.object];
        if (object.index_count == 0) {
            continue;  // not drawn, therefore not pickable. picking.h's first rule.
        }
        // CAMERA-RELATIVE, because that is the space the view was built in and therefore the space
        // `ray_through_pixel` produces a ray in. A record in absolute world coordinates would be a
        // pick that missed by the camera's position, which at the origin is invisible and at
        // `--origin 1000000` is total.
        const Vec3 centre{static_cast<f32>(object.world_position[0] - camera.position[0]),
                          static_cast<f32>(object.world_position[1] - camera.position[1]),
                          static_cast<f32>(object.world_position[2] - camera.position[2])};
        render::GpuInstance record;
        record.bounds_center[0] = centre.x;
        record.bounds_center[1] = centre.y;
        record.bounds_center[2] = centre.z;
        // The unit box's half-diagonal, scaled. A sphere around the box rather than the box, which
        // is what `GpuInstance` carries and what `picking.h` says a hit is against.
        record.bounds_radius = 0.8661F * object.scale;
        record.set_stable_id(entry.identity);
        record.flags = render::kInstanceActive;
        record.layer_mask = 0xFFFF'FFFFU;
        const u32 slot = static_cast<u32>(instances.size());
        if (Status pushed = instances.push_back(record); !pushed) {
            return pushed;
        }
        render::DrawKeyInputs key;
        key.layer = render::SortLayer::Opaque;
        key.view_depth = length(centre);
        render::DrawItem item;
        item.key = render::make_sort_key(key);
        item.stable_id = entry.identity;
        item.instance_slot = slot;
        item.surface = 0;
        if (Status pushed = draws.push_back(item); !pushed) {
            return pushed;
        }
    }
    render::sort_draws(draws.span());
    return ok();
}

first_light::Camera WorldView::framing(const first_light::Scene& scene) const noexcept {
    if (placed_.empty()) {
        return scene.camera_at(0.12F);
    }
    // The box that holds every placed node, and a camera far enough back to see all of it.
    const Span<const first_light::Object> objects = scene.objects();
    f64 low[3] = {0.0, 0.0, 0.0};
    f64 high[3] = {0.0, 0.0, 0.0};
    bool first = true;
    for (const Placed& entry : placed_) {
        if (entry.object >= objects.size()) {
            continue;
        }
        const first_light::Object& object = objects[entry.object];
        for (u32 axis = 0; axis < 3; ++axis) {
            const f64 half = static_cast<f64>(object.scale);
            const f64 minimum = object.world_position[axis] - half;
            const f64 maximum = object.world_position[axis] + half;
            low[axis] = first ? minimum : math::min(low[axis], minimum);
            high[axis] = first ? maximum : math::max(high[axis], maximum);
        }
        first = false;
    }
    const f64 centre[3] = {(low[0] + high[0]) * 0.5, (low[1] + high[1]) * 0.5,
                           (low[2] + high[2]) * 0.5};
    f64 extent = 1.0;
    for (u32 axis = 0; axis < 3; ++axis) {
        extent = math::max(extent, high[axis] - low[axis]);
    }
    const f64 distance = (extent * 1.8) + 4.0;

    first_light::Camera camera = scene.camera_at(0.12F);
    camera.position[0] = centre[0] + (distance * 0.65);
    camera.position[1] = centre[1] + (distance * 0.55);
    camera.position[2] = centre[2] + (distance * 0.65);
    camera.forward = normalize(Vec3{static_cast<f32>(centre[0] - camera.position[0]),
                                    static_cast<f32>(centre[1] - camera.position[1]),
                                    static_cast<f32>(centre[2] - camera.position[2])});
    camera.up = Vec3{0.0F, 1.0F, 0.0F};
    return camera;
}

}  // namespace cy::sample::editor_window
