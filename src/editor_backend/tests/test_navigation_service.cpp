// SPDX-License-Identifier: MIT
// The engine's navigation authoring service over a fixture seam. Issue #28, tasks 2.1 and 2.3.
//
// INTEGRATION: every bake voxelises. The first case is issue #28's first acceptance criterion at
// the service boundary: what the editor receives from `navigation.bake` equals `build_tile` run
// directly on the same inputs, tile by tile.

#include <cy/editor/navigation_service.h>
#include <cy/navigation/debug.h>
#include <cy/navigation/query.h>
#include <cy/navigation/tile_identity.h>
#include <cy/test/test.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "navigation_seam.h"

using namespace cy;
using namespace cy::editor;
using namespace cy::editor::testing;
namespace nav = cy::navigation;

namespace {

struct BakeTile {
    nav::TileCoord coord;
    u32 polys = 0;
    bool empty = false;
    u64 digest = 0;
};

BakeTile read_tile(Decoder& decoder) {
    BakeTile tile;
    tile.coord.x = decoder.i32_();
    tile.coord.z = decoder.i32_();
    tile.coord.layer = decoder.i32_();
    tile.polys = decoder.u32_();
    tile.empty = decoder.u8_() != 0;
    tile.digest = decoder.u64_();
    return tile;
}

struct Completed {
    u32 world = 0;
    u64 fingerprint = 0;
    u64 identity = 0;
    std::string sidecar;
    u32 polys = 0;
    u8 backend = 0;
    u32 tiles_built = 0;
    u32 tiles_empty = 0;
    std::vector<BakeTile> tiles;
    bool decoded = false;
};

Completed read_completed(const Event& event) {
    Decoder decoder(event.payload);
    Completed out;
    out.world = decoder.u32_();
    out.fingerprint = decoder.u64_();
    out.identity = decoder.u64_();
    out.sidecar = decoder.text();
    for (u32 counter = 0; counter < 6; ++counter) {
        const u32 value = decoder.u32_();
        out.polys = (counter == 4) ? value : out.polys;
    }
    (void)decoder.u64_();
    out.backend = decoder.u8_();
    out.tiles_built = decoder.u32_();
    out.tiles_empty = decoder.u32_();
    (void)decoder.u32_();
    const u32 count = decoder.u32_();
    for (u32 index = 0; index < count && !decoder.overrun; ++index) {
        out.tiles.push_back(read_tile(decoder));
    }
    out.decoded = decoder.done();
    return out;
}

struct StatusReply {
    bool baked = false;
    bool stale = false;
    u64 current = 0;
    u64 saved = 0;
    u64 identity = 0;
    u32 tiles = 0;
    bool sidecar_missing = false;
    bool decoded = false;
};

StatusReply read_status(const Event& event) {
    Decoder decoder(event.payload);
    StatusReply out;
    (void)decoder.u32_();
    out.baked = decoder.u8_() != 0;
    out.stale = decoder.u8_() != 0;
    out.current = decoder.u64_();
    out.saved = decoder.u64_();
    out.identity = decoder.u64_();
    out.tiles = decoder.u32_();
    for (u32 counter = 0; counter < 6; ++counter) {
        (void)decoder.u32_();
    }
    (void)decoder.u64_();
    (void)decoder.u8_();
    (void)decoder.u32_();
    (void)decoder.u32_();
    (void)decoder.u32_();  // link failures
    out.sidecar_missing = decoder.u8_() != 0;
    out.decoded = decoder.done();
    return out;
}

struct PathReply {
    bool found = false;
    bool partial = false;
    f32 cost = 0.0F;
    std::vector<Vec3> points;
};

PathReply read_path(const Event& event) {
    Decoder decoder(event.payload);
    PathReply out;
    out.found = decoder.u8_() != 0;
    out.partial = decoder.u8_() != 0;
    (void)decoder.u8_();
    out.cost = decoder.f32_();
    (void)decoder.u32_();
    const u32 count = decoder.u32_();
    for (u32 index = 0; index < count && !decoder.overrun; ++index) {
        out.points.push_back(decoder.vec3());
        (void)decoder.u8_();
    }
    CY_CHECK(decoder.done());
    return out;
}

/// A session with a fixture seam, closed on scope exit.
struct Fixture {
    FixtureSeam seam;
    NavigationService service{allocator(), &seam};
    CyServiceSession session = nullptr;

    Fixture() { CY_REQUIRE_EQ(service.open(&session), CY_RESULT_OK); }
    ~Fixture() { service.close(session); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture&&) = delete;

    Completed bake(u64 id) {
        const Event done = call(service, session, id, "navigation.bake", bake_request(settings));
        CY_REQUIRE(done.is(CY_SERVICE_EVENT_COMPLETED));
        return read_completed(done);
    }

    StatusReply status(u64 id, const Completed& saved) {
        Payload payload;
        payload.u32_(kWorld).settings(settings).u64_(saved.identity).u64_(saved.fingerprint);
        const Event done = call(service, session, id, "navigation.status", payload);
        CY_REQUIRE(done.is(CY_SERVICE_EVENT_COMPLETED));
        return read_status(done);
    }

    PathReply path(u64 id, Vec3 start, Vec3 end) {
        Payload payload;
        payload.u32_(kWorld).vec3(start).vec3(end).vec3(Vec3{0.5F, 1.0F, 0.5F});
        const Event done = call(service, session, id, "navigation.path.query", payload);
        CY_REQUIRE(done.is(CY_SERVICE_EVENT_COMPLETED));
        return read_path(done);
    }

    Event update(u64 id, const Aabb& dirty) {
        Payload payload;
        payload.u32_(kWorld).aabb(dirty);
        return call(service, session, id, "navigation.update", payload);
    }

    nav::NavBakeSettings settings = bake_settings();
};

/// The update reply's two coordinate lists: rebuilt tiles and obstacle-marked tiles.
void read_update(const Event& event, usize& rebuilt, usize& marked) {
    Decoder decoder(event.payload);
    (void)decoder.u32_();
    (void)decoder.u64_();
    (void)decoder.u64_();
    rebuilt = decoder.u32_();
    for (usize index = 0; index < rebuilt * 3; ++index) {
        (void)decoder.i32_();
    }
    marked = decoder.u32_();
    for (usize index = 0; index < marked * 3; ++index) {
        (void)decoder.i32_();
    }
    (void)decoder.u32_();
    CY_CHECK(decoder.done());
}

nav::NavObstacleShape wall() {
    nav::NavObstacleShape shape;
    shape.bounds = box(11.0F, -1.0F, 13.0F, 5.0F);
    return shape;
}

}  // namespace

CY_TEST_CASE("editor_backend: navigation bake equals build_tile tile by tile") {
    Fixture fixture;
    square_world(fixture.seam);
    const Completed baked = fixture.bake(1);
    CY_REQUIRE(baked.decoded);
    CY_CHECK_EQ(baked.world, kWorld);
    CY_CHECK_EQ(baked.tiles_built, 4U);
    CY_CHECK_EQ(baked.backend, static_cast<u8>(nav::NavBuildBackend::Engine));

    // The same inputs through the engine directly: the seam's geometry, the bake's settings and
    // the bake's own tile bounds.
    NavSourceBuffers sources(allocator());
    CY_REQUIRE(fixture.seam.gather(kWorld, sources).has_value());
    const nav::NavBakeSource source = sources.source();
    Array<nav::AreaType> areas(allocator());
    CY_REQUIRE(nav::assign_triangle_areas(source, areas).has_value());
    nav::NavSourceGeometry geometry = source.geometry;
    geometry.area = areas.span();
    Array<nav::TileCoord> coords(allocator());
    CY_REQUIRE(
        nav::tiles_overlapping(kTile, nav::surface_region(source.surfaces), coords).has_value());
    CY_REQUIRE_EQ(coords.size(), baked.tiles.size());

    const nav::NavMesh* mesh = NavigationService::mesh(fixture.session, kWorld);
    CY_REQUIRE(mesh != nullptr);
    std::vector<u64> digests;
    for (usize index = 0; index < coords.size() && index < baked.tiles.size(); ++index) {
        nav::NavBuildReport report;
        auto direct = nav::build_tile(
            allocator(), nav::build_params(fixture.settings), geometry, coords[index],
            nav::bake_tile_bounds(fixture.settings, geometry, coords[index]), report);
        CY_REQUIRE(direct.has_value());
        if (!direct.has_value()) {
            continue;
        }
        const u64 expected = nav::tile_digest(*direct);
        CY_CHECK(baked.tiles[index].coord == coords[index]);
        CY_CHECK_EQ(baked.tiles[index].digest, expected);
        CY_CHECK_GT(baked.tiles[index].polys, 0U);
        if (mesh != nullptr) {
            const u32 slot = mesh->tile_slot(coords[index]);
            CY_REQUIRE_NE(slot, 0xFFFFFFFFU);
            CY_CHECK_EQ(nav::mesh_tile_digest(*mesh, slot), expected);
        }
        digests.push_back(expected);
    }

    // The identity names the fingerprint and the tiles, and the sidecar is stored under it.
    const u64 fingerprint =
        nav::source_fingerprint(fixture.settings, source, kNavigationBakeVersion);
    CY_CHECK_EQ(baked.fingerprint, fingerprint);
    CY_CHECK_EQ(baked.identity,
                nav::bake_identity(fingerprint, Span<const u64>(digests.data(), digests.size())));
    CY_CHECK(fixture.seam.sidecars.contains(baked.identity));
    CY_CHECK_EQ(baked.sidecar, "navigation/" + std::to_string(baked.identity) + ".cynavmesh");
}

CY_TEST_CASE("editor_backend: navigation bake emits one PROGRESS per tile then one COMPLETED") {
    Fixture fixture;
    square_world(fixture.seam);
    CY_REQUIRE_EQ(submit(fixture.service, fixture.session, 5, "navigation.bake",
                         bake_request(fixture.settings)),
                  CY_RESULT_OK);
    const std::vector<Event> events = drain(fixture.service, fixture.session, 5);
    CY_REQUIRE_EQ(events.size(), usize{5});
    for (usize index = 0; index + 1 < events.size(); ++index) {
        CY_CHECK(events[index].is(CY_SERVICE_EVENT_PROGRESS));
        Decoder decoder(events[index].payload);
        CY_CHECK_EQ(decoder.u32_(), static_cast<u32>(index + 1));
        CY_CHECK_EQ(decoder.u32_(), 4U);
        (void)read_tile(decoder);
        CY_CHECK(decoder.done());
    }
    CY_CHECK((!events.empty() && events.back().is(CY_SERVICE_EVENT_COMPLETED)));
    bool present = true;
    (void)poll_once(fixture.service, fixture.session, present);
    CY_CHECK_FALSE(present);
}

CY_TEST_CASE("editor_backend: a cancelled navigation bake ends in CANCELLED and keeps no mesh") {
    Fixture fixture;
    square_world(fixture.seam);
    CY_REQUIRE_EQ(submit(fixture.service, fixture.session, 9, "navigation.bake",
                         bake_request(fixture.settings)),
                  CY_RESULT_OK);
    bool present = false;
    const Event first = poll_once(fixture.service, fixture.session, present);
    CY_REQUIRE(present);
    CY_CHECK(first.is(CY_SERVICE_EVENT_PROGRESS));
    CY_REQUIRE_EQ(fixture.service.cancel(fixture.session, 9), CY_RESULT_OK);
    const Event cancelled = poll_once(fixture.service, fixture.session, present);
    CY_REQUIRE(present);
    CY_CHECK(cancelled.is(CY_SERVICE_EVENT_CANCELLED));
    CY_CHECK_EQ(cancelled.request, 9U);
    (void)poll_once(fixture.service, fixture.session, present);
    CY_CHECK_FALSE(present);
    CY_CHECK(NavigationService::mesh(fixture.session, kWorld) == nullptr);
    CY_CHECK_EQ(fixture.service.cancel(fixture.session, 9), CY_RESULT_NOT_FOUND);
}

CY_TEST_CASE("editor_backend: a second navigation request while a bake is pending is busy") {
    Fixture fixture;
    square_world(fixture.seam);
    CY_REQUIRE_EQ(submit(fixture.service, fixture.session, 20, "navigation.bake",
                         bake_request(fixture.settings)),
                  CY_RESULT_OK);
    bool present = false;
    (void)poll_once(fixture.service, fixture.session, present);
    Payload overlay;
    overlay.u32_(kWorld).u32_(1);
    CY_REQUIRE_EQ(submit(fixture.service, fixture.session, 21, "navigation.overlay.set", overlay),
                  CY_RESULT_OK);
    const Event busy = poll_once(fixture.service, fixture.session, present);
    CY_REQUIRE(present);
    CY_CHECK_EQ(busy.request, 21U);
    CY_CHECK(busy.is(CY_SERVICE_EVENT_FAILED));
    CY_CHECK_EQ(busy.code(), "navigation.busy");
    // The bake is unaffected and still ends in exactly one COMPLETED.
    const std::vector<Event> rest = drain(fixture.service, fixture.session, 20);
    CY_REQUIRE_FALSE(rest.empty());
    CY_CHECK((!rest.empty() && rest.back().is(CY_SERVICE_EVENT_COMPLETED)));
}

CY_TEST_CASE("editor_backend: navigation without a seam is unavailable and not advertised") {
    NavigationService service(allocator());
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(service.open(&session), CY_RESULT_OK);
    const Event refused =
        call(service, session, 1, "navigation.bake", bake_request(bake_settings()));
    CY_CHECK(refused.is(CY_SERVICE_EVENT_FAILED));
    CY_CHECK_EQ(refused.code(), "navigation.bake.unavailable");
    const Event picked = call(service, session, 2, "navigation.point.pick");
    CY_CHECK_EQ(picked.code(), "navigation.point.pick.unavailable");

    u64 features = 1;
    const Event bare = call(service, session, 3, "capabilities.get");
    CY_CHECK(bare.is(CY_SERVICE_EVENT_COMPLETED));
    const std::vector<std::string> none = capability_names(bare, features);
    CY_CHECK_EQ(none.size(), usize{1});
    CY_CHECK_EQ(features, 0U);
    service.close(session);

    Fixture fixture;
    const Event full = call(fixture.service, fixture.session, 4, "capabilities.get");
    const std::vector<std::string> names = capability_names(full, features);
    CY_CHECK_EQ(names.size(), usize{9});
    CY_CHECK_EQ(features, kNavigationFeature);
    for (const char* operation :
         {"navigation.bake", "navigation.status", "navigation.update", "navigation.path.query",
          "navigation.flowfield.query", "navigation.point.pick", "navigation.overlay.set",
          "navigation.clear"}) {
        CY_CHECK(std::ranges::find(names, operation) != names.end());
    }
}

CY_TEST_CASE(
    "editor_backend: an obstacle added through the service blocks the path and "
    "removing it restores it") {
    Fixture fixture;
    corridor_world(fixture.seam);
    (void)fixture.bake(1);
    const Vec3 start{2.0F, 0.0F, 2.0F};
    const Vec3 end{14.0F, 0.0F, 2.0F};
    const PathReply open = fixture.path(2, start, end);
    CY_REQUIRE(open.found);
    CY_CHECK_FALSE(open.partial);

    // The host syncs a NavObstacle and sends the update: nothing is rebuilt, one tile is marked.
    fixture.seam.obstacle_list.push_back(wall());
    const Event placed = fixture.update(3, wall().bounds);
    CY_REQUIRE(placed.is(CY_SERVICE_EVENT_COMPLETED));
    usize rebuilt = 9;
    usize marked = 9;
    read_update(placed, rebuilt, marked);
    CY_CHECK_EQ(rebuilt, usize{0});
    CY_CHECK_EQ(marked, usize{1});
    const PathReply blocked = fixture.path(4, start, end);
    CY_CHECK((!blocked.found || blocked.partial));

    // The engine agrees when asked directly.
    const nav::NavMesh* mesh = NavigationService::mesh(fixture.session, kWorld);
    CY_REQUIRE(mesh != nullptr);
    if (mesh != nullptr) {
        nav::PathFilter filter;
        filter.node_budget = 8192;
        nav::PathCorridor corridor(allocator());
        const nav::PathResult direct =
            nav::find_path(*mesh, start, end, Vec3{0.5F, 1.0F, 0.5F}, filter, corridor);
        CY_CHECK((!direct.found || direct.partial));
    }

    fixture.seam.obstacle_list.clear();
    const Event cleared = fixture.update(5, wall().bounds);
    CY_REQUIRE(cleared.is(CY_SERVICE_EVENT_COMPLETED));
    read_update(cleared, rebuilt, marked);
    CY_CHECK_EQ(marked, usize{1});
    const PathReply restored = fixture.path(6, start, end);
    CY_CHECK(restored.found);
    CY_CHECK_FALSE(restored.partial);
    CY_CHECK_EQ(restored.cost, open.cost);
}

CY_TEST_CASE("editor_backend: navigation status reports a stale bake after a source change") {
    Fixture fixture;
    square_world(fixture.seam);
    const Completed baked = fixture.bake(1);
    const StatusReply fresh = fixture.status(2, baked);
    CY_CHECK(fresh.baked);
    CY_CHECK_FALSE(fresh.stale);
    CY_CHECK_EQ(fresh.current, baked.fingerprint);
    CY_CHECK_EQ(fresh.identity, baked.identity);
    CY_CHECK_EQ(fresh.tiles, 4U);

    // A mesh moves under the surface: the engine's recomputed fingerprint no longer matches.
    Vec3& moved = fixture.seam.geometry.vertices[0];
    const Vec3 original = moved;
    moved.y += 0.25F;
    const StatusReply stale = fixture.status(3, baked);
    CY_CHECK(stale.stale);
    CY_CHECK_NE(stale.current, baked.fingerprint);

    // Undoing the edit makes the bake current again.
    moved = original;
    const StatusReply current = fixture.status(4, baked);
    CY_CHECK_FALSE(current.stale);

    // A settings edit is a source change too.
    fixture.settings.agent_radius = 0.5F;
    const StatusReply resized = fixture.status(5, baked);
    CY_CHECK(resized.stale);
}

CY_TEST_CASE("editor_backend: a stale flag raised outside the sources is the bake's to consume") {
    Fixture fixture;
    square_world(fixture.seam);
    const Completed baked = fixture.bake(1);
    CY_CHECK_EQ(fixture.seam.bakes_committed, 1U);

    // The terrain tools flag the navmesh stale; the fingerprint alone would call it current.
    fixture.seam.external_stale = true;
    const StatusReply flagged = fixture.status(2, baked);
    CY_CHECK(flagged.stale);
    CY_CHECK_EQ(flagged.current, baked.fingerprint);

    // Bake consumes the flag: the next status is current and nothing else cleared it.
    const Completed rebaked = fixture.bake(3);
    CY_CHECK_EQ(fixture.seam.bakes_committed, 2U);
    CY_CHECK_FALSE(fixture.seam.external_stale);
    CY_CHECK_FALSE(fixture.status(4, rebaked).stale);

    // A cancelled bake commits nothing, so it consumes nothing.
    fixture.seam.external_stale = true;
    CY_REQUIRE_EQ(submit(fixture.service, fixture.session, 5, "navigation.bake",
                         bake_request(fixture.settings)),
                  CY_RESULT_OK);
    CY_REQUIRE_EQ(fixture.service.cancel(fixture.session, 5), CY_RESULT_OK);
    const std::vector<Event> cancelled = drain(fixture.service, fixture.session, 5);
    CY_REQUIRE_FALSE(cancelled.empty());
    CY_CHECK(cancelled.back().is(CY_SERVICE_EVENT_CANCELLED));
    CY_CHECK(fixture.seam.external_stale);
    CY_CHECK_EQ(fixture.seam.bakes_committed, 2U);
}

CY_TEST_CASE("editor_backend: navigation status restores a saved bake into a new session") {
    Fixture first;
    square_world(first.seam);
    const Completed baked = first.bake(1);

    NavigationService service(allocator(), &first.seam);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(service.open(&session), CY_RESULT_OK);
    CY_CHECK(NavigationService::mesh(session, kWorld) == nullptr);
    Payload payload;
    payload.u32_(kWorld).settings(first.settings).u64_(baked.identity).u64_(baked.fingerprint);
    const Event restored = call(service, session, 2, "navigation.status", payload);
    CY_REQUIRE(restored.is(CY_SERVICE_EVENT_COMPLETED));
    const StatusReply reply = read_status(restored);
    CY_CHECK(reply.baked);
    CY_CHECK_FALSE(reply.stale);
    CY_CHECK_EQ(reply.identity, baked.identity);
    const nav::NavMesh* mesh = NavigationService::mesh(session, kWorld);
    CY_REQUIRE(mesh != nullptr);
    CY_CHECK_EQ(mesh->tile_count(), 4U);

    // A recorded bake whose sidecar the host cannot find (not committed, or deleted) is reported
    // as unbaked with the sidecar missing and the current fingerprint, and the session drops the
    // mesh it held rather than answer queries on a bake the document does not record.
    Payload missing;
    missing.u32_(kWorld).settings(first.settings).u64_(baked.identity + 1).u64_(1);
    const Event absent = call(service, session, 3, "navigation.status", missing);
    CY_REQUIRE(absent.is(CY_SERVICE_EVENT_COMPLETED));
    const StatusReply lost = read_status(absent);
    CY_CHECK(lost.decoded);
    CY_CHECK_FALSE(lost.baked);
    CY_CHECK(lost.sidecar_missing);
    CY_CHECK_FALSE(lost.stale);
    CY_CHECK_EQ(lost.current, baked.fingerprint);
    CY_CHECK(NavigationService::mesh(session, kWorld) == nullptr);
    CY_CHECK_FALSE(reply.sidecar_missing);

    // A sidecar that is there but does not decode is still a failure, by name.
    first.seam.sidecars[baked.identity + 2] = {1, 2, 3};
    Payload corrupt;
    corrupt.u32_(kWorld).settings(first.settings).u64_(baked.identity + 2).u64_(1);
    const Event refused = call(service, session, 4, "navigation.status", corrupt);
    CY_CHECK_EQ(refused.code(), "navigation.bake.load-failed");
    service.close(session);
}

CY_TEST_CASE("editor_backend: navigation path query returns points, cost and a partial flag") {
    Fixture fixture;
    corridor_world(fixture.seam);
    (void)fixture.bake(1);
    const Vec3 start{2.0F, 0.0F, 2.0F};
    const Vec3 end{14.0F, 0.0F, 2.0F};
    const PathReply path = fixture.path(2, start, end);
    CY_REQUIRE(path.found);
    CY_CHECK_FALSE(path.partial);
    CY_CHECK_GT(path.cost, 10.0F);
    CY_REQUIRE(path.points.size() >= 2);
    if (path.points.size() >= 2) {
        CY_CHECK_NEAR(path.points.front().x, start.x, 0.5F);
        CY_CHECK_NEAR(path.points.back().x, end.x, 0.5F);
    }

    // A wall across the corridor: the search ends at the closest reachable point, flagged.
    fixture.seam.obstacle_list.push_back(wall());
    CY_REQUIRE(fixture.update(3, wall().bounds).is(CY_SERVICE_EVENT_COMPLETED));
    const PathReply partial = fixture.path(4, start, end);
    CY_CHECK(partial.found);
    CY_CHECK(partial.partial);

    Payload unbaked;
    unbaked.u32_(kWorld + 1).vec3(start).vec3(end).vec3(Vec3{0.5F, 1.0F, 0.5F});
    const Event refused =
        call(fixture.service, fixture.session, 5, "navigation.path.query", unbaked);
    CY_CHECK_EQ(refused.code(), "navigation.world.unbaked");
    const Event malformed = call(fixture.service, fixture.session, 6, "navigation.path.query");
    CY_CHECK_EQ(malformed.code(), "navigation.request.malformed");
}

CY_TEST_CASE("editor_backend: navigation point pick returns a hit and a miss") {
    Fixture fixture;
    square_world(fixture.seam);
    (void)fixture.bake(1);
    Payload payload;
    payload.u32_(kWorld).u32_(0).u64_(42).f32_(100.0F).f32_(80.0F);

    fixture.seam.ray = Ray{Vec3{4.0F, 10.0F, 5.0F}, Vec3{0.0F, -1.0F, 0.0F}};
    const Event hit = call(fixture.service, fixture.session, 2, "navigation.point.pick", payload);
    CY_REQUIRE(hit.is(CY_SERVICE_EVENT_COMPLETED));
    Decoder decoder(hit.payload);
    CY_CHECK_EQ(decoder.u8_(), 1U);
    const Vec3 point = decoder.vec3();
    CY_CHECK_NEAR(point.x, 4.0F, 0.001F);
    CY_CHECK_NEAR(point.z, 5.0F, 0.001F);
    CY_CHECK_NEAR(point.y, 0.0F, 0.5F);
    CY_CHECK_NE(decoder.u64_(), 0U);
    (void)decoder.f32_();
    CY_CHECK(decoder.done());

    fixture.seam.ray = Ray{Vec3{40.0F, 10.0F, 40.0F}, Vec3{0.0F, -1.0F, 0.0F}};
    const Event miss = call(fixture.service, fixture.session, 3, "navigation.point.pick", payload);
    CY_REQUIRE(miss.is(CY_SERVICE_EVENT_COMPLETED));
    Decoder missed(miss.payload);
    CY_CHECK_EQ(missed.u8_(), 0U);

    fixture.seam.ray_available = false;
    const Event failed =
        call(fixture.service, fixture.session, 4, "navigation.point.pick", payload);
    CY_CHECK_EQ(failed.code(), "navigation.point.pick.ray-failed");
}

CY_TEST_CASE("editor_backend: navigation flow field and overlay operations answer") {
    Fixture fixture;
    corridor_world(fixture.seam);
    (void)fixture.bake(1);
    Payload field;
    field.u32_(kWorld).vec3(Vec3{14.0F, 0.0F, 2.0F}).aabb(box(0.0F, 0.0F, 16.0F, 4.0F)).f32_(1.0F);
    const Event flow =
        call(fixture.service, fixture.session, 2, "navigation.flowfield.query", field);
    CY_REQUIRE(flow.is(CY_SERVICE_EVENT_COMPLETED));
    Decoder decoder(flow.payload);
    const u32 width = decoder.u32_();
    const u32 depth = decoder.u32_();
    CY_CHECK_EQ(width, 16U);
    CY_CHECK_EQ(depth, 4U);
    CY_CHECK_EQ(decoder.f32_(), 1.0F);
    (void)decoder.u32_();
    u32 reachable = 0;
    for (u32 cell = 0; cell < width * depth && !decoder.overrun; ++cell) {
        (void)decoder.f32_();
        (void)decoder.f32_();
        reachable += decoder.u8_();
    }
    CY_CHECK(decoder.done());
    CY_CHECK_GT(reachable, 0U);

    Payload huge;
    huge.u32_(kWorld).vec3(Vec3{}).aabb(box(0.0F, 0.0F, 1000.0F, 1000.0F)).f32_(1.0F);
    const Event refused =
        call(fixture.service, fixture.session, 3, "navigation.flowfield.query", huge);
    CY_CHECK_EQ(refused.code(), "navigation.flowfield.too-large");

    Payload overlay;
    overlay.u32_(kWorld).u32_(static_cast<u32>(nav::NavDebugFlags::Polygons));
    CY_CHECK(call(fixture.service, fixture.session, 4, "navigation.overlay.set", overlay)
                 .is(CY_SERVICE_EVENT_COMPLETED));
    CY_CHECK_EQ(NavigationService::overlay(fixture.session, kWorld),
                static_cast<u32>(nav::NavDebugFlags::Polygons));
    Payload bad;
    bad.u32_(kWorld).u32_(0x80000000U);
    CY_CHECK_EQ(call(fixture.service, fixture.session, 5, "navigation.overlay.set", bad).code(),
                "navigation.overlay.invalid");
}

CY_TEST_CASE(
    "editor_backend: navigation bake refuses bad settings, an unknown world and no "
    "surface") {
    Fixture fixture;
    square_world(fixture.seam);
    nav::NavBakeSettings automatic = bake_settings();
    automatic.backend = nav::NavBuildBackend::Automatic;
    CY_CHECK_EQ(
        call(fixture.service, fixture.session, 1, "navigation.bake", bake_request(automatic))
            .code(),
        "navigation.settings.invalid");
    Payload unknown;
    unknown.u32_(kWorld + 1).settings(bake_settings());
    CY_CHECK_EQ(call(fixture.service, fixture.session, 2, "navigation.bake", unknown).code(),
                "navigation.world.unknown");
    fixture.seam.surfaces.clear();
    CY_CHECK_EQ(
        call(fixture.service, fixture.session, 3, "navigation.bake", bake_request(bake_settings()))
            .code(),
        "navigation.surface.missing");
    CY_CHECK_EQ(call(fixture.service, fixture.session, 4, "navigation.teleport").code(),
                "navigation.operation.unsupported");
}

namespace {

/// A strip of mud across the corridor, at ten times the cost.
nav::NavAreaVolume mud_strip(f32 cost) {
    return nav::NavAreaVolume{9, box(6.0F, -1.0F, 10.0F, 5.0F), 5, cost};
}

/// Every resident tile's digest in `session`'s mesh of the world.
std::vector<std::pair<std::pair<i32, i32>, u64>> resident(CyServiceSession session) {
    std::vector<std::pair<std::pair<i32, i32>, u64>> out;
    const nav::NavMesh* mesh = NavigationService::mesh(session, kWorld);
    if (mesh == nullptr) {
        return out;
    }
    for (u32 slot = 0; slot < mesh->tile_capacity(); ++slot) {
        const nav::TileCoord coord = mesh->tile_coord(slot);
        if (mesh->tile_slot(coord) == slot) {
            out.push_back({{coord.x, coord.z}, nav::mesh_tile_digest(*mesh, slot)});
        }
    }
    std::ranges::sort(out);
    return out;
}

/// The digests a fresh bake of the seam's current sources produces.
std::vector<std::pair<std::pair<i32, i32>, u64>> fresh(FixtureSeam& seam,
                                                       const nav::NavBakeSettings& settings) {
    NavigationService service(allocator(), &seam);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(service.open(&session), CY_RESULT_OK);
    CY_CHECK(call(service, session, 1, "navigation.bake", bake_request(settings))
                 .is(CY_SERVICE_EVENT_COMPLETED));
    auto out = resident(session);
    service.close(session);
    return out;
}

}  // namespace

CY_TEST_CASE("editor_backend: a restored bake keeps its area costs in a new session and on undo") {
    Fixture fixture;
    corridor_world(fixture.seam);
    fixture.seam.areas.push_back(mud_strip(10.0F));
    const Completed dear = fixture.bake(1);
    const Vec3 start{2.0F, 0.0F, 2.0F};
    const Vec3 end{14.0F, 0.0F, 2.0F};
    const PathReply before = fixture.path(2, start, end);
    CY_REQUIRE(before.found);

    // Reopened in a new session: the sidecar comes back and the path costs what it did.
    NavigationService service(allocator(), &fixture.seam);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(service.open(&session), CY_RESULT_OK);
    Payload restore;
    restore.u32_(kWorld).settings(fixture.settings).u64_(dear.identity).u64_(dear.fingerprint);
    CY_REQUIRE(
        call(service, session, 3, "navigation.status", restore).is(CY_SERVICE_EVENT_COMPLETED));
    Payload query;
    query.u32_(kWorld).vec3(start).vec3(end).vec3(Vec3{0.5F, 1.0F, 0.5F});
    const Event reopened = call(service, session, 4, "navigation.path.query", query);
    CY_REQUIRE(reopened.is(CY_SERVICE_EVENT_COMPLETED));
    const PathReply after = read_path(reopened);
    CY_CHECK(after.found);
    CY_CHECK_EQ(after.cost, before.cost);
    service.close(session);

    // Undo: a cheaper rebake is replaced by the recorded one, and its costs come back with it.
    fixture.seam.areas[0].cost = 1.0F;
    const Completed cheap = fixture.bake(5);
    CY_REQUIRE_NE(cheap.identity, dear.identity);
    fixture.seam.areas[0].cost = 10.0F;
    CY_CHECK_FALSE(fixture.status(6, dear).stale);
    const PathReply undone = fixture.path(7, start, end);
    CY_CHECK_EQ(undone.cost, before.cost);
}

CY_TEST_CASE(
    "editor_backend: an update after restoring a stale bake rebuilds every changed tile, not only "
    "the dirty box") {
    Fixture first;
    square_world(first.seam);
    const Completed baked = first.bake(1);

    // Between sessions the sources change far from where the next edit happens.
    first.seam.areas.push_back(nav::NavAreaVolume{4, box(10.0F, 10.0F, 14.0F, 14.0F), 5, 3.0F});
    NavigationService service(allocator(), &first.seam);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(service.open(&session), CY_RESULT_OK);
    Payload restore;
    restore.u32_(kWorld).settings(first.settings).u64_(baked.identity).u64_(baked.fingerprint);
    const Event restored = call(service, session, 2, "navigation.status", restore);
    CY_REQUIRE(restored.is(CY_SERVICE_EVENT_COMPLETED));
    CY_CHECK(read_status(restored).stale);
    // The change is real: a fresh bake of the new sources differs from the restored mesh.
    CY_REQUIRE(resident(session) != fresh(first.seam, first.settings));

    // An obstacle is placed in tile (0, 0): its box is all the host dirties.
    first.seam.obstacle_list.push_back(nav::NavObstacleShape{});
    first.seam.obstacle_list.back().bounds = box(2.0F, 2.0F, 3.0F, 3.0F);
    Payload update;
    update.u32_(kWorld).aabb(first.seam.obstacle_list.back().bounds);
    CY_REQUIRE(
        call(service, session, 3, "navigation.update", update).is(CY_SERVICE_EVENT_COMPLETED));
    // The mud's tile (1, 1) is rebuilt too: the live mesh is a bake of one set of sources.
    CY_CHECK(resident(session) == fresh(first.seam, first.settings));
    service.close(session);
}

CY_TEST_CASE(
    "editor_backend: an edit that changes the geometry's height range rebuilds every tile") {
    // Recast quantises heights from the bottom of the build box, so there a new floor changes
    // every tile; the engine back end samples exact heights and would not show it.
    if (!nav::recast_available()) {
        CY_TEST_MESSAGE("SKIP: this build has no Recast (CY_NAVIGATION is off)");
        return;
    }
    Fixture fixture;
    fixture.settings.backend = nav::NavBuildBackend::Recast;
    square_world(fixture.seam);
    (void)fixture.bake(1);
    const auto flat = resident(fixture.session);

    // A ledge below the ground in tile (1, 1) lowers every tile's build box.
    fixture.seam.geometry.triangle(Vec3{12.0F, -1.3F, 12.0F}, Vec3{13.0F, -1.3F, 13.0F},
                                   Vec3{13.0F, -1.3F, 12.0F});
    const auto expected = fresh(fixture.seam, fixture.settings);
    CY_REQUIRE_EQ(expected.size(), flat.size());
    CY_REQUIRE(!expected.empty());
    CY_CHECK_NE(expected.front().second, flat.front().second);  // tile (0, 0) changed
    CY_REQUIRE(fixture.update(2, box(12.0F, 12.0F, 13.0F, 13.0F)).is(CY_SERVICE_EVENT_COMPLETED));
    CY_CHECK(resident(fixture.session) == expected);
}

CY_TEST_CASE("editor_backend: navigation clear drops the mesh and refuses later queries") {
    Fixture fixture;
    corridor_world(fixture.seam);
    (void)fixture.bake(1);
    CY_REQUIRE(NavigationService::mesh(fixture.session, kWorld) != nullptr);
    Payload clear;
    clear.u32_(kWorld);
    const Event cleared = call(fixture.service, fixture.session, 2, "navigation.clear", clear);
    CY_CHECK(cleared.is(CY_SERVICE_EVENT_COMPLETED));
    CY_CHECK(NavigationService::mesh(fixture.session, kWorld) == nullptr);
    Payload query;
    query.u32_(kWorld).vec3(Vec3{2.0F, 0.0F, 2.0F}).vec3(Vec3{14.0F, 0.0F, 2.0F});
    query.vec3(Vec3{0.5F, 1.0F, 0.5F});
    CY_CHECK_EQ(call(fixture.service, fixture.session, 3, "navigation.path.query", query).code(),
                "navigation.world.unbaked");
    // Clearing a world with no mesh is not an error.
    CY_CHECK(call(fixture.service, fixture.session, 4, "navigation.clear", clear)
                 .is(CY_SERVICE_EVENT_COMPLETED));
    CY_CHECK_EQ(call(fixture.service, fixture.session, 5, "navigation.clear").code(),
                "navigation.request.malformed");
}
