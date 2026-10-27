// SPDX-License-Identifier: MIT
// The first-person weapon view on a device. `camera-system` — "One camera, several views": "WHEN a
// first-person camera renders the world and a separate weapon view THEN both SHALL be render views
// derived from one evaluated camera".
//
//   off is the frame before            the frame without a weapon view, and with one that selects
//   no
//                                      instance, against the reference a build from before the
//                                      weapon view wrote — byte for byte
//   it never clips into a wall          a weapon pushed into a wall covers exactly the texels it
//                                      covers with no wall; drawn as world geometry, the wall cuts
//                                      the barrel off
//   its own near plane                  held nearer than the world's near plane, it is still whole
//   depth composition                   the weapon in front of a world it reaches into, held to a
//                                      golden image, and every texel off the weapon untouched
//   lit as the world lights it          the weapon view's surfaces shaded exactly as the same
//                                      surfaces drawn as world geometry, a small light included
//   its field of view is its own        the world's lens changed, the weapon's texels the same
//   bytes its motion vectors                  carried by a turning camera it has none; raised to
//   the eye
//                                      its motion is its lens's, texel by texel
//   the shadow policy                   it receives the world's shadow by default and casts none;
//                                      each half switched on its own
//   at 1080p                            the frame's GPU time with the weapon view and without
//
// Vulkan with validation and synchronisation validation on. Without a GPU the suite SKIPS LOUDLY.

#include "golden.h"
#include "weapon_scene.h"

#include <cy/test/test.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace cy;
using namespace cy::weapon_test;

namespace {

// The scene's own extent, `pipeline_test::kWidth` by `kHeight`: 480 by 270.

[[nodiscard]] bool updating_references() noexcept {
    const char* value = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

/// Write a picture to the suite's artefact directory, for the change's review.
void save(const char* name, Span<const u32> texels, u32 width, u32 height) noexcept {
    render_test::Image image(allocator());
    if (!render_test::adopt(image, texels, width, height).has_value()) {
        return;
    }
    const char* directory = std::getenv("CY_TEST_ARTEFACT_DIR");
    if (directory == nullptr || *directory == '\0') {
        directory = CY_TEST_BINARY_DIR;
    }
    char path[1024];
    (void)std::snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (render_test::write_png(path, image).has_value()) {
        std::fprintf(stderr, "wrote %s (%ux%u)\n", path, width, height);
    }
}

/// Hold `pixels` to `references/<name>`, or write it under CY_RENDER_UPDATE_GOLDEN.
void hold_to_golden(const char* directory, const char* name, Span<const u32> pixels, u32 width,
                    u32 height, bool exact) {
    render_test::Image rendered(allocator());
    CY_REQUIRE(render_test::adopt(rendered, pixels, width, height).has_value());
    char path[1024];
    (void)std::snprintf(path, sizeof(path), "%s/references/%s", directory, name);
    if (updating_references() && !exact) {
        CY_CHECK(render_test::write_png(path, rendered).has_value());
        std::fprintf(stderr, "wrote %s — look at it, then commit it. This run FAILS on purpose.\n",
                     path);
        CY_TEST_FAIL_CHECK("references were regenerated; this mode never passes");
        return;
    }
    render_test::Image reference(allocator());
    CY_REQUIRE(render_test::read_png(path, reference).has_value());
    const render_test::Comparison compared = render_test::compare(reference, rendered);
    u32 identical = 0;
    for (usize texel = 0; compared.comparable && texel < rendered.texels.size(); ++texel) {
        identical += rendered.texels[texel] == reference.texels[texel] ? 1U : 0U;
    }
    std::fprintf(stderr,
                 "%s: %u of %zu texels byte-identical, %u over tolerance (%u off an edge), worst "
                 "delta %u at (%u, %u)\n",
                 name, identical, rendered.texels.size(), compared.differing,
                 compared.differing_off_edge, compared.max_channel_delta, compared.worst_x,
                 compared.worst_y);
    CY_REQUIRE(compared.comparable);
    // The golden rule, so a conformant driver other than the one that wrote the file passes; on
    // the machine that wrote it every texel is the same byte, which the line above reports.
    CY_CHECK_EQ(compared.differing_off_edge, 0U);
    CY_CHECK_LE(compared.differing, compared.edge_texels);
}

/// A scene and its run, built and released together.
class Bench {
public:
    explicit Bench(DeviceFixture& fixture, u32 width = kWidth, u32 height = kHeight)
        : scene_(allocator()), width_(width), height_(height) {
        run_.fixture = &fixture;
    }
    ~Bench() {
        if (built_) {
            release_scene(run_);
        }
    }
    Bench(const Bench&) = delete;
    Bench& operator=(const Bench&) = delete;

    [[nodiscard]] WeaponRun& run() noexcept { return run_; }
    [[nodiscard]] FrameScene& scene() noexcept { return scene_; }
    [[nodiscard]] bool build() {
        built_ = build_scene(run_, scene_, width_, height_);
        return built_;
    }
    /// One frame with the wall where the run says.
    [[nodiscard]] bool frame() {
        const Vec3 wall{run_.wall_x, 0.0F, -(run_.wall_distance + 0.1F)};
        if (!scene_.move_box(kWall, run_.wall ? wall : kParked).has_value()) {
            return false;
        }
        return render_frame(run_);
    }
    [[nodiscard]] bool mask(std::vector<u8>& out) {
        const Vec3 wall{run_.wall_x, 0.0F, -(run_.wall_distance + 0.1F)};
        if (!scene_.move_box(kWall, run_.wall ? wall : kParked).has_value()) {
            return false;
        }
        return weapon_mask(run_, out);
    }

private:
    WeaponRun run_;
    FrameScene scene_;
    u32 width_;
    u32 height_;
    bool built_ = false;
};

/// How many texels of `a` and `b` differ.
[[nodiscard]] u32 mask_difference(const std::vector<u8>& a, const std::vector<u8>& b) {
    u32 differing = 0;
    for (usize texel = 0; texel < a.size(); ++texel) {
        differing += a[texel] != b[texel] ? 1U : 0U;
    }
    return differing;
}

/// Every masked texel's HDR colour in `a` the same bytes as in `b`: how many are not.
[[nodiscard]] u32 colour_difference(const std::vector<u8>& mask, const std::vector<u16>& a,
                                    const std::vector<u16>& b, f32* worst = nullptr) {
    u32 differing = 0;
    f32 largest = 0.0F;
    for (usize texel = 0; texel < mask.size(); ++texel) {
        if (mask[texel] == 0) {
            continue;
        }
        bool same = true;
        for (u32 channel = 0; channel < 3U; ++channel) {
            const u16 left = a[(texel * 4U) + channel];
            const u16 right = b[(texel * 4U) + channel];
            if (left != right) {
                same = false;
                largest = std::max(largest, std::fabs(from_half(left) - from_half(right)) /
                                                std::max(from_half(right), 1e-3F));
            }
        }
        differing += same ? 0U : 1U;
    }
    if (worst != nullptr) {
        *worst = largest;
    }
    return differing;
}

}  // namespace

CY_TEST_CASE("off is the frame before: no weapon view, and one with nothing in it") {
    DeviceFixture fixture("render.weapon_view off");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // THE SCENE `render.pipeline` HAS ALWAYS RENDERED, with the weapon view's pipelines created:
    // `frame_scene_before_bloom.png` is that suite's capture from a build before bloom — and so
    // before the weapon view — copied unchanged, and the reference every frame-shader change in
    // the tree is held to.
    struct Off {
        bool weapon_view = false;
    } off;
    FrameSceneHooks hooks;
    hooks.user = &off;
    hooks.pipelines = [](PipelineSetup& setup, void*) noexcept { setup.viewmodel = true; };
    hooks.before_assemble = [](RenderGraph&, rendering::assembly::AssemblyView& view, FrameSinks&,
                               void* user) noexcept -> Status {
        if (static_cast<Off*>(user)->weapon_view) {
            // A weapon view of a layer no instance is on: culled, written, and drawing nothing.
            ViewmodelSettings settings;
            settings.layers = kWeaponLayer;
            view.viewmodel = assembly_view(settings, view.view, 480.0F / 270.0F);
        }
        return ok();
    };
    // TWO SCENES, EACH RENDERING ITS FIRST FRAME: a second frame of one scene would have a
    // temporal history the first did not, and the resolve would blend it.
    std::vector<u32> without;
    {
        FrameScene scene(allocator());
        scene.set_hooks(hooks);
        CY_REQUIRE(scene.build(fixture.device()).has_value());
        scene.set_read_back(true);
        AssemblyReport report;
        CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
        without.assign(scene.pixels().begin(), scene.pixels().end());
        CY_CHECK_EQ(report.viewmodel_draws, 0U);
        CY_CHECK_EQ(scene.recorded().viewmodel_draws, 0U);
    }
    hold_to_golden(CY_PIPELINE_TEST_DIR, "frame_scene_before_bloom.png",
                   Span<const u32>(without.data(), without.size()), kWidth, kHeight, true);

    off.weapon_view = true;
    FrameScene scene(allocator());
    scene.set_hooks(hooks);
    CY_REQUIRE(scene.build(fixture.device()).has_value());
    scene.set_read_back(true);
    AssemblyReport report;
    CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
    CY_CHECK_EQ(report.viewmodel_draws, 0U);
    u32 identical = 0;
    for (usize texel = 0; texel < without.size(); ++texel) {
        identical += scene.pixels()[texel] == without[texel] ? 1U : 0U;
    }
    std::fprintf(stderr, "an empty weapon view: %u of %zu texels byte-identical\n", identical,
                 without.size());
    CY_CHECK_EQ(identical, static_cast<u32>(without.size()));
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the weapon never clips into a wall it touches") {
    DeviceFixture fixture("render.weapon_view wall");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // ONE LENS FOR BOTH, the world's 0.9 radians: the weapon lands on the same texels whichever
    // way it is drawn, so the only difference between the two pictures is the wall.
    Bench bench(fixture);
    WeaponRun& run = bench.run();
    run.settings.vertical_fov_radians = 0.9F;
    run.lantern = true;
    CY_REQUIRE(bench.build());

    // Its silhouette in open air, and pushed into a wall 0.45 m out — the barrel reaches 0.85 m.
    run.wall = false;
    std::vector<u8> open;
    CY_REQUIRE(bench.mask(open));
    run.wall = true;
    std::vector<u8> walled;
    CY_REQUIRE(bench.mask(walled));
    const std::vector<u32> with_view(bench.scene().pixels().begin(), bench.scene().pixels().end());
    CY_CHECK_EQ(run.report.viewmodel_draws, kPartCount);
    CY_CHECK_EQ(bench.scene().recorded().viewmodel_draws, kPartCount * 2U);
    const u32 open_texels = count(open);
    const u32 lost = mask_difference(open, walled);
    std::fprintf(stderr, "with the weapon view: %u texels in open air, %u differ in the wall\n",
                 open_texels, lost);
    CY_CHECK_GT(open_texels, 3000U);
    CY_CHECK_EQ(lost, 0U);

    // THE CONTROL: the same weapon as world geometry. The wall hides the barrel and the front of
    // the receiver — the clipping the weapon view exists to prevent.
    run.weapon_view = false;
    run.wall = false;
    std::vector<u8> world_open;
    CY_REQUIRE(bench.mask(world_open));
    run.wall = true;
    std::vector<u8> world_walled;
    CY_REQUIRE(bench.mask(world_walled));
    const std::vector<u32> without_view(bench.scene().pixels().begin(),
                                        bench.scene().pixels().end());
    std::fprintf(stderr, "as world geometry: %u texels in open air, %u in the wall\n",
                 count(world_open), count(world_walled));
    CY_CHECK_EQ(count(world_open), open_texels);
    CY_CHECK_LT(count(world_walled) * 10U, count(world_open) * 8U);
    save("weapon_wall_with_view.png", Span<const u32>(with_view.data(), with_view.size()), kWidth,
         kHeight);
    save("weapon_wall_without_view.png", Span<const u32>(without_view.data(), without_view.size()),
         kWidth, kHeight);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("its own near plane: a weapon nearer than the world's near plane is still whole") {
    DeviceFixture fixture("render.weapon_view near");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Bench bench(fixture);
    WeaponRun& run = bench.run();
    run.settings.vertical_fov_radians = 0.9F;
    run.wall = false;
    // Pulled back to the eye and centred: the receiver's back end is 5 cm out, inside the world's
    // 10 cm near plane and in the middle of the picture.
    run.hold = Vec3{0.0F, -0.04F, -0.06F};
    CY_REQUIRE(bench.build());
    std::vector<u8> weapon;
    CY_REQUIRE(bench.mask(weapon));
    run.weapon_view = false;
    std::vector<u8> world;
    CY_REQUIRE(bench.mask(world));
    std::fprintf(stderr, "held at the eye: %u texels through the weapon view, %u as world\n",
                 count(weapon), count(world));
    CY_CHECK_GT(count(weapon), count(world) + 500U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("depth composition: the weapon in front of the world it reaches into, held to gold") {
    DeviceFixture fixture("render.weapon_view composition");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Bench bench(fixture);
    WeaponRun& run = bench.run();
    run.ring = true;
    run.lantern = true;
    // A pillar the barrel reaches into, the world's ring of boxes beyond it.
    run.wall_distance = 0.55F;
    run.wall_x = 0.3F;
    run.wall_half_width = 0.14F;
    run.settings.vertical_fov_radians = 0.95F;
    CY_REQUIRE(bench.build());
    bench.scene().set_fov(1.25F);
    std::vector<u8> mask;
    CY_REQUIRE(bench.mask(mask));
    const std::vector<u16> with_weapon = run.hdr;
    const std::vector<u32> picture(bench.scene().pixels().begin(), bench.scene().pixels().end());
    // Every texel OFF the weapon is the world's own, byte for byte: the weapon took exactly its
    // texels and moved nothing behind them.
    run.weapon_present = false;
    CY_REQUIRE(bench.frame());
    std::vector<u8> off(mask.size(), 0);
    for (usize texel = 0; texel < mask.size(); ++texel) {
        off[texel] = mask[texel] == 0 ? 1 : 0;
    }
    CY_CHECK_EQ(colour_difference(off, with_weapon, run.hdr), 0U);
    hold_to_golden(CY_VIEWMODEL_TEST_DIR, "weapon_view_composition.png",
                   Span<const u32>(picture.data(), picture.size()), kWidth, kHeight, false);
    save("weapon_view_composition.png", Span<const u32>(picture.data(), picture.size()), kWidth,
         kHeight);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("lit as the world lights it, and its field of view is its own") {
    DeviceFixture fixture("render.weapon_view lighting");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Bench bench(fixture);
    WeaponRun& run = bench.run();
    run.wall = false;
    run.ring = true;
    run.lantern = true;
    run.settings.vertical_fov_radians = 0.9F;
    CY_REQUIRE(bench.build());

    // THE REFERENCE: the weapon drawn as world geometry through the world's 0.9 radians — the
    // lighting the frame has always computed for those surfaces.
    run.weapon_view = false;
    std::vector<u8> world_mask;
    CY_REQUIRE(bench.mask(world_mask));
    const std::vector<u16> as_world = run.hdr;

    // The weapon view through the same lens: the same texels, the same bytes. Its depth is
    // squeezed and nothing it is lit by is.
    run.weapon_view = true;
    std::vector<u8> view_mask;
    CY_REQUIRE(bench.mask(view_mask));
    const std::vector<u16> as_view = run.hdr;
    f32 worst = 0.0F;
    CY_CHECK_EQ(mask_difference(world_mask, view_mask), 0U);
    const u32 relit = colour_difference(view_mask, as_view, as_world, &worst);
    std::fprintf(stderr, "lit as the world: %u of %u weapon texels differ (worst %.5f)\n", relit,
                 count(view_mask), static_cast<double>(worst));
    CY_CHECK_EQ(relit, 0U);

    // THE WORLD'S LENS WIDENED AND NARROWED: the weapon's texels are the same texels, the same
    // bytes — its own lens, and its light lists found through the world's projection of where it
    // is rather than through the pixel it landed on.
    for (const f32 fov : {1.35F, 0.6F}) {
        bench.scene().set_fov(fov);
        std::vector<u8> mask;
        CY_REQUIRE(bench.mask(mask));
        const u32 moved = mask_difference(mask, view_mask);
        const u32 recoloured = colour_difference(view_mask, run.hdr, as_view, &worst);
        std::fprintf(stderr,
                     "world lens %.2f: %u weapon texels moved, %u recoloured (worst %.5f)\n",
                     static_cast<double>(fov), moved, recoloured, static_cast<double>(worst));
        CY_CHECK_EQ(moved, 0U);
        CY_CHECK_EQ(recoloured, 0U);
    }
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("motion vectors: none for a weapon a turning camera carries, its lens's when raised") {
    DeviceFixture fixture("render.weapon_view motion");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Bench bench(fixture);
    WeaponRun& run = bench.run();
    run.wall = false;
    run.ring = true;
    run.settings.vertical_fov_radians = 0.95F;
    CY_REQUIRE(bench.build());
    bench.scene().set_fov(1.25F);
    std::vector<u8> mask;
    CY_REQUIRE(bench.mask(mask));

    // THE CAMERA TURNS FOUR DEGREES AND STEPS FORWARD, carrying the weapon.
    const f32 turn = 4.0F * 3.14159265F / 180.0F;
    bench.scene().set_camera(Vec3{0.0F, 0.0F, -0.05F}, Vec3{std::sin(turn), 0.0F, -std::cos(turn)});
    CY_REQUIRE(bench.frame());
    f32 weapon_worst = 0.0F;
    f32 world_least = 1.0F;
    u32 world_still = 0;
    u32 world_texels = 0;
    for (usize texel = 0; texel < mask.size(); ++texel) {
        const Vec2 motion = velocity_at(run, texel);
        const f32 length = std::sqrt((motion.x * motion.x) + (motion.y * motion.y));
        if (mask[texel] != 0) {
            weapon_worst = std::max(weapon_worst, length);
            continue;
        }
        const Vec3 colour = hdr_at(run, texel);
        if (colour.x + colour.y + colour.z <= 0.0F) {
            continue;
        }
        ++world_texels;
        world_least = std::min(world_least, length);
        world_still += length < 1e-3F ? 1U : 0U;
    }
    std::fprintf(stderr,
                 "turning: the weapon's largest motion %.6f; the world's least %.6f, %u of %u "
                 "still\n",
                 static_cast<double>(weapon_worst), static_cast<double>(world_least), world_still,
                 world_texels);
    CY_CHECK_LT(weapon_worst, 2e-4F);
    CY_CHECK_EQ(world_still, 0U);

    // THE SIGHTS COME UP: the camera still, the weapon's lens 0.95 radians to 0.75. A point at NDC
    // n this frame was at n * k last frame, k = tan(0.75 / 2) / tan(0.95 / 2), whatever its depth —
    // so its motion is known texel for texel. The texels are the raised weapon's own.
    bench.scene().set_camera(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F});
    run.settings.vertical_fov_radians = 0.75F;
    std::vector<u8> raised;
    CY_REQUIRE(bench.mask(raised));
    run.settings.vertical_fov_radians = 0.95F;
    CY_REQUIRE(bench.frame());
    run.settings.vertical_fov_radians = 0.75F;
    CY_REQUIRE(bench.frame());
    const f32 k = std::tan(0.75F * 0.5F) / std::tan(0.95F * 0.5F);
    const Vec2 jitter = bench.scene().assembly().temporal().jitter().current();
    f32 worst_error = 0.0F;
    u32 checked = 0;
    for (usize texel = 0; texel < raised.size(); ++texel) {
        if (raised[texel] == 0) {
            continue;
        }
        const Vec2 motion = velocity_at(run, texel);
        const u32 x = static_cast<u32>(texel % kWidth);
        const u32 y = static_cast<u32>(texel / kWidth);
        // The texel centre's UNJITTERED NDC, which is what the prepass takes the motion from.
        const f32 ndc_x = (((static_cast<f32>(x) + 0.5F) / static_cast<f32>(kWidth)) * 2.0F) -
                          1.0F - (jitter.x * 2.0F / static_cast<f32>(kWidth));
        const f32 ndc_y = 1.0F -
                          (((static_cast<f32>(y) + 0.5F) / static_cast<f32>(kHeight)) * 2.0F) -
                          (jitter.y * 2.0F / static_cast<f32>(kHeight));
        // The prepass's convention: (previous - current) / 2 in x, (current - previous) / 2 in y.
        const f32 expected_x = ((ndc_x * k) - ndc_x) * 0.5F;
        const f32 expected_y = (ndc_y - (ndc_y * k)) * 0.5F;
        worst_error = std::max(worst_error, std::max(std::fabs(motion.x - expected_x),
                                                     std::fabs(motion.y - expected_y)));
        ++checked;
    }
    std::fprintf(stderr, "the sights up: %u weapon texels, the worst error %.6f\n", checked,
                 static_cast<double>(worst_error));
    CY_CHECK_GT(checked, 3000U);
    // Half floats: the motion is a few hundredths, stored with eleven bits of mantissa.
    CY_CHECK_LT(worst_error, 2e-4F);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE(
    "the shadow policy: the world's shadow received by default, none cast, both settable") {
    DeviceFixture fixture("render.weapon_view shadows");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Bench bench(fixture);
    WeaponRun& run = bench.run();
    run.wall = false;
    run.shadows = true;
    run.overhang = true;
    run.travel = sun_ahead();
    CY_REQUIRE(bench.build());
    // Wide, so the floor the weapon's shadow falls on is in view.
    bench.scene().set_fov(1.4F);

    // RECEIVED: under a slab that shades it from the sun, the weapon is darker than with the
    // policy off — sampled where it is in the world.
    std::vector<u8> mask;
    CY_REQUIRE(bench.mask(mask));
    const std::vector<u16> received = run.hdr;
    run.settings.shadows.receive = false;
    CY_REQUIRE(bench.frame());
    const std::vector<u16> ignored = run.hdr;
    f64 shaded = 0.0;
    f64 lit = 0.0;
    for (usize texel = 0; texel < mask.size(); ++texel) {
        if (mask[texel] == 0) {
            continue;
        }
        for (u32 channel = 0; channel < 3U; ++channel) {
            shaded += static_cast<f64>(from_half(received[(texel * 4U) + channel]));
            lit += static_cast<f64>(from_half(ignored[(texel * 4U) + channel]));
        }
    }
    std::fprintf(stderr, "the weapon under the slab: %.1f received, %.1f ignored\n", shaded, lit);
    // The sun is the light the slab takes away; the sky's ambient stays. At least a twentieth of
    // the weapon's radiance is the sun's here — 16 per cent, measured.
    CY_CHECK_LT(shaded, lit * 0.95);

    // CAST: with the slab gone, nothing of the world changes for the weapon unless it casts; when
    // it does, the floor under it darkens and nothing brightens.
    run.settings.shadows.receive = true;
    run.weapon_present = true;
    run.travel = sun_behind();
    CY_REQUIRE(bench.scene().move_box(kFirstWorld, kParked).has_value());
    CY_REQUIRE(bench.frame());
    const std::vector<u16> silent = run.hdr;
    run.settings.shadows.cast = true;
    CY_REQUIRE(bench.frame());
    u32 darker = 0;
    u32 brighter = 0;
    for (usize texel = 0; texel < mask.size(); ++texel) {
        if (mask[texel] != 0) {
            continue;
        }
        const f32 before = from_half(silent[(texel * 4U) + 1]);
        const f32 after = from_half(run.hdr[(texel * 4U) + 1]);
        darker += after < before ? 1U : 0U;
        brighter += after > before ? 1U : 0U;
    }
    std::fprintf(stderr, "casting: %u world texels darker, %u brighter\n", darker, brighter);
    CY_CHECK_GT(darker, 50U);
    CY_CHECK_EQ(brighter, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("at 1080p: the frame's GPU time with the weapon view and with the weapon as world") {
    DeviceFixture fixture("render.weapon_view 1080p");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Bench bench(fixture, 1920, 1080);
    WeaponRun& run = bench.run();
    run.ring = true;
    run.lantern = true;
    run.wall_distance = 0.55F;
    run.wall_x = 0.3F;
    run.wall_half_width = 0.14F;
    CY_REQUIRE(bench.build());
    bench.scene().set_fov(1.25F);
    if (!run.timer) {
        std::fprintf(stderr, "no timestamp queries on this device; nothing to time\n");
        return;
    }
    constexpr u32 kFrames = 24;
    f64 totals[2][3] = {};
    for (u32 pass = 0; pass < 2U; ++pass) {
        run.weapon_view = pass == 0U;
        for (u32 frame = 0; frame < kFrames + 4U; ++frame) {
            CY_REQUIRE(bench.frame());
            if (frame < 4U || !run.timed) {
                continue;  // warm-up
            }
            totals[pass][0] += run.stamps_ms[1] - run.stamps_ms[0];
            totals[pass][1] += run.stamps_ms[2] - run.stamps_ms[1];
            totals[pass][2] += run.stamps_ms[3];
        }
        if (pass == 0U) {
            save("weapon_view_1080p.png", bench.scene().pixels(), 1920, 1080);
        }
    }
    for (u32 pass = 0; pass < 2U; ++pass) {
        std::fprintf(stderr,
                     "1080p, %s: prepass %.3f ms, opaque %.3f ms, frame to the post-process %.3f "
                     "ms (mean of %u)\n",
                     pass == 0U ? "weapon view" : "weapon as world geometry",
                     totals[pass][0] / kFrames, totals[pass][1] / kFrames,
                     totals[pass][2] / kFrames, kFrames);
    }
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
