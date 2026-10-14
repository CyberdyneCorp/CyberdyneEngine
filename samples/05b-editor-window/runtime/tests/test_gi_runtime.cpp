// SPDX-License-Identifier: MIT
// The runtime host's GI seam over a `.cyworld` room. Issue #35, the tooling half.
//
// `tests/data/gi_test_room.cyworld` is what the editor's `gi.*` commands write: a floor, a crate
// marked SlowDynamic, a 30 cm lamp drawn with a glowing cooked material, a cart with a rigid body,
// a sun, the world's GI settings (high quality, a 2.5 ms budget), a GI volume over the crate that
// halves its indirect diffuse, sized by its transform's scale, and a rule that keeps the lamp
// glass in the cache. The suite reads it as the runtime does and checks that every authored word
// reached the engine's GI, that `illumination.report` answers over the composite binding the
// runtime installs, that every GI view draws at the frame's size, and that the volumes' boxes are
// drawn where the volume is. No device: the frame's view is built directly.

#include <cy/core/assets/cooked.h>
#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/editor/composite_service.h>
#include <cy/editor/illumination_service.h>
#include <cy/import/model.h>
#include <cy/rendering/gi/debug_views.h>
#include <cy/scene/serialization/authoring_schema.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/servers/render/picking.h>
#include <cy/test/test.h>
#include <cy_reflect_generated_scene.h>

#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

#include "gi_runtime.h"
#include "overlay.h"

using namespace cy;
using namespace cy::sample::editor_window;
namespace gi = cy::rendering::gi;
namespace ser = cy::scene::serialization;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

constexpr const char* kWorldPath = "gi_test_room.cyworld";

[[nodiscard]] std::string read_text(const std::string& path) {
    Array<u8> bytes(allocator());
    CY_REQUIRE(assets::fs::read_whole(path.c_str(), bytes).has_value());
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// The room's project: its world and primitives copied into a temporary directory, and the lamp's
/// glass cooked into `.cy/cooked/` as the importer would cook a glowing glTF material.
struct Project {
    std::filesystem::path root;

    Project() {
        static u32 counter = 0;
        root = std::filesystem::temp_directory_path() /
               ("cy-gi-runtime-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(root / ".cy" / "cooked");
        for (const char* name : {"gi_box.cyprim", "gi_floor.cyprim", kWorldPath}) {
            std::filesystem::copy_file(std::filesystem::path(CY_GI_TEST_DATA) / name, root / name);
        }
        import::StandardMaterial glass;
        glass.base_colour[0] = glass.base_colour[1] = glass.base_colour[2] = 0.9F;
        glass.metallic = 0.0F;
        glass.roughness = 0.4F;
        glass.emissive[0] = 60.0F;
        glass.emissive[1] = 55.0F;
        glass.emissive[2] = 50.0F;
        Array<u8> payload(allocator());
        CY_REQUIRE(import::write_cooked_material(glass, payload).has_value());
        Array<u8> cooked(allocator());
        CY_REQUIRE(assets::write_cooked_asset(assets::AssetKind::Material, assets::VariantKey{},
                                              payload.span(), cooked)
                       .has_value());
        const std::string path = (root / ".cy/cooked/lampglass.cyasset").string();
        CY_REQUIRE(
            assets::fs::write_atomic(path.c_str(), cooked.data(), cooked.size()).has_value());
    }
    ~Project() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;
};

struct Runtime {
    Project project;
    reflect::TypeRegistry registry;
    ser::AuthoringSchema schema{allocator()};
    ser::World world{allocator()};
    std::string text = read_text((project.root / kWorldPath).string());
    AuthoredIllumination source{allocator(), project.root.string()};

    Runtime() {
        CY_REQUIRE(reflect::register_scene_types(registry).has_value());
        CY_REQUIRE(ser::build_authoring_schema(registry, schema).has_value());
        reload(text);
        source.bind(&world);
    }

    void reload(const std::string& next) {
        ser::World replacement(allocator());
        CY_REQUIRE(ser::read_world(next, kWorldPath, replacement).has_value());
        CY_REQUIRE(ser::resolve_against(replacement, schema).has_value());
        world = std::move(replacement);
        source.world_changed();
    }

    [[nodiscard]] u64 identity(std::string_view name) const {
        for (const ser::WorldNode& node : world.nodes()) {
            if (node.live && world.text(node.name) == name) {
                return node.identity;
            }
        }
        CY_REQUIRE_MESSAGE(false, "no node named " << name);
        return 0;
    }
};

/// A view from above and in front of the room, built as the runtime builds a frame's.
[[nodiscard]] render::View front_view(u32 width, u32 height) noexcept {
    render::View view;
    view.desc.purpose = render::ViewPurpose::EditorViewport;
    view.desc.viewport = render::ViewportRect{0, 0, width, height};
    view.desc.projection.kind = render::ProjectionKind::Perspective;
    view.desc.projection.fov_y_radians = 0.9F;
    view.desc.projection.near_plane = 0.1F;
    view.desc.projection.far_plane = 0.0F;
    const Vec3 forward = normalize(Vec3{0.0F, -0.45F, -1.0F});
    const Vec3 right = normalize(cross(forward, Vec3{0.0F, 1.0F, 0.0F}));
    const Vec3 up = cross(right, forward);
    view.desc.camera.rotation = Quat::from_basis(right, up, -forward);
    view.refresh();
    return view;
}

const Vec3 kEye{0.0F, 4.0F, 7.0F};

/// A report's entry points into the report, so a temporary one is refused at compile time.
const editor::IlluminationEmitterEntry* emitter(const editor::IlluminationReport&& report,
                                                u64 node) = delete;

[[nodiscard]] const editor::IlluminationEmitterEntry* emitter(const editor::IlluminationReport& r,
                                                              u64 node) noexcept {
    for (const editor::IlluminationEmitterEntry& entry : r.emitters) {
        if (entry.node == node) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace

CY_TEST_CASE("every authored GI word in the world reaches the engine's GI") {
    Runtime runtime;
    editor::IlluminationPreview* preview = runtime.source.preview();
    CY_REQUIRE(preview != nullptr);
    CY_CHECK_EQ(runtime.source.mesh_failures(), 0U);
    const editor::IlluminationScene& scene = runtime.source.scene();
    CY_CHECK_EQ(scene.objects.size(), 4U);
    CY_CHECK_EQ(scene.lights.size(), 1U);
    CY_CHECK(scene.lights[0].directional);
    CY_CHECK_NEAR(scene.lights[0].direction.y, -0.7071F, 1.0e-3F);

    // The world's settings.
    const gi::GiRuntimeSettings settings = preview->system().runtime_settings();
    CY_CHECK_EQ(settings.quality, gi::GiQuality::High);
    CY_CHECK_EQ(settings.budget_ms, 2.5F);

    // The volume: centred on its translation, as large as its scale, halving the diffuse there.
    CY_REQUIRE_EQ(preview->system().volumes().size(), 1U);
    const gi::GiVolume& volume = preview->system().volumes().volumes()[0];
    CY_CHECK_NEAR(volume.bounds.min.x, 0.5F, 1.0e-5F);
    CY_CHECK_NEAR(volume.bounds.max.y, 1.5F, 1.0e-5F);
    CY_CHECK_EQ(volume.priority, 1);
    CY_CHECK_EQ(preview->system().region_settings(Vec3{1.5F, 0.5F, 0.0F}).diffuse_intensity, 0.5F);
    CY_CHECK_EQ(preview->system().region_settings(Vec3{-3.0F, 0.5F, 0.0F}).diffuse_intensity, 1.0F);

    // The classes: the crate as authored, the cart derived from its rigid body.
    const editor::IlluminationReport report = preview->report(nullptr);
    for (const editor::IlluminationObjectEntry& object : report.objects) {
        if (object.node == runtime.identity("Crate")) {
            CY_CHECK_EQ(object.klass, gi::IlluminationClass::SlowDynamic);
            CY_CHECK(object.overridden);
        }
        if (object.node == runtime.identity("Cart")) {
            CY_CHECK_EQ(object.klass, gi::IlluminationClass::FastDynamic);
            CY_CHECK_FALSE(object.overridden);
        }
    }

    // The lamp: its cooked glass emits, and the authored rule keeps it in the cache.
    const editor::IlluminationEmitterEntry* lamp = emitter(report, runtime.identity("Lamp"));
    CY_REQUIRE(lamp != nullptr);
    CY_CHECK_EQ(lamp->material, "lampglass");
    CY_CHECK_EQ(lamp->override_mode, gi::EmitterOverride::CacheOnly);
    CY_CHECK_EQ(lamp->classification, gi::EmitterClass::CacheOnly);
    CY_CHECK_GT(lamp->power, 1.0F);
}

CY_TEST_CASE("a document change is applied in place, and a moved mesh rebuilds the GI") {
    Runtime runtime;
    CY_REQUIRE(runtime.source.preview() != nullptr);
    editor::IlluminationPreview& preview = *runtime.source.preview();
    CY_CHECK_EQ(preview.rebuilds(), 1U);

    // The rule removed: the lamp is measured again, and promoted.
    std::string text = runtime.text;
    const std::string rule = "    field 61 \"cache-only\"\n";
    text.replace(text.find(rule), rule.size(), "    field 61 \"automatic\"\n");
    runtime.reload(text);
    CY_REQUIRE(runtime.source.preview() != nullptr);
    CY_CHECK_EQ(preview.rebuilds(), 1U);
    // The report held, not a temporary: the entry points into it. (Reading it through a
    // temporary's pointer read freed memory, which AppleClang's build saw as a cache-only lamp.)
    const editor::IlluminationReport report = preview.report(nullptr);
    const editor::IlluminationEmitterEntry* lamp = emitter(report, runtime.identity("Lamp"));
    CY_REQUIRE(lamp != nullptr);
    CY_CHECK_EQ(lamp->override_mode, gi::EmitterOverride::Automatic);
    CY_CHECK_EQ(lamp->classification, gi::EmitterClass::PromotedLight);

    // The crate moved: a new geometry. (Before the tier changes: a rebuild converges on the
    // world's tier, and on the software one it costs this case twice as much.)
    const std::string crate = "    field 2 1.5 0.5 0\n    field 3 1 1 1\n";
    text.replace(text.find(crate), crate.size(), "    field 2 2.5 0.5 0\n    field 3 1 1 1\n");
    runtime.reload(text);
    CY_REQUIRE(runtime.source.preview() != nullptr);
    CY_CHECK_EQ(preview.rebuilds(), 2U);

    // Hardware ray tracing switched off in the world's settings: the software tier answers.
    const std::string hardware = "    field 23 true\n";
    text.replace(text.find(hardware), hardware.size(), "    field 23 false\n");
    runtime.reload(text);
    CY_REQUIRE(runtime.source.preview() != nullptr);
    CY_CHECK_EQ(preview.report(nullptr).world_tier, gi::RadianceSource::SoftwareTrace);
    CY_CHECK_EQ(preview.rebuilds(), 2U);
}

CY_TEST_CASE("illumination.report answers over the runtime's composite binding") {
    Runtime runtime;
    editor::IlluminationService service(allocator(), &runtime.source);
    editor::CompositeEditorService composite(allocator());
    CY_REQUIRE(composite.route("illumination.", service).has_value());
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(composite.open(&session), CY_RESULT_OK);
    constexpr u32 kWidth = 320;
    constexpr u32 kHeight = 180;
    runtime.source.record_frame(11, front_view(kWidth, kHeight), kEye);

    // A pick at the frame's centre lands on the floor, and the report explains it.
    Array<u8> payload(allocator());
    const auto put32 = [&payload](u32 value) {
        for (u32 byte = 0; byte < 4; ++byte) {
            CY_REQUIRE(payload.push_back(static_cast<u8>(value >> (byte * 8U))).has_value());
        }
    };
    put32(1);
    CY_REQUIRE(payload.push_back(1).has_value());
    put32(0);
    put32(11);
    put32(0);
    const f32 centre[2] = {160.0F, 120.0F};
    for (const f32 value : centre) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        put32(bits);
    }
    CyServiceRequest request{};
    request.struct_size = sizeof(request);
    request.request_id = 5;
    request.schema_version = 1;
    request.operation = "illumination.report";
    request.payload = payload.data();
    request.payload_size = payload.size();
    CY_REQUIRE_EQ(composite.submit(session, request), CY_RESULT_OK);
    CyServiceEvent event{};
    bool has = false;
    for (u32 attempt = 0; attempt < 8 && !has; ++attempt) {
        CY_REQUIRE_EQ(composite.poll(session, event, has), CY_RESULT_OK);
    }
    CY_REQUIRE(has);
    CY_CHECK_EQ(event.kind, static_cast<u32>(CY_SERVICE_EVENT_COMPLETED));
    const Ray ray = runtime.source.pick_ray(0, 11, 160.0F, 120.0F).value();
    Array<u8> expected(allocator());
    CY_REQUIRE(editor::encode_illumination_report(runtime.source.preview()->report(&ray), expected)
                   .has_value());
    CY_REQUIRE_EQ(event.payload_size, expected.size());
    CY_CHECK(std::memcmp(event.payload, expected.data(), expected.size()) == 0);
    CY_CHECK(runtime.source.preview()->report(&ray).explained);
    composite.close(session);
}

CY_TEST_CASE("every GI view of the viewport is drawn at the frame's size, over the room") {
    Runtime runtime;
    constexpr u32 kWidth = 160;
    constexpr u32 kHeight = 96;
    const render::View view = front_view(kWidth, kHeight);
    for (u32 index = 0; index < render::kDebugViewModeCount; ++index) {
        const auto mode = static_cast<render::DebugViewMode>(index);
        const bool gi_view = gi::gi_debug_view_from_name(render::debug_view_mode_name(mode)) !=
                             gi::GiDebugView::Count;
        Array<u32> texels(allocator());
        const Status drawn =
            runtime.source.render_view(mode, view, kEye, kWidth, kHeight, 4, texels);
        CY_TEST_INFO(render::debug_view_mode_name(mode));
        CY_CHECK_EQ(drawn.has_value(), gi_view);
        if (!gi_view) {
            continue;
        }
        CY_REQUIRE_EQ(texels.size(), usize{kWidth} * kHeight);
        u32 opaque = 0;
        for (const u32 texel : texels) {
            opaque += (texel >> 24U) != 0U ? 1U : 0U;
        }
        CY_CHECK_GT(opaque, 0U);
        // Drawn at a quarter: each texel covers a four-pixel square.
        CY_CHECK_EQ(texels[(40U * kWidth) + 41U], texels[(41U * kWidth) + 40U]);
    }
}

CY_TEST_CASE("the GI volume's box is drawn where the document places it") {
    Runtime runtime;
    std::vector<GiVolumeBox> boxes;
    CY_REQUIRE(runtime.source.volumes(boxes).has_value());
    CY_REQUIRE_EQ(boxes.size(), 1U);
    CY_CHECK_EQ(boxes[0].node, runtime.identity("Crate Volume"));
    CY_CHECK_NEAR(boxes[0].bounds.max.z, 1.0F, 1.0e-5F);
    constexpr u32 kWidth = 320;
    constexpr u32 kHeight = 180;
    std::vector<u8> pixels(usize{kWidth} * kHeight * 4U, 0U);
    const Canvas canvas{pixels.data(), kWidth, kHeight};
    const render::View view = front_view(kWidth, kHeight);
    draw_gi_volumes(canvas, view, kEye, {boxes.data(), boxes.size()});
    u32 drawn = 0;
    u32 left_of_centre = 0;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            if (pixels[(((usize{y} * kWidth) + x) * 4U) + 3U] != 0U) {
                drawn += 1;
                left_of_centre += x < kWidth / 2U ? 1U : 0U;
            }
        }
    }
    // The volume is right of the camera's line of sight: its edges are, too.
    CY_CHECK_GT(drawn, 50U);
    CY_CHECK_LT(left_of_centre * 4U, drawn);
    // Every corner the box has projects into the frame through the frame's own projection.
    Vec2 corner;
    CY_CHECK(render::project_to_pixel(view, boxes[0].bounds.max - kEye, corner));
}
