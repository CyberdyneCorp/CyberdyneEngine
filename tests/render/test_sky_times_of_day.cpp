// THE SKY, AS AN IMAGE. M11.c task 5.2, `m11c:sky-as-an-image`.
//
// ================================================================================================
// WHY A PICTURE AND NOT A TABLE
// ================================================================================================
//
// `atmosphere-sky-and-clouds` has been measured by five criteria over four suites, and every one of
// them reads a NUMBER: a transmittance, a radiance at a direction, a count of directions
// integrated, a cell of a cloud shadow field. The row's own requirement is not about numbers —
// "aerial perspective on distant geometry consistent with the sky rather than a separately tuned
// fog" is a claim about what the sky LOOKS like, and a table cannot carry it.
//
// The weak form of this check existed and is what M11.c's gate refused: `just test-render sky
// --times-of-day 4 --compare-golden` selected the same seventeen render cases as `just test-render`
// with no arguments at all, because ctest discards a positional argument it does not recognise.
// Nothing photographed the sky and nothing compared a photograph. This suite is the strong form: it
// renders the sky, it compares it against committed references, and it fails when the atmosphere
// stops working.
//
// ================================================================================================
// FOUR TIMES OF DAY, AND WHY ONE WOULD NOT DO
// ================================================================================================
//
// The claim the row makes is about the atmosphere's RESPONSE TO SUN ELEVATION: Rayleigh scattering
// reddening a long path at dawn, the ozone layer holding the zenith blue after the sun has gone,
// multiple scattering carrying a third of the horizon at noon and none of it at midnight. One
// still frame is consistent with a painted gradient. Four at +0.62, +61.44, -5.64 and -14.56
// degrees of sun elevation are not — the numbers `solve_celestial()` answered and every run prints
// — and each case here asserts that its frame matches ITS OWN reference and
// matches NONE OF THE OTHER THREE — so a renderer that had quietly stopped responding to the sun
// would produce four frames that agree, and agreement is what these cases fail on.
//
// The elevations are not chosen, either: the times are hours of a real clock at 52 degrees north on
// the June solstice, and `solve_celestial()` answers what the sun does there. Each case asserts the
// elevation it was photographed at, so a reference cannot silently become a reference of a
// different sky.
//
// ================================================================================================
// WHAT IS ON THE PROCESSOR AND WHAT IS ON THE DEVICE — STATED, BECAUSE IT MATTERS
// ================================================================================================
//
// There is no atmosphere evaluated in a shader anywhere in this engine. `samples/10-world` and
// `samples/12-beauty` both draw the sky as triangles whose every vertex carries the radiance
// `rendering::sky::compose_sky()` answered for its direction, and the fragment stage exposes and
// tone maps what the rasteriser interpolated. This suite does exactly that and nothing else, so
// that what it photographs is the engine's sky path rather than a second sky written for a test:
//
//   on the processor   `compose_sky()` per grid vertex — the atmosphere's single scattering through
//                      the transmittance table, the tabulated multiple scattering, the sun's disc,
//                      and the cloud march through the reconstructed density field.
//   on the device      the rasteriser's interpolation across each cell of the grid, the exposure
//                      divide, Reinhard, the gamma encode and the quantisation to `Rgba8Unorm`.
//
// The grid is the dome tessellated IN THE PROJECTION rather than in latitude and longitude: every
// part of the picture then resolves the sky equally, and the tessellation is a property of the
// image the reference is of rather than of a radius nobody can see in it. `kVertexStride` is 2
// pixels, which at 192x108 is 97x55 = 5 335 directions composed per frame.
//
// ================================================================================================
// THE EXPOSURE IS A COMMITTED CONSTANT PER TIME OF DAY, AND NOT AN AUTO-EXPOSURE
// ================================================================================================
//
// `samples/10-world` exposes each frame on the sky's own mean radiance, which is what a camera
// does and exactly what this suite must not do: an auto-exposure DIVIDES OUT any change to the
// atmosphere's overall brightness, so a mutation that halved every scattering coefficient would
// produce very nearly the same picture. The four numbers in `kTimes` were measured once, from the
// mean sky luminance at each time (printed by every run, so the reader can see what they were
// derived from), and then written down. A picture taken at a fixed exposure responds to the
// atmosphere's ABSOLUTE radiance, which is the property the reference is meant to be able to fail
// on.
//
// They differ by six orders of magnitude between noon and midnight because the sky does. Exposing
// midnight at noon's setting would commit a black rectangle, and a black rectangle is a reference
// that cannot fail.
//
// ================================================================================================
// THE TOLERANCE IS MEASURED, AND THE MEASUREMENT IS A CASE IN THIS SUITE
// ================================================================================================
//
// `golden.h`'s `kChannelTolerance` is 2 eight-bit steps and its derivation is the quantisation of a
// gamma-encoded target plus one step of headroom. That derivation is about A frame; what it does
// not say is how much THIS frame actually moves, and a tolerance nobody measured against its own
// picture is a round number that hides a regression.
//
// So it is measured here, twice over, and the numbers are printed by the run:
//
//   RUN TO RUN      the same time of day rendered twice, through two independent submissions.
//                   Measured on this host: 0 texels differ at all, worst channel delta 0. The
//                   composition is deterministic and the rasteriser is, so this is the expected
//                   answer and it is asserted rather than assumed — a non-zero here would mean the
//                   picture had a source of variation nobody declared.
//   ONE ULP IN      the whole frame's exposure scale perturbed by one unit in the last place of an
//                   f32 (a relative 1.19e-7) and rendered again. That is the smallest difference an
//                   implementation of `pow` or of the interpolation can have, and what it moves is
//                   what a conformant second implementation could move. Measured on this host:
//                   NO texel of the four frames moves past the tolerance, and the largest single
//                   channel it moves at all is 1 step — at dawn; noon, dusk and night do not move.
//
// One step is therefore the physical difference and two is one step of headroom over it, which is
// what `kChannelTolerance` already is — so the number is not changed, it is EARNED. The case that
// measures it asserts both bounds, so the derivation cannot quietly stop being true.
//
// ================================================================================================
// WHAT THIS SUITE REFUSES TO REPORT
// ================================================================================================
//
// Three ways a golden-image case passes while measuring nothing, each asserted before any
// comparison:
//
//   * THE DRAW NEVER RAN. The readback buffer is prefilled with a sentinel no tone map can produce
//     — 0xFF00FF (magenta, and this sky has no magenta in it) — and a frame that still carries one
//     is counted rather than inferred.
//   * THE PICTURE IS FLAT. A frame of one colour compares perfectly against a reference of that
//     colour and says nothing about an atmosphere. Every case counts the distinct texel values in
//     its own frame and requires hundreds, and separately requires the darkest and brightest
//     luminance in the frame to be far apart — a sky is a gradient or it is not a sky.
//   * THE FOUR ARE THE SAME PICTURE. Each case compares its frame against the other three
//     references and requires a large mean absolute difference, which is the "response to sun
//     elevation" claim made falsifiable rather than described.
//
// ================================================================================================
// THE MUTATIONS THIS SUITE WAS PROVED RED BY
// ================================================================================================
//
// A golden image whose subject can be disabled while it stays green is the defect this project has
// shipped nine times, so the subject was disabled three ways and the pixels were counted. The
// DECLARED one is m11c.toml's `[criterion.falsifies]` and it is the first below; the other two were
// run by hand on this host, on build/m11c-final, and restored.
//
//   THE SUN STOPS MOVING. `delete-lines` of `state.sun.direction = horizon_direction(latitude,
//   declination, hour_angle);` in src/rendering/sky/src/celestial.cpp. The tree still builds and
//   `CelestialState` keeps its default +Y, so all four times of day are photographs of a sun at the
//   zenith — exactly the "renderer that had quietly stopped responding to the sun" this suite's
//   four references exist to refute. ALL 20 736 TEXELS of dawn, dusk and night move and 19 313 of
//   noon, worst channel delta 209, 144, 251 and 255; the flatness guard fires too (night collapses
//   to 1 distinct colour) and so does the elevation assertion. 4 of 5 cases red.
//
//   THE CLOUDS LEAVE THE PICTURE. `delete-lines` of `background = (background *
//   clouds.transmittance) + clouds.scattering;` in src/rendering/sky/src/composition.cpp. The march
//   still runs and its result still reaches `SkyCompositionSample`; what stops is the compositing,
//   so nothing a table reads changes. 15 555, 16 303, 11 323 and 16 004 of 20 736 texels move,
//   worst channel delta 163, 150, 195 and 187. 4 of 5 cases red.
//
//   MIE SCATTERS ISOTROPICALLY. `mie_phase(cos_theta, atmosphere.mie_anisotropy)` substituted with
//   `mie_phase(cos_theta, 0.0F)` in src/rendering/sky/src/tables.cpp — the forward lobe that makes
//   the sky bright around the sun, and the subtlest of the three. 523 texels move at dawn (worst
//   channel delta 56) and 114 at noon (worst 6); DUSK AND NIGHT DO NOT MOVE AT ALL, because the sun
//   is below the horizon and there is no forward lobe left to lose. 2 of 5 cases red — which is the
//   measurement that says the 2-step tolerance is tight enough to catch a physical regression that
//   moves half a percent of one frame.
//
// ================================================================================================
// REGENERATING A REFERENCE
// ================================================================================================
//
// `CY_RENDER_UPDATE_GOLDEN=1 ctest -R render.sky_times_of_day` writes each reference and then
// FAILS, naming what it wrote — `test_golden_frame.cpp`'s mechanism and its argument: a mode that
// regenerated and then passed would let a run with the variable set in its environment launder a
// defect into the repository.

#include <cy/test/test.h>

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/backends/rhi/device.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/sky/atmosphere.h>
#include <cy/rendering/sky/celestial.h>
#include <cy/rendering/sky/clouds.h>
#include <cy/rendering/sky/composition.h>
#include <cy/rendering/sky/tables.h>
#include <cy/world/coordinates.h>

#include "device.h"
#include "golden.h"
#include "shaders/sky_frame_spirv.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cy::render_test {
namespace {

using rendering::PassContext;
using rendering::ResourceId;
using rhi::Access;
using rhi::QueueKind;
namespace sky = rendering::sky;

// --- The picture
// ----------------------------------------------------------------------------------

/// The same 192x108 `render.golden` commits, and for the same reason: a reference is a committed
/// binary this repository's encoder stores rather than deflates, so one costs about 62 KiB. Four of
/// them is a quarter of a megabyte, which is what four times of day are worth.
constexpr u32 kWidth = 192;
constexpr u32 kHeight = 108;
constexpr u32 kTexels = kWidth * kHeight;

/// Pixels between grid vertices. See the header: the dome is tessellated in the projection.
constexpr u32 kVertexStride = 2;
constexpr u32 kGridColumns = (kWidth / kVertexStride) + 1;
constexpr u32 kGridRows = (kHeight / kVertexStride) + 1;
constexpr u32 kVertexCount = kGridColumns * kGridRows;
constexpr u32 kIndexCount = (kGridColumns - 1) * (kGridRows - 1) * 6;

/// THE FRAME IS THE WHOLE SKY, AND NOT A CAMERA'S VIEW OF PART OF IT.
///
/// A pinhole camera at a fixed heading photographs the sun at noon and misses it at every other
/// time of day — at 52 degrees north on the June solstice the sun rises at azimuth 51 and sets at
/// 321, and no fixed field of view holds both of those and the south at once. A camera that TURNED
/// to follow the sun would photograph the same picture four times and call it a response to
/// elevation, which is the one thing these four references exist to refute.
///
/// So the image is an equirectangular panorama: the horizontal axis is the whole 360 degrees of
/// azimuth with south at the centre and north at both edges, and the vertical axis runs from 15
/// degrees below the horizon to the zenith. Every direction the sun can be in is in the frame at
/// every time of day, the horizon is a line across it, and the sun's own azimuth moves across the
/// picture as the day turns — which is a second thing the four references can fail on.
constexpr f32 kAzimuthCentreDegrees = 180.0F;
constexpr f32 kElevationLowDegrees = -15.0F;
constexpr f32 kElevationHighDegrees = 90.0F;
/// Eye height, in metres. Two metres above the ground: a person's, and the altitude at which a
/// horizon is a horizon.
constexpr f32 kEyeAltitudeMetres = 2.0F;

constexpr f32 kPi = 3.14159265358979323846F;

[[nodiscard]] f32 radians_of(f32 degrees) noexcept {
    return degrees * (kPi / 180.0F);
}

/// One time of day: what it is called, where the clock is, and what it is exposed at.
struct TimeOfDayCase {
    const char* name;
    /// In [0, 1). 0 is midnight, 0.5 is local noon — `sky::TimeOfDay`'s own domain.
    f32 fraction;
    /// The mean sky luminance this time was measured at, in nits, and therefore the divisor the
    /// picture is exposed by. See the header for why this is a constant and not an auto-exposure.
    f32 exposure_nits;
    /// The sun elevation `solve_celestial()` answers at this fraction, in degrees. Asserted, so a
    /// reference cannot become a reference of a different sky without a case going red.
    f32 sun_elevation_degrees;
};

/// 03:50, 12:00, 21:07 and 00:00 at 52 degrees north on the June solstice — the sun just up, the
/// sun at its highest, the sun six degrees down in the west, and the darkest the sky gets at this
/// latitude on this day. FOUR DISTINCT ELEVATIONS, which is what the row's claim needs and what an
/// obvious choice of 06:00 and 18:00 does NOT give: those two are symmetric about local noon, so
/// the sun stands at the same 18.27 degrees in both and two of the four references would have been
/// photographs of one elevation. Measured, not assumed — the first run of this suite printed them.
///
/// The names are the reference file names.
constexpr TimeOfDayCase kTimes[4] = {
    {"sky_dawn", 0.16F, 382.209F, 0.618F},
    {"sky_noon", 0.50F, 9480.26F, 61.440F},
    {"sky_dusk", 0.88F, 3.66650F, -5.641F},
    {"sky_night", 0.00F, 0.00218226F, -14.560F},
};

constexpr f32 kLatitudeDegrees = 52.0F;
constexpr f32 kDayOfYear = 172.0F;  // the June solstice, `TimeOfDay`'s own default
/// The seed the cloud map is generated from. Any constant would do; this one is written down so
/// that the weather in the four references is the same weather and a reader can reproduce it.
constexpr u64 kCloudSeed = 0xC10'D5EULL;

/// The sentinel the readback buffer is prefilled with, so that a draw that never ran is COUNTED
/// rather than inferred from a plausible-looking picture. Opaque magenta: `tonemap()` is monotonic
/// per channel and this sky has no channel pairing that produces it.
constexpr u32 kNeverWritten = 0xFFFF00FFU;

struct GridVertex {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 r = 0.0F;
    f32 g = 0.0F;
    f32 b = 0.0F;
};

struct SkyFramePush {
    f32 scale = 1.0F;
    f32 pad0 = 0.0F;
    f32 pad1 = 0.0F;
    f32 pad2 = 0.0F;
};

// --- The directions
// -------------------------------------------------------------------------------

/// The direction a point of the image looks along, in the engine's own frame: +Y up, +X east, -Z
/// north — `celestial.cpp`'s frame, which is where the sun's direction comes from, so the sun in
/// the picture is the sun the model placed.
///
/// Clip +Y is the TOP of the target — the engine's viewport has a negative height,
/// `VulkanCommandBuffer::set_viewport` flips it — so a vertex at +1 is the zenith and the horizon
/// is a line near the bottom, which is where a reader expects to find it.
[[nodiscard]] Vec3 ray_through(f32 clip_x, f32 clip_y) noexcept {
    const f32 azimuth = radians_of(kAzimuthCentreDegrees + (clip_x * 180.0F));
    const f32 middle = (kElevationHighDegrees + kElevationLowDegrees) * 0.5F;
    const f32 half = (kElevationHighDegrees - kElevationLowDegrees) * 0.5F;
    const f32 elevation = radians_of(middle + (clip_y * half));
    return Vec3{std::cos(elevation) * std::sin(azimuth), std::sin(elevation),
                -std::cos(elevation) * std::cos(azimuth)};
}

// --- The sky
// --------------------------------------------------------------------------------------

/// Everything `compose_sky()` needs, built once per case: the atmosphere, its tables, the weather
/// map and the layers the reconstruction reads. The configuration is `samples/10-world`'s, because
/// a sky photographed at settings nothing ships is a sky nobody has seen.
class SkyFixture {
public:
    explicit SkyFixture(Allocator& allocator) noexcept : tables_(allocator), map_(allocator) {}

    [[nodiscard]] Status build() noexcept {
        atmosphere_ = sky::Atmosphere{};
        if (Status configured = tables_.configure(sky::SkyTableQuality::Medium); !configured) {
            return configured;
        }
        if (Expected<bool, Error> built = tables_.build(atmosphere_); !built) {
            return make_unexpected(built.error());
        }
        model_ = sky::CelestialModel{};
        model_.latitude_degrees = kLatitudeDegrees;

        if (Status configured = map_.configure(40, 1'000.0F); !configured) {
            return configured;
        }
        if (Status generated = map_.generate(kCloudSeed, 0.45F, 0.3F); !generated) {
            return generated;
        }
        layers_ = sky::default_cloud_layers();
        field_ = sky::CloudField{};
        field_.map = &map_;
        field_.layers = layers_;
        field_.seed = kCloudSeed;

        quality_ = sky::CloudQuality{};
        quality_.steps = 32;
        quality_.light_steps = 4;
        quality_.octaves = 3;
        quality_.multiple_scattering = true;
        return ok();
    }

    /// The composition inputs at one time of day. NO STARS: `samples/10-world` draws them as the
    /// points they are rather than through the dome, for the reason its own comment gives — a dome
    /// affordable enough to march clouds through would turn a star into a lozenge.
    [[nodiscard]] sky::SkyCompositionInputs at(const TimeOfDayCase& when) noexcept {
        sky::TimeOfDay time;
        time.fraction = when.fraction;
        time.day_of_year = kDayOfYear;
        celestial_ = sky::solve_celestial(model_, time);

        sky::SkyCompositionInputs inputs;
        inputs.atmosphere = &atmosphere_;
        inputs.tables = &tables_;
        inputs.celestial = &celestial_;
        inputs.stars = nullptr;
        inputs.clouds = &field_;
        inputs.cloud_quality = quality_;
        inputs.time_seconds = static_cast<f64>(when.fraction) * 86'400.0;
        inputs.view = sky::planetary_view(
            atmosphere_, world::WorldVec3d{0.0, static_cast<f64>(kEyeAltitudeMetres), 0.0});
        return inputs;
    }

    [[nodiscard]] f32 sun_elevation_degrees() const noexcept {
        return std::asin(celestial_.sun.direction.y) * (180.0F / kPi);
    }

private:
    sky::Atmosphere atmosphere_;
    sky::AtmosphereTables tables_;
    sky::CelestialModel model_;
    sky::CelestialState celestial_;
    sky::CloudWeatherMap map_;
    sky::CloudLayerSet layers_;
    sky::CloudField field_;
    sky::CloudQuality quality_;
};

/// Compose the grid's radiances, and report the mean luminance over it — the number the exposure
/// constants in `kTimes` were derived from, printed by every run so that the derivation is visible
/// rather than asserted.
[[nodiscard]] Status compose_grid(SkyFixture& fixture, const TimeOfDayCase& when,
                                  Array<GridVertex>& vertices, f32& mean_luminance) noexcept {
    const sky::SkyCompositionInputs inputs = fixture.at(when);
    if (Status sized = vertices.resize(kVertexCount); !sized) {
        return sized;
    }
    f64 total = 0.0;
    for (u32 row = 0; row < kGridRows; ++row) {
        for (u32 column = 0; column < kGridColumns; ++column) {
            const f32 clip_x =
                (static_cast<f32>(column) / static_cast<f32>(kGridColumns - 1) * 2.0F) - 1.0F;
            const f32 clip_y =
                1.0F - (static_cast<f32>(row) / static_cast<f32>(kGridRows - 1) * 2.0F);
            const Vec3 direction = ray_through(clip_x, clip_y);
            const sky::SkyCompositionSample composed = sky::compose_sky(inputs, direction);
            GridVertex& vertex = vertices[(static_cast<usize>(row) * kGridColumns) + column];
            vertex.x = clip_x;
            vertex.y = clip_y;
            vertex.r = composed.radiance.x;
            vertex.g = composed.radiance.y;
            vertex.b = composed.radiance.z;
            total +=
                static_cast<f64>((composed.radiance.x * 0.2126F) + (composed.radiance.y * 0.7152F) +
                                 (composed.radiance.z * 0.0722F));
        }
    }
    mean_luminance = static_cast<f32>(total / kVertexCount);
    return ok();
}

void build_indices(u32* out) noexcept {
    u32 next = 0;
    for (u32 row = 0; row + 1 < kGridRows; ++row) {
        for (u32 column = 0; column + 1 < kGridColumns; ++column) {
            const u32 top_left = (row * kGridColumns) + column;
            const u32 top_right = top_left + 1;
            const u32 bottom_left = top_left + kGridColumns;
            const u32 bottom_right = bottom_left + 1;
            out[next++] = top_left;
            out[next++] = bottom_left;
            out[next++] = top_right;
            out[next++] = top_right;
            out[next++] = bottom_left;
            out[next++] = bottom_right;
        }
    }
}

// --- The frame
// ------------------------------------------------------------------------------------

struct PassState {
    rendering::GraphExecutor* executor = nullptr;
    rhi::GraphicsPipelineHandle pipeline;
    rhi::PipelineLayoutHandle layout;
    rhi::BufferHandle vertices;
    rhi::BufferHandle indices;
    rhi::BufferHandle readback;
    ResourceId color = rendering::kInvalidResource;
    SkyFramePush push{};
};

void record_draw(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<PassState*>(user);
    rhi::RenderAttachment color;
    color.view = state->executor->view(state->color);
    color.load = rhi::LoadOp::Clear;
    color.store = rhi::StoreOp::Store;
    // The clear is BLACK and not a stand-in sky, `samples/10-world`'s own argument: the grid covers
    // every pixel, so a frame in which the clear shows is a frame in which the draw failed.
    color.clear.color[3] = 1.0F;

    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, kWidth, kHeight};
    info.color_attachments = Span<const rhi::RenderAttachment>(&color, 1);

    context.commands->begin_rendering(info);
    context.commands->set_viewport(
        rhi::Viewport{0.0F, 0.0F, static_cast<f32>(kWidth), static_cast<f32>(kHeight), 0.0F, 1.0F});
    context.commands->set_scissor(rhi::Rect2D{0, 0, kWidth, kHeight});
    context.commands->bind_graphics_pipeline(state->pipeline);
    context.commands->push_constants(
        state->layout, rhi::ShaderStage::Fragment, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&state->push), sizeof(SkyFramePush)));
    const u64 offset = 0;
    context.commands->bind_vertex_buffers(0, Span<const rhi::BufferHandle>(&state->vertices, 1),
                                          Span<const u64>(&offset, 1));
    // `wide` is true: 32-bit indices, because the grid has more than 65 536 vertices at no stride
    // this file would want and a 16-bit index list would be a limit nobody could see in the
    // picture.
    context.commands->bind_index_buffer(state->indices, 0, true);
    context.commands->draw_indexed(kIndexCount, 1, 0, 0, 0);
    context.commands->end_rendering();
}

void record_readback(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<PassState*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{kWidth, kHeight, 1};
    context.commands->copy_texture_to_buffer(state->executor->texture(state->color),
                                             state->readback,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

/// The pipeline, the buffers and the one draw. Created once per case, as every device suite in this
/// directory does — see device.h for why a device is not shared between cases.
class SkyPipeline {
public:
    [[nodiscard]] Status create(rhi::Device& device) noexcept {
        device_ = &device;
        rhi::ShaderModuleDescription vertex;
        vertex.name = "sky frame vertex";
        vertex.stage = rhi::ShaderStage::Vertex;
        vertex.spirv =
            Span<const u32>(kSkyFrameVertexSpirv, sizeof(kSkyFrameVertexSpirv) / sizeof(u32));
        Expected<rhi::ShaderModuleHandle, Error> vertex_module =
            device.create_shader_module(vertex);
        if (!vertex_module) {
            return make_unexpected(vertex_module.error());
        }
        vertex_ = *vertex_module;

        rhi::ShaderModuleDescription fragment;
        fragment.name = "sky frame fragment";
        fragment.stage = rhi::ShaderStage::Fragment;
        fragment.spirv =
            Span<const u32>(kSkyFrameFragmentSpirv, sizeof(kSkyFrameFragmentSpirv) / sizeof(u32));
        Expected<rhi::ShaderModuleHandle, Error> fragment_module =
            device.create_shader_module(fragment);
        if (!fragment_module) {
            return make_unexpected(fragment_module.error());
        }
        fragment_ = *fragment_module;

        const rhi::PushConstantRange range{rhi::ShaderStage::Fragment, 0, sizeof(SkyFramePush)};
        rhi::PipelineLayoutDescription layout;
        layout.name = "sky frame layout";
        layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
        Expected<rhi::PipelineLayoutHandle, Error> layout_handle =
            device.create_pipeline_layout(layout);
        if (!layout_handle) {
            return make_unexpected(layout_handle.error());
        }
        layout_ = *layout_handle;

        rhi::ColorAttachmentState color;
        color.format = rhi::Format::Rgba8Unorm;
        const rhi::VertexBinding binding{0, sizeof(GridVertex), rhi::VertexInputRate::PerVertex};
        const rhi::VertexAttribute attributes[2] = {
            {0, 0, rhi::Format::Rg32Sfloat, 0},
            {1, 0, rhi::Format::Rgb32Sfloat, sizeof(f32) * 2},
        };
        rhi::GraphicsPipelineDescription pipeline;
        pipeline.name = "sky frame";
        pipeline.layout = layout_;
        pipeline.vertex_shader = vertex_;
        pipeline.fragment_shader = fragment_;
        pipeline.vertex_bindings = Span<const rhi::VertexBinding>(&binding, 1);
        pipeline.vertex_attributes = Span<const rhi::VertexAttribute>(attributes, 2);
        pipeline.color_attachments = Span<const rhi::ColorAttachmentState>(&color, 1);
        pipeline.rasterisation.cull_mode = rhi::CullMode::None;
        pipeline.depth_stencil.depth_test_enable = false;
        pipeline.depth_stencil.depth_write_enable = false;
        Expected<rhi::GraphicsPipelineHandle, Error> handle =
            device.create_graphics_pipeline(pipeline);
        if (!handle) {
            return make_unexpected(handle.error());
        }
        pipeline_ = *handle;

        rhi::BufferDescription vertices;
        vertices.name = "sky frame vertices";
        vertices.size = static_cast<u64>(kVertexCount) * sizeof(GridVertex);
        vertices.usage = rhi::BufferUsage::Vertex;
        vertices.memory = rhi::MemoryUse::Upload;
        Expected<rhi::BufferHandle, Error> vertex_buffer = device.create_buffer(vertices);
        if (!vertex_buffer) {
            return make_unexpected(vertex_buffer.error());
        }
        vertices_ = *vertex_buffer;

        rhi::BufferDescription indices;
        indices.name = "sky frame indices";
        indices.size = static_cast<u64>(kIndexCount) * sizeof(u32);
        indices.usage = rhi::BufferUsage::Index;
        indices.memory = rhi::MemoryUse::Upload;
        Expected<rhi::BufferHandle, Error> index_buffer = device.create_buffer(indices);
        if (!index_buffer) {
            return make_unexpected(index_buffer.error());
        }
        indices_ = *index_buffer;
        auto* mapped_indices = static_cast<u32*>(device.buffer_mapped_pointer(indices_));
        if (mapped_indices == nullptr) {
            return fail(ErrorCode::Internal, "the sky index buffer is not mapped");
        }
        build_indices(mapped_indices);

        rhi::BufferDescription readback;
        readback.name = "sky frame readback";
        readback.size = static_cast<u64>(kTexels) * sizeof(u32);
        readback.usage = rhi::BufferUsage::TransferDestination;
        readback.memory = rhi::MemoryUse::Readback;
        Expected<rhi::BufferHandle, Error> buffer = device.create_buffer(readback);
        if (!buffer) {
            return make_unexpected(buffer.error());
        }
        readback_ = *buffer;
        return ok();
    }

    ~SkyPipeline() {
        if (device_ == nullptr) {
            return;
        }
        (void)device_->wait_idle();
        device_->destroy_buffer(readback_);
        device_->destroy_buffer(indices_);
        device_->destroy_buffer(vertices_);
        device_->destroy_graphics_pipeline(pipeline_);
        device_->destroy_pipeline_layout(layout_);
        device_->destroy_shader_module(fragment_);
        device_->destroy_shader_module(vertex_);
    }

    SkyPipeline() noexcept = default;
    SkyPipeline(const SkyPipeline&) = delete;
    SkyPipeline& operator=(const SkyPipeline&) = delete;

    /// Draw the grid and copy the target back. `sentinels` is how many texels the draw did not
    /// write, which the case asserts is zero — see the header.
    [[nodiscard]] Status render(Allocator& allocator, const Array<GridVertex>& grid, f32 scale,
                                Image& out, u32& sentinels) noexcept {
        rhi::Device& device = *device_;
        auto* mapped = static_cast<GridVertex*>(device.buffer_mapped_pointer(vertices_));
        if (mapped == nullptr) {
            return fail(ErrorCode::Internal, "the sky vertex buffer is not mapped");
        }
        for (u32 index = 0; index < kVertexCount; ++index) {
            mapped[index] = grid[index];
        }
        auto* readback_texels = static_cast<u32*>(device.buffer_mapped_pointer(readback_));
        if (readback_texels == nullptr) {
            return fail(ErrorCode::Internal, "the sky readback buffer is not mapped");
        }
        for (u32 index = 0; index < kTexels; ++index) {
            readback_texels[index] = kNeverWritten;
        }

        if (Expected<u32, Error> began = device.begin_frame(); !began) {
            return make_unexpected(began.error());
        }
        rendering::RenderGraph graph(allocator);
        rendering::GraphExecutor executor(allocator, device);

        rendering::TextureRequest color_request;
        color_request.name = "sky frame colour";
        color_request.format = rhi::Format::Rgba8Unorm;
        color_request.width = kWidth;
        color_request.height = kHeight;
        const ResourceId color = graph.create_texture(color_request);

        rendering::BufferRequest readback_request;
        readback_request.name = "sky frame readback";
        readback_request.size = static_cast<u64>(kTexels) * sizeof(u32);
        readback_request.extra_usage = rhi::BufferUsage::TransferDestination;
        const ResourceId color_out = graph.import_buffer(readback_request, readback_);

        PassState state;
        state.executor = &executor;
        state.pipeline = pipeline_;
        state.layout = layout_;
        state.vertices = vertices_;
        state.indices = indices_;
        state.readback = readback_;
        state.color = color;
        state.push.scale = scale;

        graph.add_pass("sky frame draw", QueueKind::Graphics)
            .write(color, Access::ColorAttachmentWrite)
            .record(&record_draw, &state);
        graph.add_pass("sky frame readback", QueueKind::Graphics)
            .read(color, Access::TransferRead)
            .write(color_out, Access::TransferWrite)
            .record(&record_readback, &state);
        graph.add_pass("sky frame host", QueueKind::Graphics)
            .read(color_out, Access::HostRead)
            .side_effect();
        if (Status declared = graph.status(); !declared) {
            return declared;
        }
        Expected<rendering::ExecutionResult, Error> result =
            executor.execute(graph, rendering::CompileOptions{}, rendering::ExecuteOptions{});
        if (!result) {
            return make_unexpected(result.error());
        }
        if (Status idle = device.wait_idle(); !idle) {
            return idle;
        }
        if (Status ended = device.end_frame(); !ended) {
            return ended;
        }

        Array<u32> texels(allocator);
        if (Status sized = texels.resize(kTexels); !sized) {
            return sized;
        }
        sentinels = 0;
        for (u32 index = 0; index < kTexels; ++index) {
            const u32 texel = readback_texels[index];
            sentinels += texel == kNeverWritten ? 1U : 0U;
            texels[index] = texel;
        }
        executor.release();
        return adopt(out, texels.span(), kWidth, kHeight);
    }

private:
    rhi::Device* device_ = nullptr;
    rhi::ShaderModuleHandle vertex_;
    rhi::ShaderModuleHandle fragment_;
    rhi::PipelineLayoutHandle layout_;
    rhi::GraphicsPipelineHandle pipeline_;
    rhi::BufferHandle readback_;
    rhi::BufferHandle vertices_;
    rhi::BufferHandle indices_;
};

// --- What a frame is made of
// ----------------------------------------------------------------------

/// How many distinct texel values a frame holds, and how far apart its darkest and brightest
/// luminances are. A flat frame compares perfectly against a flat reference and says nothing about
/// an atmosphere, so both are asserted before any comparison is believed.
struct Variety {
    u32 distinct = 0;
    u32 darkest = 255;
    u32 brightest = 0;
};

[[nodiscard]] u32 luminance_of(u32 texel) noexcept {
    const u32 red = texel & 0xFFU;
    const u32 green = (texel >> 8U) & 0xFFU;
    const u32 blue = (texel >> 16U) & 0xFFU;
    return ((red * 54U) + (green * 183U) + (blue * 19U)) >> 8U;
}

[[nodiscard]] Variety variety_of(const Image& image, Allocator& allocator) noexcept {
    Variety variety;
    // A presence bit per 24-bit colour would be two megabytes; sorting 20 736 words is cheaper and
    // exact, and this is not a hot path.
    Array<u32> sorted(allocator);
    if (!sorted.resize(image.texels.size())) {
        return variety;
    }
    for (usize index = 0; index < image.texels.size(); ++index) {
        sorted[index] = image.texels[index] & 0x00FFFFFFU;
        const u32 luminance = luminance_of(image.texels[index]);
        variety.darkest = luminance < variety.darkest ? luminance : variety.darkest;
        variety.brightest = luminance > variety.brightest ? luminance : variety.brightest;
    }
    std::sort(sorted.data(), sorted.data() + sorted.size());
    for (usize index = 0; index < sorted.size(); ++index) {
        if (index == 0 || sorted[index] != sorted[index - 1]) {
            ++variety.distinct;
        }
    }
    return variety;
}

/// The mean absolute channel difference between two images, in eight-bit steps. The cross-time
/// comparison's metric — `test_material_binding.cpp`'s, because two pictures of the same sky at
/// different elevations differ everywhere by a little rather than anywhere by a lot, and a count of
/// differing texels would saturate at "all of them" for every pair.
[[nodiscard]] f64 mean_absolute_difference(const Image& left, const Image& right) noexcept {
    if (left.width != right.width || left.height != right.height) {
        return -1.0;
    }
    u64 total = 0;
    for (usize index = 0; index < left.texels.size(); ++index) {
        for (u32 shift = 0; shift < 24; shift += 8) {
            const auto a = static_cast<i32>((left.texels[index] >> shift) & 0xFFU);
            const auto b = static_cast<i32>((right.texels[index] >> shift) & 0xFFU);
            total += static_cast<u64>(a > b ? a - b : b - a);
        }
    }
    return static_cast<f64>(total) / (static_cast<f64>(left.texels.size()) * 3.0);
}

// --- References
// -----------------------------------------------------------------------------------

const char* reference_path(const char* name) noexcept {
    static char storage[1024];
    std::snprintf(storage, sizeof(storage), "%s/references/%s.png", CY_RENDER_TEST_DIR, name);
    return storage;
}

bool updating_references() noexcept {
    const char* value = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

/// Compare against the committed reference, or write it when the run was asked to. Returns false
/// when a reference was written, which the case turns into a failure — `test_golden_frame.cpp`'s
/// mechanism and its argument.
bool check_against_reference(const Image& rendered, const char* name) {
    const char* path = reference_path(name);
    if (updating_references()) {
        const Status written = write_png(path, rendered);
        CY_CHECK(written.has_value());
        std::fprintf(stderr,
                     "CY_RENDER_UPDATE_GOLDEN: wrote %s. Look at it, then commit it — this run "
                     "fails on purpose so that a regenerating run can never be a passing one.\n",
                     path);
        CY_CHECK_FALSE(updating_references());
        return false;
    }

    Image reference(rendered.texels.allocator());
    const Status read = read_png(path, reference);
    if (!read) {
        std::fprintf(stderr, "sky: %s: %s\n", path, read.error().message);
        CY_CHECK(read.has_value());
        return false;
    }

    const Comparison comparison = compare(reference, rendered);
    CY_CHECK(comparison.comparable);
    std::fprintf(stderr,
                 "%s: %u texels over tolerance (%u away from an edge), worst channel delta %u at "
                 "(%u, %u), %u edge texels in the reference\n",
                 name, comparison.differing, comparison.differing_off_edge,
                 comparison.max_channel_delta, comparison.worst_x, comparison.worst_y,
                 comparison.edge_texels);
    if (comparison.differing != 0 || !comparison.comparable) {
        char diff_path[1024];
        std::snprintf(diff_path, sizeof(diff_path), "%s-difference.png", name);
        (void)write_difference(diff_path, reference, rendered);
        std::fprintf(stderr, "sky: the difference is at %s\n", diff_path);
    }
    // A texel may differ only where the reference has a high-contrast neighbour — in this picture
    // the sun's disc and the sharp edge of a cloud — and only by the tolerance golden.h derives and
    // the last case in this file measures against this very frame.
    CY_CHECK_EQ(comparison.differing_off_edge, 0U);
    CY_CHECK_LE(comparison.differing, comparison.edge_texels);
    // And the stronger claim, which holds on the machine the reference came from and which a
    // regression breaks first.
    CY_CHECK_EQ(comparison.differing, 0U);
    return true;
}

/// Read a committed reference, for the cross-time comparison.
[[nodiscard]] bool load_reference(Image& out, const char* name) {
    const Status read = read_png(reference_path(name), out);
    if (!read) {
        std::fprintf(stderr, "sky: %s: %s\n", reference_path(name), read.error().message);
    }
    return read.has_value();
}

/// One time of day, photographed and judged. The body of all four cases, because four copies of it
/// would be four places for one of them to stop asserting something.
void photograph(u32 which) {
    const TimeOfDayCase& when = kTimes[which];
    DeviceFixture fixture("vulkan", "cy_test_render_sky_times_of_day");
    if (!fixture.is(rhi::BackendKind::Vulkan)) {
        fixture.report_skip();
        return;
    }
    Allocator& allocator = fixture.allocator();

    SkyFixture sky_fixture(allocator);
    CY_REQUIRE(sky_fixture.build().has_value());

    Array<GridVertex> grid(allocator);
    f32 mean_luminance = 0.0F;
    CY_REQUIRE(compose_grid(sky_fixture, when, grid, mean_luminance).has_value());

    // THE SUN IS WHERE THE MODEL PUT IT. Asserted rather than trusted, so that a reference cannot
    // become a photograph of a different time of day without this line going red first.
    const f32 elevation = sky_fixture.sun_elevation_degrees();
    std::fprintf(stderr, "%s: sun elevation %.2f deg, mean sky %.6g nits, exposed at %.6g nits\n",
                 when.name, static_cast<double>(elevation), static_cast<double>(mean_luminance),
                 static_cast<double>(when.exposure_nits));
    CY_CHECK_NEAR(elevation, when.sun_elevation_degrees, 0.05F);

    SkyPipeline pipeline;
    CY_REQUIRE(pipeline.create(fixture.device()).has_value());

    Image rendered(allocator);
    u32 sentinels = 0;
    const f32 scale = 1.0F / (when.exposure_nits * 4.0F);
    CY_REQUIRE(pipeline.render(allocator, grid, scale, rendered, sentinels).has_value());
    // THE DRAW RAN. Every texel of the target was written by it; a surviving sentinel is a pass
    // that photographed nothing.
    CY_CHECK_EQ(sentinels, 0U);

    // THE PICTURE IS A PICTURE. A frame of one colour would compare perfectly against a reference
    // of one colour.
    const Variety variety = variety_of(rendered, allocator);
    std::fprintf(stderr, "%s: %u distinct colours, luminance %u to %u\n", when.name,
                 variety.distinct, variety.darkest, variety.brightest);
    CY_CHECK_GT(variety.distinct, 200U);
    CY_CHECK_GT(variety.brightest - variety.darkest, 40U);

    (void)check_against_reference(rendered, when.name);

    // AND IT IS NOT ONE OF THE OTHER THREE. This is the "four, because the claim is about the
    // atmosphere's response to sun elevation" sentence, made falsifiable: a sky that had stopped
    // responding to the sun would render four frames that agree, and each of these comparisons
    // would then fall under the bound.
    for (u32 other = 0; other < 4; ++other) {
        if (other == which) {
            continue;
        }
        Image reference(allocator);
        CY_REQUIRE(load_reference(reference, kTimes[other].name));
        const f64 difference = mean_absolute_difference(rendered, reference);
        std::fprintf(stderr, "%s against %s: mean |delta| %.3f/255\n", when.name,
                     kTimes[other].name, difference);
        CY_CHECK_GT(difference, 8.0);
    }

    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

// ==================================================================================================
// THE TOLERANCE, MEASURED AGAINST THESE FRAMES RATHER THAN ASSUMED FROM golden.h's ARGUMENT.
//
// See the header: a tolerance nobody measured against its own picture is a round number that hides
// a regression. This case is the measurement, and it asserts both halves of it so the derivation
// cannot quietly stop being true.
// ==================================================================================================
void measure_tolerance() {
    DeviceFixture fixture("vulkan", "cy_test_render_sky_times_of_day");
    if (!fixture.is(rhi::BackendKind::Vulkan)) {
        fixture.report_skip();
        return;
    }
    Allocator& allocator = fixture.allocator();

    SkyFixture sky_fixture(allocator);
    CY_REQUIRE(sky_fixture.build().has_value());
    SkyPipeline pipeline;
    CY_REQUIRE(pipeline.create(fixture.device()).has_value());

    u32 worst_between_runs = 0;
    u32 worst_under_one_ulp = 0;
    u32 moved_under_one_ulp = 0;
    for (u32 which = 0; which < 4; ++which) {
        const TimeOfDayCase& when = kTimes[which];
        Array<GridVertex> grid(allocator);
        f32 mean_luminance = 0.0F;
        CY_REQUIRE(compose_grid(sky_fixture, when, grid, mean_luminance).has_value());

        const f32 scale = 1.0F / (when.exposure_nits * 4.0F);
        // ONE UNIT IN THE LAST PLACE of the f32 scale: the smallest perturbation an implementation
        // of the exposure divide, the interpolation or `pow` can differ by, and therefore the
        // smallest thing a conformant second implementation could move.
        const f32 nudged = std::nextafter(scale, scale * 2.0F);

        Image first(allocator);
        Image again(allocator);
        Image perturbed(allocator);
        u32 sentinels = 0;
        CY_REQUIRE(pipeline.render(allocator, grid, scale, first, sentinels).has_value());
        CY_CHECK_EQ(sentinels, 0U);
        CY_REQUIRE(pipeline.render(allocator, grid, scale, again, sentinels).has_value());
        CY_CHECK_EQ(sentinels, 0U);
        CY_REQUIRE(pipeline.render(allocator, grid, nudged, perturbed, sentinels).has_value());
        CY_CHECK_EQ(sentinels, 0U);

        const Comparison repeat = compare(first, again);
        const Comparison one_ulp = compare(first, perturbed);
        std::fprintf(stderr,
                     "%s: run to run worst channel delta %u; one ULP in the exposure moves %u "
                     "texel(s), worst channel delta %u\n",
                     when.name, repeat.max_channel_delta, one_ulp.differing,
                     one_ulp.max_channel_delta);
        worst_between_runs = repeat.max_channel_delta > worst_between_runs
                                 ? repeat.max_channel_delta
                                 : worst_between_runs;
        worst_under_one_ulp = one_ulp.max_channel_delta > worst_under_one_ulp
                                  ? one_ulp.max_channel_delta
                                  : worst_under_one_ulp;
        moved_under_one_ulp += one_ulp.differing;
    }

    std::fprintf(stderr,
                 "the tolerance is %u steps; two runs move %u and one ULP moves %u over %u "
                 "texel(s) of the four frames\n",
                 kChannelTolerance, worst_between_runs, worst_under_one_ulp, moved_under_one_ulp);
    // The composition is deterministic and so is the rasteriser: two runs of the same frame are the
    // same bytes. A non-zero here is a source of variation nobody declared.
    CY_CHECK_EQ(worst_between_runs, 0U);
    // And one step is the physical difference the tolerance is one step of headroom over. A
    // measurement that came back at the tolerance itself would mean the tolerance had stopped being
    // headroom, which is the day this bound stops meaning anything.
    CY_CHECK_LT(worst_under_one_ulp, kChannelTolerance);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

}  // namespace
}  // namespace cy::render_test

CY_TEST_CASE(
    "render.sky_times_of_day: the tolerance is larger than what two runs and one ULP move") {
    cy::render_test::measure_tolerance();
}

CY_TEST_CASE("render.sky_times_of_day: dawn matches its reference and no other") {
    cy::render_test::photograph(0);
}

CY_TEST_CASE("render.sky_times_of_day: noon matches its reference and no other") {
    cy::render_test::photograph(1);
}

CY_TEST_CASE("render.sky_times_of_day: dusk matches its reference and no other") {
    cy::render_test::photograph(2);
}

CY_TEST_CASE("render.sky_times_of_day: night matches its reference and no other") {
    cy::render_test::photograph(3);
}
