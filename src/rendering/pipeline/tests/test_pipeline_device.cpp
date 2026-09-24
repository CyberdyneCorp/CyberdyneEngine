// THE FRAME, CAPTURED. M8.c tasks 1b.2 and 1b.4, and the source of task 5.5's before-and-after
// pair.
//
// ================================================================================================
// WHAT THIS SUITE PRODUCES THAT NO EARLIER ONE COULD
// ================================================================================================
//
// M8.b's artefact drew its picture: it projected the frame's own draw list through the frame's own
// matrices and rendered shapes with the CPU, because `FrameAssembly` hands each pass's record
// callback to its caller and no caller supplied one. This suite READS THE FRAME'S OUTPUT IMAGE OFF
// THE DEVICE. Every texel below came out of a fragment shader.
//
// The three pictures it writes are the milestone's, and they are written every run rather than only
// on failure, because task 5.5 wants the before-and-after pair and a picture that only exists when
// something is broken is not a pair:
//
//   pipeline-frame-with-callbacks.png     the frame this layer records
//   pipeline-frame-without-callbacks.png  THE SAME FRAME, assembled identically, with an empty
//                                         `FrameSinks` — which is what the engine did before M8.c
//   pipeline-frame-particles.png          the same frame again with the particle renderer attached
//                                         through `PassExtension`
//
// They land in CY_TEST_ARTEFACT_DIR, else the suite's build directory — never the caller's working
// directory; `docs/design/images/` holds the committed copies.
//
// ================================================================================================
// EVERY ASSERTION HERE CAN FAIL
// ================================================================================================
//
// The comparison is between two runs of the SAME code path with one argument changed, so there is
// no tolerance to tune and nothing to regenerate. Delete the `attach` calls in
// `FrameRecorder::sinks()` and the second case's `differing` count goes to zero and the case goes
// red; that is the check, and it was run.

#include "frame_scene.h"

#include "golden.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#if defined(CY_TEST_PIPELINE_METAL)
#    include <cy/backends/rhi-metal/backend.h>
#else
#    include <cy/backends/rhi/vulkan/vulkan_backend.h>
#endif
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/pipeline/material_textures.h>
#include <cy/servers/render/server.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace cy;
using namespace cy::pipeline_test;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
    }
    std::fprintf(stderr, "graphics validation %s: %s\n",
                 severity == rhi::ValidationSeverity::Error ? "error" : "warning",
                 message != nullptr ? message : "");
}

/// A device per case, for the reason tests/render/device.h gives: synchronisation validation keeps
/// per-queue state for the process's lifetime, and recycled handles across two devices in one
/// process produce phantom cross-test hazards that look damning and are not.
class DeviceFixture {
public:
    DeviceFixture() noexcept : allocator_(system_allocator(MemoryDomain::Gpu)) {
#if defined(CY_TEST_PIPELINE_METAL)
        (void)rhi::metal::register_metal_backend();
        constexpr const char* backend = "metal";
#else
        (void)rhi::vulkan::register_vulkan_backend();
        constexpr const char* backend = "vulkan";
#endif
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_test_render_pipeline_frame";
        description.enable_validation = true;
        description.enable_synchronisation_validation = true;
        device_ = rhi::create_device(allocator_, backend, description, selection_);
        if (device_.has_value()) {
            device_.value()->set_validation_callback(&count_validation, &errors_);
        }
    }

    ~DeviceFixture() {
        if (device_.has_value()) {
            (void)device_.value()->wait_idle();
            rhi::destroy_device(allocator_, device_.value());
        }
    }

    DeviceFixture(const DeviceFixture&) = delete;
    DeviceFixture& operator=(const DeviceFixture&) = delete;

    [[nodiscard]] bool has_gpu() const noexcept {
        return device_.has_value() &&
#if defined(CY_TEST_PIPELINE_METAL)
               device_.value()->capabilities().backend() == rhi::BackendKind::Metal;
#else
               device_.value()->capabilities().backend() == rhi::BackendKind::Vulkan;
#endif
    }
    [[nodiscard]] rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] u32 validation_errors() const noexcept { return errors_; }

    void report_skip() const noexcept {
        std::fprintf(stderr,
                     "no requested graphics device on this machine; the backend selected was '%s' "
                     "because %s\n",
                     selection_.selected != nullptr ? selection_.selected : "(none)",
                     selection_.reason != nullptr ? selection_.reason : "(no reason given)");
    }

private:
    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    u32 errors_ = 0;
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

/// Write one capture, and say where. A picture nobody can find is a picture nobody looks at.
///
/// WHERE THE RUN OWNS, never the caller's working directory: a bare filename lands in whatever
/// directory the suite was started from, which for a criterion is the repository root, and
/// `falsify --mutate-the-tree` refuses the dirty tree that leaves behind. CY_TEST_ARTEFACT_DIR when
/// the harness sets it, else the build tree this binary was configured into.
void save(const char* name, Span<const u32> texels) noexcept {
    render_test::Image image(allocator());
    if (!render_test::adopt(image, texels, kWidth, kHeight).has_value()) {
        return;
    }
    const char* directory = std::getenv("CY_TEST_ARTEFACT_DIR");
    if (directory == nullptr || *directory == '\0') {
        directory = CY_TEST_BINARY_DIR;
    }
    char path[1024];
    (void)std::snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (render_test::write_png(path, image).has_value()) {
        std::fprintf(stderr, "wrote %s (%ux%u)\n", path, kWidth, kHeight);
    }
}

/// How many texels are not the clear colour. The clear is a dark neutral rather than black, so
/// "something was shaded here" and "nothing was" are different numbers rather than the same one.
[[nodiscard]] u32 shaded_texels(Span<const u32> texels) noexcept {
    u32 shaded = 0;
    for (const u32 texel : texels) {
        const u32 red = texel & 0xFFU;
        const u32 green = (texel >> 8U) & 0xFFU;
        const u32 blue = (texel >> 16U) & 0xFFU;
        if (red > 24U || green > 24U || blue > 24U) {
            ++shaded;
        }
    }
    return shaded;
}

}  // namespace

#if defined(CY_TEST_PIPELINE_METAL)
CY_TEST_CASE("Metal forward frame samples the material texture selected by its slot") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }

    Allocator& gpu = system_allocator(MemoryDomain::Gpu);
    render::RenderServer server(gpu);
    render::RenderServerConfig config;
    config.debug_primitive_capacity = 16;
    config.debug_label_capacity = 4;
    CY_REQUIRE(server.configure(config).has_value());
    CY_REQUIRE(server.initialize().has_value());

    render::TextureRecord description;
    description.format = render::TextureFormat::Rgba8Unorm;
    description.usage_class = render::TextureUsageClass::Data;
    description.width = 1;
    description.height = 1;
    description.mip_levels = 1;
    description.name = Name::intern("metal material red");
    const auto red = server.create_texture(description);
    CY_REQUIRE(red.has_value());
    description.name = Name::intern("metal material blue");
    const auto blue = server.create_texture(description);
    CY_REQUIRE(blue.has_value());

    rendering::pipeline::MaterialTextureTable textures;
    rhi::SamplerDescription sampler;
    sampler.name = "metal material test sampler";
    CY_REQUIRE(textures.initialize(fixture.device(), gpu, sampler).has_value());
    const u8 red_pixel[4] = {255, 0, 0, 255};
    const u8 blue_pixel[4] = {0, 0, 255, 255};
    const rendering::pipeline::TextureUpload uploads[2] = {
        {*red, {red_pixel, 4}},
        {*blue, {blue_pixel, 4}},
    };
    CY_REQUIRE(textures.upload(server, {uploads, 2}).has_value());
    rendering::pipeline::MaterialTextureSlot resident[2];
    CY_REQUIRE_EQ(textures.slots({resident, 2}), usize{2});

    FrameScene scene(allocator());
    CY_REQUIRE(scene.build(fixture.device()).has_value());
    scene.set_read_back(true);
    rendering::assembly::AssemblyReport report;
    CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
    Array<u32> constant_frame(allocator());
    CY_REQUIRE(constant_frame.append(scene.pixels()).has_value());
    CY_REQUIRE(scene.bind_material_texture(textures.slot_of(*red), {resident, 2}));
    CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
    Array<u32> red_frame(allocator());
    CY_REQUIRE(red_frame.append(scene.pixels()).has_value());
    CY_REQUIRE(scene.bind_material_texture(textures.slot_of(*blue), {resident, 2}));
    CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());

    usize changed = 0;
    usize differs_from_constant = 0;
    for (usize pixel = 0; pixel < red_frame.size(); ++pixel) {
        changed += static_cast<usize>(red_frame[pixel] != scene.pixels()[pixel]);
        differs_from_constant += static_cast<usize>(red_frame[pixel] != constant_frame[pixel]);
    }
    CY_CHECK(changed > 1000);
    CY_CHECK(differs_from_constant > 1000);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
#endif

CY_TEST_CASE("the frame is CAPTURED: the layer's callbacks put shaded texels on the device") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    FrameScene scene(allocator());
    const Status built = scene.build(fixture.device());
    if (!built) {
        std::fprintf(stderr, "pipeline build failed: %s\n", built.error().message);
    }
    CY_REQUIRE(built.has_value());
    scene.set_read_back(true);

    rendering::assembly::AssemblyReport report;
    CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
    CY_CHECK(report.executed);
    CY_CHECK_EQ(scene.recorded().passes, 6U);
    CY_CHECK_EQ(scene.recorded().temporal_resolves, 1U);
    CY_CHECK_EQ(scene.recorded().opaque_draws, report.draws);
    // A frame that renders but trips validation is not a frame that works.
    CY_CHECK_EQ(fixture.validation_errors(), 0U);

    const u32 shaded = shaded_texels(scene.pixels());
    std::fprintf(stderr, "with callbacks: %u of %u texels shaded, %u draws, %u passes recorded\n",
                 shaded, kWidth * kHeight, scene.recorded().opaque_draws, scene.recorded().passes);
    // Twelve cubes in front of the camera cover a real fraction of a 480x270 frame. The bound is
    // deliberately far from what the scene produces: it is a check that ANYTHING was drawn, and the
    // case that follows is what makes it a check that the RIGHT thing was.
    CY_CHECK_GT(shaded, 2000U);
    save("pipeline-frame-with-callbacks.png", scene.pixels());
}

CY_TEST_CASE("the identical frame WITHOUT the callbacks is blank, and that difference is 1b") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    FrameScene scene(allocator());
    CY_REQUIRE(scene.build(fixture.device()).has_value());
    scene.set_read_back(true);

    // THE BEFORE. Same scene, same assembly, same graph, same device frame — an empty `FrameSinks`.
    rendering::assembly::AssemblyReport blank;
    CY_REQUIRE(scene.render(RecordMode::None, blank).has_value());
    CY_CHECK(blank.executed);
    CY_CHECK_GT(blank.draws, 0U);
    CY_CHECK_EQ(scene.recorded().passes, 0U);
    Array<u32> before(allocator());
    CY_REQUIRE(before.resize(scene.pixels().size()).has_value());
    for (usize index = 0; index < before.size(); ++index) {
        before[index] = scene.pixels()[index];
    }
    CY_CHECK_EQ(scene.differing_texels(before.span()), 0U);
    const u32 blank_shaded = shaded_texels(before.span());
    save("pipeline-frame-without-callbacks.png", before.span());

    // THE AFTER.
    rendering::assembly::AssemblyReport recorded;
    CY_REQUIRE(scene.render(RecordMode::Callbacks, recorded).has_value());
    const u32 shaded = shaded_texels(scene.pixels());
    const u32 differing = scene.differing_texels(before.span());
    std::fprintf(stderr, "before: %u shaded; after: %u shaded; %u of %u texels differ\n",
                 blank_shaded, shaded, differing, kWidth * kHeight);

    // The assembly decided the same frame both times — the layer changes what is RECORDED and
    // nothing else.
    CY_CHECK_EQ(recorded.draws, blank.draws);
    CY_CHECK_EQ(recorded.passes_declared, blank.passes_declared);
    // And the pictures are different. Remove the callbacks and this number is zero.
    CY_CHECK_GT(differing, 2000U);
    CY_CHECK_GT(shaded, blank_shaded);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the particle renderer draws through the layer, and the picture says so") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    FrameScene scene(allocator());
    CY_REQUIRE(scene.build(fixture.device()).has_value());
    scene.set_read_back(true);

    rendering::assembly::AssemblyReport without;
    CY_REQUIRE(scene.render(RecordMode::Callbacks, without).has_value());
    Array<u32> plain(allocator());
    CY_REQUIRE(plain.resize(scene.pixels().size()).has_value());
    for (usize index = 0; index < plain.size(); ++index) {
        plain[index] = scene.pixels()[index];
    }

    rendering::assembly::AssemblyReport with;
    CY_REQUIRE(scene.render(RecordMode::CallbacksAndParticles, with).has_value());
    const u32 differing = scene.differing_texels(plain.span());
    std::fprintf(stderr, "%u particles in one draw changed %u of %u texels\n",
                 scene.particle_report().particles, differing, kWidth * kHeight);

    CY_CHECK_EQ(scene.recorded().extensions_run, 1U);
    CY_CHECK_EQ(scene.particle_report().draws, 1U);
    CY_CHECK_EQ(scene.particle_report().particles, kParticleCount);
    // The effect is composited into the frame's own colour target through a pipeline whose layout
    // is compatible with the frame's. If the set layouts had diverged, the sets the recorder bound
    // would have been invalidated and this number would be zero — with validation errors beside it.
    CY_CHECK_GT(differing, 500U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
    save("pipeline-frame-particles.png", scene.pixels());
}

CY_TEST_CASE("many frames on the device, and a teardown while it is still busy") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    auto* scene = new FrameScene(allocator());
    CY_REQUIRE(scene->build(fixture.device()).has_value());

    // A one-frame suite cannot see a defect that begins at frame 3 — M4's `render.frames` suite's
    // own sentence, and the reason the ring is read after EVERY frame rather than at the end.
    const u32 turns = (fixture.device().frames_in_flight() * 8U) + 1U;
    for (u32 frame = 0; frame < turns; ++frame) {
        rendering::assembly::AssemblyReport report;
        CY_REQUIRE(scene->render(RecordMode::CallbacksAndParticles, report).has_value());
        if (fixture.validation_errors() != 0U) {
            std::fprintf(stderr, "validation first spoke at frame %u\n", frame);
        }
        CY_REQUIRE_EQ(fixture.validation_errors(), 0U);
    }
    delete scene;
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

// ================================================================================================
// TEMPORAL ANTI-ALIASING, ON THE DEVICE. M11.c task 3.4.
// ================================================================================================
//
// `temporal_resolves == 1` above says the resolve RAN. These say what it DID, with the same scene,
// on the same device, and with the negative control inside the case rather than in a reader's
// head. Three claims, each of which a working-looking frame can get wrong:
//
//   1. IT ACCUMULATES. A still camera under a moving jitter changes its picture every frame; a
//      resolve that blends history damps that change and one that does not passes it straight
//      through. The control is the same scene with the history CUT every frame — the invalidation
//      path — which is exactly "a temporal pass that did nothing", and the two are compared.
//   2. IT ANTI-ALIASES. A pinned capture must still MOVE its jitter. The frame after sixteen is a
//      picture no single frame drew: its edges are averages.
//   3. IT IS DETERMINISTIC. `temporal-rendering`'s "Determinism and capture": two runs in pinned
//      mode give identical temporal state and identical images. Byte for byte, not within a
//      tolerance — every committed reference in this tree depends on it.

namespace {

inline constexpr u32 kTemporalFrames = 16;
/// `JitterConfig::length`'s default: the frames after which every jitter phase has been drawn once.
inline constexpr u32 kJitterCycle = 8;

/// Mean absolute difference per colour channel, in 8-bit steps, over the whole frame.
[[nodiscard]] double mean_delta(Span<const u32> a, Span<const u32> b) noexcept {
    if (a.size() != b.size() || a.empty()) {
        return -1.0;
    }
    u64 total = 0;
    for (usize index = 0; index < a.size(); ++index) {
        for (u32 channel = 0; channel < 3U; ++channel) {
            const auto x = static_cast<i32>((a[index] >> (channel * 8U)) & 0xFFU);
            const auto y = static_cast<i32>((b[index] >> (channel * 8U)) & 0xFFU);
            total += static_cast<u64>(x > y ? x - y : y - x);
        }
    }
    return static_cast<double>(total) / (static_cast<double>(a.size()) * 3.0);
}

[[nodiscard]] Status keep(Array<u32>& into, Span<const u32> texels) noexcept {
    if (Status sized = into.resize(texels.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < texels.size(); ++index) {
        into[index] = texels[index];
    }
    return ok();
}

/// What a run of `kTemporalFrames` frames left behind: the first frame, the last two, and whether
/// every frame after the first blended a history.
struct TemporalRun {
    explicit TemporalRun(Allocator& alloc) noexcept
        : first(alloc), penultimate(alloc), last(alloc), sums(alloc), cycle_average(alloc) {}
    Array<u32> first;
    Array<u32> penultimate;
    Array<u32> last;
    /// Per-channel sums over the last `kJitterCycle` frames, and their average: with the history
    /// cut every frame, that is the box-filtered picture of every jitter phase once — what an
    /// accumulating resolve on a still camera should approach.
    Array<u32> sums;
    Array<u32> cycle_average;
    u32 resolves = 0;
    u32 invalidated = 0;
    Vec2 last_jitter{0.0F, 0.0F};
    bool pinned = false;
};

void accumulate(TemporalRun& out, Span<const u32> texels) noexcept {
    if (out.sums.size() != texels.size() * 3U && !out.sums.resize(texels.size() * 3U)) {
        return;
    }
    for (usize index = 0; index < texels.size(); ++index) {
        for (u32 channel = 0; channel < 3U; ++channel) {
            out.sums[(index * 3U) + channel] += (texels[index] >> (channel * 8U)) & 0xFFU;
        }
    }
}

/// Build `FrameScene`, pin its jitter at index 0, and render. With `cut_every_frame` the history
/// is invalidated through the framework's own `signal_cut` before each frame — the control.
[[nodiscard]] Status run_temporal(rhi::Device& device, bool cut_every_frame,
                                  TemporalRun& out) noexcept {
    FrameScene scene(allocator());
    if (Status built = scene.build(device); !built) {
        return built;
    }
    scene.set_read_back(true);
    scene.assembly().temporal().pin_jitter(0);
    for (u32 frame = 0; frame < kTemporalFrames; ++frame) {
        if (cut_every_frame) {
            scene.assembly().temporal().signal_cut(rendering::TemporalInvalidation::Explicit);
        }
        rendering::assembly::AssemblyReport report;
        if (Status rendered = scene.render(RecordMode::Callbacks, report); !rendered) {
            return rendered;
        }
        out.resolves += scene.recorded().temporal_resolves;
        out.invalidated += report.temporal_invalidated ? 1U : 0U;
        out.last_jitter = report.jitter;
        out.pinned = report.jitter_pinned;
        if (frame + kJitterCycle >= kTemporalFrames) {
            accumulate(out, scene.pixels());
        }
        if (frame == 0) {
            if (Status kept = keep(out.first, scene.pixels()); !kept) {
                return kept;
            }
        } else if (frame + 2U == kTemporalFrames) {
            if (Status kept = keep(out.penultimate, scene.pixels()); !kept) {
                return kept;
            }
        }
    }
    if (Status sized =
            out.cycle_average.resize(out.last.empty() ? scene.pixels().size() : out.last.size());
        !sized) {
        return sized;
    }
    for (usize index = 0; index < out.cycle_average.size(); ++index) {
        u32 packed = 0xFF000000U;
        for (u32 channel = 0; channel < 3U; ++channel) {
            const u32 sum = out.sums[(index * 3U) + channel];
            packed |= ((sum + (kJitterCycle / 2U)) / kJitterCycle) << (channel * 8U);
        }
        out.cycle_average[index] = packed;
    }
    return keep(out.last, scene.pixels());
}

/// Texels that differ by more than one 8-bit step in any channel.
[[nodiscard]] u32 differing(Span<const u32> a, Span<const u32> b) noexcept {
    u32 count = 0;
    for (usize index = 0; index < a.size() && index < b.size(); ++index) {
        for (u32 channel = 0; channel < 3U; ++channel) {
            const auto x = static_cast<i32>((a[index] >> (channel * 8U)) & 0xFFU);
            const auto y = static_cast<i32>((b[index] >> (channel * 8U)) & 0xFFU);
            if (x - y > 1 || y - x > 1) {
                ++count;
                break;
            }
        }
    }
    return count;
}

}  // namespace

CY_TEST_CASE("temporal anti-aliasing accumulates a pinned history into a picture no frame drew") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
#if defined(CY_TEST_PIPELINE_METAL)
    // The native Metal frame's geometry capture is black (see the first case), so there is no
    // picture here to measure a resolve against.
    return;
#endif
    TemporalRun blended(allocator());
    CY_REQUIRE(run_temporal(fixture.device(), false, blended).has_value());
    TemporalRun control(allocator());
    CY_REQUIRE(run_temporal(fixture.device(), true, control).has_value());

    const double settle = mean_delta(blended.last.span(), blended.penultimate.span());
    const double settle_control = mean_delta(control.last.span(), control.penultimate.span());
    const double moved = mean_delta(blended.last.span(), blended.first.span());
    const u32 edges = differing(blended.last.span(), blended.first.span());
    // HOW CLOSE EACH GETS TO THE BOX-FILTERED PICTURE, which is the cut control's eight phases
    // averaged. A resolve that reprojects a still camera's history to anywhere but the same texel
    // resamples it every frame, and that shows here as a blur the average does not have.
    const double to_average = mean_delta(blended.last.span(), control.cycle_average.span());
    const double single_to_average = mean_delta(control.last.span(), control.cycle_average.span());
    std::fprintf(stderr,
                 "temporal: against the %u-phase average, the resolved frame is %.4f/255 away and "
                 "a single jittered frame %.4f/255\n",
                 kJitterCycle, to_average, single_to_average);
    save("pipeline-temporal-phase-average.png", control.cycle_average.span());
    std::fprintf(stderr,
                 "temporal: frame-to-frame change %.4f/255 blended against %.4f/255 cut every "
                 "frame; frame %u against frame 1: %u texels, mean %.4f/255; jitter (%.3f, %.3f) "
                 "%s\n",
                 settle, settle_control, kTemporalFrames, edges, moved,
                 static_cast<double>(blended.last_jitter.x),
                 static_cast<double>(blended.last_jitter.y), blended.pinned ? "pinned" : "free");
    save("pipeline-temporal-first.png", blended.first.span());
    save("pipeline-temporal-converged.png", blended.last.span());

    // The resolve ran every frame in both runs, and only the control was ever invalidated past the
    // first frame. Without this the comparison below could be between two runs of nothing.
    CY_CHECK_EQ(blended.resolves, kTemporalFrames);
    CY_CHECK_EQ(control.resolves, kTemporalFrames);
    CY_CHECK(blended.pinned);
    CY_CHECK_LE(blended.invalidated, 1U);
    CY_CHECK_EQ(control.invalidated, kTemporalFrames);

    // 2 FIRST, because it is what makes 1 mean anything: THE PINNED JITTER MOVES. With it held at
    // one sample the cut-every-frame control draws the same picture every frame, and so does the
    // blended run — accumulation over identical frames is invisible and anti-aliases nothing.
    CY_CHECK_GT(settle_control, 0.02);
    CY_CHECK_GT(edges, 200U);

    // 1. THE HISTORY IS BLENDED: frame-to-frame change is damped well below what the jitter alone
    //    produces. A feedback of 0.9 predicts about a tenth; half is the bound, far from both.
    CY_CHECK_LT(settle, settle_control * 0.5);
    // 3. AND IT APPROACHES THE BOX-FILTERED PICTURE rather than blurring past it. Measured 0.426
    //    against 0.821 for a single jittered frame; the resolve that re-sampled its history at the
    //    wrong offset every frame measured 0.624, which this bound refuses.
    CY_CHECK_LT(to_average, single_to_average * 0.7);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("temporal anti-aliasing is deterministic: two pinned runs draw the identical frame") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    TemporalRun one(allocator());
    CY_REQUIRE(run_temporal(fixture.device(), false, one).has_value());
    TemporalRun two(allocator());
    CY_REQUIRE(run_temporal(fixture.device(), false, two).has_value());

    CY_REQUIRE_EQ(one.last.size(), two.last.size());
    u32 unequal = 0;
    for (usize index = 0; index < one.last.size(); ++index) {
        unequal += one.last[index] != two.last[index] ? 1U : 0U;
    }
    std::fprintf(stderr, "temporal determinism: %u of %zu texels differ after %u pinned frames\n",
                 unequal, one.last.size(), kTemporalFrames);
    // BYTE FOR BYTE. A tolerance here would be a tolerance in every golden image downstream.
    CY_CHECK_EQ(unequal, 0U);
    CY_CHECK_EQ(one.last_jitter.x, two.last_jitter.x);
    CY_CHECK_EQ(one.last_jitter.y, two.last_jitter.y);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
