// samples/07-fidelity/capture.cpp — turns the virtual-geometry frame into PNGs.
//
// WHY THIS EXISTS. M7 put 4,478,208 source triangles on the device through cluster traversal and a
// visibility buffer, and for two milestones its only committed picture was a chart — because at M7
// nothing in this engine could turn a frame into a file. M8.c built that, and everything else was
// already read back per pixel: a visible-cluster index, a triangle index, and the resolved world
// normal. This writes three views of one frame, which is what
// `docs/design/images/virtual-geometry-*` are and how they are regenerated:
//
//     cy_fidelity_capture <stem> [threshold_pixels]
//
// The threshold is the point. It is the geometric error the cluster hierarchy is allowed to commit
// in screen space, so running the same view at 1, 4 and 16 pixels shows the hierarchy CHOOSING —
// same camera, same coverage, visibly coarser clusters. That is the "no manual level-of-detail
// authoring" claim rendered rather than asserted, and a still frame cannot make it alone.
//
// It is not a test. Nothing here asserts; the golden-image suite under tests/render owns that, and
// this borrows only its PNG writer so there is one encoder in the tree rather than two.
#include "frame.h"
#include "scene.h"

#include <cy/core/memory/system_allocator.h>
#include "golden.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace cy;
using namespace cy::sample::fidelity;

namespace {

/// A stable, well-separated colour per index, stepped by the golden ratio so ADJACENT indices land
/// far apart on the wheel. That separation is the whole point of the cluster view: neighbouring
/// clusters have to be distinguishable or the picture says nothing.
[[nodiscard]] u32 hue(u32 index) noexcept {
    const f32 scaled = static_cast<f32>(index) * 0.6180339887F;
    const f32 h = scaled - static_cast<f32>(static_cast<i32>(scaled));
    const f32 s = 0.68F;
    const f32 v = 0.96F;
    const auto sector = static_cast<i32>(h * 6.0F);
    const f32 f = (h * 6.0F) - static_cast<f32>(sector);
    const f32 p = v * (1.0F - s);
    const f32 q = v * (1.0F - (s * f));
    const f32 t = v * (1.0F - (s * (1.0F - f)));
    f32 r = v;
    f32 g = t;
    f32 b = p;
    switch (sector % 6) {
        case 1:
            r = q;
            g = v;
            b = p;
            break;
        case 2:
            r = p;
            g = v;
            b = t;
            break;
        case 3:
            r = p;
            g = q;
            b = v;
            break;
        case 4:
            r = t;
            g = p;
            b = v;
            break;
        case 5:
            r = v;
            g = p;
            b = q;
            break;
        default:
            break;
    }
    const auto byte = [](f32 value) { return static_cast<u32>(value * 255.0F) & 0xFFU; };
    return 0xFF000000U | (byte(b) << 16U) | (byte(g) << 8U) | byte(r);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string stem = argc > 1 ? argv[1] : "/tmp/vg";
    const f32 threshold = argc > 2 ? std::strtof(argv[2], nullptr) : 1.0F;

    Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Renderer);

    Scene scene(allocator);
    if (Status built = build_scene(SceneOptions{}, scene); !built) {
        std::printf("scene: %s\n", built.error().message);
        return 1;
    }

    FrameOptions options;
    options.width = 1280;
    options.height = 720;
    options.frames = 1;
    options.warmup_frames = 2;
    options.threshold_pixels = threshold;

    FrameReport report(allocator);
    Capture capture(allocator);
    if (Status ran = render_frames(scene, options, report, &capture); !ran) {
        std::printf("frame: %s\n", ran.error().message);
        return 1;
    }
    if (!report.device) {
        std::printf("no device answered (%s); nothing captured\n", report.reason);
        return 1;
    }

    std::printf("source %llu tri  distinct %u tri  cooked %u clusters / %u pages\n",
                static_cast<unsigned long long>(scene.source_triangles), scene.distinct_triangles,
                scene.clusters, scene.pages);
    std::printf("covered %llu px  clusters %llu  materials %u  threshold %.2f px\n",
                static_cast<unsigned long long>(report.covered_pixels),
                static_cast<unsigned long long>(report.visible_clusters), report.materials_seen,
                static_cast<double>(threshold));

    render_test::Image shaded(allocator);
    render_test::Image clusters(allocator);
    render_test::Image triangles(allocator);
    for (render_test::Image* image : {&shaded, &clusters, &triangles}) {
        image->width = options.width;
        image->height = options.height;
        if (Status sized = image->texels.resize(static_cast<usize>(options.width) * options.height);
            !sized) {
            return 1;
        }
        for (u32& texel : image->texels) {
            texel = 0xFF141414U;
        }
    }

    const u64 count = static_cast<u64>(options.width) * options.height;
    for (u64 i = 0; i < count && i < capture.samples.size(); ++i) {
        if (!capture.samples[i].covered()) {
            continue;
        }
        // Shaded: the resolved world normal under one direction. This is what the M7 resolve
        // evaluates, so the picture is the resolve's own answer rather than a re-derivation.
        const Vec4 n =
            i < capture.resolved.size() ? capture.resolved[i] : Vec4{0.5F, 0.5F, 1.0F, 0.0F};
        const f32 lambert = std::abs(((n.x * 2.0F - 1.0F) * 0.35F) + ((n.y * 2.0F - 1.0F) * 0.72F) +
                                     ((n.z * 2.0F - 1.0F) * 0.60F));
        const f32 lit = 0.16F + (0.84F * lambert);
        const auto grey = [lit](f32 tint) { return static_cast<u32>(lit * tint) & 0xFFU; };
        shaded.texels[i] =
            0xFF000000U | (grey(206.0F) << 16U) | (grey(222.0F) << 8U) | grey(232.0F);

        // STABLE IDENTITY, not the traversal index. `samples[i].visible` indexes the visible list
        // in atomic-append order, so coluring by it repaints the whole image on every run and the
        // picture cannot be regenerated. The (instance, cluster) pair behind it does not move.
        const u32 slot = capture.samples[i].visible;
        const u32 identity = slot < capture.visible.size()
                                 ? ((capture.visible[slot].cluster * 2654435761U) ^
                                    (capture.visible[slot].instance * 2246822519U))
                                 : slot;
        clusters.texels[i] = hue(identity >> 11U);
        triangles.texels[i] = hue((capture.samples[i].triangle * 2654435761U) >> 15U);
    }

    const auto save = [&](const char* suffix, const render_test::Image& image) {
        const std::string path = stem + suffix;
        if (Status wrote = render_test::write_png(path.c_str(), image); !wrote) {
            std::printf("write %s: %s\n", path.c_str(), wrote.error().message);
            return;
        }
        std::printf("wrote %s\n", path.c_str());
    };
    save("-shaded.png", shaded);
    save("-clusters.png", clusters);
    save("-triangles.png", triangles);
    return 0;
}
