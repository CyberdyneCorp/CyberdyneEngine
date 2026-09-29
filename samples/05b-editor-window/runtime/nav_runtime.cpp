// SPDX-License-Identifier: MIT
// The runtime host's navigation seam, change tracking and in-process requests. See nav_runtime.h.

#include "nav_runtime.h"

#include <cy/core/assets/cooked.h>
#include <cy/core/assets/file.h>
#include <cy/core/memory/hash.h>
#include <cy/core/values/name.h>
#include <cy/import/gltf.h>
#include <cy/import/mesh.h>
#include <cy/import/primitive.h>
#include <cy/servers/render/picking.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

namespace cy::sample::editor_window {

namespace ser = scene::serialization;
namespace nav = navigation;

struct AuthoredNavigationSource::LoadedMesh {
    import::MeshData data;
    Aabb bounds;
};

namespace {

constexpr std::string_view kWorldType = "NavigationWorld";
constexpr std::string_view kSurfaceType = "NavMeshSurface";
constexpr std::string_view kAreaType = "NavArea";
constexpr std::string_view kObstacleType = "NavObstacle";
constexpr std::string_view kLinkType = "NavLink";
constexpr std::string_view kMeshType = "MeshRenderer";
constexpr usize kFramesKept = 64;

// --- Reading components by name ---------------------------------------------------------------

/// One component of one node, read by field name. `present` is false when the node lacks it.
class ComponentReader {
public:
    ComponentReader(const ser::World& world, const ser::WorldNode& node,
                    std::string_view type_name) noexcept
        : world_(&world) {
        for (const ser::WorldComponent& component : node.components()) {
            const ser::WorldTypeDecl* type = world.type(component.file_type);
            if (type != nullptr && world.text(type->name) == type_name) {
                component_ = &component;
                type_ = type;
                return;
            }
        }
    }

    [[nodiscard]] bool present() const noexcept { return component_ != nullptr; }

    [[nodiscard]] const ser::WorldValue* value(std::string_view name) const noexcept {
        if (component_ == nullptr) {
            return nullptr;
        }
        for (const ser::WorldFieldDecl& declaration : type_->fields()) {
            if (world_->text(declaration.name) == name) {
                const ser::WorldField* field = component_->find(declaration.file_field);
                return field == nullptr ? nullptr : &field->value;
            }
        }
        return nullptr;
    }

    [[nodiscard]] f32 real(std::string_view name, f32 fallback) const noexcept {
        const ser::WorldValue* found = value(name);
        if (found == nullptr) {
            return fallback;
        }
        if (found->kind == ser::WorldValueKind::Float) {
            return found->lanes[0];
        }
        return found->kind == ser::WorldValueKind::Int ? static_cast<f32>(found->integer)
                                                       : fallback;
    }

    [[nodiscard]] i64 integer(std::string_view name, i64 fallback) const noexcept {
        const ser::WorldValue* found = value(name);
        return found != nullptr && found->kind == ser::WorldValueKind::Int ? found->integer
                                                                           : fallback;
    }

    [[nodiscard]] bool flag(std::string_view name, bool fallback) const noexcept {
        const ser::WorldValue* found = value(name);
        return found != nullptr && found->kind == ser::WorldValueKind::Bool ? found->integer != 0
                                                                            : fallback;
    }

    [[nodiscard]] Vec3 vector(std::string_view name, Vec3 fallback) const noexcept {
        const ser::WorldValue* found = value(name);
        return found != nullptr && found->kind == ser::WorldValueKind::Vec3
                   ? Vec3{found->lanes[0], found->lanes[1], found->lanes[2]}
                   : fallback;
    }

    [[nodiscard]] std::string text(std::string_view name) const {
        const ser::WorldValue* found = value(name);
        if (found == nullptr || found->kind != ser::WorldValueKind::Text) {
            return {};
        }
        const Span<const u8> bytes = world_->blob(*found);
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }

    /// A u64 recorded as an `int` (its bit pattern) or as decimal or `0x` text.
    [[nodiscard]] u64 word(std::string_view name) const {
        const ser::WorldValue* found = value(name);
        if (found != nullptr && found->kind == ser::WorldValueKind::Int) {
            u64 bits = 0;
            std::memcpy(&bits, &found->integer, sizeof(bits));
            return bits;
        }
        const std::string spelled = text(name);
        return spelled.empty() ? 0 : std::strtoull(spelled.c_str(), nullptr, 0);
    }

    [[nodiscard]] u32 world_id() const noexcept {
        return static_cast<u32>(integer("world", 0) & 0xFFFF'FFFF);
    }

private:
    const ser::World* world_;
    const ser::WorldComponent* component_ = nullptr;
    const ser::WorldTypeDecl* type_ = nullptr;
};

[[nodiscard]] nav::AreaType area_value(const ComponentReader& reader,
                                       nav::AreaType fallback) noexcept {
    const i64 area = reader.integer("area", fallback);
    return area >= 0 && std::cmp_less(area, nav::kAreaCount) ? static_cast<nav::AreaType>(area)
                                                             : fallback;
}

[[nodiscard]] nav::NavBakeSettings read_settings(const ComponentReader& reader) {
    nav::NavBakeSettings settings;
    settings.agent_radius = reader.real("agent_radius", settings.agent_radius);
    settings.agent_height = reader.real("agent_height", settings.agent_height);
    settings.max_slope_degrees = reader.real("max_slope", settings.max_slope_degrees);
    settings.step_height = reader.real("step_height", settings.step_height);
    settings.cell_size = reader.real("cell_size", settings.cell_size);
    settings.cell_height = reader.real("cell_height", settings.cell_height);
    settings.tile_size = reader.real("tile_size", settings.tile_size);
    if (reader.value("layers") != nullptr) {
        settings.layers = reader.word("layers");
    }
    if (reader.value("tags") != nullptr) {
        settings.tags = reader.word("tags");
    }
    const i64 backend = reader.integer("backend", static_cast<i64>(settings.backend));
    settings.backend = static_cast<nav::NavBuildBackend>(static_cast<u8>(backend & 0xFF));
    return settings;
}

// --- The contributions ------------------------------------------------------------------------

[[nodiscard]] Aabb local_box(const ComponentReader& reader, const Mat4& matrix) noexcept {
    const Vec3 low = reader.vector("bounds.min", Vec3{-0.5F, -0.5F, -0.5F});
    const Vec3 high = reader.vector("bounds.max", Vec3{0.5F, 0.5F, 0.5F});
    return transformed(Aabb::from_min_max(low, high), matrix);
}

[[nodiscard]] nav::NavSurfaceVolume surface_of(const ComponentReader& reader, u64 node,
                                               const Mat4& matrix) noexcept {
    return nav::NavSurfaceVolume{node, local_box(reader, matrix), reader.flag("exclude", false)};
}

[[nodiscard]] nav::NavAreaVolume area_of(const ComponentReader& reader, u64 node,
                                         const Mat4& matrix) noexcept {
    return nav::NavAreaVolume{node, local_box(reader, matrix), area_value(reader, nav::kAreaGround),
                              reader.real("cost", 1.0F)};
}

[[nodiscard]] nav::NavObstacleShape obstacle_of(const ComponentReader& reader,
                                                const Mat4& matrix) noexcept {
    nav::NavObstacleShape shape;
    shape.centre = transform_point(matrix, Vec3{}) + reader.vector("shape.offset", Vec3{});
    shape.radius = reader.real("shape.radius", 0.0F);
    shape.height = reader.real("shape.height", 0.0F);
    const Vec3 half = reader.vector("shape.half_extents", Vec3{0.5F, 0.5F, 0.5F});
    shape.bounds = Aabb::from_center_extents(shape.centre, half);
    shape.area = area_value(reader, nav::kAreaNull);
    return shape;
}

[[nodiscard]] nav::NavLink link_of(const ComponentReader& reader, const Mat4& matrix) {
    nav::NavLink link;
    link.from = transform_point(matrix, reader.vector("from", Vec3{}));
    link.to = transform_point(matrix, reader.vector("to", Vec3{0.0F, 0.0F, 1.0F}));
    link.cost = reader.real("cost", 1.0F);
    link.area = area_value(reader, nav::kAreaGround);
    link.bidirectional = reader.flag("bidirectional", true);
    link.requires_capabilities = reader.word("requires_capabilities");
    const std::string action = reader.text("action");
    link.action = action.empty() ? Name{} : Name::intern(action);
    return link;
}

/// A digest accumulated field by field, so padding never enters it.
class Digest {
public:
    Digest& add(u64 value) noexcept {
        value_ = hash_combine(value_, value);
        return *this;
    }
    Digest& add(f32 value) noexcept {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return add(static_cast<u64>(bits));
    }
    Digest& add(Vec3 value) noexcept { return add(value.x).add(value.y).add(value.z); }
    Digest& add(const Aabb& value) noexcept { return add(value.min).add(value.max); }
    Digest& add(std::string_view value) noexcept {
        return add(hash_bytes(value.data(), value.size()));
    }
    [[nodiscard]] u64 value() const noexcept { return value_; }

private:
    u64 value_ = 0x9E37'79B9'7F4A'7C15ULL;
};

[[nodiscard]] Aabb link_bounds(const nav::NavLink& link) noexcept {
    return merge(Aabb::from_point(link.from), Aabb::from_point(link.to));
}

/// The Nav* contributions of one node, in `NavRole` order.
void describe_nav_node(const ser::World& world, const ser::WorldNode& node, const Mat4& matrix,
                       std::vector<NavNodeState>& out) {
    const ComponentReader surface(world, node, kSurfaceType);
    if (surface.present()) {
        const nav::NavSurfaceVolume volume = surface_of(surface, node.identity, matrix);
        out.push_back({node.identity, NavRole::Surface, surface.world_id(), !volume.exclude,
                       Digest{}.add(volume.bounds).add(u64{volume.exclude}).value(),
                       volume.bounds});
    }
    const ComponentReader area(world, node, kAreaType);
    if (area.present()) {
        const nav::NavAreaVolume volume = area_of(area, node.identity, matrix);
        out.push_back({node.identity, NavRole::Area, area.world_id(), false,
                       Digest{}.add(volume.bounds).add(u64{volume.area}).add(volume.cost).value(),
                       volume.bounds});
    }
    const ComponentReader obstacle(world, node, kObstacleType);
    if (obstacle.present()) {
        const nav::NavObstacleShape shape = obstacle_of(obstacle, matrix);
        const u64 digest = Digest{}
                               .add(shape.centre)
                               .add(shape.radius)
                               .add(shape.height)
                               .add(shape.bounds)
                               .add(u64{shape.area})
                               .value();
        out.push_back(
            {node.identity, NavRole::Obstacle, obstacle.world_id(), false, digest, shape.extent()});
    }
    const ComponentReader link(world, node, kLinkType);
    if (link.present()) {
        const nav::NavLink value = link_of(link, matrix);
        const u64 digest = Digest{}
                               .add(value.from)
                               .add(value.to)
                               .add(value.cost)
                               .add(u64{value.area})
                               .add(u64{value.bidirectional})
                               .add(value.requires_capabilities)
                               .add(link.text("action"))
                               .value();
        out.push_back(
            {node.identity, NavRole::Link, link.world_id(), false, digest, link_bounds(value)});
    }
}

[[nodiscard]] bool overlaps_including(Span<const nav::NavSurfaceVolume> surfaces,
                                      const Aabb& bounds) noexcept {
    return std::ranges::any_of(surfaces, [&](const nav::NavSurfaceVolume& surface) {
        return !surface.exclude && surface.bounds.intersects(bounds);
    });
}

/// Appends a mesh's triangles, transformed into world space, to the source buffers.
[[nodiscard]] Status append_mesh(const import::MeshData& mesh, const Mat4& matrix,
                                 editor::NavSourceBuffers& out) noexcept {
    const usize base = out.vertices.size();
    if (base + mesh.positions.size() > std::numeric_limits<u32>::max()) {
        return fail(ErrorCode::OutOfRange, "navigation sources exceed 2^32 vertices");
    }
    for (const Vec3 position : mesh.positions) {
        if (Status pushed = out.vertices.push_back(transform_point(matrix, position)); !pushed) {
            return pushed;
        }
    }
    for (const u32 index : mesh.indices) {
        if (Status pushed = out.indices.push_back(static_cast<u32>(base) + index); !pushed) {
            return pushed;
        }
    }
    return ok();
}

[[nodiscard]] Status read_primitive(const std::string& path, Allocator& allocator,
                                    import::MeshData& out) noexcept {
    Array<u8> source(allocator);
    if (Status status = assets::fs::read_whole(path.c_str(), source); !status) {
        return status;
    }
    const std::string_view text(reinterpret_cast<const char*>(source.data()), source.size());
    Expected<import::PrimitiveSpec, Error> spec = import::parse_primitive_source(text);
    if (!spec) {
        return make_unexpected(spec.error());
    }
    return import::build_primitive_mesh(*spec, out);
}

[[nodiscard]] Status read_cooked(const std::string& path, Allocator& allocator,
                                 import::MeshData& out) noexcept {
    Array<u8> cooked(allocator);
    if (Status status = assets::fs::read_whole(path.c_str(), cooked); !status) {
        return status;
    }
    Expected<Span<const u8>, Error> payload =
        assets::read_cooked_payload(cooked.data(), cooked.size(), true);
    if (!payload) {
        return make_unexpected(payload.error());
    }
    return import::read_cooked_mesh(*payload, out);
}

[[nodiscard]] std::string sidecar_name(u64 identity) {
    char name[64] = {};
    (void)std::snprintf(name, sizeof(name), "navigation/%016" PRIx64 ".cynavmesh", identity);
    return name;
}

// --- Payloads ---------------------------------------------------------------------------------

class Writer {
public:
    Writer& u8_(u8 value) {
        bytes.push_back(value);
        return *this;
    }
    Writer& u32_(u32 value) {
        for (u32 shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<u8>((value >> shift) & 0xFFU));
        }
        return *this;
    }
    Writer& u64_(u64 value) {
        u32_(static_cast<u32>(value & 0xFFFF'FFFFU));
        return u32_(static_cast<u32>(value >> 32U));
    }
    Writer& f32_(f32 value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return u32_(bits);
    }
    Writer& vec3(Vec3 value) { return f32_(value.x).f32_(value.y).f32_(value.z); }
    Writer& settings(const nav::NavBakeSettings& value) {
        f32_(value.agent_radius).f32_(value.agent_height).f32_(value.max_slope_degrees);
        f32_(value.step_height).f32_(value.cell_size).f32_(value.cell_height).f32_(value.tile_size);
        return u64_(value.layers).u64_(value.tags).u8_(static_cast<u8>(value.backend));
    }

    std::vector<u8> bytes;
};

/// Reads an event payload; a read past the end sets `overrun` and yields zero.
class Reader {
public:
    explicit Reader(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    u8 u8_() noexcept { return take(1) ? bytes_[cursor_ - 1] : u8{0}; }
    u32 u32_() noexcept {
        if (!take(4)) {
            return 0;
        }
        u32 value = 0;
        for (u32 byte = 0; byte < 4; ++byte) {
            value |= static_cast<u32>(bytes_[cursor_ - 4 + byte]) << (byte * 8U);
        }
        return value;
    }
    i32 i32_() noexcept {
        const u32 bits = u32_();
        i32 value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    u64 u64_() noexcept {
        const u64 low = u32_();
        return low | (static_cast<u64>(u32_()) << 32U);
    }
    f32 f32_() noexcept {
        const u32 bits = u32_();
        f32 value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    Vec3 vec3() noexcept {
        const f32 x = f32_();
        const f32 y = f32_();
        return Vec3{x, y, f32_()};
    }
    std::string text() {
        const u32 length = u32_();
        if (!take(length)) {
            return {};
        }
        return {reinterpret_cast<const char*>(bytes_.data() + cursor_ - length), length};
    }
    void coords(std::vector<nav::TileCoord>& out) {
        const u32 count = u32_();
        for (u32 index = 0; index < count && !overrun; ++index) {
            const i32 x = i32_();
            const i32 z = i32_();
            out.push_back(nav::TileCoord{x, z, i32_()});
        }
    }

    bool overrun = false;

private:
    bool take(usize count) noexcept {
        if (overrun || bytes_.size() - cursor_ < count) {
            overrun = true;
            return false;
        }
        cursor_ += count;
        return true;
    }

    Span<const u8> bytes_;
    usize cursor_ = 0;
};

[[nodiscard]] bool terminal(u32 kind) noexcept {
    return kind == CY_SERVICE_EVENT_COMPLETED || kind == CY_SERVICE_EVENT_FAILED ||
           kind == CY_SERVICE_EVENT_CANCELLED;
}

[[nodiscard]] Span<const u8> payload_of(const CyServiceEvent& event) noexcept {
    return {event.payload, static_cast<usize>(event.payload_size)};
}

using NodeKey = std::pair<u64, NavRole>;

[[nodiscard]] std::map<NodeKey, const NavNodeState*> index_nodes(
    const std::vector<NavNodeState>& nodes) {
    std::map<NodeKey, const NavNodeState*> out;
    for (const NavNodeState& node : nodes) {
        out.emplace(NodeKey{node.identity, node.role}, &node);
    }
    return out;
}

/// Every state that changed between the two lists: its old version (or null) and its new one.
[[nodiscard]] std::vector<std::pair<const NavNodeState*, const NavNodeState*>> changes_between(
    const std::vector<NavNodeState>& before, const std::vector<NavNodeState>& after) {
    std::map<NodeKey, const NavNodeState*> old = index_nodes(before);
    std::vector<std::pair<const NavNodeState*, const NavNodeState*>> out;
    for (const NavNodeState& next : after) {
        const auto found = old.find(NodeKey{next.identity, next.role});
        const NavNodeState* previous = found == old.end() ? nullptr : found->second;
        if (found != old.end()) {
            old.erase(found);
        }
        if (previous == nullptr || previous->digest != next.digest ||
            previous->world != next.world) {
            out.emplace_back(previous, &next);
        }
    }
    for (const auto& [key, removed] : old) {
        out.emplace_back(removed, nullptr);
    }
    return out;
}

void grow(std::map<u32, Aabb>& dirty, u32 world, const Aabb& bounds) {
    if (bounds.is_empty()) {
        return;
    }
    auto [entry, inserted] = dirty.emplace(world, bounds);
    if (!inserted) {
        entry->second = merge(entry->second, bounds);
    }
}

/// A mesh dirties every world whose including surface overlaps it, before or after the edit.
void grow_mesh(std::map<u32, Aabb>& dirty, const Aabb& mesh,
               const std::vector<NavNodeState>& before, const std::vector<NavNodeState>& after) {
    for (const std::vector<NavNodeState>* list : {&before, &after}) {
        for (const NavNodeState& state : *list) {
            if (state.role == NavRole::Surface && state.including &&
                state.bounds.intersects(mesh)) {
                grow(dirty, state.world, mesh);
            }
        }
    }
}

}  // namespace

// --- Document records -------------------------------------------------------------------------

Status read_navigation_worlds(const ser::World& world, Array<NavWorldRecord>& out) noexcept {
    for (const ser::WorldNode& node : world.nodes()) {
        const ComponentReader reader(world, node, kWorldType);
        if (!node.live || !reader.present()) {
            continue;
        }
        const u32 id = reader.world_id();
        const bool seen = std::ranges::any_of(
            out.span(), [id](const NavWorldRecord& record) { return record.world == id; });
        if (seen) {
            continue;
        }
        NavWorldRecord record;
        record.world = id;
        record.settings = read_settings(reader);
        record.overlay = static_cast<u32>(reader.integer("overlay", 0) & 0xFFFF'FFFF);
        record.bake_identity = reader.word("bake_identity");
        record.source_fingerprint = reader.word("source_fingerprint");
        if (Status pushed = out.push_back(record); !pushed) {
            return pushed;
        }
    }
    return ok();
}

std::vector<NavDirtyRegion> dirty_regions(const std::vector<NavNodeState>& before,
                                          const std::vector<NavNodeState>& after) {
    std::map<u32, Aabb> dirty;
    for (const auto& [previous, next] : changes_between(before, after)) {
        for (const NavNodeState* state : {previous, next}) {
            if (state == nullptr) {
                continue;
            }
            if (state->role == NavRole::Mesh) {
                grow_mesh(dirty, state->bounds, before, after);
            } else {
                grow(dirty, state->world, state->bounds);
            }
        }
    }
    std::vector<NavDirtyRegion> out;
    out.reserve(dirty.size());
    for (const auto& [world, region] : dirty) {
        out.push_back(NavDirtyRegion{world, region});
    }
    return out;
}

// --- AuthoredNavigationSource -----------------------------------------------------------------

AuthoredNavigationSource::AuthoredNavigationSource(Allocator& allocator,
                                                   std::string project) noexcept
    : allocator_(&allocator), project_(std::move(project)), sidecar_root_(project_) {}

AuthoredNavigationSource::~AuthoredNavigationSource() = default;

void AuthoredNavigationSource::record_frame(u64 frame, const render::View& view, Vec3 eye) {
    frames_.push_back(Frame{frame, NavOverlayView{view, eye}});
    if (frames_.size() > kFramesKept) {
        frames_.pop_front();
    }
}

const NavOverlayView* AuthoredNavigationSource::frame_view(u64 frame) const noexcept {
    if (frames_.empty()) {
        return nullptr;
    }
    if (frame == 0) {
        return &frames_.back().view;
    }
    for (const Frame& held : frames_) {
        if (held.identity == frame) {
            return &held.view;
        }
    }
    return nullptr;
}

const AuthoredNavigationSource::LoadedMesh* AuthoredNavigationSource::mesh(
    const std::string& reference) noexcept {
    const auto found = meshes_.find(reference);
    if (found != meshes_.end()) {
        return found->second.get();
    }
    auto loaded = std::make_unique<LoadedMesh>();
    const bool primitive = reference.ends_with(".cyprim");
    const std::string path =
        primitive ? project_ + "/" + reference : project_ + "/.cy/cooked/" + reference + ".cyasset";
    Status read = primitive ? read_primitive(path, *allocator_, loaded->data)
                            : read_cooked(path, *allocator_, loaded->data);
    if (read) {
        read = loaded->data.validate();
    }
    if (!read) {
        mesh_failures_ += 1;
        std::fprintf(stderr, "navigation source mesh '%s': %s\n", reference.c_str(),
                     read.error().message);
        return nullptr;
    }
    loaded->bounds = loaded->data.bounds();
    return meshes_.emplace(reference, std::move(loaded)).first->second.get();
}

Status AuthoredNavigationSource::world_matrices(Array<Mat4>& out) const noexcept {
    const Span<const ser::WorldNode> nodes = world_->nodes().span();
    if (Status sized = out.resize(nodes.size()); !sized) {
        return sized;
    }
    for (usize row = 0; row < nodes.size(); ++row) {
        Transform local;
        (void)ser::transform_of(*world_, nodes[row], local);
        out[row] = local.to_matrix();
        const u32 parent = nodes[row].parent;
        if (parent < row && nodes[parent].live) {
            out[row] = out[parent] * out[row];
        }
    }
    return ok();
}

Status AuthoredNavigationSource::describe(std::vector<NavNodeState>& out) noexcept {
    out.clear();
    if (world_ == nullptr) {
        return ok();
    }
    Array<Mat4> matrices(*allocator_);
    if (Status built = world_matrices(matrices); !built) {
        return built;
    }
    const Span<const ser::WorldNode> nodes = world_->nodes().span();
    for (usize row = 0; row < nodes.size(); ++row) {
        const ser::WorldNode& node = nodes[row];
        if (!node.live) {
            continue;
        }
        const std::string reference = ComponentReader(*world_, node, kMeshType).text("mesh");
        if (!reference.empty()) {
            const LoadedMesh* loaded = mesh(reference);
            const Aabb bounds =
                loaded == nullptr ? Aabb{} : transformed(loaded->bounds, matrices[row]);
            out.push_back({node.identity, NavRole::Mesh, 0, false,
                           Digest{}.add(reference).add(bounds).value(), bounds});
        }
        describe_nav_node(*world_, node, matrices[row], out);
    }
    return ok();
}

Status AuthoredNavigationSource::worlds(Array<u32>& out) noexcept {
    if (world_ == nullptr) {
        return ok();
    }
    std::vector<u32> ids;
    for (const ser::WorldNode& node : world_->nodes()) {
        for (const std::string_view type : {kWorldType, kSurfaceType}) {
            const ComponentReader reader(*world_, node, type);
            if (node.live && reader.present()) {
                ids.push_back(reader.world_id());
            }
        }
    }
    std::ranges::sort(ids);
    const auto [first, last] = std::ranges::unique(ids);
    ids.erase(first, last);
    for (const u32 id : ids) {
        if (Status pushed = out.push_back(id); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status AuthoredNavigationSource::gather_meshes(Span<const Mat4> matrices,
                                               editor::NavSourceBuffers& out) noexcept {
    const Span<const ser::WorldNode> nodes = world_->nodes().span();
    for (usize row = 0; row < nodes.size(); ++row) {
        const std::string reference = ComponentReader(*world_, nodes[row], kMeshType).text("mesh");
        const LoadedMesh* loaded =
            reference.empty() || !nodes[row].live ? nullptr : mesh(reference);
        if (loaded == nullptr ||
            !overlaps_including(out.surfaces.span(), transformed(loaded->bounds, matrices[row]))) {
            continue;
        }
        if (Status appended = append_mesh(loaded->data, matrices[row], out); !appended) {
            return appended;
        }
    }
    return ok();
}

Status AuthoredNavigationSource::gather(u32 world, editor::NavSourceBuffers& out) noexcept {
    if (world_ == nullptr) {
        return fail(ErrorCode::NotFound, "no world is open");
    }
    Array<Mat4> matrices(*allocator_);
    if (Status built = world_matrices(matrices); !built) {
        return built;
    }
    const Span<const ser::WorldNode> nodes = world_->nodes().span();
    for (usize row = 0; row < nodes.size(); ++row) {
        const ComponentReader surface(*world_, nodes[row], kSurfaceType);
        const ComponentReader area(*world_, nodes[row], kAreaType);
        Status pushed = ok();
        if (nodes[row].live && surface.present() && surface.world_id() == world) {
            pushed =
                out.surfaces.push_back(surface_of(surface, nodes[row].identity, matrices[row]));
        }
        if (pushed && nodes[row].live && area.present() && area.world_id() == world) {
            pushed = out.areas.push_back(area_of(area, nodes[row].identity, matrices[row]));
        }
        if (!pushed) {
            return pushed;
        }
    }
    return gather_meshes(matrices.span(), out);
}

Status AuthoredNavigationSource::obstacles(u32 world, Array<nav::NavObstacleShape>& out) noexcept {
    if (world_ == nullptr) {
        return ok();
    }
    Array<Mat4> matrices(*allocator_);
    if (Status built = world_matrices(matrices); !built) {
        return built;
    }
    const Span<const ser::WorldNode> nodes = world_->nodes().span();
    for (usize row = 0; row < nodes.size(); ++row) {
        const ComponentReader reader(*world_, nodes[row], kObstacleType);
        if (!nodes[row].live || !reader.present() || reader.world_id() != world) {
            continue;
        }
        if (Status pushed = out.push_back(obstacle_of(reader, matrices[row])); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status AuthoredNavigationSource::links(u32 world, Array<nav::NavLink>& out) noexcept {
    if (world_ == nullptr) {
        return ok();
    }
    Array<Mat4> matrices(*allocator_);
    if (Status built = world_matrices(matrices); !built) {
        return built;
    }
    const Span<const ser::WorldNode> nodes = world_->nodes().span();
    for (usize row = 0; row < nodes.size(); ++row) {
        const ComponentReader reader(*world_, nodes[row], kLinkType);
        if (!nodes[row].live || !reader.present() || reader.world_id() != world) {
            continue;
        }
        if (Status pushed = out.push_back(link_of(reader, matrices[row])); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status AuthoredNavigationSource::store_bake(u64 identity, Span<const u8> bytes,
                                            Array<char>& out_path) noexcept {
    const std::string relative = sidecar_name(identity);
    const std::string directory = sidecar_root_ + "/navigation";
    const std::string path = sidecar_root_ + "/" + relative;
    if (Status made = assets::fs::create_directories(directory.c_str()); !made) {
        return made;
    }
    // Content-addressed: a sidecar already under this identity holds these bytes, so it is kept
    // rather than rewritten.
    if (!assets::fs::exists(path.c_str())) {
        if (Status written = assets::fs::write_atomic(path.c_str(), bytes.data(), bytes.size());
            !written) {
            return written;
        }
    }
    return out_path.append(Span<const char>(relative.data(), relative.size()));
}

Status AuthoredNavigationSource::load_bake(u64 identity, Array<u8>& out) noexcept {
    const std::string path = sidecar_root_ + "/" + sidecar_name(identity);
    return assets::fs::read_whole(path.c_str(), out);
}

Expected<Ray, Error> AuthoredNavigationSource::pick_ray(u32, u64 frame, f32 x, f32 y) noexcept {
    const NavOverlayView* held = frame_view(frame);
    if (held == nullptr) {
        return make_unexpected(
            Error{ErrorCode::Unavailable, "the runtime no longer holds that frame's view"});
    }
    // The view is camera-relative, as the renderer's is: the ray starts at the eye.
    Ray ray = render::ray_through_pixel(held->view, x, y);
    ray.origin = ray.origin + held->eye;
    return ray;
}

// --- NavigationDriver -------------------------------------------------------------------------

Status NavigationDriver::document_changed(AuthoredNavigationSource& source) {
    std::vector<NavNodeState> nodes;
    if (Status described = source.describe(nodes); !described) {
        return described;
    }
    std::vector<NavWorldRecord> records;
    if (source.bound() != nullptr) {
        Array<NavWorldRecord> read;
        if (Status status = read_navigation_worlds(*source.bound(), read); !status) {
            return status;
        }
        records.assign(read.span().begin(), read.span().end());
    }
    queue_world_changes(records);
    for (const NavDirtyRegion& dirty : dirty_regions(nodes_, nodes)) {
        queue_update(dirty.world, dirty.region);
    }
    nodes_ = std::move(nodes);
    worlds_ = std::move(records);
    return ok();
}

void NavigationDriver::queue_clear(u32 world) {
    Writer writer;
    writer.u32_(world);
    queue_.push_back(Job{JobKind::Clear, world, std::move(writer.bytes)});
}

void NavigationDriver::queue_world_changes(const std::vector<NavWorldRecord>& next) {
    // A world whose recorded bake went away (the first bake undone, or the world removed) must
    // not keep answering queries on the mesh the service still holds for it.
    for (const NavWorldRecord& held : worlds_) {
        const auto now = std::ranges::find_if(
            next, [&](const NavWorldRecord& record) { return record.world == held.world; });
        if (held.bake_identity != 0 && (now == next.end() || now->bake_identity == 0)) {
            queue_clear(held.world);
        }
    }
    for (const NavWorldRecord& record : next) {
        const auto previous = std::ranges::find_if(
            worlds_, [&](const NavWorldRecord& held) { return held.world == record.world; });
        const bool known = previous != worlds_.end();
        if (record.bake_identity != 0 &&
            (!known || previous->bake_identity != record.bake_identity)) {
            Writer writer;
            writer.u32_(record.world)
                .settings(record.settings)
                .u64_(record.bake_identity)
                .u64_(record.source_fingerprint);
            queue_.push_back(Job{JobKind::Restore, record.world, std::move(writer.bytes)});
        }
        if (record.overlay != (known ? previous->overlay : 0U)) {
            Writer writer;
            writer.u32_(record.world).u32_(record.overlay);
            queue_.push_back(Job{JobKind::Overlay, record.world, std::move(writer.bytes)});
        }
    }
}

void NavigationDriver::queue_update(u32 world, const Aabb& region) {
    Writer writer;
    writer.u32_(world).vec3(region.min).vec3(region.max);
    for (Job& job : queue_) {
        if (job.kind == JobKind::Update && job.world == world) {
            // One queued update per world: widen it rather than rebuilding twice.
            Reader reader(Span<const u8>(job.payload.data(), job.payload.size()));
            (void)reader.u32_();
            const Vec3 low = reader.vec3();
            const Aabb merged = merge(Aabb::from_min_max(low, reader.vec3()), region);
            Writer widened;
            widened.u32_(world).vec3(merged.min).vec3(merged.max);
            job.payload = std::move(widened.bytes);
            return;
        }
    }
    queue_.push_back(Job{JobKind::Update, world, std::move(writer.bytes)});
}

void NavigationDriver::pump() noexcept {
    if (in_flight_ != 0 || backoff_ || queue_.empty()) {
        return;
    }
    current_ = std::move(queue_.front());
    queue_.pop_front();
    static constexpr const char* kOperations[] = {"navigation.status", "navigation.overlay.set",
                                                  "navigation.update", "navigation.clear"};
    const u64 id = kInternalRequest | next_request_++;
    const CyServiceRequest request{sizeof(CyServiceRequest),
                                   1,
                                   id,
                                   kOperations[static_cast<usize>(current_.kind)],
                                   current_.payload.data(),
                                   current_.payload.size()};
    if (service_->submit(session_, request) != CY_RESULT_OK) {
        // Refused outright (the service already holds a busy refusal): keep the job and retry it
        // on the next frame, as for a `navigation.busy` answer, rather than lose a restore or an
        // update until the next document change.
        last_failure_ = "the navigation service refused the runtime's request";
        queue_.push_front(std::move(current_));
        backoff_ = true;
        return;
    }
    in_flight_ = id;
}

bool NavigationDriver::observe(const CyServiceEvent& event) {
    if ((event.request_id & kInternalRequest) != 0) {
        if (event.request_id == in_flight_ && terminal(event.kind)) {
            finish_internal(event);
        }
        return true;
    }
    const auto query = queries_.find(event.request_id);
    if (query == queries_.end()) {
        return false;
    }
    if (event.kind == CY_SERVICE_EVENT_COMPLETED) {
        record_query(query->second, payload_of(event));
    }
    if (terminal(event.kind)) {
        queries_.erase(query);
    }
    return false;
}

void NavigationDriver::finish_internal(const CyServiceEvent& event) {
    in_flight_ = 0;
    Reader reader(payload_of(event));
    if (event.kind != CY_SERVICE_EVENT_COMPLETED) {
        (void)reader.u32_();
        const std::string code = reader.text();
        if (code == "navigation.busy") {
            // The editor's request goes first; this one is retried on the next frame.
            queue_.push_front(std::move(current_));
            backoff_ = true;
            return;
        }
        last_failure_ = code;
        return;
    }
    if (current_.kind == JobKind::Restore) {
        restores_completed_ += 1;
        return;
    }
    if (current_.kind == JobKind::Clear) {
        clears_completed_ += 1;
        return;
    }
    if (current_.kind != JobKind::Update) {
        return;
    }
    NavUpdateResult result;
    result.world = reader.u32_();
    (void)reader.u64_();
    result.identity = reader.u64_();
    reader.coords(result.rebuilt);
    reader.coords(result.marked);
    if (!reader.overrun) {
        last_update_ = std::move(result);
        updates_completed_ += 1;
    }
}

void NavigationDriver::editor_request(u64 id, std::string_view operation, Span<const u8> payload) {
    const bool path = operation == "navigation.path.query";
    const bool flow = operation == "navigation.flowfield.query";
    if (!path && !flow) {
        return;
    }
    Reader reader(payload);
    Query query;
    query.flow = flow;
    query.world = reader.u32_();
    if (flow) {
        (void)reader.vec3();  // the target
        const Vec3 low = reader.vec3();
        query.region = Aabb::from_min_max(low, reader.vec3());
    }
    if (!reader.overrun) {
        queries_[id] = query;
    }
}

void NavigationDriver::record_query(const Query& query, Span<const u8> payload) {
    WorldOverlay& overlay = overlays_[query.world];
    Reader reader(payload);
    if (!query.flow) {
        (void)reader.u8_();  // found
        (void)reader.u8_();  // partial
        (void)reader.u8_();  // budget exceeded
        (void)reader.f32_();
        (void)reader.u32_();
        const u32 count = reader.u32_();
        overlay.path.clear();
        for (u32 index = 0; index < count && !reader.overrun; ++index) {
            overlay.path.push_back(reader.vec3());
            (void)reader.u8_();
        }
        return;
    }
    NavFlowOverlay& flow = overlay.flow;
    flow.region = query.region;
    flow.width = reader.u32_();
    flow.depth = reader.u32_();
    flow.cell = reader.f32_();
    (void)reader.u32_();
    flow.directions.clear();
    flow.reachable.clear();
    const usize cells = static_cast<usize>(flow.width) * flow.depth;
    for (usize index = 0; index < cells && !reader.overrun; ++index) {
        const f32 dx = reader.f32_();
        const f32 dz = reader.f32_();
        const bool pushed = flow.directions.push_back(Vec2{dx, dz}).has_value() &&
                            flow.reachable.push_back(reader.u8_()).has_value();
        if (!pushed) {
            break;
        }
    }
    overlay.has_flow = !reader.overrun && flow.directions.size() == cells;
}

void NavigationDriver::overlay_worlds(CyServiceSession navigation_session,
                                      std::vector<NavOverlayWorld>& out) const {
    out.clear();
    for (const NavWorldRecord& record : worlds_) {
        if (record.bake_identity == 0) {
            continue;  // the document records no accepted bake for this world
        }
        NavOverlayWorld world;
        world.world = record.world;
        world.mesh = editor::NavigationService::mesh(navigation_session, record.world);
        world.flags = editor::NavigationService::overlay(navigation_session, record.world);
        const auto overlay = overlays_.find(record.world);
        if (overlay != overlays_.end()) {
            world.path = Span<const Vec3>(overlay->second.path.data(), overlay->second.path.size());
            world.flow = overlay->second.has_flow ? &overlay->second.flow : nullptr;
        }
        out.push_back(world);
    }
}

void draw_editor_navigation(const NavigationDriver& driver, CyServiceSession navigation_session,
                            const NavOverlayView& view, const Canvas& canvas,
                            std::vector<NavOverlayWorld>& worlds) {
    driver.overlay_worlds(navigation_session, worlds);
    draw_navigation_overlays(canvas, view,
                             Span<const NavOverlayWorld>(worlds.data(), worlds.size()));
}

u32 drain_service_events(abi::EditorServiceBackend& service, CyServiceSession session,
                         NavigationDriver& driver, u32 limit, ServiceEventSink forward,
                         void* user) {
    driver.begin_frame();
    driver.pump();
    u32 polled = 0;
    while (polled < limit) {
        CyServiceEvent event{};
        bool present = false;
        if (service.poll(session, event, present) != CY_RESULT_OK || !present) {
            break;
        }
        polled += 1;
        if (!driver.observe(event) && forward != nullptr) {
            forward(user, event);
        }
        // A finished runtime request frees the service for the next one in the same frame.
        driver.pump();
    }
    return polled;
}

}  // namespace cy::sample::editor_window
