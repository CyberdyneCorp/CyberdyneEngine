// SPDX-License-Identifier: MIT
// The engine's navigation authoring service. See cy/editor/navigation_service.h and the
// "Navigation operations" section of src/editor_backend/README.md for the wire format.

#include <cy/editor/navigation_service.h>

#include <cy/core/math/geometry.h>
#include <cy/navigation/bake_codec.h>
#include <cy/navigation/debug.h>
#include <cy/navigation/flow_field.h>
#include <cy/navigation/query.h>
#include <cy/navigation/tile_identity.h>

#include "service_wire.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <new>
#include <string_view>
#include <utility>

namespace cy::editor {
namespace {

namespace nav = cy::navigation;
using wire::put_f32;
using wire::put_i32;
using wire::put_text;
using wire::put_u32;
using wire::put_u64;
using wire::put_u8;
using wire::put_vec3;
using wire::Reader;

/// Navigation worlds one session keeps a context for.
constexpr u32 kMaxWorlds = 16;
/// The A* expansion budget of a test path.
constexpr u32 kPathBudget = 8192;
/// How far a link endpoint may lie from the mesh and still snap onto it.
constexpr Vec3 kLinkSnap{1.0F, 2.0F, 1.0F};
/// The largest flow field a query may ask for: 256 x 256 cells.
constexpr u32 kMaxFlowCells = 65536;
/// How far a pick ray reaches, in metres.
constexpr f32 kPickDistance = 100000.0F;
/// The polygon picks and the overlay report, when a request names none.
constexpr u32 kAllOverlayFlags = static_cast<u32>(nav::NavDebugFlags::All);

/// A navigation mesh owned through a pointer, allocated from the service's allocator: empty until
/// a bake or a restore creates it.
class OwnedMesh {
public:
    OwnedMesh() noexcept = default;
    ~OwnedMesh() { reset(); }
    OwnedMesh(const OwnedMesh&) = delete;
    OwnedMesh& operator=(const OwnedMesh&) = delete;
    OwnedMesh(OwnedMesh&&) = delete;
    OwnedMesh& operator=(OwnedMesh&&) = delete;

    /// Replaces the mesh with an empty one of `tile_size`.
    [[nodiscard]] bool create(Allocator& allocator, f32 tile_size) noexcept {
        reset();
        void* memory = allocator.allocate(sizeof(nav::NavMesh), alignof(nav::NavMesh));
        if (memory == nullptr) {
            return false;
        }
        allocator_ = &allocator;
        mesh_ = new (memory) nav::NavMesh(allocator, Name::intern("editor.navigation"), tile_size);
        return true;
    }

    void reset() noexcept {
        if (mesh_ != nullptr) {
            mesh_->~NavMesh();
            allocator_->deallocate(mesh_, sizeof(nav::NavMesh), alignof(nav::NavMesh));
            mesh_ = nullptr;
        }
    }

    /// Takes `other`'s mesh, leaving `other` empty.
    void take(OwnedMesh& other) noexcept {
        reset();
        allocator_ = other.allocator_;
        mesh_ = other.mesh_;
        other.mesh_ = nullptr;
    }

    [[nodiscard]] nav::NavMesh* get() const noexcept { return mesh_; }
    [[nodiscard]] nav::NavMesh& operator*() const noexcept { return *mesh_; }
    [[nodiscard]] nav::NavMesh* operator->() const noexcept { return mesh_; }
    [[nodiscard]] explicit operator bool() const noexcept { return mesh_ != nullptr; }

private:
    Allocator* allocator_ = nullptr;
    nav::NavMesh* mesh_ = nullptr;
};

/// One navigation world's state in a session.
struct NavContext {
    NavContext(Allocator& allocator, u32 id) noexcept
        : world(id),
          report(allocator),
          obstacle_ids(allocator),
          obstacle_shapes(allocator),
          link_ids(allocator) {}

    u32 world = 0;
    OwnedMesh mesh;
    nav::NavBakeSettings settings;
    nav::NavAreaCosts costs = nav::NavAreaCosts::uniform();
    nav::NavBakeReport report;
    /// The identity of the mesh as it stands, which a live `navigation.update` moves.
    u64 identity = 0;
    /// The identity of the bake the mesh was built or restored from: its sidecar's name.
    u64 sidecar = 0;
    u64 fingerprint = 0;
    u32 overlay = 0;
    u32 link_failures = 0;
    Array<nav::ObstacleId> obstacle_ids;
    Array<nav::NavObstacleShape> obstacle_shapes;
    Array<nav::LinkId> link_ids;
};

/// A bake in flight: one tile per poll into a staging mesh that replaces the world's on success.
struct BakeJob {
    explicit BakeJob(Allocator& allocator) noexcept
        : sources(allocator), coords(allocator), report(allocator) {}

    bool active = false;
    u32 world = 0;
    u32 next = 0;
    u64 fingerprint = 0;
    nav::NavBakeSettings settings;
    NavSourceBuffers sources;
    Array<nav::TileCoord> coords;
    OwnedMesh staging;
    nav::NavBakeReport report;
};

struct NavigationSession {
    explicit NavigationSession(Allocator& allocator) noexcept
        : request_payload(allocator), event_payload(allocator), job(allocator) {}

    u64 request = 0;
    u32 schema = 0;
    char operation[64] = {};
    Array<u8> request_payload;
    Array<u8> event_payload;
    bool pending = false;
    bool cancelled = false;
    bool failed_event = false;
    bool progress_event = false;
    /// A request refused because another was pending, owed its `navigation.busy` event.
    u64 busy_request = 0;
    NavContext* contexts[kMaxWorlds] = {};
    BakeJob job;
};

[[nodiscard]] NavigationSession* state_of(CyServiceSession session) noexcept {
    return reinterpret_cast<NavigationSession*>(session);
}

/// What an operation handler works with.
struct Call {
    NavigationSession& session;
    NavigationSourceRuntime& runtime;
    Allocator& allocator;
};

[[nodiscard]] CyResult written(const Status& status) noexcept {
    return status ? CY_RESULT_OK : CY_RESULT_OUT_OF_MEMORY;
}

CyResult failed(NavigationSession& session, std::string_view code,
                std::string_view detail) noexcept {
    session.failed_event = true;
    return written(wire::encode_failure(session.event_payload, code, detail));
}

void clear_sources(NavSourceBuffers& sources) noexcept {
    sources.vertices.clear();
    sources.indices.clear();
    sources.layer.clear();
    sources.tag.clear();
    sources.area.clear();
    sources.surfaces.clear();
    sources.areas.clear();
}

void clear_report(nav::NavBakeReport& report) noexcept {
    report.totals = {};
    report.tiles_built = 0;
    report.tiles_empty = 0;
    report.cancelled = false;
    report.tiles.clear();
}

void reset_job(BakeJob& job) noexcept {
    job.active = false;
    job.next = 0;
    job.coords.clear();
    job.staging.reset();
    clear_sources(job.sources);
    clear_report(job.report);
}

// --- Request decoding -------------------------------------------------------------------------

/// The settings block: seven f32 (agent radius, agent height, max slope, step height, cell size,
/// cell height, tile size), u64 layers, u64 tags, u8 backend.
[[nodiscard]] nav::NavBakeSettings read_settings(Reader& reader) noexcept {
    nav::NavBakeSettings settings;
    settings.agent_radius = reader.read_f32();
    settings.agent_height = reader.read_f32();
    settings.max_slope_degrees = reader.read_f32();
    settings.step_height = reader.read_f32();
    settings.cell_size = reader.read_f32();
    settings.cell_height = reader.read_f32();
    settings.tile_size = reader.read_f32();
    settings.layers = reader.read_u64();
    settings.tags = reader.read_u64();
    settings.backend = static_cast<nav::NavBuildBackend>(reader.read_u8());
    return settings;
}

[[nodiscard]] Reader reader_of(const NavigationSession& session) noexcept {
    return Reader(session.request_payload.span());
}

// --- Contexts ---------------------------------------------------------------------------------

[[nodiscard]] NavContext* find_context(NavigationSession& session, u32 world) noexcept {
    for (NavContext* context : session.contexts) {
        if (context != nullptr && context->world == world) {
            return context;
        }
    }
    return nullptr;
}

[[nodiscard]] NavContext* context_for(NavigationSession& session, Allocator& allocator,
                                      u32 world) noexcept {
    if (NavContext* found = find_context(session, world); found != nullptr) {
        return found;
    }
    for (NavContext*& slot : session.contexts) {
        if (slot == nullptr) {
            void* memory = allocator.allocate(sizeof(NavContext), alignof(NavContext));
            if (memory == nullptr) {
                return nullptr;
            }
            slot = new (memory) NavContext(allocator, world);
            return slot;
        }
    }
    return nullptr;
}

void destroy_contexts(NavigationSession& session, Allocator& allocator) noexcept {
    for (NavContext*& context : session.contexts) {
        if (context != nullptr) {
            context->~NavContext();
            allocator.deallocate(context, sizeof(NavContext), alignof(NavContext));
            context = nullptr;
        }
    }
}

[[nodiscard]] bool world_known(NavigationSourceRuntime& runtime, Allocator& allocator,
                               u32 world) noexcept {
    Array<u32> worlds(allocator);
    if (!runtime.worlds(worlds)) {
        return false;
    }
    return std::ranges::any_of(worlds.span(), [world](u32 id) { return id == world; });
}

/// The baked context of `world`, or a failure event naming why there is none.
[[nodiscard]] NavContext* baked_context(Call& call, u32 world, CyResult& result) noexcept {
    NavContext* context = find_context(call.session, world);
    if (context == nullptr || !context->mesh) {
        result = failed(call.session, "navigation.world.unbaked",
                        "this navigation world has no baked mesh in this session");
        return nullptr;
    }
    result = CY_RESULT_OK;
    return context;
}

// --- Obstacles and links ----------------------------------------------------------------------

[[nodiscard]] bool same_shape(const nav::NavObstacleShape& a,
                              const nav::NavObstacleShape& b) noexcept {
    return a.centre == b.centre && a.radius == b.radius && a.height == b.height &&
           a.bounds.min == b.bounds.min && a.bounds.max == b.bounds.max && a.area == b.area;
}

[[nodiscard]] bool contains_coord(Span<const nav::TileCoord> coords,
                                  nav::TileCoord coord) noexcept {
    return std::ranges::any_of(
        coords, [coord](const nav::TileCoord& candidate) { return candidate == coord; });
}

[[nodiscard]] Status merge_coords(Array<nav::TileCoord>& into,
                                  Span<const nav::TileCoord> from) noexcept {
    for (const nav::TileCoord& coord : from) {
        if (!contains_coord(into.span(), coord)) {
            if (Status pushed = into.push_back(coord); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

/// The index of an unclaimed shape in `wanted` equal to `shape`, or `wanted.size()`.
[[nodiscard]] usize match_shape(Span<const nav::NavObstacleShape> wanted, Span<const bool> claimed,
                                const nav::NavObstacleShape& shape) noexcept {
    for (usize index = 0; index < wanted.size(); ++index) {
        if (!claimed[index] && same_shape(wanted[index], shape)) {
            return index;
        }
    }
    return wanted.size();
}

/// Removes every obstacle the host no longer declares, keeping those it still does.
[[nodiscard]] Status drop_stale_obstacles(NavContext& context,
                                          Span<const nav::NavObstacleShape> wanted,
                                          Array<bool>& claimed,
                                          Array<nav::TileCoord>& affected) noexcept {
    usize kept = 0;
    for (usize index = 0; index < context.obstacle_ids.size(); ++index) {
        const usize match = match_shape(wanted, claimed.span(), context.obstacle_shapes[index]);
        if (match < wanted.size()) {
            claimed[match] = true;
            context.obstacle_ids[kept] = context.obstacle_ids[index];
            context.obstacle_shapes[kept] = context.obstacle_shapes[index];
            ++kept;
            continue;
        }
        auto cleared = nav::clear_obstacle(*context.mesh, context.obstacle_ids[index]);
        if (!cleared) {
            return make_unexpected(cleared.error());
        }
        if (Status merged = merge_coords(affected, cleared->affected.span()); !merged) {
            return merged;
        }
    }
    (void)context.obstacle_ids.resize(kept);
    (void)context.obstacle_shapes.resize(kept);
    return ok();
}

/// Places every obstacle the host declares that the mesh does not hold yet.
[[nodiscard]] Status place_new_obstacles(NavContext& context,
                                         Span<const nav::NavObstacleShape> wanted,
                                         Span<const bool> claimed,
                                         Array<nav::TileCoord>& affected) noexcept {
    for (usize index = 0; index < wanted.size(); ++index) {
        if (claimed[index]) {
            continue;
        }
        auto placed = nav::place_obstacle(*context.mesh, wanted[index]);
        if (!placed) {
            return make_unexpected(placed.error());
        }
        if (Status merged = merge_coords(affected, placed->affected.span()); !merged) {
            return merged;
        }
        if (Status id = context.obstacle_ids.push_back(placed->id); !id) {
            return id;
        }
        if (Status shape = context.obstacle_shapes.push_back(wanted[index]); !shape) {
            return shape;
        }
    }
    return ok();
}

/// Brings the mesh's obstacles in line with the host's: unchanged footprints stay, removed ones
/// are cleared and new ones placed. `affected` receives the tiles under every footprint that
/// changed. No tile is rebuilt.
[[nodiscard]] Status sync_obstacles(Call& call, NavContext& context,
                                    Array<nav::TileCoord>& affected) noexcept {
    Array<nav::NavObstacleShape> wanted(call.allocator);
    if (Status gathered = call.runtime.obstacles(context.world, wanted); !gathered) {
        return gathered;
    }
    Array<bool> claimed(call.allocator);
    if (Status sized = claimed.resize(wanted.size()); !sized) {
        return sized;
    }
    for (bool& flag : claimed) {
        flag = false;
    }
    if (Status dropped = drop_stale_obstacles(context, wanted.span(), claimed, affected);
        !dropped) {
        return dropped;
    }
    return place_new_obstacles(context, wanted.span(), claimed.span(), affected);
}

/// Replaces the mesh's links with the host's. A link that does not snap onto the mesh is counted
/// in `link_failures` rather than failing the request.
[[nodiscard]] Status sync_links(Call& call, NavContext& context) noexcept {
    Array<nav::NavLink> wanted(call.allocator);
    if (Status gathered = call.runtime.links(context.world, wanted); !gathered) {
        return gathered;
    }
    for (const nav::LinkId id : context.link_ids.span()) {
        (void)context.mesh->remove_link(id);
    }
    context.link_ids.clear();
    context.link_failures = 0;
    for (const nav::NavLink& link : wanted.span()) {
        auto added = context.mesh->add_link(link, kLinkSnap);
        if (!added) {
            ++context.link_failures;
            continue;
        }
        if (Status pushed = context.link_ids.push_back(*added); !pushed) {
            return pushed;
        }
    }
    return ok();
}

// --- Event payloads ---------------------------------------------------------------------------

[[nodiscard]] Status put_coord(Array<u8>& out, nav::TileCoord coord) noexcept {
    if (Status x = put_i32(out, coord.x); !x) {
        return x;
    }
    if (Status z = put_i32(out, coord.z); !z) {
        return z;
    }
    return put_i32(out, coord.layer);
}

[[nodiscard]] Status put_coords(Array<u8>& out, Span<const nav::TileCoord> coords) noexcept {
    if (Status count = put_u32(out, static_cast<u32>(coords.size())); !count) {
        return count;
    }
    for (const nav::TileCoord& coord : coords) {
        if (Status put = put_coord(out, coord); !put) {
            return put;
        }
    }
    return ok();
}

/// i32 x, i32 z, i32 layer, u32 polys, u8 empty, u64 digest.
[[nodiscard]] Status put_tile(Array<u8>& out, const nav::NavBakeTile& tile) noexcept {
    if (Status coord = put_coord(out, tile.coord); !coord) {
        return coord;
    }
    if (Status polys = put_u32(out, tile.polys); !polys) {
        return polys;
    }
    if (Status empty = put_u8(out, tile.empty ? u8{1} : u8{0}); !empty) {
        return empty;
    }
    return put_u64(out, tile.digest);
}

/// u32 triangles in, filtered, steep, spans, polys, vertices; u64 duration; u8 backend; u32 tiles
/// built, u32 tiles empty.
[[nodiscard]] Status put_report(Array<u8>& out, const nav::NavBakeReport& report) noexcept {
    const nav::NavBuildReport& totals = report.totals;
    const u32 counters[] = {totals.triangles_in,    totals.triangles_filtered,
                            totals.triangles_steep, totals.spans,
                            totals.polys,           totals.vertices};
    for (const u32 counter : counters) {
        if (Status put = put_u32(out, counter); !put) {
            return put;
        }
    }
    if (Status duration = put_u64(out, totals.duration_ns); !duration) {
        return duration;
    }
    if (Status backend = put_u8(out, static_cast<u8>(totals.backend)); !backend) {
        return backend;
    }
    if (Status built = put_u32(out, report.tiles_built); !built) {
        return built;
    }
    return put_u32(out, report.tiles_empty);
}

// --- navigation.bake --------------------------------------------------------------------------

[[nodiscard]] CyResult bake_start(Call& call) noexcept {
    NavigationSession& session = call.session;
    BakeJob& job = session.job;
    Reader reader = reader_of(session);
    const u32 world = reader.read_u32();
    const nav::NavBakeSettings settings = read_settings(reader);
    if (!reader.complete()) {
        return failed(session, "navigation.request.malformed",
                      "navigation.bake takes a world and a settings block");
    }
    if (Status valid = nav::validate_bake_settings(settings); !valid) {
        return failed(session, "navigation.settings.invalid", valid.error().message);
    }
    if (!world_known(call.runtime, call.allocator, world)) {
        return failed(session, "navigation.world.unknown", "the host declares no such world");
    }
    reset_job(job);
    if (Status gathered = call.runtime.gather(world, job.sources); !gathered) {
        return failed(session, "navigation.source.failed", gathered.error().message);
    }
    const nav::NavBakeSource source = job.sources.source();
    if (Status tiled = nav::tiles_overlapping(settings.tile_size,
                                              nav::surface_region(source.surfaces), job.coords);
        !tiled) {
        return failed(session, "navigation.bake.failed", tiled.error().message);
    }
    if (job.coords.empty()) {
        return failed(session, "navigation.surface.missing",
                      "the world has no including NavMeshSurface to bake");
    }
    job.world = world;
    job.settings = settings;
    job.fingerprint = nav::source_fingerprint(settings, source, kNavigationBakeVersion);
    if (!job.staging.create(call.allocator, settings.tile_size)) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    job.staging->set_profile(nav::agent_profile(settings, Name::intern("editor.agent")));
    job.report.totals.backend = settings.backend;
    job.active = true;
    return CY_RESULT_OK;
}

/// The interior of tile `coord`'s XZ square: overlaps that tile and no neighbour.
[[nodiscard]] Aabb tile_interior(f32 tile_size, nav::TileCoord coord) noexcept {
    const f32 x = static_cast<f32>(coord.x) * tile_size;
    const f32 z = static_cast<f32>(coord.z) * tile_size;
    const f32 inset = tile_size * 0.25F;
    return Aabb::from_min_max(Vec3{x + inset, 0.0F, z + inset},
                              Vec3{x + tile_size - inset, 0.0F, z + tile_size - inset});
}

void accumulate(nav::NavBakeReport& into, const nav::NavBakeReport& tile) noexcept {
    nav::NavBuildReport& totals = into.totals;
    totals.triangles_in = tile.totals.triangles_in;
    totals.triangles_filtered = tile.totals.triangles_filtered;
    totals.triangles_steep = tile.totals.triangles_steep;
    totals.spans += tile.totals.spans;
    totals.polys += tile.totals.polys;
    totals.vertices += tile.totals.vertices;
    totals.duration_ns += tile.totals.duration_ns;
    totals.backend = tile.totals.backend;
    into.tiles_built += tile.tiles_built;
    into.tiles_empty += tile.tiles_empty;
}

/// Bakes the next tile and describes it as a PROGRESS event: u32 done, u32 total, then the tile.
[[nodiscard]] CyResult bake_next_tile(Call& call) noexcept {
    NavigationSession& session = call.session;
    BakeJob& job = session.job;
    const nav::TileCoord coord = job.coords[job.next];
    auto baked =
        nav::rebake_tiles(call.allocator, job.settings, job.sources.source(),
                          tile_interior(job.settings.tile_size, coord), *job.staging, nullptr);
    if (!baked || baked->tiles.size() != 1) {
        const char* detail =
            baked ? "a tile bake visited more than one tile" : baked.error().message;
        reset_job(job);
        return failed(session, "navigation.bake.failed", detail);
    }
    accumulate(job.report, *baked);
    if (Status listed = job.report.tiles.push_back(baked->tiles[0]); !listed) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    ++job.next;
    session.progress_event = true;
    session.event_payload.clear();
    if (!put_u32(session.event_payload, job.next) ||
        !put_u32(session.event_payload, static_cast<u32>(job.coords.size()))) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    return written(put_tile(session.event_payload, baked->tiles[0]));
}

/// Moves the staging mesh into the world's context and applies the host's obstacles and links.
[[nodiscard]] Status commit_bake(Call& call, NavContext& context, u64 identity) noexcept {
    BakeJob& job = call.session.job;
    context.mesh.take(job.staging);
    context.settings = job.settings;
    context.costs = nav::area_costs(job.sources.areas.span());
    context.identity = identity;
    context.sidecar = identity;
    context.fingerprint = job.fingerprint;
    context.obstacle_ids.clear();
    context.obstacle_shapes.clear();
    context.link_ids.clear();
    std::swap(context.report.tiles, job.report.tiles);
    context.report.totals = job.report.totals;
    context.report.tiles_built = job.report.tiles_built;
    context.report.tiles_empty = job.report.tiles_empty;
    context.report.cancelled = false;
    Array<nav::TileCoord> affected(call.allocator);
    if (Status placed = sync_obstacles(call, context, affected); !placed) {
        return placed;
    }
    return sync_links(call, context);
}

/// u32 world, u64 fingerprint, u64 identity, text sidecar, the report, u32 link failures, u32 tile
/// count, then each tile.
[[nodiscard]] Status encode_completed(Array<u8>& out, const NavContext& context,
                                      std::string_view sidecar) noexcept {
    out.clear();
    if (!put_u32(out, context.world) || !put_u64(out, context.fingerprint) ||
        !put_u64(out, context.identity) || !put_text(out, sidecar) ||
        !put_report(out, context.report) || !put_u32(out, context.link_failures) ||
        !put_u32(out, static_cast<u32>(context.report.tiles.size()))) {
        return fail(ErrorCode::OutOfMemory, "navigation completion payload");
    }
    for (const nav::NavBakeTile& tile : context.report.tiles.span()) {
        if (Status put = put_tile(out, tile); !put) {
            return put;
        }
    }
    return ok();
}

/// Encodes the staging mesh as a `.cynavmesh` and hands it to the host under `identity`.
[[nodiscard]] Status save_bake(Call& call, u64 identity, Array<char>& sidecar) noexcept {
    const BakeJob& job = call.session.job;
    Array<u8> bytes(call.allocator);
    if (Status encoded = nav::encode_nav_bake(job.settings, job.fingerprint, *job.staging, bytes);
        !encoded) {
        return encoded;
    }
    return call.runtime.store_bake(identity, bytes.span(), sidecar);
}

[[nodiscard]] CyResult bake_finish(Call& call) noexcept {
    NavigationSession& session = call.session;
    BakeJob& job = session.job;
    auto identity = nav::mesh_bake_identity(job.fingerprint, *job.staging);
    Array<char> sidecar(call.allocator);
    const Status saved =
        identity ? save_bake(call, *identity, sidecar) : Status(make_unexpected(identity.error()));
    if (!saved) {
        reset_job(job);
        return failed(session, "navigation.bake.store-failed", saved.error().message);
    }
    NavContext* context = context_for(session, call.allocator, job.world);
    if (context == nullptr) {
        reset_job(job);
        return failed(session, "navigation.world.limit",
                      "this session already holds sixteen navigation worlds");
    }
    const Status committed = commit_bake(call, *context, *identity);
    reset_job(job);
    if (!committed) {
        return failed(session, "navigation.source.failed", committed.error().message);
    }
    return written(encode_completed(session.event_payload, *context,
                                    std::string_view(sidecar.data(), sidecar.size())));
}

CyResult bake(Call& call) noexcept {
    BakeJob& job = call.session.job;
    if (!job.active) {
        const CyResult started = bake_start(call);
        if (started != CY_RESULT_OK || call.session.failed_event) {
            return started;
        }
    }
    if (job.next < job.coords.size()) {
        return bake_next_tile(call);
    }
    return bake_finish(call);
}

// --- navigation.status ------------------------------------------------------------------------

/// Installs the sidecar saved under `identity` as the world's mesh: how an undone bake comes back.
[[nodiscard]] Status restore_bake(Call& call, u32 world, u64 identity) noexcept {
    Array<u8> bytes(call.allocator);
    if (Status loaded = call.runtime.load_bake(identity, bytes); !loaded) {
        return loaded;
    }
    auto asset = nav::decode_nav_bake(call.allocator, bytes.span());
    if (!asset) {
        return make_unexpected(asset.error());
    }
    NavContext* context = context_for(call.session, call.allocator, world);
    if (context == nullptr) {
        return fail(ErrorCode::OutOfRange, "this session already holds sixteen navigation worlds");
    }
    const nav::NavBakeSettings settings = asset->settings;
    const u64 fingerprint = asset->source_fingerprint;
    OwnedMesh mesh;
    if (!mesh.create(call.allocator, settings.tile_size)) {
        return fail(ErrorCode::OutOfMemory, "no memory for the restored navigation mesh");
    }
    mesh->set_profile(nav::agent_profile(settings, Name::intern("editor.agent")));
    if (Status installed = nav::install_nav_bake(std::move(*asset), *mesh); !installed) {
        return installed;
    }
    context->mesh.take(mesh);
    context->settings = settings;
    context->identity = identity;
    context->sidecar = identity;
    context->fingerprint = fingerprint;
    context->obstacle_ids.clear();
    context->obstacle_shapes.clear();
    context->link_ids.clear();
    clear_report(context->report);
    Array<nav::TileCoord> affected(call.allocator);
    if (Status placed = sync_obstacles(call, *context, affected); !placed) {
        return placed;
    }
    return sync_links(call, *context);
}

/// u32 world, u8 baked, u8 stale, u64 current fingerprint, u64 saved fingerprint, u64 identity,
/// u32 resident tiles, the last report, u32 link failures.
[[nodiscard]] CyResult encode_status(NavigationSession& session, u32 world,
                                     const NavContext* context, u64 current, u64 saved) noexcept {
    const bool baked = context != nullptr && context->mesh.get() != nullptr;
    Array<u8>& out = session.event_payload;
    out.clear();
    const nav::NavBakeReport empty(out.allocator());
    const nav::NavBakeReport& report = baked ? context->report : empty;
    const bool encoded =
        put_u32(out, world) && put_u8(out, baked ? u8{1} : u8{0}) &&
        put_u8(out, (baked && current != saved) ? u8{1} : u8{0}) && put_u64(out, current) &&
        put_u64(out, saved) && put_u64(out, baked ? context->identity : 0) &&
        put_u32(out, baked ? context->mesh->tile_count() : 0) && put_report(out, report) &&
        put_u32(out, baked ? context->link_failures : 0);
    return encoded ? CY_RESULT_OK : CY_RESULT_OUT_OF_MEMORY;
}

CyResult status(Call& call) noexcept {
    NavigationSession& session = call.session;
    Reader reader = reader_of(session);
    const u32 world = reader.read_u32();
    const nav::NavBakeSettings settings = read_settings(reader);
    const u64 saved_identity = reader.read_u64();
    const u64 saved_fingerprint = reader.read_u64();
    if (!reader.complete()) {
        return failed(session, "navigation.request.malformed",
                      "navigation.status takes a world, a settings block and the saved bake");
    }
    if (Status valid = nav::validate_bake_settings(settings); !valid) {
        return failed(session, "navigation.settings.invalid", valid.error().message);
    }
    if (!world_known(call.runtime, call.allocator, world)) {
        return failed(session, "navigation.world.unknown", "the host declares no such world");
    }
    const NavContext* held = find_context(session, world);
    const bool restore =
        saved_identity != 0 && (held == nullptr || !held->mesh || held->sidecar != saved_identity);
    if (restore) {
        if (Status restored = restore_bake(call, world, saved_identity); !restored) {
            return failed(session, "navigation.bake.load-failed", restored.error().message);
        }
    }
    NavSourceBuffers sources(call.allocator);
    if (Status gathered = call.runtime.gather(world, sources); !gathered) {
        return failed(session, "navigation.source.failed", gathered.error().message);
    }
    const u64 current = nav::source_fingerprint(settings, sources.source(), kNavigationBakeVersion);
    const NavContext* context = find_context(session, world);
    u64 saved = saved_fingerprint;
    if (saved == 0 && context != nullptr) {
        saved = context->fingerprint;
    }
    return encode_status(session, world, context, current, saved);
}

// --- navigation.update ------------------------------------------------------------------------

/// Rebuilds the tiles under `dirty` when the sources changed since the mesh was built. Obstacles
/// and links do not enter the fingerprint, so an obstacle edit rebuilds nothing.
[[nodiscard]] Status rebuild_changed(Call& call, NavContext& context, const Aabb& dirty,
                                     Array<nav::TileCoord>& rebuilt) noexcept {
    NavSourceBuffers sources(call.allocator);
    if (Status gathered = call.runtime.gather(context.world, sources); !gathered) {
        return gathered;
    }
    const nav::NavBakeSource source = sources.source();
    const u64 fingerprint =
        nav::source_fingerprint(context.settings, source, kNavigationBakeVersion);
    if (fingerprint == context.fingerprint) {
        return ok();
    }
    auto report =
        nav::rebake_tiles(call.allocator, context.settings, source, dirty, *context.mesh, nullptr);
    if (!report) {
        return make_unexpected(report.error());
    }
    for (const nav::NavBakeTile& tile : report->tiles.span()) {
        if (Status pushed = rebuilt.push_back(tile.coord); !pushed) {
            return pushed;
        }
    }
    auto identity = nav::mesh_bake_identity(fingerprint, *context.mesh);
    if (!identity) {
        return make_unexpected(identity.error());
    }
    context.fingerprint = fingerprint;
    context.identity = *identity;
    context.costs = nav::area_costs(source.areas);
    return ok();
}

CyResult update(Call& call) noexcept {
    NavigationSession& session = call.session;
    Reader reader = reader_of(session);
    const u32 world = reader.read_u32();
    const Aabb dirty = reader.read_aabb();
    if (!reader.complete()) {
        return failed(session, "navigation.request.malformed",
                      "navigation.update takes a world and a dirty box");
    }
    CyResult result = CY_RESULT_OK;
    NavContext* context = baked_context(call, world, result);
    if (context == nullptr) {
        return result;
    }
    Array<nav::TileCoord> rebuilt(call.allocator);
    Array<nav::TileCoord> marked(call.allocator);
    Status updated = rebuild_changed(call, *context, dirty, rebuilt);
    if (updated) {
        updated = sync_obstacles(call, *context, marked);
    }
    if (updated) {
        updated = sync_links(call, *context);
    }
    if (!updated) {
        return failed(session, "navigation.update.failed", updated.error().message);
    }
    Array<u8>& out = session.event_payload;
    out.clear();
    const bool encoded = put_u32(out, world) && put_u64(out, context->fingerprint) &&
                         put_u64(out, context->identity) && put_coords(out, rebuilt.span()) &&
                         put_coords(out, marked.span()) && put_u32(out, context->link_failures);
    return encoded ? CY_RESULT_OK : CY_RESULT_OUT_OF_MEMORY;
}

// --- navigation.path.query --------------------------------------------------------------------

CyResult path_query(Call& call) noexcept {
    NavigationSession& session = call.session;
    Reader reader = reader_of(session);
    const u32 world = reader.read_u32();
    const Vec3 start = reader.read_vec3();
    const Vec3 end = reader.read_vec3();
    const Vec3 extents = reader.read_vec3();
    if (!reader.complete()) {
        return failed(session, "navigation.request.malformed",
                      "navigation.path.query takes a world, two points and search extents");
    }
    CyResult result = CY_RESULT_OK;
    const NavContext* context = baked_context(call, world, result);
    if (context == nullptr) {
        return result;
    }
    nav::PathFilter filter;
    filter.costs = context->costs;
    filter.node_budget = kPathBudget;
    nav::PathCorridor corridor(call.allocator);
    const nav::PathResult path =
        nav::find_path(*context->mesh, start, end, extents, filter, corridor);
    Array<nav::PathPoint> points(call.allocator);
    if (path.found) {
        if (Status straight = nav::straighten(*context->mesh, corridor, start, end, points);
            !straight) {
            return failed(session, "navigation.path.failed", straight.error().message);
        }
    }
    Array<u8>& out = session.event_payload;
    out.clear();
    bool encoded =
        put_u8(out, path.found ? u8{1} : u8{0}) && put_u8(out, path.partial ? u8{1} : u8{0}) &&
        put_u8(out, path.budget_exceeded ? u8{1} : u8{0}) && put_f32(out, path.cost) &&
        put_u32(out, path.nodes_expanded) && put_u32(out, static_cast<u32>(points.size()));
    for (usize index = 0; encoded && index < points.size(); ++index) {
        encoded = put_vec3(out, points[index].position) &&
                  put_u8(out, points[index].enters_link ? u8{1} : u8{0});
    }
    return encoded ? CY_RESULT_OK : CY_RESULT_OUT_OF_MEMORY;
}

// --- navigation.flowfield.query ---------------------------------------------------------------

[[nodiscard]] bool flow_extent_valid(const Aabb& region, f32 cell) noexcept {
    if (!std::isfinite(cell) || cell <= 0.0F || region.is_empty()) {
        return false;
    }
    const f32 width = std::ceil((region.max.x - region.min.x) / cell);
    const f32 depth = std::ceil((region.max.z - region.min.z) / cell);
    return std::isfinite(width) && std::isfinite(depth) &&
           width * depth <= static_cast<f32>(kMaxFlowCells);
}

/// u32 width, u32 depth, f32 cell size, u32 unreachable, then per cell f32 dx, f32 dz, u8
/// reachable, row by row.
[[nodiscard]] CyResult encode_flow(Array<u8>& out, const nav::FlowField& field,
                                   const nav::FlowFieldUpdate& built) noexcept {
    out.clear();
    bool encoded = put_u32(out, field.width()) && put_u32(out, field.depth()) &&
                   put_f32(out, field.params().cell_size) && put_u32(out, built.unreachable);
    for (u32 index = 0; encoded && index < field.cell_count(); ++index) {
        const Vec3 centre = field.cell_centre(index);
        const Vec3 direction = field.direction_at(centre);
        encoded = put_f32(out, direction.x) && put_f32(out, direction.z) &&
                  put_u8(out, field.reachable_at(centre) ? u8{1} : u8{0});
    }
    return encoded ? CY_RESULT_OK : CY_RESULT_OUT_OF_MEMORY;
}

CyResult flowfield_query(Call& call) noexcept {
    NavigationSession& session = call.session;
    Reader reader = reader_of(session);
    const u32 world = reader.read_u32();
    const Vec3 target = reader.read_vec3();
    const Aabb region = reader.read_aabb();
    const f32 cell = reader.read_f32();
    if (!reader.complete()) {
        return failed(session, "navigation.request.malformed",
                      "navigation.flowfield.query takes a world, a target, a region and a cell");
    }
    if (!flow_extent_valid(region, cell)) {
        return failed(session, "navigation.flowfield.too-large",
                      "a flow field covers a non-empty region of at most 65536 cells");
    }
    CyResult result = CY_RESULT_OK;
    const NavContext* context = baked_context(call, world, result);
    if (context == nullptr) {
        return result;
    }
    nav::FlowFieldParams params;
    params.region = region;
    params.cell_size = cell;
    params.costs = context->costs;
    nav::FlowField field(call.allocator, params);
    auto built = field.build(*context->mesh, Span<const Vec3>(&target, 1));
    if (!built) {
        return failed(session, "navigation.flowfield.failed", built.error().message);
    }
    return encode_flow(session.event_payload, field, *built);
}

// --- navigation.point.pick --------------------------------------------------------------------

struct PickHit {
    bool hit = false;
    f32 distance = 0.0F;
    nav::PolyRef poly;
};

void pick_polygon(const nav::NavMesh& mesh, nav::PolyRef ref, const Ray& ray,
                  PickHit& best) noexcept {
    Vec3 corners[nav::kMaxPolyVertices] = {};
    const u32 count = mesh.poly_vertices(ref, corners, nav::kMaxPolyVertices);
    for (u32 corner = 2; corner < count; ++corner) {
        geom::TriangleHit hit;
        if (geom::ray_triangle(ray, corners[0], corners[corner - 1], corners[corner], kPickDistance,
                               false, hit) &&
            (!best.hit || hit.t < best.distance)) {
            best = PickHit{true, hit.t, ref};
        }
    }
}

[[nodiscard]] PickHit pick_mesh(const nav::NavMesh& mesh, const Ray& ray) noexcept {
    PickHit best;
    for (u32 slot = 0; slot < mesh.tile_capacity(); ++slot) {
        if (mesh.tile_slot(mesh.tile_coord(slot)) != slot) {
            continue;
        }
        for (u32 index = 0; index < mesh.tile_poly_count(slot); ++index) {
            pick_polygon(mesh, mesh.tile_poly(slot, index), ray, best);
        }
    }
    return best;
}

CyResult point_pick(Call& call) noexcept {
    NavigationSession& session = call.session;
    Reader reader = reader_of(session);
    const u32 world = reader.read_u32();
    const u32 viewport = reader.read_u32();
    const u64 frame = reader.read_u64();
    const f32 x = reader.read_f32();
    const f32 y = reader.read_f32();
    if (!reader.complete()) {
        return failed(session, "navigation.request.malformed",
                      "navigation.point.pick takes a world, a viewport, a frame and a pixel");
    }
    CyResult result = CY_RESULT_OK;
    const NavContext* context = baked_context(call, world, result);
    if (context == nullptr) {
        return result;
    }
    auto ray = call.runtime.pick_ray(viewport, frame, x, y);
    if (!ray) {
        return failed(session, "navigation.point.pick.ray-failed", ray.error().message);
    }
    const PickHit best = pick_mesh(*context->mesh, *ray);
    Array<u8>& out = session.event_payload;
    out.clear();
    const Vec3 point = best.hit ? ray->at(best.distance) : Vec3{0.0F, 0.0F, 0.0F};
    const bool encoded = put_u8(out, best.hit ? u8{1} : u8{0}) && put_vec3(out, point) &&
                         put_u64(out, best.hit ? best.poly.bits() : 0) &&
                         put_f32(out, best.distance);
    return encoded ? CY_RESULT_OK : CY_RESULT_OUT_OF_MEMORY;
}

// --- navigation.overlay.set -------------------------------------------------------------------

CyResult overlay_set(Call& call) noexcept {
    NavigationSession& session = call.session;
    Reader reader = reader_of(session);
    const u32 world = reader.read_u32();
    const u32 flags = reader.read_u32();
    if (!reader.complete()) {
        return failed(session, "navigation.request.malformed",
                      "navigation.overlay.set takes a world and its flags");
    }
    if ((flags & ~kAllOverlayFlags) != 0) {
        return failed(session, "navigation.overlay.invalid",
                      "the flags name an overlay navigation does not draw");
    }
    if (!world_known(call.runtime, call.allocator, world)) {
        return failed(session, "navigation.world.unknown", "the host declares no such world");
    }
    NavContext* context = context_for(session, call.allocator, world);
    if (context == nullptr) {
        return failed(session, "navigation.world.limit",
                      "this session already holds sixteen navigation worlds");
    }
    context->overlay = flags;
    session.event_payload.clear();
    const bool encoded =
        put_u32(session.event_payload, world) && put_u32(session.event_payload, flags);
    return encoded ? CY_RESULT_OK : CY_RESULT_OUT_OF_MEMORY;
}

// --- Dispatch ---------------------------------------------------------------------------------

struct Operation {
    const char* name;
    CyResult (*run)(Call& call) noexcept;
};

constexpr Operation kOperations[] = {
    {"navigation.bake", bake},
    {"navigation.status", status},
    {"navigation.update", update},
    {"navigation.path.query", path_query},
    {"navigation.flowfield.query", flowfield_query},
    {"navigation.point.pick", point_pick},
    {"navigation.overlay.set", overlay_set},
};

[[nodiscard]] const Operation* find_operation(std::string_view name) noexcept {
    for (const Operation& operation : kOperations) {
        if (name == operation.name) {
            return &operation;
        }
    }
    return nullptr;
}

/// u32 1, u32 count, the operation names, u64 feature bits.
CyResult capabilities(NavigationSession& session, bool seam) noexcept {
    Array<u8>& out = session.event_payload;
    out.clear();
    const u32 count = seam ? static_cast<u32>(std::size(kOperations)) + 1U : 1U;
    bool encoded = put_u32(out, 1) && put_u32(out, count) && put_text(out, "capabilities.get");
    for (usize index = 0; seam && encoded && index < std::size(kOperations); ++index) {
        encoded = put_text(out, kOperations[index].name).has_value();
    }
    encoded = encoded && put_u64(out, seam ? kNavigationFeature : 0U);
    return encoded ? CY_RESULT_OK : CY_RESULT_OUT_OF_MEMORY;
}

CyResult unavailable(NavigationSession& session, std::string_view operation) noexcept {
    char code[96] = {};
    constexpr std::string_view suffix = ".unavailable";
    std::memcpy(code, operation.data(), operation.size());
    std::memcpy(code + operation.size(), suffix.data(), suffix.size());
    return failed(session, std::string_view(code, operation.size() + suffix.size()),
                  "the runtime host supplied no navigation source");
}

CyResult dispatch(NavigationSession& session, NavigationSourceRuntime* runtime,
                  Allocator& allocator) noexcept {
    const std::string_view operation(session.operation);
    if (session.schema != 1) {
        return failed(session, "navigation.schema.unsupported", "this operation supports schema 1");
    }
    if (operation == "capabilities.get") {
        return capabilities(session, runtime != nullptr);
    }
    const Operation* found = find_operation(operation);
    if (found == nullptr) {
        return failed(session, "navigation.operation.unsupported",
                      "this backend does not support the operation");
    }
    if (runtime == nullptr) {
        return unavailable(session, operation);
    }
    Call call{session, *runtime, allocator};
    return found->run(call);
}

void begin_event(CyServiceEvent& event, u64 request, u32 kind) noexcept {
    event = {};
    event.struct_size = sizeof(CyServiceEvent);
    event.request_id = request;
    event.schema_version = 1;
    event.kind = kind;
}

}  // namespace

const navigation::NavMesh* NavigationService::mesh(CyServiceSession session, u32 world) noexcept {
    if (session == nullptr) {
        return nullptr;
    }
    const NavContext* context = find_context(*state_of(session), world);
    return context != nullptr ? context->mesh.get() : nullptr;
}

u32 NavigationService::overlay(CyServiceSession session, u32 world) noexcept {
    if (session == nullptr) {
        return 0;
    }
    const NavContext* context = find_context(*state_of(session), world);
    return context != nullptr ? context->overlay : 0;
}

CyResult NavigationService::open(CyServiceSession* out_session) noexcept {
    void* memory = allocator_->allocate(sizeof(NavigationSession), alignof(NavigationSession));
    if (memory == nullptr) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    *out_session = reinterpret_cast<CyServiceSession>(new (memory) NavigationSession(*allocator_));
    return CY_RESULT_OK;
}

void NavigationService::close(CyServiceSession session) noexcept {
    if (session == nullptr) {
        return;
    }
    NavigationSession* state = state_of(session);
    reset_job(state->job);
    destroy_contexts(*state, *allocator_);
    state->~NavigationSession();
    allocator_->deallocate(state, sizeof(NavigationSession), alignof(NavigationSession));
}

CyResult NavigationService::submit(CyServiceSession session,
                                   const CyServiceRequest& request) noexcept {
    NavigationSession* state = state_of(session);
    if (request.request_id == 0 || request.operation == nullptr) {
        return CY_RESULT_INVALID_ARGUMENT;
    }
    if (state->pending) {
        // Answered with `navigation.busy` on the next poll, so the refused request still ends in
        // exactly one terminal event. Only one refusal is held at a time.
        if (state->busy_request != 0 || request.request_id == state->request) {
            return CY_RESULT_ALREADY_EXISTS;
        }
        state->busy_request = request.request_id;
        return CY_RESULT_OK;
    }
    const usize operation_size = std::strlen(request.operation);
    if (operation_size >= sizeof(state->operation)) {
        return CY_RESULT_INVALID_ARGUMENT;
    }
    std::memcpy(state->operation, request.operation, operation_size + 1);
    state->request_payload.clear();
    if (Status copied = state->request_payload.append({request.payload, request.payload_size});
        !copied) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    state->request = request.request_id;
    state->schema = request.schema_version;
    state->pending = true;
    state->cancelled = false;
    return CY_RESULT_OK;
}

CyResult NavigationService::cancel(CyServiceSession session, u64 request_id) noexcept {
    NavigationSession* state = state_of(session);
    if (!state->pending || state->request != request_id) {
        return CY_RESULT_NOT_FOUND;
    }
    state->cancelled = true;
    return CY_RESULT_OK;
}

CyResult NavigationService::poll(CyServiceSession session, CyServiceEvent& out_event,
                                 bool& out_has_event) noexcept {
    NavigationSession* state = state_of(session);
    out_has_event = false;
    if (state->busy_request != 0) {
        begin_event(out_event, state->busy_request, CY_SERVICE_EVENT_FAILED);
        state->busy_request = 0;
        if (!wire::encode_failure(state->event_payload, "navigation.busy",
                                  "another navigation request is pending in this session")) {
            return CY_RESULT_OUT_OF_MEMORY;
        }
        out_event.payload = state->event_payload.data();
        out_event.payload_size = state->event_payload.size();
        out_has_event = true;
        return CY_RESULT_OK;
    }
    if (!state->pending) {
        return CY_RESULT_OK;
    }
    begin_event(out_event, state->request, CY_SERVICE_EVENT_COMPLETED);
    state->failed_event = false;
    state->progress_event = false;
    state->event_payload.clear();
    if (state->cancelled) {
        reset_job(state->job);
        out_event.kind = CY_SERVICE_EVENT_CANCELLED;
    } else if (const CyResult result = dispatch(*state, runtime_, *allocator_);
               result != CY_RESULT_OK) {
        return result;
    }
    if (state->failed_event) {
        out_event.kind = CY_SERVICE_EVENT_FAILED;
    } else if (state->progress_event) {
        out_event.kind = CY_SERVICE_EVENT_PROGRESS;
    }
    out_event.payload = state->event_payload.data();
    out_event.payload_size = state->event_payload.size();
    out_has_event = true;
    state->pending = out_event.kind == CY_SERVICE_EVENT_PROGRESS;
    return CY_RESULT_OK;
}

}  // namespace cy::editor
