// SPDX-License-Identifier: MIT
// The GI debug views, each against a golden image.
//
// `rendering-global-illumination` — "GI debug visualisation": the nineteen views of the GI state
// `gi/debug_views.h` draws, of the divided room converged, from one camera, each compared with a
// committed reference (`references/gi_view_<name>.png`) by `tests/render/golden.h`'s metric; each
// view differs from every other; the probe-visibility view paints the leak a wall-blind probe
// makes; the tier view colours each pixel by the tier that answered; and every view's name is a
// `render::DebugViewMode`'s, so the frame and the editor ask for it by that name.
//
// `CY_RENDER_UPDATE_GOLDEN=1` rewrites the references and fails, on purpose.

#include <cy/test/test.h>

#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/rendering/gi/debug_views.h>
#include <cy/rendering/gi/system.h>
#include <cy/servers/render/types.h>

#include "golden.h"
#include "rooms.h"
#include "support.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using cy::Aabb;
using cy::f32;
using cy::f64;
using cy::i32;
using cy::Mat4;
using cy::u32;
using cy::u64;
using cy::usize;
using cy::Vec3;
using cy::Vec4;
using namespace cy::rendering::gi;  // NOLINT(google-build-using-namespace)
namespace render_test = cy::render_test;

constexpr u32 kWidth = 96;
constexpr u32 kHeight = 72;
constexpr f32 kFovY = 1.2F;

/// The camera: in the dark half, looking through the doorway at the lit one.
const Vec3 kEye{0.6F, 0.4F, 3.4F};
const Vec3 kTarget{-0.8F, -0.6F, -2.0F};
/// The previous frame's camera, for the reprojection the denoiser views read.
const Vec3 kPreviousEye{1.0F, 0.4F, 3.4F};

/// The room's solids as boxes: the hollow room (inside it), and the divider's two halves.
struct Solid {
    Aabb box;
    bool hollow = false;
};

[[nodiscard]] std::vector<Solid> solids() {
    const f32 t = gi_rooms::kDividerHalfThickness;
    return {
        Solid{Aabb::from_center_extents(
                  Vec3{}, Vec3{gi_support::kRoomX, gi_support::kRoomY, gi_support::kRoomZ}),
              true},
        Solid{Aabb::from_min_max(Vec3{-gi_support::kRoomX, -gi_support::kRoomY, -t},
                                 Vec3{-1.0F, gi_support::kRoomY, t}),
              false},
        Solid{Aabb::from_min_max(Vec3{1.0F, -gi_support::kRoomY, -t},
                                 Vec3{gi_support::kRoomX, gi_support::kRoomY, t}),
              false},
    };
}

/// A ray against one box: the entry of a solid one, the exit of the hollow room. The normal faces
/// the ray's origin.
[[nodiscard]] bool hit_box(const Solid& solid, Vec3 origin, Vec3 direction, f32& t, Vec3& normal) {
    f32 near = -1.0e30F;
    f32 far = 1.0e30F;
    i32 near_axis = 0;
    i32 far_axis = 0;
    for (i32 axis = 0; axis < 3; ++axis) {
        const f32 d = direction[axis];
        const f32 o = origin[axis];
        if (std::fabs(d) < 1.0e-9F) {
            if (o < solid.box.min[axis] || o > solid.box.max[axis]) {
                return false;
            }
            continue;
        }
        f32 a = (solid.box.min[axis] - o) / d;
        f32 b = (solid.box.max[axis] - o) / d;
        if (a > b) {
            std::swap(a, b);
        }
        if (a > near) {
            near = a;
            near_axis = axis;
        }
        if (b < far) {
            far = b;
            far_axis = axis;
        }
    }
    if (near > far) {
        return false;
    }
    const i32 axis = solid.hollow ? far_axis : near_axis;
    t = solid.hollow ? far : near;
    if (t <= 1.0e-4F) {
        return false;
    }
    normal = Vec3{};
    normal[axis] = direction[axis] > 0.0F ? -1.0F : 1.0F;
    return true;
}

struct Frame {
    std::vector<GiViewPixel> pixels;
    std::vector<f32> depth;
    std::vector<Vec3> normal;
    std::vector<Vec3> colour;
    Mat4 world_to_view = Mat4::identity();
    Mat4 world_to_clip = Mat4::identity();
};

[[nodiscard]] Frame trace_frame(Vec3 eye) {
    Frame frame;
    frame.world_to_view = cy::look_at(eye, kTarget);
    frame.world_to_clip =
        cy::perspective_reversed_z(kFovY, static_cast<f32>(kWidth) / kHeight, 0.05F, 50.0F) *
        frame.world_to_view;
    const Vec3 forward = cy::normalize(kTarget - eye);
    const Vec3 right = cy::normalize(cy::cross(forward, Vec3{0.0F, 1.0F, 0.0F}));
    const Vec3 up = cy::cross(right, forward);
    const f32 tan_y = std::tan(kFovY * 0.5F);
    const f32 tan_x = tan_y * static_cast<f32>(kWidth) / kHeight;
    const std::vector<Solid> boxes = solids();
    frame.pixels.resize(static_cast<size_t>(kWidth) * kHeight);
    frame.depth.assign(frame.pixels.size(), 0.0F);
    frame.normal.assign(frame.pixels.size(), Vec3{0.0F, 1.0F, 0.0F});
    frame.colour.assign(frame.pixels.size(), Vec3{});
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const f32 ndc_x = (((static_cast<f32>(x) + 0.5F) / kWidth) * 2.0F) - 1.0F;
            const f32 ndc_y = 1.0F - (((static_cast<f32>(y) + 0.5F) / kHeight) * 2.0F);
            const Vec3 direction =
                cy::normalize(forward + (right * (ndc_x * tan_x)) + (up * (ndc_y * tan_y)));
            f32 best = 1.0e30F;
            Vec3 best_normal{};
            for (const Solid& solid : boxes) {
                f32 t = 0.0F;
                Vec3 n{};
                if (hit_box(solid, eye, direction, t, n) && t < best) {
                    best = t;
                    best_normal = n;
                }
            }
            GiViewPixel& pixel = frame.pixels[(static_cast<size_t>(y) * kWidth) + x];
            pixel.position = eye + (direction * best);
            pixel.normal = best_normal;
            pixel.depth = best * cy::dot(direction, forward);
            // Smooth walls, rough floor: the specular views see both.
            pixel.roughness = best_normal.y > 0.5F ? 0.7F : 0.15F;
            frame.depth[(static_cast<size_t>(y) * kWidth) + x] = pixel.depth;
            frame.normal[(static_cast<size_t>(y) * kWidth) + x] = pixel.normal;
        }
    }
    return frame;
}

/// The divided room converged, two reflection probes and an emissive panel, and the frame.
struct Scene {
    gi_rooms::LitRoom room;
    Frame frame = trace_frame(kEye);
    Frame previous = trace_frame(kPreviousEye);
    ScreenView screen;
    std::vector<cy::i32> history_source;
    std::vector<f32> history_confidence;
    u64 frame_number = 0;

    explicit Scene(IlluminationSettings settings = gi_support::room_settings())
        : room(true, settings) {
        ReflectionProbe lit_half;
        lit_half.position = Vec3{-2.0F, 0.0F, -2.0F};
        lit_half.is_box = true;
        lit_half.box = Aabb::from_min_max(Vec3{-5.0F, -3.0F, -5.0F}, Vec3{5.0F, 3.0F, 0.0F});
        lit_half.capture_mode = ProbeCaptureMode::Realtime;
        ReflectionProbe dark_half = lit_half;
        dark_half.position = Vec3{0.0F, 0.0F, 2.0F};
        dark_half.box = Aabb::from_min_max(Vec3{-5.0F, -3.0F, 0.0F}, Vec3{5.0F, 3.0F, 5.0F});
        CY_REQUIRE(room.system.reflection_probes().add(lit_half).has_value());
        CY_REQUIRE(room.system.reflection_probes().add(dark_half).has_value());

        // A small bright lamp on the dark half's wall, promoted to a light by the classification.
        Surfel lamp;
        lamp.position = Vec3{3.95F, 0.5F, 2.0F};
        lamp.normal = Vec3{-1.0F, 0.0F, 0.0F};
        lamp.albedo = Vec3{0.8F, 0.8F, 0.8F};
        lamp.emission = Vec3{400.0F, 280.0F, 150.0F};
        lamp.area = 0.09F;
        lamp.instance_id = 30;
        lamp.material_id = 30;
        CY_REQUIRE(
            room.system
                .ingest_cell(2, Aabb::from_center_extents(lamp.position, Vec3{0.5F, 0.5F, 0.5F}),
                             {&lamp, 1}, 0)
                .has_value());
        frame_number = room.converge(1, 16);

        for (usize index = 0; index < frame.pixels.size(); ++index) {
            Vec3 radiance{};
            cy::u32 age = 0;
            (void)room.system.surfaces().radiance_at(frame.pixels[index].position,
                                                     frame.pixels[index].normal, radiance, age);
            frame.colour[index] = radiance;
            if (std::fabs(frame.pixels[index].normal.x) > 0.5F &&
                frame.pixels[index].position.x > 3.9F &&
                cy::length(frame.pixels[index].position - lamp.position) < 0.1F) {
                frame.pixels[index].emission = lamp.emission;
            }
        }
        screen.width = kWidth;
        screen.height = kHeight;
        screen.depth = {frame.depth.data(), frame.depth.size()};
        screen.normal = {frame.normal.data(), frame.normal.size()};
        screen.colour = {frame.colour.data(), frame.colour.size()};
        screen.world_to_view = frame.world_to_view;
        screen.world_to_clip = frame.world_to_clip;
        screen.camera_position = kEye;

        // The reprojection: each pixel's surface into the previous camera, kept where the
        // previous frame saw the same surface there.
        history_source.assign(frame.pixels.size(), -1);
        history_confidence.assign(frame.pixels.size(), 0.0F);
        for (usize index = 0; index < frame.pixels.size(); ++index) {
            const Vec3 p = frame.pixels[index].position;
            const Vec4 clip = previous.world_to_clip * Vec4{p.x, p.y, p.z, 1.0F};
            if (clip.w <= 0.0F) {
                continue;
            }
            const f32 sx = (((clip.x / clip.w) * 0.5F) + 0.5F) * kWidth;
            const f32 sy = (0.5F - ((clip.y / clip.w) * 0.5F)) * kHeight;
            if (sx < 0.0F || sy < 0.0F || sx >= kWidth || sy >= kHeight) {
                continue;
            }
            const usize source = (static_cast<usize>(sy) * kWidth) + static_cast<usize>(sx);
            const f32 apart = cy::length(previous.pixels[source].position - p);
            if (apart < 0.25F) {
                history_source[index] = static_cast<cy::i32>(source);
                history_confidence[index] = 1.0F - (apart / 0.25F);
            }
        }
    }

    [[nodiscard]] GiViewCamera camera() const noexcept {
        GiViewCamera out;
        out.width = kWidth;
        out.height = kHeight;
        out.position = kEye;
        out.world_to_clip = frame.world_to_clip;
        return out;
    }

    [[nodiscard]] GiDebugInputs inputs() const noexcept {
        GiDebugInputs out;
        out.pixels = {frame.pixels.data(), frame.pixels.size()};
        out.screen = &screen;
        out.history.source = {history_source.data(), history_source.size()};
        out.history.confidence = {history_confidence.data(), history_confidence.size()};
        out.field_slice_height = -0.5F;
        out.frame = frame_number;
        out.exposure = 4.0F;
        return out;
    }

    [[nodiscard]] std::vector<u32> draw(GiDebugView view) {
        std::vector<u32> texels(static_cast<size_t>(kWidth) * kHeight);
        const cy::Status drawn = render_gi_debug_view(room.system, view, camera(), inputs(),
                                                      {texels.data(), texels.size()});
        CY_REQUIRE(drawn.has_value());
        return texels;
    }
};

[[nodiscard]] const char* reference_path(GiDebugView view) {
    static char storage[1024];
    (void)std::snprintf(storage, sizeof(storage), "%s/references/gi_view_%s.png", CY_GI_TEST_DIR,
                        gi_debug_view_name(view));
    return storage;
}

[[nodiscard]] bool updating_references() noexcept {
    const char* value = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

[[nodiscard]] u32 channel(u32 texel, u32 index) noexcept {
    return (texel >> (index * 8U)) & 0xFFU;
}

}  // namespace

CY_TEST_CASE("every GI debug view is a render debug view mode of the same name") {
    for (u32 index = 0; index < kGiDebugViewCount; ++index) {
        const auto view = static_cast<GiDebugView>(index);
        const cy::render::DebugViewMode mode =
            cy::render::debug_view_mode_from_name(gi_debug_view_name(view));
        CY_CHECK_MESSAGE(mode != cy::render::DebugViewMode::Off, gi_debug_view_name(view)
                                                                     << " is not a DebugViewMode");
        CY_CHECK(gi_debug_view_from_name(cy::render::debug_view_mode_name(mode)) == view);
    }
    // And every Gi* mode the frame offers is drawn here; the lightmap views are drawn by the frame.
    u32 gi_modes = 0;
    for (u32 index = 0; index < cy::render::kDebugViewModeCount; ++index) {
        const char* name =
            cy::render::debug_view_mode_name(static_cast<cy::render::DebugViewMode>(index));
        if (std::strncmp(name, "Gi", 2) == 0 && std::strcmp(name, "GiContribution") != 0) {
            gi_modes += 1;
            CY_CHECK_MESSAGE(gi_debug_view_from_name(name) != GiDebugView::Count,
                             name << " has no GI view");
        }
    }
    CY_CHECK_EQ(gi_modes, kGiDebugViewCount);
}

namespace {

/// Draw views [first, last) of one scene and hold each to its golden image.
void check_views(u32 first, u32 last) {
    Scene scene;
    for (u32 index = first; index < last; ++index) {
        const auto view = static_cast<GiDebugView>(index);
        const auto started = std::chrono::steady_clock::now();
        std::vector<u32> texels = scene.draw(view);
        const f64 drawn_ms =
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        render_test::Image rendered(cy::current_allocator());
        CY_REQUIRE(render_test::adopt(rendered, {texels.data(), texels.size()}, kWidth, kHeight)
                       .has_value());
        u32 drawn = 0;
        for (const u32 texel : texels) {
            drawn += channel(texel, 3) != 0 ? 1U : 0U;
        }
        CY_CHECK_MESSAGE(drawn > 0U, gi_debug_view_name(view) << " drew nothing");
        if (updating_references()) {
            CY_CHECK(render_test::write_png(reference_path(view), rendered).has_value());
            std::fprintf(stderr, "wrote %s\n", reference_path(view));
            CY_TEST_FAIL_CHECK("references were regenerated; this mode never passes");
            continue;
        }
        render_test::Image reference(cy::current_allocator());
        const cy::Status read = render_test::read_png(reference_path(view), reference);
        if (!read) {
            std::fprintf(stderr, "%s: %s\n", reference_path(view), read.error().message);
        }
        CY_REQUIRE(read.has_value());
        const render_test::Comparison comparison = render_test::compare(reference, rendered);
        std::printf("%-22s %5u drawn in %6.2f ms, %u differing (%u off an edge), worst delta %u\n",
                    gi_debug_view_name(view), drawn, drawn_ms, comparison.differing,
                    comparison.differing_off_edge, comparison.max_channel_delta);
        CY_CHECK(comparison.comparable);
#if defined(__linux__) && defined(__x86_64__) && !defined(__FMA__)
        // Where the references were drawn (Linux x86-64, GCC 13, clang 18 and clang 22 alike,
        // no contracted multiply-adds): exactly.
        CY_CHECK_MESSAGE(comparison.differing == 0U, gi_debug_view_name(view)
                                                         << " differs from its golden image");
#else
        // Elsewhere the views are drawn from tens of frames of converged floating-point GI through
        // another libm (and, on Apple silicon, contracted multiply-adds): a decision at a threshold
        // — a screen hit, a card tie — may land the other side at a pixel. An edge texel may
        // differ, and one per cent of the image beyond that. Measured: the same views built with
        // clang 22 and contracted multiply-adds on x86-64-v3 differ in 3 texels of one view
        // (GiProbeVisibility), and in none of the other eighteen.
        constexpr u32 kPortableTexels = (kWidth * kHeight) / 100U;
        CY_CHECK_MESSAGE(comparison.differing <= comparison.edge_texels + kPortableTexels,
                         gi_debug_view_name(view) << " differs from its golden image");
#endif
    }
}

}  // namespace

// Four cases rather than one, so each holds its one scene and a handful of views inside the
// integration suite's budget in every profile.
CY_TEST_CASE("the probe views match their golden images") {
    check_views(0, 5);
}

CY_TEST_CASE("the cache, field, card and reflection views match their golden images") {
    check_views(5, 10);
}

CY_TEST_CASE("the trace and confidence views match their golden images") {
    check_views(10, 14);
}

CY_TEST_CASE("the denoiser and contribution views match their golden images") {
    check_views(14, kGiDebugViewCount);
}

CY_TEST_CASE("no two GI debug views are the same picture") {
    // Each view is held to its own reference above, so two distinct references are two distinct
    // views.
    std::vector<std::vector<u32>> pictures;
    for (u32 index = 0; index < kGiDebugViewCount; ++index) {
        render_test::Image reference(cy::current_allocator());
        CY_REQUIRE(render_test::read_png(reference_path(static_cast<GiDebugView>(index)), reference)
                       .has_value());
        pictures.emplace_back(reference.texels.begin(), reference.texels.end());
    }
    for (u32 a = 0; a < pictures.size(); ++a) {
        for (u32 b = a + 1; b < pictures.size(); ++b) {
            CY_CHECK_MESSAGE(pictures[a] != pictures[b],
                             gi_debug_view_name(static_cast<GiDebugView>(a))
                                 << " and " << gi_debug_view_name(static_cast<GiDebugView>(b))
                                 << " are the same picture");
        }
    }
}

CY_TEST_CASE("the tier view colours each pixel by the tier that answered it") {
    Scene scene;
    const std::vector<u32> tiers = scene.draw(GiDebugView::Tier);
    u32 screen = 0;
    u32 software = 0;
    u32 hardware = 0;
    for (const u32 texel : tiers) {
        if (channel(texel, 3) == 0U) {
            continue;
        }
        const u32 r = channel(texel, 0);
        const u32 g = channel(texel, 1);
        const u32 b = channel(texel, 2);
        screen += (b > 200U && r < 80U) ? 1U : 0U;
        software += (g > 180U && r < 80U && b < 100U) ? 1U : 0U;
        hardware += (r > 200U && g > 100U && g < 180U && b < 40U) ? 1U : 0U;
    }
    std::printf("tier view: %u screen, %u software, %u hardware pixels\n", screen, software,
                hardware);
    // The acceleration service is inactive here: the software tier answers what the screen does
    // not, and nothing is coloured as the hardware tier.
    CY_CHECK_GT(screen, 0U);
    CY_CHECK_GT(software, 0U);
    CY_CHECK_EQ(hardware, 0U);
}

CY_TEST_CASE("the probe visibility view paints the leak a wall-blind probe makes") {
    // The same room with the visibility term opened: probes beyond the divider light the wall in
    // front of the camera, and the view must turn it red; with the term as configured, it does not.
    Scene sealed;
    const std::vector<u32> clean = sealed.draw(GiDebugView::ProbeVisibility);
    IlluminationSettings leaky = gi_support::room_settings();
    leaky.probes.visibility_bias_metres = 6.0F;
    Scene leaking(leaky);
    const std::vector<u32> leaked = leaking.draw(GiDebugView::ProbeVisibility);
    // A surface whose probe weight is all clean is drawn kGreen * 0.8, red 31: anything redder
    // carries some leak.
    const auto red = [](const std::vector<u32>& texels) {
        u32 count = 0;
        for (const u32 texel : texels) {
            count += (channel(texel, 3) != 0U && channel(texel, 0) > 40U &&
                      channel(texel, 0) > channel(texel, 2) + 20U)
                         ? 1U
                         : 0U;
        }
        return count;
    };
    std::printf("probe visibility: %u red pixels with the term opened, %u with it configured\n",
                red(leaked), red(clean));
    CY_CHECK_GT(red(leaked), red(clean) + 50U);
}
