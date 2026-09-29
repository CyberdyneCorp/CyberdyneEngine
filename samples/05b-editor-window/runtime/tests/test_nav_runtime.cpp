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
#include <cy/navigation/tile_identity.h>
#include <cy/scene/serialization/authoring_schema.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/test/test.h>
#include <cy_reflect_generated_scene.h>

#include <unistd.h>

#include <cinttypes>
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
        if (coord == std::pair<i32, i32>{0, 0}) {
            CY_CHECK_NE(edited_digests.at(coord), digest);
        } else {
            CY_CHECK_EQ(edited_digests.at(coord), digest);
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
