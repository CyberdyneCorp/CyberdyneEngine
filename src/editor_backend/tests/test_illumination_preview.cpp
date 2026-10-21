// SPDX-License-Identifier: MIT
// The editor's global illumination: the authored world lit by CyberGI, the in-place application of
// the `gi.*` commands' document state, the debug views and the diagnostics report, and
// `illumination.report` over the service envelope. Issue #35, the tooling half.

#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/editor/illumination_preview.h>
#include <cy/editor/illumination_service.h>
#include <cy/rendering/gi/debug_views.h>
#include <cy/test/test.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../src/service_wire.h"
#include "illumination_world.h"

using namespace cy;
using cy::editor::IlluminationEmission;
using cy::editor::IlluminationPreview;
using cy::editor::IlluminationReport;
using cy::editor::IlluminationViewCamera;
namespace gi = cy::rendering::gi;

namespace {

using illumination_test::allocator;
using illumination_test::camera;
using illumination_test::kCart;
using illumination_test::kCrate;
using illumination_test::kLamp;
using illumination_test::World;

/// A report's entry points into the report, so a temporary one is refused at compile time.
const editor::IlluminationEmitterEntry* emitter(const IlluminationReport&& report,
                                                u64 node) = delete;

[[nodiscard]] const editor::IlluminationEmitterEntry* emitter(const IlluminationReport& report,
                                                              u64 node) noexcept {
    for (const editor::IlluminationEmitterEntry& entry : report.emitters) {
        if (entry.node == node) {
            return &entry;
        }
    }
    return nullptr;
}

[[nodiscard]] gi::IlluminationClass class_of(const IlluminationReport& report, u64 node) noexcept {
    for (const editor::IlluminationObjectEntry& entry : report.objects) {
        if (entry.node == node) {
            return entry.klass;
        }
    }
    return gi::IlluminationClass::Count;
}

/// Leaves the test world with only its crate, under the same sun. (In place: a world's meshes
/// point into its own cube.)
void keep_crate_alone(World& world) {
    const cy::editor::IlluminationObject crate = world.objects[1];
    CY_REQUIRE_EQ(crate.node, kCrate);
    world.objects.assign(1, crate);
}

}  // namespace

CY_TEST_CASE("the authored world is lit by the GI, and its volumes and settings apply in place") {
    World world;
    IlluminationPreview preview(allocator());
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    CY_CHECK(preview.ready());
    CY_CHECK_EQ(preview.rebuilds(), 1U);
    IlluminationReport report = preview.report(nullptr);
    CY_CHECK_GT(report.cards, 0U);
    CY_CHECK_GT(report.probes, 0U);
    CY_CHECK_GT(report.convergence, 0.5F);
    CY_CHECK_EQ(report.objects.size(), 4U);

    // The floor beside the crate takes its red bounce.
    const gi::ResolveResult beside = preview.system().indirect_diffuse(
        Vec3{0.75F, 0.01F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}, gi::SurfaceProperties{});
    CY_CHECK_GT(beside.radiance.x, 0.0F);

    // Unchanged, and a volume placed: applied in place, no rebuild.
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    gi::GiVolume volume;
    volume.bounds = Aabb::from_center_extents(Vec3{1.5F, 0.5F, 0.0F}, Vec3{1.0F, 1.0F, 1.0F});
    volume.sets = gi::gi_volume_bit(gi::GiVolumeParameter::DiffuseIntensity);
    volume.diffuse_intensity = 0.5F;
    world.volumes.push_back(volume);
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    CY_CHECK_EQ(preview.rebuilds(), 1U);
    CY_CHECK_EQ(preview.applications(), 2U);
    CY_CHECK_EQ(preview.system().region_settings(Vec3{1.5F, 0.5F, 0.0F}).diffuse_intensity, 0.5F);
    world.settings.quality = gi::GiQuality::High;
    world.settings.hardware_ray_tracing = false;
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    CY_CHECK_EQ(preview.rebuilds(), 1U);
    CY_CHECK_EQ(preview.system().runtime_settings().quality, gi::GiQuality::High);
    CY_CHECK_EQ(preview.report(nullptr).world_tier, gi::RadianceSource::SoftwareTrace);
}

CY_TEST_CASE("only a world's geometry decides a rebuild, and its settings survive one") {
    // The crate alone: this case converges the GI twice, and the eight-metre floor's cards are
    // most of what a convergence costs (about four times the crate's under Debug).
    World world;
    keep_crate_alone(world);
    IlluminationPreview preview(allocator());
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    CY_CHECK_EQ(preview.rebuilds(), 1U);
    world.settings.budget_ms = 1.5F;
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    CY_CHECK_EQ(preview.rebuilds(), 1U);
    CY_CHECK_EQ(preview.applications(), 1U);

    // A moved crate is a new geometry, rebuilt at the world's settings.
    world.objects[0].transform = Mat4::from_translation(Vec3{2.0F, 0.5F, 0.0F});
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    CY_CHECK_EQ(preview.rebuilds(), 2U);
    CY_CHECK_EQ(preview.system().runtime_settings().budget_ms, 1.5F);
    CY_CHECK_EQ(preview.report(nullptr).world_tier, gi::RadianceSource::HardwareTrace);
    CY_CHECK_EQ(preview.report(nullptr).objects.size(), 1U);
}

CY_TEST_CASE("the sky is applied in place, as the settings are: nothing is rebuilt") {
    // A viewport lit by the GI sets the sky to its own flat ambient, which moves with the frame's
    // sun; before, the sky was part of the geometry's digest and each move rebuilt and converged
    // the whole GI again.
    World world;
    keep_crate_alone(world);
    IlluminationPreview preview(allocator());
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    CY_CHECK_EQ(preview.rebuilds(), 1U);
    world.sky.zenith = Vec3{12000.0F, 12000.0F, 12000.0F};
    world.sky.horizon = world.sky.zenith;
    world.sky.ground = world.sky.zenith;
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    CY_CHECK_EQ(preview.rebuilds(), 1U);
    CY_CHECK_EQ(preview.applications(), 1U);
    CY_CHECK_EQ(preview.system().sky_term().zenith.x, 12000.0F);
    CY_CHECK_EQ(preview.system().sky_term().ground.y, 12000.0F);
}

CY_TEST_CASE("an author's emitter rule switches a lamp off in place, and the report says so") {
    World world;
    IlluminationPreview preview(allocator());
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    IlluminationReport report = preview.report(nullptr);
    const editor::IlluminationEmitterEntry* lamp = emitter(report, kLamp);
    CY_REQUIRE(lamp != nullptr);
    CY_CHECK_EQ(lamp->material, "materials/lamp-glass");
    CY_CHECK_EQ(lamp->classification, gi::EmitterClass::PromotedLight);
    CY_CHECK_EQ(lamp->override_mode, gi::EmitterOverride::Automatic);
    CY_CHECK_GT(lamp->power, 1.0F);
    CY_CHECK_GT(preview.system().emitters().lights().size(), 0U);

    world.emission.push_back(IlluminationEmission{2, gi::EmitterOverride::Off});
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    CY_CHECK_EQ(preview.rebuilds(), 1U);
    report = preview.report(nullptr);
    lamp = emitter(report, kLamp);
    CY_REQUIRE(lamp != nullptr);
    CY_CHECK_EQ(lamp->classification, gi::EmitterClass::Disabled);
    CY_CHECK_EQ(lamp->override_mode, gi::EmitterOverride::Off);
    CY_CHECK(preview.system().emitters().lights().empty());
    CY_CHECK_EQ(report.emissive.disabled_emitters, 1U);

    world.emission[0].mode = gi::EmitterOverride::CacheOnly;
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    report = preview.report(nullptr);
    CY_CHECK_EQ(emitter(report, kLamp)->classification, gi::EmitterClass::CacheOnly);
    CY_CHECK(preview.system().emitters().lights().empty());

    // An emitter rule for a material the scene does not have is refused.
    world.emission.push_back(IlluminationEmission{99, gi::EmitterOverride::Off});
    CY_CHECK_FALSE(preview.sync(world.scene()).has_value());
}

CY_TEST_CASE("an object's class set in the document reaches the GI in place") {
    World world;
    IlluminationPreview preview(allocator());
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    // Derived: a 0.8 m movable cart is FastDynamic; the static crate is Static.
    IlluminationReport report = preview.report(nullptr);
    CY_CHECK_EQ(class_of(report, kCart), gi::IlluminationClass::FastDynamic);
    CY_CHECK_EQ(class_of(report, kCrate), gi::IlluminationClass::Static);

    world.objects[3].has_class = true;
    world.objects[3].klass = gi::IlluminationClass::Static;
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    CY_CHECK_EQ(preview.rebuilds(), 1U);
    report = preview.report(nullptr);
    CY_CHECK_EQ(class_of(report, kCart), gi::IlluminationClass::Static);

    world.objects[3].has_class = false;
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    CY_CHECK_EQ(class_of(preview.report(nullptr), kCart), gi::IlluminationClass::FastDynamic);
}

CY_TEST_CASE("every GI debug view draws over the authored world, and nothing over the sky") {
    World world;
    IlluminationPreview preview(allocator());
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    const IlluminationViewCamera view = camera(48, 32);
    Array<u32> previous(allocator());
    u32 distinct = 0;
    for (u32 index = 0; index < gi::kGiDebugViewCount; ++index) {
        const auto which = static_cast<gi::GiDebugView>(index);
        CY_TEST_INFO(gi::gi_debug_view_name(which));
        Array<u32> texels(allocator());
        CY_REQUIRE(preview.render_view(which, view, texels).has_value());
        CY_REQUIRE_EQ(texels.size(), 48U * 32U);
        u32 opaque = 0;
        for (const u32 texel : texels) {
            opaque += (texel >> 24U) != 0U ? 1U : 0U;
        }
        CY_CHECK_GT(opaque, 0U);
        // The top row looks over the floor's far edge at the sky.
        CY_CHECK_EQ(texels[24] >> 24U, 0U);
        if (!previous.empty() &&
            std::memcmp(previous.data(), texels.data(), texels.size() * sizeof(u32)) != 0) {
            distinct += 1;
        }
        previous = std::move(texels);
    }
    CY_CHECK_GT(distinct, 10U);
}

CY_TEST_CASE("the report explains a picked point with the GI's own explain") {
    World world;
    IlluminationPreview preview(allocator());
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    const Ray down{Vec3{0.75F, 5.0F, 0.0F}, Vec3{0.0F, -1.0F, 0.0F}};
    const IlluminationReport report = preview.report(&down);
    CY_REQUIRE(report.explained);
    CY_CHECK_NEAR(report.point.y, 0.0F, 1.0e-4F);
    CY_CHECK_NEAR(report.normal.y, 1.0F, 1.0e-4F);
    const gi::ResolveResult direct =
        preview.system().indirect_diffuse(report.point, report.normal, gi::SurfaceProperties{});
    CY_CHECK_EQ(report.query.diffuse.radiance.x, direct.radiance.x);
    CY_CHECK_EQ(report.query.diffuse.radiance.y, direct.radiance.y);
    CY_CHECK_GT(report.query.source_count, 0U);
    // Into the sky: nothing to explain.
    const Ray up{Vec3{0.0F, 5.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}};
    CY_CHECK_FALSE(preview.report(&up).explained);
}

// --- The service ---------------------------------------------------------------------------------

namespace {

class FakeRuntime final : public editor::IlluminationRuntime {
public:
    IlluminationPreview* lit = nullptr;
    editor::IlluminationPreview* preview() noexcept override { return lit; }
    Expected<Ray, Error> pick_ray(u32 viewport, u64 frame, f32 x, f32 y) noexcept override {
        if (viewport != 0 || frame != 7) {
            return fail(ErrorCode::NotFound, "no such frame");
        }
        (void)x;
        (void)y;
        return Ray{Vec3{0.75F, 5.0F, 0.0F}, Vec3{0.0F, -1.0F, 0.0F}};
    }
};

struct Answer {
    u32 kind = 0;
    std::vector<u8> payload;
};

Answer ask(editor::IlluminationService& service, CyServiceSession session, const char* operation,
           const Array<u8>& payload, u64 request = 1) {
    CyServiceRequest submitted{};
    submitted.struct_size = sizeof(submitted);
    submitted.request_id = request;
    submitted.schema_version = 1;
    submitted.operation = operation;
    submitted.payload = payload.data();
    submitted.payload_size = payload.size();
    CY_REQUIRE_EQ(service.submit(session, submitted), CY_RESULT_OK);
    CyServiceEvent event{};
    bool has = false;
    CY_REQUIRE_EQ(service.poll(session, event, has), CY_RESULT_OK);
    CY_REQUIRE(has);
    CY_CHECK_EQ(event.request_id, request);
    Answer answer;
    answer.kind = event.kind;
    answer.payload.assign(event.payload, event.payload + event.payload_size);
    return answer;
}

[[nodiscard]] Array<u8> report_request(bool pick, u64 frame = 7) {
    Array<u8> out(allocator());
    editor::wire::Writer writer(out);
    writer.u32v(1).u8v(pick ? 1U : 0U);
    if (pick) {
        writer.u32v(0).u64v(frame).f32v(10.0F).f32v(12.0F);
    }
    CY_REQUIRE(writer.status().has_value());
    return out;
}

}  // namespace

CY_TEST_CASE("illumination.report answers the GI's report, explaining a picked pixel") {
    World world;
    IlluminationPreview preview(allocator());
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    FakeRuntime runtime;
    runtime.lit = &preview;
    editor::IlluminationService service(allocator(), &runtime);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(service.open(&session), CY_RESULT_OK);

    const Answer answer = ask(service, session, "illumination.report", report_request(true));
    CY_REQUIRE_EQ(answer.kind, static_cast<u32>(CY_SERVICE_EVENT_COMPLETED));
    // The reply is the encoding of the preview's own report, byte for byte.
    const Ray down{Vec3{0.75F, 5.0F, 0.0F}, Vec3{0.0F, -1.0F, 0.0F}};
    Array<u8> expected(allocator());
    CY_REQUIRE(editor::encode_illumination_report(preview.report(&down), expected).has_value());
    CY_REQUIRE_EQ(answer.payload.size(), expected.size());
    CY_CHECK(std::memcmp(answer.payload.data(), expected.data(), expected.size()) == 0);
    // Its head: format, then the settings' mode by name.
    editor::wire::Reader reader({answer.payload.data(), answer.payload.size()});
    CY_CHECK_EQ(reader.read_u32(), 1U);
    CY_CHECK_EQ(reader.read_text(), std::string_view(gi::gi_mode_name(gi::GiMode::Dynamic)));

    // A frame the host does not know, a malformed request and an unknown operation each fail with
    // a code.
    CY_CHECK_EQ(ask(service, session, "illumination.report", report_request(true, 9), 2).kind,
                static_cast<u32>(CY_SERVICE_EVENT_FAILED));
    Array<u8> malformed(allocator());
    CY_REQUIRE(malformed.push_back(1).has_value());
    CY_CHECK_EQ(ask(service, session, "illumination.report", malformed, 3).kind,
                static_cast<u32>(CY_SERVICE_EVENT_FAILED));
    CY_CHECK_EQ(ask(service, session, "illumination.nothing", report_request(false), 4).kind,
                static_cast<u32>(CY_SERVICE_EVENT_FAILED));
    const Answer listed = ask(service, session, "capabilities.get", Array<u8>(allocator()), 5);
    CY_CHECK_EQ(listed.kind, static_cast<u32>(CY_SERVICE_EVENT_COMPLETED));
    const std::string text(listed.payload.begin(), listed.payload.end());
    CY_CHECK(text.find("illumination.report") != std::string::npos);
    service.close(session);
}

CY_TEST_CASE("illumination.report with no world is unavailable, naming why") {
    FakeRuntime runtime;
    editor::IlluminationService service(allocator(), &runtime);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(service.open(&session), CY_RESULT_OK);
    const Answer answer = ask(service, session, "illumination.report", report_request(false));
    CY_CHECK_EQ(answer.kind, static_cast<u32>(CY_SERVICE_EVENT_FAILED));
    const std::string text(answer.payload.begin(), answer.payload.end());
    CY_CHECK(text.find("illumination.report.unavailable") != std::string::npos);
    service.close(session);
}

// --- The wire the editor decodes -----------------------------------------------------------------

CY_TEST_CASE("the report's wire is the committed fixture the editor decodes") {
    // A report with every part present and fixed numbers, encoded by the engine. The editor's
    // decoder (`cy-editor-services::illumination_requests`) reads the same file, so the two sides
    // cannot drift apart. `CY_UPDATE_ILLUMINATION_WIRE=1` rewrites it.
    IlluminationReport report;
    report.settings.quality = gi::GiQuality::High;
    report.settings.budget_ms = 2.5F;
    report.world_tier = gi::RadianceSource::HardwareTrace;
    report.convergence = 0.75F;
    report.cards = 120;
    report.probes = 64;
    report.volumes = 1;
    report.lights = 2;
    report.rebuilds = 1;
    report.applications = 3;
    report.profile.frame = 40;
    report.profile.allocation_ms = 2.5F;
    report.profile.measured_ms = 1.75F;
    report.profile.stage_ms[static_cast<u32>(gi::GiStage::ProbeUpdate)] = 1.5;
    report.profile.probes_updated = 64;
    report.profile.surface_pages_updated = 120;
    report.profile.software_rays = 900;
    report.profile.hardware_rays = 1200;
    report.profile.surface_cache_hit_rate = 0.5F;
    report.profile.radiance_cache_hit_rate = 0.25F;
    report.emissive.emitters = 1;
    report.emissive.total_power = 41.0F;
    report.emissive.promoted_power = 41.0F;
    report.emitters.push_back(editor::IlluminationEmitterEntry{
        kLamp, "materials/lamp-glass", gi::EmitterClass::PromotedLight,
        gi::EmitterOverride::Automatic, 16, 0.25F, 41.0F});
    report.objects.push_back(
        editor::IlluminationObjectEntry{kCart, gi::IlluminationClass::FastDynamic, false});
    report.explained = true;
    report.point = Vec3{0.75F, 0.0F, 0.0F};
    report.normal = Vec3{0.0F, 1.0F, 0.0F};
    report.query.cause = gi::DarknessCause::Lit;
    report.query.diffuse.radiance = Vec3{0.5F, 0.25F, 0.125F};
    report.query.diffuse.confidence = 0.875F;
    report.query.diffuse.dominant = gi::RadianceSource::RadianceCache;
    report.query.probes_contributing = 6;
    report.query.probes_in_reach = 8;
    report.query.world_tier = gi::RadianceSource::HardwareTrace;
    report.query.convergence = 0.9375F;
    report.query.source_count = 1;
    report.query.sources[0].source = gi::RadianceSource::RadianceCache;
    report.query.sources[0].radiance = Vec3{0.5F, 0.25F, 0.125F};
    report.query.sources[0].confidence = 0.875F;
    report.query.sources[0].weight_share = 1.0F;
    Array<u8> bytes(allocator());
    CY_REQUIRE(editor::encode_illumination_report(report, bytes).has_value());
    const std::filesystem::path path =
        std::filesystem::path(CY_ILLUMINATION_WIRE_DIR) / "illumination_report_v1.wire";
    const char* update = std::getenv("CY_UPDATE_ILLUMINATION_WIRE");
    if (update != nullptr && std::string_view(update) == "1") {
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    std::ifstream input(path, std::ios::binary);
    CY_REQUIRE(input.good());
    std::ostringstream committed;
    committed << input.rdbuf();
    const std::string expected = committed.str();
    CY_REQUIRE_EQ(expected.size(), bytes.size());
    CY_CHECK(std::memcmp(expected.data(), bytes.data(), bytes.size()) == 0);
}
