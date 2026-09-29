// SPDX-License-Identifier: MIT
// The runtime host's navigation seam over a `.cyworld` map, through the composite binding the
// runtime installs. Issue #28, tasks 3.1, 3.2 and 3.4 to 3.6, and acceptance criteria 1 and 5 of
// `implement-issue-28-navigation-authoring`.
//
// `tests/data/nav_test_map.cyworld` is a 16 m square of ground (four 8 m tiles), a crate, one
// navigation world with an including surface, a mud area, a pillar obstacle and a link. The suite
// drives it the way the runtime does: the editor's requests go to one CompositeEditorService
// (MaterialService and NavigationService), the document changes the editor syncs are replayed by
// re-reading the world text, and `drain_service_events` runs the runtime's own requests. No
// device is needed: the frame's view is recorded directly.

#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/editor/composite_service.h>
#include <cy/editor/material_service.h>
#include <cy/editor/navigation_service.h>
#include <cy/navigation/bake.h>
#include <cy/navigation/debug.h>
#include <cy/navigation/tile_identity.h>
#include <cy/scene/serialization/authoring_schema.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/servers/render/picking.h>
#include <cy/test/test.h>
#include <cy_reflect_generated_scene.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "nav_runtime.h"
#include "nav_test_support.h"
#include "navigation_seam.h"

using namespace cy;
using namespace cy::editor;
using namespace cy::editor::testing;
using namespace cy::sample::editor_window;
using cy::sample::editor_window::testing::overhead_view;
namespace nav = cy::navigation;
namespace ser = cy::scene::serialization;

namespace {

constexpr u32 kMapWorld = 1;
constexpr const char* kMapPath = "nav_test_map.cyworld";

[[nodiscard]] std::string read_text(const std::string& path) {
    Array<u8> bytes(allocator());
    CY_REQUIRE(assets::fs::read_whole(path.c_str(), bytes).has_value());
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// `text` with the one line `from` replaced by `to`.
[[nodiscard]] std::string edited(std::string text, const std::string& from, const std::string& to) {
    const usize at = text.find(from);
    CY_REQUIRE_NE(at, std::string::npos);
    if (at != std::string::npos) {
        text.replace(at, from.size(), to);
    }
    return text;
}

/// The recorded bake identity and fingerprint, spelled as the document's int fields.
[[nodiscard]] std::string recorded(std::string text, u64 identity, u64 fingerprint) {
    const auto spelled = [](u64 value) {
        i64 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return std::to_string(bits);
    };
    text = edited(std::move(text), "    field 20 0\n", "    field 20 " + spelled(identity) + "\n");
    return edited(std::move(text), "    field 21 0\n",
                  "    field 21 " + spelled(fingerprint) + "\n");
}

struct BakeTile {
    nav::TileCoord coord;
    u32 polys = 0;
    u64 digest = 0;
};

struct Baked {
    u64 fingerprint = 0;
    u64 identity = 0;
    std::string sidecar;
    u32 tiles_built = 0;
    std::vector<BakeTile> tiles;
    bool decoded = false;
};

[[nodiscard]] Baked read_baked(const Event& event) {
    Decoder decoder(event.payload);
    Baked out;
    CY_CHECK_EQ(decoder.u32_(), kMapWorld);
    out.fingerprint = decoder.u64_();
    out.identity = decoder.u64_();
    out.sidecar = decoder.text();
    for (u32 counter = 0; counter < 6; ++counter) {
        (void)decoder.u32_();
    }
    (void)decoder.u64_();
    (void)decoder.u8_();
    out.tiles_built = decoder.u32_();
    (void)decoder.u32_();
    (void)decoder.u32_();
    const u32 count = decoder.u32_();
    for (u32 index = 0; index < count && !decoder.overrun; ++index) {
        BakeTile tile;
        tile.coord.x = decoder.i32_();
        tile.coord.z = decoder.i32_();
        tile.coord.layer = decoder.i32_();
        tile.polys = decoder.u32_();
        (void)decoder.u8_();
        tile.digest = decoder.u64_();
        out.tiles.push_back(tile);
    }
    out.decoded = decoder.done();
    return out;
}

using Digests = std::map<std::pair<i32, i32>, u64>;

[[nodiscard]] Digests digests_of(const nav::NavMesh* mesh) {
    Digests out;
    if (mesh == nullptr) {
        return out;
    }
    for (u32 slot = 0; slot < mesh->tile_capacity(); ++slot) {
        const nav::TileCoord coord = mesh->tile_coord(slot);
        if (mesh->tile_slot(coord) == slot) {
            out[{coord.x, coord.z}] = nav::mesh_tile_digest(*mesh, slot);
        }
    }
    return out;
}

/// The runtime's navigation half over the test map: world, seam, composite binding and driver.
struct Runtime {
    reflect::TypeRegistry registry;
    ser::AuthoringSchema schema{allocator()};
    ser::World world{allocator()};
    std::string text = read_text(std::string(CY_NAV_TEST_DATA) + "/" + kMapPath);
    std::string sidecars;
    AuthoredNavigationSource source{allocator(), CY_NAV_TEST_DATA};
    MaterialService material{allocator()};
    NavigationService navigation{allocator(), &source};
    CompositeEditorService composite{allocator()};
    CyServiceSession session = nullptr;
    std::unique_ptr<NavigationDriver> driver;
    u64 next_id = 1;

    Runtime() {
        static u32 counter = 0;
        sidecars =
            (std::filesystem::temp_directory_path() /
             ("cy-nav-runtime-" + std::to_string(::getpid()) + "-" + std::to_string(counter++)))
                .string();
        source.set_sidecar_root(sidecars);
        CY_REQUIRE(reflect::register_scene_types(registry).has_value());
        CY_REQUIRE(ser::build_authoring_schema(registry, schema).has_value());
        reload(text);
        source.bind(&world);
        CY_REQUIRE(composite.route("material.", material).has_value());
        CY_REQUIRE(composite.route("preview.", material).has_value());
        CY_REQUIRE(composite.route("vfx.", material).has_value());
        CY_REQUIRE(composite.route("navigation.", navigation).has_value());
        CY_REQUIRE_EQ(composite.open(&session), CY_RESULT_OK);
        driver = std::make_unique<NavigationDriver>(composite, session);
    }
    ~Runtime() {
        driver.reset();
        composite.close(session);
        std::error_code ignored;
        std::filesystem::remove_all(sidecars, ignored);
    }
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    /// Replaces the world with `next`, as `WorldView::sync` does for an editor snapshot.
    void reload(const std::string& next) {
        ser::World replacement(allocator());
        CY_REQUIRE(ser::read_world(next, kMapPath, replacement).has_value());
        CY_REQUIRE(ser::resolve_against(replacement, schema).has_value());
        world = std::move(replacement);
    }

    /// Syncs `next` and lets the runtime's own requests finish.
    void sync(const std::string& next) {
        reload(next);
        CY_REQUIRE(driver->document_changed(source).has_value());
        settle();
    }

    void settle() {
        for (u32 frame = 0; frame < 4096 && !driver->idle(); ++frame) {
            (void)drain_service_events(composite, session, *driver, 64, nullptr, nullptr);
        }
        CY_CHECK(driver->idle());
    }

    [[nodiscard]] nav::NavBakeSettings settings() const {
        Array<NavWorldRecord> records(allocator());
        CY_REQUIRE(read_navigation_worlds(world, records).has_value());
        CY_REQUIRE_EQ(records.size(), usize{1});
        return records.empty() ? nav::NavBakeSettings{} : records[0].settings;
    }

    [[nodiscard]] Event request(const char* operation, const Payload& payload) {
        CY_REQUIRE(driver->idle());
        return call(composite, session, next_id++, operation, payload);
    }

    [[nodiscard]] Baked bake() {
        Payload payload;
        payload.u32_(kMapWorld).settings(settings());
        const Event done = request("navigation.bake", payload);
        CY_REQUIRE(done.is(CY_SERVICE_EVENT_COMPLETED));
        return read_baked(done);
    }

    /// `navigation.status` against a saved bake: whether it is stale.
    [[nodiscard]] bool stale(const Baked& saved) {
        Payload payload;
        payload.u32_(kMapWorld).settings(settings()).u64_(saved.identity).u64_(saved.fingerprint);
        const Event done = request("navigation.status", payload);
        CY_REQUIRE(done.is(CY_SERVICE_EVENT_COMPLETED));
        Decoder decoder(done.payload);
        (void)decoder.u32_();
        CY_CHECK_EQ(decoder.u8_(), 1U);  // baked
        return decoder.u8_() != 0;
    }

    [[nodiscard]] const nav::NavMesh* mesh() const {
        return NavigationService::mesh(composite.child_session(session, navigation), kMapWorld);
    }
};

}  // namespace

CY_TEST_CASE("editor runtime: baking the test map equals build_tile tile by tile") {
    Runtime runtime;
    Array<u32> worlds(allocator());
    CY_REQUIRE(runtime.source.worlds(worlds).has_value());
    CY_REQUIRE_EQ(worlds.size(), usize{1});
    if (worlds.size() != 1) {
        return;
    }
    CY_CHECK_EQ(worlds[0], kMapWorld);

    // The material half of the composite binding still answers.
    const Event catalogue = runtime.request("material.catalogue.get", Payload{});
    CY_CHECK(catalogue.is(CY_SERVICE_EVENT_COMPLETED));

    const Baked baked = runtime.bake();
    CY_REQUIRE(baked.decoded);
    CY_CHECK_EQ(baked.tiles_built, 4U);
    CY_CHECK_EQ(runtime.source.mesh_failures(), 0U);

    // The same sources through the engine directly: the seam's triangles, the map's settings, the
    // bake's own tile bounds.
    const nav::NavBakeSettings settings = runtime.settings();
    NavSourceBuffers sources(allocator());
    CY_REQUIRE(runtime.source.gather(kMapWorld, sources).has_value());
    // 17 x 17 ground vertices plus a box, from both MeshRenderer nodes.
    CY_CHECK_GT(sources.vertices.size(), usize{289});
    // In world space: the ground spans 0..16 in x and z, and the crate's top is at 1 m.
    Aabb extent;
    for (const Vec3 vertex : sources.vertices) {
        extent = merge(extent, Aabb::from_point(vertex));
    }
    CY_CHECK_NEAR(extent.min.x, 0.0F, 1e-4F);
    CY_CHECK_NEAR(extent.max.x, 16.0F, 1e-4F);
    CY_CHECK_NEAR(extent.max.z, 16.0F, 1e-4F);
    CY_CHECK_NEAR(extent.max.y, 1.0F, 1e-4F);
    const nav::NavBakeSource source = sources.source();
    Array<nav::AreaType> areas(allocator());
    CY_REQUIRE(nav::assign_triangle_areas(source, areas).has_value());
    nav::NavSourceGeometry geometry = source.geometry;
    geometry.area = areas.span();
    Array<nav::TileCoord> coords(allocator());
    CY_REQUIRE(
        nav::tiles_overlapping(settings.tile_size, nav::surface_region(source.surfaces), coords)
            .has_value());
    CY_REQUIRE_EQ(coords.size(), baked.tiles.size());
    const nav::NavMesh* mesh = runtime.mesh();
    CY_REQUIRE(mesh != nullptr);
    std::vector<u64> expected;
    for (usize index = 0; index < coords.size() && index < baked.tiles.size(); ++index) {
        nav::NavBuildReport report;
        auto direct =
            nav::build_tile(allocator(), nav::build_params(settings), geometry, coords[index],
                            nav::bake_tile_bounds(settings, geometry, coords[index]), report);
        CY_REQUIRE(direct.has_value());
        if (!direct.has_value()) {
            continue;
        }
        const u64 digest = nav::tile_digest(*direct);
        CY_CHECK(baked.tiles[index].coord == coords[index]);
        CY_CHECK_EQ(baked.tiles[index].digest, digest);
        CY_CHECK_GT(baked.tiles[index].polys, 0U);
        if (mesh != nullptr) {
            const u32 slot = mesh->tile_slot(coords[index]);
            CY_REQUIRE_NE(slot, 0xFFFFFFFFU);
            CY_CHECK_EQ(nav::mesh_tile_digest(*mesh, slot), digest);
        }
        expected.push_back(digest);
    }
    const u64 fingerprint = nav::source_fingerprint(settings, source, kNavigationBakeVersion);
    CY_CHECK_EQ(baked.fingerprint, fingerprint);
    CY_CHECK_EQ(baked.identity,
                nav::bake_identity(fingerprint, Span<const u64>(expected.data(), expected.size())));

    // The content-addressed sidecar is on disk under that identity, and reads back.
    char name[64] = {};
    (void)std::snprintf(name, sizeof(name), "navigation/%016" PRIx64 ".cynavmesh", baked.identity);
    CY_CHECK_EQ(baked.sidecar, std::string(name));
    Array<u8> sidecar(allocator());
    CY_CHECK(runtime.source.load_bake(baked.identity, sidecar).has_value());
    CY_CHECK_GT(sidecar.size(), usize{0});
}

CY_TEST_CASE(
    "editor runtime: moving a mesh marks the navigation bake stale; moving it back "
    "clears the flag") {
    Runtime runtime;
    runtime.sync(runtime.text);
    const Baked baked = runtime.bake();
    CY_REQUIRE(baked.decoded);
    CY_CHECK_FALSE(runtime.stale(baked));

    // The crate moves a metre inside the surface: the runtime rebuilds the tile under it in
    // process, and the saved bake no longer matches the sources.
    const std::string moved =
        edited(runtime.text, "    field 2 12 0.5 4\n", "    field 2 12 0.5 5\n");
    const u32 before = runtime.driver->updates_completed();
    runtime.sync(moved);
    CY_CHECK_EQ(runtime.driver->updates_completed(), before + 1);
    CY_CHECK_EQ(runtime.driver->last_update().world, kMapWorld);
    CY_CHECK_FALSE(runtime.driver->last_update().rebuilt.empty());
    CY_CHECK(runtime.stale(baked));

    runtime.sync(runtime.text);
    CY_CHECK_FALSE(runtime.stale(baked));
    CY_CHECK_EQ(digests_of(runtime.mesh()).size(), baked.tiles.size());
    for (const BakeTile& tile : baked.tiles) {
        CY_CHECK_EQ(digests_of(runtime.mesh())[{tile.coord.x, tile.coord.z}], tile.digest);
    }
}

CY_TEST_CASE(
    "editor runtime: an area edit on the test map rebuilds only the affected tiles; "
    "reverting restores their digests") {
    Runtime runtime;
    runtime.sync(runtime.text);
    const Baked baked = runtime.bake();
    CY_REQUIRE(baked.decoded);
    const Digests original = digests_of(runtime.mesh());
    CY_REQUIRE_EQ(original.size(), usize{4});

    // The mud moves from (1..3, 1..3) to (1..3, 5..7): both boxes lie in tile (0, 0).
    const std::string moved = edited(runtime.text, "    field 2 2 0 2\n", "    field 2 2 0 6\n");
    runtime.sync(moved);
    const NavUpdateResult& update = runtime.driver->last_update();
    CY_REQUIRE_EQ(update.rebuilt.size(), usize{1});
    if (!update.rebuilt.empty()) {
        CY_CHECK(update.rebuilt[0] == (nav::TileCoord{0, 0, 0}));
    }
    const Digests edited_digests = digests_of(runtime.mesh());
    for (const auto& [coord, digest] : original) {
        const auto now = edited_digests.find(coord);
        CY_CHECK(now != edited_digests.end());
        if (now == edited_digests.end()) {
            continue;  // `at` would abort in this exception-free build
        }
        if (coord == std::pair<i32, i32>{0, 0}) {
            CY_CHECK_NE(now->second, digest);
        } else {
            CY_CHECK_EQ(now->second, digest);
        }
    }

    runtime.sync(runtime.text);
    CY_CHECK(digests_of(runtime.mesh()) == original);
}

CY_TEST_CASE("editor runtime: navigation.point.pick hits the navmesh and misses beyond it") {
    Runtime runtime;
    const Baked baked = runtime.bake();
    CY_REQUIRE(baked.decoded);
    constexpr u32 kSide = 64;
    const NavOverlayView view = overhead_view(Vec3{8.0F, 20.0F, 8.0F}, kSide, kSide);
    runtime.source.record_frame(42, view.view, view.eye);

    const auto pick = [&](u64 frame, f32 x, f32 y) {
        Payload payload;
        payload.u32_(kMapWorld).u32_(1).u64_(frame).f32_(x).f32_(y);
        return runtime.request("navigation.point.pick", payload);
    };
    // The centre of the frame looks at (8, 0, 8), on the ground and away from every obstacle.
    const Event centre = pick(42, 32.0F, 32.0F);
    CY_REQUIRE(centre.is(CY_SERVICE_EVENT_COMPLETED));
    Decoder hit(centre.payload);
    CY_CHECK_EQ(hit.u8_(), 1U);
    const Vec3 point = hit.vec3();
    CY_CHECK_NEAR(point.x, 8.0F, 0.2F);
    CY_CHECK_NEAR(point.z, 8.0F, 0.2F);
    CY_CHECK_NEAR(point.y, 0.0F, 0.5F);
    CY_CHECK_NE(hit.u64_(), 0U);

    // The frame's corner looks past the map's edge: nothing is there to hit.
    const Event corner = pick(42, 0.5F, 0.5F);
    CY_REQUIRE(corner.is(CY_SERVICE_EVENT_COMPLETED));
    Decoder miss(corner.payload);
    CY_CHECK_EQ(miss.u8_(), 0U);

    // A frame the runtime no longer holds is refused by name rather than answered with another.
    const Event unknown = pick(99, 32.0F, 32.0F);
    CY_CHECK(unknown.is(CY_SERVICE_EVENT_FAILED));
    CY_CHECK_EQ(unknown.code(), "navigation.point.pick.ray-failed");
}

CY_TEST_CASE(
    "editor runtime: an undone bake reloads its sidecar and the overlay follows the "
    "recorded identity") {
    Runtime runtime;
    runtime.sync(runtime.text);
    const Baked first = runtime.bake();
    CY_REQUIRE(first.decoded);
    const Digests first_digests = digests_of(runtime.mesh());
    // The editor records the bake and switches the overlay on.
    const std::string shown = edited(runtime.text, "    field 19 0\n", "    field 19 1\n");
    const std::string first_recorded = recorded(shown, first.identity, first.fingerprint);
    runtime.sync(first_recorded);
    std::vector<NavOverlayWorld> overlays;
    runtime.driver->overlay_worlds(
        runtime.composite.child_session(runtime.session, runtime.navigation), overlays);
    CY_REQUIRE_EQ(overlays.size(), usize{1});
    if (!overlays.empty()) {
        CY_CHECK(overlays[0].mesh == runtime.mesh());
        CY_CHECK_EQ(overlays[0].flags, 1U);
    }

    // A second bake after the crate moved, recorded in its turn.
    const std::string moved =
        edited(first_recorded, "    field 2 12 0.5 4\n", "    field 2 12 0.5 5\n");
    runtime.sync(moved);
    const Baked second = runtime.bake();
    CY_REQUIRE(second.decoded);
    CY_REQUIRE_NE(second.identity, first.identity);
    runtime.sync(recorded(edited(shown, "    field 2 12 0.5 4\n", "    field 2 12 0.5 5\n"),
                          second.identity, second.fingerprint));
    CY_CHECK(digests_of(runtime.mesh()) != first_digests);

    // Undo: the document names the first bake again, and the runtime reloads its sidecar.
    const u32 restores = runtime.driver->restores_completed();
    runtime.sync(first_recorded);
    CY_CHECK_EQ(runtime.driver->restores_completed(), restores + 1);
    CY_CHECK(digests_of(runtime.mesh()) == first_digests);
    CY_CHECK_FALSE(runtime.stale(first));

    // A document that records no bake draws no overlay for the world.
    runtime.sync(runtime.text);
    runtime.driver->overlay_worlds(
        runtime.composite.child_session(runtime.session, runtime.navigation), overlays);
    CY_CHECK(overlays.empty());
}

CY_TEST_CASE("editor runtime: the per-frame drain forwards every bake PROGRESS and one COMPLETED") {
    Runtime runtime;
    struct Forwarded {
        std::vector<u32> kinds;
        static void sink(void* user, const CyServiceEvent& event) {
            static_cast<Forwarded*>(user)->kinds.push_back(event.kind);
        }
    } forwarded;
    Payload payload;
    payload.u32_(kMapWorld).settings(runtime.settings());
    const CyServiceRequest request{
        sizeof(CyServiceRequest), 1, 77, "navigation.bake", payload.bytes.data(),
        payload.bytes.size()};
    runtime.driver->editor_request(77, "navigation.bake",
                                   {payload.bytes.data(), payload.bytes.size()});
    CY_REQUIRE_EQ(runtime.composite.submit(runtime.session, request), CY_RESULT_OK);
    // One event per frame: the bake is spread over frames and every event still arrives.
    u32 frames = 0;
    while (frames < 64 &&
           (forwarded.kinds.empty() || forwarded.kinds.back() == CY_SERVICE_EVENT_PROGRESS)) {
        (void)drain_service_events(runtime.composite, runtime.session, *runtime.driver, 1,
                                   &Forwarded::sink, &forwarded);
        ++frames;
    }
    CY_REQUIRE_EQ(forwarded.kinds.size(), usize{5});
    for (usize index = 0; index + 1 < forwarded.kinds.size(); ++index) {
        CY_CHECK_EQ(forwarded.kinds[index], static_cast<u32>(CY_SERVICE_EVENT_PROGRESS));
    }
    if (!forwarded.kinds.empty()) {
        CY_CHECK_EQ(forwarded.kinds.back(), static_cast<u32>(CY_SERVICE_EVENT_COMPLETED));
    }
    CY_CHECK_GE(frames, 5U);
}

namespace {

/// A path query on the test map through the composite binding.
struct MapPath {
    std::string code;  // empty when answered
    bool found = false;
    bool partial = false;
    f32 cost = 0.0F;
};

[[nodiscard]] MapPath map_path(Runtime& runtime, Vec3 start, Vec3 end) {
    Payload payload;
    payload.u32_(kMapWorld).vec3(start).vec3(end).vec3(Vec3{0.5F, 1.0F, 0.5F});
    const Event done = runtime.request("navigation.path.query", payload);
    MapPath out;
    if (!done.is(CY_SERVICE_EVENT_COMPLETED)) {
        out.code = done.code();
        return out;
    }
    Decoder decoder(done.payload);
    out.found = decoder.u8_() != 0;
    out.partial = decoder.u8_() != 0;
    (void)decoder.u8_();
    out.cost = decoder.f32_();
    return out;
}

/// A NavObstacle node over (12, 0, 12), appended the way the editor's `navigation.obstacle.add`
/// writes one into the document.
constexpr const char* kBlocker =
    "node 7 - \"nav\" \"Blocker\"\n"
    "  component 1\n"
    "    field 1 0 0 0 1\n"
    "    field 2 12 0 12\n"
    "    field 3 1 1 1\n"
    "  component 6\n"
    "    field 50 1\n"
    "    field 51 0 0 0\n"
    "    field 52 1.5\n"
    "    field 53 2\n"
    "    field 54 63\n";

}  // namespace

CY_TEST_CASE(
    "editor runtime: undoing the first bake drops the engine's mesh and refuses path queries") {
    Runtime runtime;
    runtime.sync(runtime.text);
    const Baked first = runtime.bake();
    CY_REQUIRE(first.decoded);
    const std::string first_recorded = recorded(runtime.text, first.identity, first.fingerprint);
    runtime.sync(first_recorded);
    const Vec3 start{6.0F, 0.0F, 6.0F};
    const Vec3 end{10.0F, 0.0F, 10.0F};
    const MapPath baked = map_path(runtime, start, end);
    CY_CHECK(baked.code.empty());
    CY_CHECK(baked.found);

    // Undo: the document records no bake again. The engine must follow it back to unbaked.
    const u32 clears = runtime.driver->clears_completed();
    runtime.sync(runtime.text);
    CY_CHECK_EQ(runtime.driver->clears_completed(), clears + 1);
    CY_CHECK(runtime.mesh() == nullptr);
    CY_CHECK_EQ(map_path(runtime, start, end).code, "navigation.world.unbaked");

    // Redo: the recorded identity comes back and its sidecar is reloaded.
    runtime.sync(first_recorded);
    CY_CHECK(runtime.mesh() != nullptr);
    CY_CHECK(map_path(runtime, start, end).found);
}

CY_TEST_CASE(
    "editor runtime: a NavObstacle added to the document blocks the path; removing it "
    "restores the path") {
    Runtime runtime;
    runtime.sync(runtime.text);
    const Baked baked = runtime.bake();
    CY_REQUIRE(baked.decoded);
    const Vec3 start{6.0F, 0.0F, 6.0F};
    const Vec3 end{12.0F, 0.0F, 12.0F};
    const MapPath open = map_path(runtime, start, end);
    CY_REQUIRE(open.code.empty());
    CY_REQUIRE(open.found);
    CY_CHECK_FALSE(open.partial);

    // The editor adds the obstacle: the runtime reads it from the synced document and marks the
    // tile under it, rebuilding nothing.
    const u32 before = runtime.driver->updates_completed();
    runtime.sync(runtime.text + kBlocker);
    CY_CHECK_EQ(runtime.driver->updates_completed(), before + 1);
    CY_CHECK(runtime.driver->last_update().rebuilt.empty());
    CY_CHECK_FALSE(runtime.driver->last_update().marked.empty());
    const MapPath blocked = map_path(runtime, start, end);
    CY_CHECK(blocked.code.empty());
    CY_CHECK((!blocked.found || blocked.partial));

    // Removing it (an undo) restores the path and its cost.
    runtime.sync(runtime.text);
    const MapPath restored = map_path(runtime, start, end);
    CY_CHECK(restored.found);
    CY_CHECK_FALSE(restored.partial);
    CY_CHECK_EQ(restored.cost, open.cost);
}

CY_TEST_CASE(
    "editor runtime: shrinking the surface leaves the mesh equal to a fresh bake of the "
    "shrunk map") {
    Runtime runtime;
    runtime.sync(runtime.text);
    const Baked baked = runtime.bake();
    CY_REQUIRE(baked.decoded);
    CY_REQUIRE_EQ(digests_of(runtime.mesh()).size(), usize{4});

    // The surface shrinks to the z < 8 half: tiles (0, 1) and (1, 1) leave the surface region.
    // The crate stays inside it, so the geometry's height range holds and the runtime takes the
    // incremental path rather than a whole-surface rebuild.
    const std::string shrunk =
        edited(runtime.text, "    field 32 16 3 16\n", "    field 32 16 3 7\n");
    runtime.sync(shrunk);
    const Digests incremental = digests_of(runtime.mesh());

    Runtime fresh;
    fresh.sync(shrunk);
    const Baked rebaked = fresh.bake();
    CY_REQUIRE(rebaked.decoded);
    const Digests full = digests_of(fresh.mesh());
    CY_CHECK_EQ(full.size(), usize{2});
    CY_CHECK(incremental == full);
}

namespace {

/// Every walkable polygon of a mesh with its effective area, as the overlay is told about it.
class PolygonList final : public nav::NavDebugSink {
public:
    struct Entry {
        std::vector<Vec3> corners;
        nav::AreaType area = 0;
    };
    void polygon(nav::PolyRef, Span<const Vec3> corners, nav::AreaType area) noexcept override {
        entries.push_back(Entry{std::vector<Vec3>(corners.begin(), corners.end()), area});
    }
    std::vector<Entry> entries;
};

/// Whether `point` is inside the convex projected polygon of either winding.
[[nodiscard]] bool inside_convex(const std::vector<Vec2>& polygon, Vec2 point) {
    bool positive = false;
    bool negative = false;
    for (usize index = 0; index < polygon.size(); ++index) {
        const Vec2 a = polygon[index];
        const Vec2 b = polygon[(index + 1) % polygon.size()];
        const f32 side = ((b.x - a.x) * (point.y - a.y)) - ((b.y - a.y) * (point.x - a.x));
        positive = positive || side > 1e-3F;
        negative = negative || side < -1e-3F;
    }
    return !(positive && negative);
}

/// A projected polygon with its pixel bounds, so the per-pixel scan tests only the polygons whose
/// bounds hold the pixel centre (a debug build scans 16384 centres against every polygon).
struct ProjectedPolygon {
    std::vector<Vec2> corners;
    Vec2 low{};
    Vec2 high{};
};

[[nodiscard]] ProjectedPolygon bounded(std::vector<Vec2> corners) {
    ProjectedPolygon out{std::move(corners), {}, {}};
    if (out.corners.empty()) {
        return out;
    }
    out.low = out.corners.front();
    out.high = out.corners.front();
    for (const Vec2 corner : out.corners) {
        out.low = Vec2{std::min(out.low.x, corner.x), std::min(out.low.y, corner.y)};
        out.high = Vec2{std::max(out.high.x, corner.x), std::max(out.high.y, corner.y)};
    }
    return out;
}

/// Whether `point` is inside the polygon: its bounds first, then the convex test.
[[nodiscard]] bool inside_projected(const ProjectedPolygon& polygon, Vec2 point) {
    constexpr f32 kSlack = 1e-3F;
    if (point.x < polygon.low.x - kSlack || point.x > polygon.high.x + kSlack ||
        point.y < polygon.low.y - kSlack || point.y > polygon.high.y + kSlack) {
        return false;
    }
    return inside_convex(polygon.corners, point);
}

/// The pixel an overlay polygon of `area` leaves on a cleared canvas.
[[nodiscard]] std::array<u8, 3> area_pixel(nav::AreaType area) {
    const u32 colour = nav_area_colour(area);
    std::array<u8, 3> out{};
    for (u32 channel = 0; channel < 3; ++channel) {
        const f32 source = static_cast<f32>((colour >> (16U - (channel * 8U))) & 0xFFU);
        out[channel] = static_cast<u8>(std::lround(source * 0.45F));
    }
    return out;
}

}  // namespace

CY_TEST_CASE(
    "editor runtime: the frame overlay over the baked test map covers its polygons in their "
    "area colours") {
    Runtime runtime;
    runtime.sync(runtime.text);
    const Baked baked = runtime.bake();
    CY_REQUIRE(baked.decoded);
    // The editor records the bake with the polygon overlay on, as the Navigation panel does.
    const std::string shown = edited(runtime.text, "    field 19 0\n", "    field 19 1\n");
    runtime.sync(recorded(shown, baked.identity, baked.fingerprint));
    CyServiceSession navigation =
        runtime.composite.child_session(runtime.session, runtime.navigation);

    // The frame the runtime publishes, looking down on the map, through the call main.cpp makes.
    constexpr u32 kSide = 128;
    const NavOverlayView view = overhead_view(Vec3{8.0F, 20.0F, 8.0F}, kSide, kSide);
    std::vector<u8> pixels(static_cast<usize>(kSide) * kSide * 4, u8{0});
    const Canvas canvas{pixels.data(), kSide, kSide};
    std::vector<NavOverlayWorld> scratch;
    draw_editor_navigation(*runtime.driver, navigation, view, canvas, scratch);
    CY_REQUIRE_EQ(scratch.size(), usize{1});

    const nav::NavMesh* mesh = runtime.mesh();
    CY_REQUIRE(mesh != nullptr);
    if (mesh == nullptr) {
        return;
    }
    PolygonList listed;
    nav::draw_navigation_mesh(*mesh, nav::NavDebugFlags::Polygons, listed);
    std::vector<ProjectedPolygon> projected;
    for (const PolygonList::Entry& entry : listed.entries) {
        std::vector<Vec2> corners;
        for (const Vec3 corner : entry.corners) {
            Vec2 pixel;
            CY_REQUIRE(render::project_to_pixel(view.view, corner - view.eye, pixel));
            corners.push_back(pixel);
        }
        projected.push_back(bounded(std::move(corners)));
    }

    // Every pixel centre inside exactly one projected polygon carries that polygon's area colour;
    // every pixel centre outside all of them is untouched. Pixels on a shared edge may go either
    // way and are not counted.
    u32 inside = 0;
    u32 wrong_colour = 0;
    u32 outside_drawn = 0;
    for (u32 y = 0; y < kSide; ++y) {
        for (u32 x = 0; x < kSide; ++x) {
            const Vec2 centre{static_cast<f32>(x) + 0.5F, static_cast<f32>(y) + 0.5F};
            usize containing = 0;
            usize which = 0;
            for (usize index = 0; index < projected.size(); ++index) {
                if (inside_projected(projected[index], centre)) {
                    ++containing;
                    which = index;
                }
            }
            const u8* pixel = &pixels[((static_cast<usize>(y) * kSide) + x) * 4];
            const bool drawn = pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0;
            if (containing == 0) {
                outside_drawn += drawn ? 1U : 0U;
                continue;
            }
            if (containing != 1) {
                continue;
            }
            ++inside;
            const std::array<u8, 3> want = area_pixel(listed.entries[which].area);
            const bool same = pixel[0] == want[0] && pixel[1] == want[1] && pixel[2] == want[2];
            wrong_colour += same ? 0U : 1U;
        }
    }
    CY_CHECK_GT(inside, (kSide * kSide) / 8U);
    CY_CHECK_LE(wrong_colour, inside / 100U);
    CY_CHECK_LE(outside_drawn, inside / 100U);

    // The map's own areas, by where they are: open ground at the centre, mud (area 3) around
    // (2, 0, 2), and the pillar's carved footprint around (4, 0, 12).
    const auto colour_at = [&](Vec3 world) {
        Vec2 pixel;
        std::array<u8, 3> out{};
        if (render::project_to_pixel(view.view, world - view.eye, pixel) && pixel.x >= 0.0F &&
            pixel.y >= 0.0F && pixel.x < static_cast<f32>(kSide) &&
            pixel.y < static_cast<f32>(kSide)) {
            const usize at =
                ((static_cast<usize>(pixel.y) * kSide) + static_cast<usize>(pixel.x)) * 4;
            out = {pixels[at], pixels[at + 1], pixels[at + 2]};
        }
        return out;
    };
    // The colours are spelled out rather than taken from `nav_area_colour`, so a palette change
    // shows here: ground cyan (0x28B4E6), area 3 violet (0xB464F0), and the obstacle's carved-out
    // area 63
    // (`kAreaNull`) red (0xE03C3C).
    const auto at_alpha = [](u32 colour) {
        std::array<u8, 3> out{};
        for (u32 channel = 0; channel < 3; ++channel) {
            const f32 source = static_cast<f32>((colour >> (16U - (channel * 8U))) & 0xFFU);
            out[channel] = static_cast<u8>(std::lround(source * 0.45F));
        }
        return out;
    };
    CY_CHECK(colour_at(Vec3{8.0F, 0.0F, 8.0F}) == at_alpha(0x28B4E6U));
    CY_CHECK(colour_at(Vec3{2.0F, 0.0F, 2.0F}) == at_alpha(0xB464F0U));
    CY_CHECK(colour_at(Vec3{4.0F, 0.0F, 12.0F}) == at_alpha(0xE03C3CU));

    // With the overlay flag cleared in the document, the same call draws nothing.
    runtime.sync(recorded(runtime.text, baked.identity, baked.fingerprint));
    std::ranges::fill(pixels, u8{0});
    draw_editor_navigation(*runtime.driver, navigation, view, canvas, scratch);
    CY_CHECK(std::ranges::all_of(pixels, [](u8 value) { return value == 0; }));
}

CY_TEST_CASE(
    "editor runtime: a runtime request the service refuses at submit is kept and retried") {
    Runtime runtime;
    runtime.sync(runtime.text);
    const Baked baked = runtime.bake();
    CY_REQUIRE(baked.decoded);

    // The editor has a bake in flight and a second request already refused as busy, so the
    // service holds its one busy refusal and refuses the next submit outright.
    Payload bake;
    bake.u32_(kMapWorld).settings(runtime.settings());
    const CyServiceRequest pending{
        sizeof(CyServiceRequest), 1, 900, "navigation.bake", bake.bytes.data(), bake.bytes.size()};
    CY_REQUIRE_EQ(runtime.composite.submit(runtime.session, pending), CY_RESULT_OK);
    const CyServiceRequest refused{
        sizeof(CyServiceRequest), 1, 901, "navigation.bake", bake.bytes.data(), bake.bytes.size()};
    CY_REQUIRE_EQ(runtime.composite.submit(runtime.session, refused), CY_RESULT_OK);

    // The crate moves: the runtime queues an update, and its first submit is refused.
    const u32 before = runtime.driver->updates_completed();
    runtime.reload(edited(runtime.text, "    field 2 12 0.5 4\n", "    field 2 12 0.5 5\n"));
    CY_REQUIRE(runtime.driver->document_changed(runtime.source).has_value());
    runtime.driver->pump();
    CY_CHECK_FALSE(runtime.driver->idle());

    // Once the editor's requests end, the kept update runs. (The editor's bake ran on the moved
    // crate, so the update finds nothing left to rebuild; what matters is that it ran.)
    runtime.settle();
    CY_CHECK_EQ(runtime.driver->updates_completed(), before + 1);
    CY_CHECK_EQ(runtime.driver->last_update().world, kMapWorld);
}
