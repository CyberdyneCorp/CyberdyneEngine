// The frame, recorded on a device and read back. M8.c tasks 1b.2, 5.4, 5.5 and 5.6.
// See capture.h for what each picture is of and why the control is the same code path.

#include "capture.h"

#include "internals.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/validation.h>
#include <cy/core/math/projection.h>
#include <cy/rendering/material/standard.h>
#include <cy/rendering/pipeline/frame_bindings.h>
#include <cy/rendering/pipeline/frame_pipelines.h>

#include "golden.h"

#include <cy_features.h>

#if defined(CY_RENDERER_VULKAN)
#    include <cy/backends/rhi/vulkan/vulkan_backend.h>
#endif

#include "presentation.h"

#include <cmath>
#include <cstdio>
#include <numbers>

namespace cy::sample::slice {
namespace {

using cy::rendering::assembly::AssemblyDescription;
using cy::rendering::assembly::AssemblyReport;
using cy::rendering::assembly::AssemblyView;
using cy::rendering::assembly::FrameAssembly;
using cy::rendering::assembly::FrameSinks;
using cy::rendering::pipeline::BindingCapacity;
using cy::rendering::pipeline::DrawGeometry;
using cy::rendering::pipeline::FrameBindings;
using cy::rendering::pipeline::FramePipelines;
using cy::rendering::pipeline::FrameRecorder;
using cy::rendering::pipeline::FrameUpload;
using cy::rendering::pipeline::GeometrySource;
using cy::rendering::pipeline::GlobalsData;
using cy::rendering::pipeline::InstanceTransform;
using cy::rendering::pipeline::PipelineSetup;
using cy::rendering::pipeline::upload_for;

/// The display-referred format the output is imported in — the same choice
/// `src/rendering/pipeline/tests/` records: UNORM rather than sRGB, because the resolve does not
/// apply the transfer function (a swapchain is sRGB and the hardware applies it on write), so a
/// UNORM capture reads back the numbers the shader produced.
inline constexpr rhi::Format kOutputFormat = rhi::Format::Rgba8Unorm;

/// EXPOSURE IS NOT COSMETIC. The frame's colour target holds illuminance in physical units — the
/// slice's sun is 22 000 lux — and `cy/fullscreen.slang`'s resolve divides by 2^-stops before it
/// tonemaps. Zero stops photographs a physically-lit scene as a white rectangle, which is what the
/// pipeline suite's first run produced and what this number exists to avoid repeating.
/// The pipeline suite measured -11.4 for a 22 000 lux sun; the slice's is 90 000 lux and its four
/// point lights are 4 000 each, which is two stops brighter — so this is that number minus two.
/// It was -11.4 for one run and the arena photographed as white rectangles.
inline constexpr f32 kExposureStops = -13.4F;

/// Twenty-four vertices so every face carries its own normal: a shared-vertex box shades like a
/// sphere and hides a normal defect.
constexpr u32 kVerticesPerBox = 24;
constexpr u32 kIndicesPerBox = 36;

struct Readback {
    cy::rendering::ResourceId output = cy::rendering::kInvalidResource;
    rhi::BufferHandle buffer;
    u32 width = 0;
    u32 height = 0;
};

/// Fill the output image with zeros before a frame that is going to record nothing into it.
///
/// THE CONTROL FRAME HAS TO BE CLEARED AND THIS WAS FOUND BY LOOKING. `CaptureMode::Assembled`
/// supplies an empty `FrameSinks`, so no pass writes the output — and the image is IMPORTED, so it
/// still holds whatever the previous capture in this process left in it. The first run of this
/// wrote a "no callbacks" picture that was byte-identical to the cut taken thirty-eight ticks
/// earlier: a picture of the wrong frame, on a run whose every other number was right. Zeroing it
/// is also what the image honestly IS — a frame that recorded nothing produced nothing, and a host
/// hands its renderer a fresh swapchain image rather than the last one it drew.
void record_clear(const cy::rendering::PassContext& context, void* user) noexcept {
    auto* readback = static_cast<Readback*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{readback->width, readback->height, 1};
    context.commands->copy_buffer_to_texture(readback->buffer,
                                             context.executor->texture(readback->output),
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

void record_readback(const cy::rendering::PassContext& context, void* user) noexcept {
    auto* readback = static_cast<Readback*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{readback->width, readback->height, 1};
    context.commands->copy_texture_to_buffer(context.executor->texture(readback->output),
                                             readback->buffer,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

[[nodiscard]] Expected<rhi::BufferHandle, Error> make_upload(rhi::Device& device, const char* name,
                                                             cy::u64 bytes,
                                                             rhi::BufferUsage usage) noexcept {
    rhi::BufferDescription description;
    description.name = name;
    description.size = bytes;
    description.usage = usage;
    description.memory = rhi::MemoryUse::Upload;
    return device.create_buffer(description);
}

[[nodiscard]] Status upload_bytes(rhi::Device& device, rhi::BufferHandle buffer, const void* source,
                                  cy::u64 bytes) noexcept {
    void* mapped = device.buffer_mapped_pointer(buffer);
    if (mapped == nullptr) {
        return cy::fail(cy::ErrorCode::Internal, "a capture geometry buffer is not mapped");
    }
    const auto* from = static_cast<const cy::u8*>(source);
    auto* to = static_cast<cy::u8*>(mapped);
    for (cy::u64 index = 0; index < bytes; ++index) {
        to[index] = from[index];
    }
    return cy::ok();
}

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<cy::u32*>(user);
        std::fprintf(stderr, "vulkan validation error: %s\n", message != nullptr ? message : "");
    }
}

/// A 3x5 dot-matrix font, fifteen bits a glyph, row-major from the top with the left column in the
/// high bit of each row. Small on purpose: what it has to render is a handful of counts and
/// milliseconds, and every lit cell costs one particle.
[[nodiscard]] cy::u16 glyph_of(char character) noexcept {
    switch (character) {
        case '0':
            return 0b111'101'101'101'111;
        case '1':
            return 0b010'110'010'010'111;
        case '2':
            return 0b111'001'111'100'111;
        case '3':
            return 0b111'001'111'001'111;
        case '4':
            return 0b101'101'111'001'001;
        case '5':
            return 0b111'100'111'001'111;
        case '6':
            return 0b111'100'111'101'111;
        case '7':
            return 0b111'001'001'001'001;
        case '8':
            return 0b111'101'111'101'111;
        case '9':
            return 0b111'101'111'001'111;
        case 'A':
            return 0b111'101'111'101'101;
        case 'B':
            return 0b110'101'110'101'110;
        case 'C':
            return 0b111'100'100'100'111;
        case 'D':
            return 0b110'101'101'101'110;
        case 'E':
            return 0b111'100'110'100'111;
        case 'F':
            return 0b111'100'110'100'100;
        case 'G':
            return 0b111'100'101'101'111;
        case 'H':
            return 0b101'101'111'101'101;
        case 'I':
            return 0b111'010'010'010'111;
        case 'J':
            return 0b001'001'001'101'111;
        case 'K':
            return 0b101'101'110'101'101;
        case 'L':
            return 0b100'100'100'100'111;
        case 'M':
            return 0b101'111'111'101'101;
        case 'N':
            return 0b110'101'101'101'101;
        case 'O':
            return 0b111'101'101'101'111;
        case 'P':
            return 0b111'101'111'100'100;
        case 'Q':
            return 0b111'101'101'111'011;
        case 'R':
            return 0b111'101'111'110'101;
        case 'S':
            return 0b111'100'111'001'111;
        case 'T':
            return 0b111'010'010'010'010;
        case 'U':
            return 0b101'101'101'101'111;
        case 'V':
            return 0b101'101'101'101'010;
        case 'W':
            return 0b101'101'111'111'101;
        case 'X':
            return 0b101'101'010'101'101;
        case 'Y':
            return 0b101'101'010'010'010;
        case 'Z':
            return 0b111'001'010'100'111;
        case '.':
            return 0b000'000'000'000'010;
        case ':':
            return 0b000'010'000'010'000;
        case '/':
            return 0b001'001'010'100'100;
        case '-':
            return 0b000'000'111'000'000;
        case '%':
            return 0b101'001'010'100'101;
        default:
            return 0;
    }
}

/// Lay `lines` out at the top left of the view and append one particle per lit cell.
///
/// The glyphs are placed in CAMERA SPACE and then rotated into the camera-RELATIVE world space a
/// `ParticleInstance` is declared in — `view_to_relative` is the transpose of the rotation-only
/// view matrix, which is its inverse. Doing it any other way would need a second convention for
/// where a particle lives, and `src/rendering/particles/` has exactly one.
[[nodiscard]] Status append_readout(Array<cy::rendering::particles::ParticleInstance>& out,
                                    Span<const char* const> lines, const Mat4& view_to_relative,
                                    f32 vertical_fov, f32 aspect) noexcept {
    if (lines.empty()) {
        return cy::ok();
    }
    // A plane two metres in front of the camera. Everything below is a fraction of what the lens
    // sees there, so the readout keeps its place on the screen when the cut changes lens.
    // HALF A METRE, and it was two until the first capture came back with the readout BEHIND the
    // level. Particles are drawn in the transparent stage with the depth test on, which is right
    // for an effect and wrong for a heads-up overlay; half a metre is inside every shot's near
    // geometry and still comfortably beyond the 0.1 m near plane.
    constexpr f32 kDistance = 0.5F;
    const f32 half_height = kDistance * std::tan(vertical_fov * 0.5F);
    const f32 half_width = half_height * aspect;
    const f32 cell = half_height * 0.020F;
    const f32 dot = cell * 0.44F;
    const f32 left = -half_width * 0.94F;
    const f32 top = half_height * 0.92F;
    // RADIANCE, not a colour: the frame is scene-referred and the resolve divides by 2^-stops
    // before it tonemaps, so a value of 1 is invisible at the exposure a 22 000 lux scene is
    // viewed through. 2^11.4 is the divisor, so this is a little over white.
    // TIED TO THE EXPOSURE ABOVE, because it has to be: the resolve divides by 2^-stops, so at
    // -13.4 stops the divisor is about 10 800 and an ink of 3 400 — the first value tried — reads
    // as a mid grey rather than as text. 1.4 x the divisor is a clean white after the tonemap.
    const f32 ink = 1.4F * std::exp2(-kExposureStops);

    for (usize line = 0; line < lines.size(); ++line) {
        const char* text = lines[line];
        if (text == nullptr) {
            continue;
        }
        const f32 baseline = top - (static_cast<f32>(line) * cell * 7.0F);
        for (usize column = 0; text[column] != '\0'; ++column) {
            const cy::u16 bits = glyph_of(text[column]);
            for (cy::u32 row = 0; row < 5U; ++row) {
                for (cy::u32 pixel = 0; pixel < 3U; ++pixel) {
                    const cy::u32 bit = 14U - ((row * 3U) + pixel);
                    if (((bits >> bit) & 1U) == 0U) {
                        continue;
                    }
                    const f32 x =
                        left +
                        (((static_cast<f32>(column) * 4.0F) + static_cast<f32>(pixel)) * cell);
                    const f32 y = baseline - (static_cast<f32>(row) * cell);
                    const cy::Vec4 rotated = view_to_relative * cy::Vec4{x, y, -kDistance, 0.0F};
                    cy::rendering::particles::ParticleInstance glyph;
                    glyph.position[0] = rotated.x;
                    glyph.position[1] = rotated.y;
                    glyph.position[2] = rotated.z;
                    glyph.size = dot;
                    glyph.color[0] = ink;
                    glyph.color[1] = ink;
                    glyph.color[2] = ink * 0.94F;
                    glyph.color[3] = 1.0F;
                    if (Status pushed = out.push_back(glyph); !pushed) {
                        return pushed;
                    }
                }
            }
        }
    }
    return cy::ok();
}
}  // namespace

// --- The device and everything that hangs off it
// --------------------------------------------------

struct FrameCapture::Device {
    explicit Device(Allocator& allocator) noexcept
        : assembly(allocator),
          graph(allocator),
          program(allocator),
          instances(allocator),
          lights(allocator),
          positions(allocator),
          normals(allocator),
          uvs(allocator),
          indices(allocator),
          mesh_ranges(allocator) {}

    Expected<rhi::Device*, Error> handle = cy::fail(cy::ErrorCode::Unavailable, "not created");
    rhi::BackendSelection selection{};
    cy::u32 validation_errors = 0;

    FrameAssembly assembly;
    /// A pointer rather than a member because it is REBUILT per capture — see `shoot`. `Array` and
    /// `SpatialIndex` both hold their allocator, so re-seating one in place would be a placement
    /// new on a live member, and that is the kind of cleverness a sample should not teach.
    cy::rendering::SpatialIndex* index = nullptr;
    cy::rendering::RenderGraph graph;
    cy::rendering::MaterialProgram program;
    FramePipelines pipelines;
    FrameBindings bindings;
    FrameRecorder recorder;
    cy::rendering::particles::ParticleRenderer effect;

    Array<InstanceTransform> instances;
    Array<cy::render::LightDescription> lights;

    // The level's meshes, flattened into three streams and an index buffer, one box per asset.
    Array<f32> positions;
    Array<cy::u16> normals;
    Array<f32> uvs;
    Array<cy::u16> indices;
    struct MeshRange {
        cy::u32 first_index = 0;
        cy::u32 index_count = 0;
        cy::i32 vertex_offset = 0;
    };
    Array<MeshRange> mesh_ranges;

    rhi::BufferHandle stream_buffers[3];
    rhi::BufferHandle index_buffer;
    rhi::BufferHandle readback;
    /// A page of zeros the size of one frame, uploaded into the output before a control capture.
    rhi::BufferHandle zeros;
    rhi::TextureHandle output;
    cy::u32 material_offsets[4] = {0, 0, 0, 0};

    /// The zero-fill pass's payload, held here because the graph records after `shoot` returns
    /// from declaring it.
    Readback clear;

    const cy::rendering::assembly::SceneIndex* published = nullptr;
    bool described = false;
};

namespace {

/// The surface query. Answers out of the presentation's `SceneIndex` — the mesh and material handle
/// `bind_render_assets` resolved from the authored reference — keyed by the gpu slot, which this
/// capture set to the publisher's own spatial slot.
Span<const cy::rendering::DrawSurface> capture_surface(
    const cy::rendering::VisibleInstance& instance, void* user) noexcept {
    static thread_local cy::rendering::DrawSurface surface;
    const auto& device = *static_cast<const FrameCapture::Device*>(user);
    surface = cy::rendering::DrawSurface{};
    if (device.published != nullptr) {
        const cy::rendering::assembly::SceneIndex::Surface published =
            device.published->surface_of(instance.gpu_slot);
        surface.mesh = published.mesh.index();
        surface.material = published.material.index();
    }
    surface.pipeline = 0;
    surface.blend = cy::render::BlendMode::Opaque;
    return {&surface, 1};
}

/// Where a draw's geometry is. THE SILHOUETTE IS THE MESH HANDLE'S, and this lookup is the only
/// thing that decides it: one box per mesh asset, with that asset's own proportions. Break
/// `Level::resolve_mesh` so every reference answers one handle and every prop in the picture
/// collapses to the ground's shape — which is the pixel form of the check M8.b's gate added.
bool capture_geometry(const cy::render::DrawItem& /*item*/,
                      const cy::rendering::GpuDrawInstance& instance, void* user,
                      DrawGeometry& out) noexcept {
    const auto& device = *static_cast<const FrameCapture::Device*>(user);
    if (device.published == nullptr) {
        return false;
    }
    const cy::u32 mesh = device.published->surface_of(instance.instance_slot).mesh.index();
    if (mesh >= device.mesh_ranges.size()) {
        return false;
    }
    const FrameCapture::Device::MeshRange& range = device.mesh_ranges[mesh];
    out.indices = device.index_buffer;
    out.wide_indices = false;
    out.index_count = range.index_count;
    out.first_index = range.first_index;
    out.vertex_offset = range.vertex_offset;
    return true;
}

}  // namespace

FrameCapture::FrameCapture(Allocator& allocator) noexcept
    : allocator_(&allocator), pixels_(allocator), previous_(allocator), composed_(allocator) {}

FrameCapture::~FrameCapture() {
    close();
}

const char* FrameCapture::unavailable_reason() const noexcept {
    if (available_) {
        return "";
    }
    if (device_ == nullptr) {
        return "the capture was never opened";
    }
    return device_->selection.reason != nullptr ? device_->selection.reason
                                                : "no graphics device was selected";
}

Status FrameCapture::open(cy::u32 width, cy::u32 height) noexcept {
    width_ = width;
    height_ = height;
    device_ = new (std::nothrow) Device(*allocator_);
    if (device_ == nullptr) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "the capture did not allocate");
    }
#if defined(CY_RENDERER_VULKAN)
    (void)rhi::vulkan::register_vulkan_backend();
#endif
    rhi::DeviceDescription description;
    description.application_name = "cy_sample_vertical-slice";
    // VALIDATION ON, and its errors are a REPORTED NUMBER rather than a log line nobody reads. The
    // pipeline layer's own suite found three distinct validation defects this way — an unbound
    // descriptor set, a mismatched push-constant range and a missing device feature — none of which
    // any structural test could see.
    description.enable_validation = true;
    device_->handle = rhi::create_device(*allocator_, "vulkan", description, device_->selection);
    if (!device_->handle.has_value()) {
        return cy::ok();
    }
    if (device_->handle.value()->capabilities().backend() != rhi::BackendKind::Vulkan) {
        return cy::ok();
    }
    device_->handle.value()->set_validation_callback(&count_validation,
                                                     &device_->validation_errors);
    available_ = true;
    return cy::ok();
}

void FrameCapture::close() noexcept {
    if (device_ == nullptr) {
        return;
    }
    if (device_->handle.has_value()) {
        rhi::Device& device = *device_->handle.value();
        (void)device.wait_idle();
        device_->effect.shutdown();
        device_->bindings.shutdown();
        device_->pipelines.shutdown();
        if (!device_->output.is_null()) {
            device.destroy_texture(device_->output);
        }
        rhi::BufferHandle* buffers[6] = {&device_->stream_buffers[0], &device_->stream_buffers[1],
                                         &device_->stream_buffers[2], &device_->index_buffer,
                                         &device_->readback,          &device_->zeros};
        for (rhi::BufferHandle* buffer : buffers) {
            if (!buffer->is_null()) {
                device.destroy_buffer(*buffer);
                *buffer = rhi::BufferHandle{};
            }
        }
        rhi::destroy_device(*allocator_, device_->handle.value());
    }
    delete device_->index;
    device_->index = nullptr;
    delete device_;
    device_ = nullptr;
    available_ = false;
}

// --- The level, as geometry
// -----------------------------------------------------------------------

Status FrameCapture::create_geometry() noexcept {
    Device& device = *device_;
    const auto meshes = static_cast<cy::u32>(device.mesh_ranges.size());
    if (Status sized = device.positions.resize(usize{meshes} * kVerticesPerBox * 3U); !sized) {
        return sized;
    }
    if (Status sized = device.normals.resize(usize{meshes} * kVerticesPerBox * 4U); !sized) {
        return sized;
    }
    if (Status sized = device.uvs.resize(usize{meshes} * kVerticesPerBox * 2U); !sized) {
        return sized;
    }
    if (Status sized = device.indices.resize(usize{meshes} * kIndicesPerBox); !sized) {
        return sized;
    }
    return cy::ok();
}

Status FrameCapture::create_materials() noexcept {
    Device& device = *device_;
    if (Status described = cy::rendering::describe_standard_material(
            device.program, Name::intern("standard"), cy::render::ShadingModel::Lit,
            cy::render::BlendMode::Opaque);
        !described) {
        return described;
    }
    const cy::rendering::StandardParameters ids;
    // DERIVED, NOT HARDCODED. `cy/frame.slang` reads base colour, roughness, metallic and emission
    // out of the GPU material table at word offsets it is TOLD, and this is where they come from.
    const cy::rendering::ParameterId wanted[4] = {ids.base_color_factor, ids.roughness_factor,
                                                  ids.metallic_factor, ids.emission_color};
    for (cy::u32 index = 0; index < 4U; ++index) {
        const cy::rendering::MaterialParameter* parameter = device.program.find(wanted[index]);
        if (parameter == nullptr) {
            return cy::fail(cy::ErrorCode::NotFound,
                            "the standard material lacks a parameter the frame reads");
        }
        device.material_offsets[index] = parameter->offset / 4U;
    }

    // THE PALETTE IS THE ARTEFACT'S, INDEXED BY `MaterialKind`, and the slot a draw lands on is the
    // one `Level::resolve_material` produced. Two teams are two materials over one mesh, which is
    // what makes the picture say which side a character is on.
    static constexpr f32 kColours[static_cast<usize>(MaterialKind::Count)][3] = {
        {0.34F, 0.36F, 0.38F},  // Ground
        {0.52F, 0.50F, 0.46F},  // Wall
        {0.62F, 0.44F, 0.22F},  // Crate
        {0.44F, 0.46F, 0.55F},  // Pillar
        {0.18F, 0.45F, 0.86F},  // TeamBlue
        {0.86F, 0.24F, 0.20F},  // TeamRed
    };
    cy::rendering::MaterialTable& table = device.assembly.materials();
    for (cy::u32 slot = 0; slot < static_cast<cy::u32>(MaterialKind::Count); ++slot) {
        const Expected<cy::u32, Error> allocated = table.allocate();
        if (!allocated) {
            return Status{cy::make_unexpected(allocated.error())};
        }
        if (Status defaults =
                cy::rendering::apply_standard_defaults(device.program, table, *allocated, ids);
            !defaults) {
            return defaults;
        }
        const cy::Vec4 colour{kColours[slot][0], kColours[slot][1], kColours[slot][2], 1.0F};
        if (Status set = table.set_color(device.program, *allocated, ids.base_color_factor, colour);
            !set) {
            return set;
        }
        if (Status set = table.set_float(device.program, *allocated, ids.roughness_factor,
                                         slot >= 4U ? 0.45F : 0.72F);
            !set) {
            return set;
        }
        if (Status set = table.set_float(device.program, *allocated, ids.metallic_factor, 0.0F);
            !set) {
            return set;
        }
    }
    return cy::ok();
}

Status FrameCapture::describe(const Slice& slice, const Presentation& presentation) noexcept {
    if (!available_) {
        return cy::ok();
    }
    Device& device = *device_;
    rhi::Device& handle = *device.handle.value();
    device.published = &presentation.scene_index();

    // --- One box per MESH ASSET, with that asset's own bounds.
    if (Status sized = device.mesh_ranges.resize(slice.mesh_asset_count()); !sized) {
        return sized;
    }
    if (Status made = create_geometry(); !made) {
        return made;
    }
    static constexpr Vec3 kAxes[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                                      Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
    usize vertex = 0;
    usize index = 0;
    for (usize mesh = 0; mesh < device.mesh_ranges.size(); ++mesh) {
        const Aabb box = slice.mesh_asset_bounds(static_cast<cy::u32>(mesh));
        const Vec3 centre{(box.min.x + box.max.x) * 0.5F, (box.min.y + box.max.y) * 0.5F,
                          (box.min.z + box.max.z) * 0.5F};
        const Vec3 half{(box.max.x - box.min.x) * 0.5F, (box.max.y - box.min.y) * 0.5F,
                        (box.max.z - box.min.z) * 0.5F};
        device.mesh_ranges[mesh].first_index = static_cast<cy::u32>(index);
        device.mesh_ranges[mesh].index_count = kIndicesPerBox;
        device.mesh_ranges[mesh].vertex_offset = static_cast<cy::i32>(vertex);
        cy::u16 within = 0;
        for (const Vec3 normal : kAxes) {
            const Vec3 tangent =
                std::fabs(normal.y) > 0.5F ? Vec3{1.0F, 0.0F, 0.0F} : Vec3{0.0F, 1.0F, 0.0F};
            const Vec3 bitangent{(normal.y * tangent.z) - (normal.z * tangent.y),
                                 (normal.z * tangent.x) - (normal.x * tangent.z),
                                 (normal.x * tangent.y) - (normal.y * tangent.x)};
            const cy::u16 face_first = within;
            for (cy::u32 corner = 0; corner < 4U; ++corner) {
                const f32 u = (corner == 1U || corner == 2U) ? 1.0F : -1.0F;
                const f32 v = (corner >= 2U) ? 1.0F : -1.0F;
                device.positions[(vertex * 3U) + 0] = centre.x + (normal.x * half.x) +
                                                      (tangent.x * u * half.x) +
                                                      (bitangent.x * v * half.x);
                device.positions[(vertex * 3U) + 1] = centre.y + (normal.y * half.y) +
                                                      (tangent.y * u * half.y) +
                                                      (bitangent.y * v * half.y);
                device.positions[(vertex * 3U) + 2] = centre.z + (normal.z * half.z) +
                                                      (tangent.z * u * half.z) +
                                                      (bitangent.z * v * half.z);
                cy::rendering::pipeline::pack_normal_stream(normal, tangent,
                                                            &device.normals[vertex * 4U]);
                device.uvs[(vertex * 2U) + 0] = (u * 0.5F) + 0.5F;
                device.uvs[(vertex * 2U) + 1] = (v * 0.5F) + 0.5F;
                ++vertex;
                ++within;
            }
            const cy::u16 order[6] = {0, 1, 2, 0, 2, 3};
            for (const cy::u16 step : order) {
                device.indices[index++] = static_cast<cy::u16>(face_first + step);
            }
        }
    }

    struct Request {
        const char* name;
        const void* source;
        cy::u64 bytes;
        rhi::BufferUsage usage;
        rhi::BufferHandle* out;
    };
    const Request requests[4] = {
        {"slice positions", device.positions.data(), device.positions.size() * sizeof(f32),
         rhi::BufferUsage::Vertex, &device.stream_buffers[0]},
        {"slice normals", device.normals.data(), device.normals.size() * sizeof(cy::u16),
         rhi::BufferUsage::Vertex, &device.stream_buffers[1]},
        {"slice uvs", device.uvs.data(), device.uvs.size() * sizeof(f32), rhi::BufferUsage::Vertex,
         &device.stream_buffers[2]},
        {"slice indices", device.indices.data(), device.indices.size() * sizeof(cy::u16),
         rhi::BufferUsage::Index, &device.index_buffer},
    };
    for (const Request& request : requests) {
        Expected<rhi::BufferHandle, Error> made =
            make_upload(handle, request.name, request.bytes, request.usage);
        if (!made) {
            return Status{cy::make_unexpected(made.error())};
        }
        *request.out = *made;
        if (Status written = upload_bytes(handle, *request.out, request.source, request.bytes);
            !written) {
            return written;
        }
    }

    // --- The frame itself, described the way the presentation describes its own.
    // SIZED FROM THE LEVEL AND THE SQUAD RATHER THAN FROM THE SCENE INDEX, and both halves of that
    // were learned by running it. `describe()` is called from `build()`, BEFORE the first tick, so
    // `SceneIndex::live()` is still zero here and sizing from it gave a ring of 64 that the first
    // capture overran with 227. And an instance is one row while a DRAW is one per surface per
    // pass, so a frame with fifteen shadow pages has many more of the second than the first —
    // sizing both the same way failed first with "more draws than the ring was sized for".
    const cy::u32 instances = slice.report().level_entities + slice.options().agents + 64U;
    const cy::u32 draws = (instances * 8U) + 512U;
    AssemblyDescription description;
    description.width = width_;
    description.height = height_;
    description.near_plane = 0.1F;
    description.far_plane = 400.0F;
    description.clusters = cy::rendering::ClusterGridConfig{32, 16, 32};
    description.material_capacity = static_cast<cy::u32>(MaterialKind::Count);
    description.max_draws = draws;
    description.max_instances = instances;
    description.gpu_culling = false;
    if (Status made = device.assembly.initialize(description); !made) {
        return made;
    }
    if (Status attached = device.assembly.attach_device(handle); !attached) {
        return attached;
    }

    PipelineSetup setup;
    setup.color_format = description.color_format;
    setup.depth_format = description.depth_format;
    setup.output_format = kOutputFormat;
    if (Status made = device.pipelines.initialize(handle, setup); !made) {
        return made;
    }
    const Expected<cy::rendering::ClusterGrid, Error> grid = cy::rendering::make_cluster_grid(
        description.clusters, width_, height_, description.near_plane, description.far_plane);
    if (!grid) {
        return Status{cy::make_unexpected(grid.error())};
    }
    const BindingCapacity binding_capacity = BindingCapacity::for_grid(
        *grid, description.max_draws, description.max_instances, description.material_capacity, 16);
    if (Status made = device.bindings.initialize(handle, device.pipelines, binding_capacity);
        !made) {
        return made;
    }
    if (Status made = device.recorder.initialize(device.pipelines, device.bindings); !made) {
        return made;
    }
    if (Status made = device.effect.initialize(handle, device.pipelines, 8192); !made) {
        return made;
    }
    if (Status made = create_materials(); !made) {
        return made;
    }

    GeometrySource geometry;
    geometry.streams[cy::rendering::pipeline::kPositionStream] = device.stream_buffers[0];
    geometry.streams[cy::rendering::pipeline::kNormalStream] = device.stream_buffers[1];
    geometry.streams[cy::rendering::pipeline::kUvStream] = device.stream_buffers[2];
    geometry.geometry = &capture_geometry;
    geometry.user = &device;
    device.recorder.set_geometry(geometry);

    const cy::u64 bytes = cy::u64{width_} * cy::u64{height_} * sizeof(cy::u32);
    Expected<rhi::BufferHandle, Error> readback =
        make_upload(handle, "slice readback", bytes, rhi::BufferUsage::TransferDestination);
    if (!readback) {
        return Status{cy::make_unexpected(readback.error())};
    }
    device.readback = *readback;

    Expected<rhi::BufferHandle, Error> zeros =
        make_upload(handle, "slice control clear", bytes,
                    rhi::BufferUsage::TransferSource | rhi::BufferUsage::TransferDestination);
    if (!zeros) {
        return Status{cy::make_unexpected(zeros.error())};
    }
    device.zeros = *zeros;
    if (void* mapped = handle.buffer_mapped_pointer(device.zeros); mapped != nullptr) {
        auto* words = static_cast<cy::u32*>(mapped);
        for (cy::u64 word = 0; word < bytes / sizeof(cy::u32); ++word) {
            words[word] = 0xFF000000U;  // opaque black, which is what a cleared frame is
        }
    }

    rhi::TextureDescription output;
    output.name = "slice output";
    output.format = kOutputFormat;
    output.extent = rhi::Extent3D{width_, height_, 1};
    // TransferDestination as well as TransferSource: the control capture ZEROES this image with a
    // buffer-to-image copy (see `record_clear`), and an image without that usage bit is a
    // validation error at the barrier rather than at the copy — which is how it announced itself.
    output.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSource |
                   rhi::TextureUsage::TransferDestination;
    Expected<rhi::TextureHandle, Error> created = handle.create_texture(output);
    if (!created) {
        return Status{cy::make_unexpected(created.error())};
    }
    device.output = *created;
    device.described = true;
    return cy::ok();
}

// --- One recorded frame
// ----------------------------------------------------------------------------

Status FrameCapture::read_pixels() noexcept {
    if (Status sized = pixels_.resize(usize{width_} * usize{height_}); !sized) {
        return sized;
    }
    const auto* mapped = static_cast<const cy::u32*>(
        device_->handle.value()->buffer_mapped_pointer(device_->readback));
    if (mapped == nullptr) {
        return cy::fail(cy::ErrorCode::Internal, "the capture readback buffer is not mapped");
    }
    for (usize index = 0; index < pixels_.size(); ++index) {
        pixels_[index] = mapped[index];
    }
    return cy::ok();
}

Status FrameCapture::rebuild_scene(const Presentation& presentation,
                                   const ViewState& view_state) noexcept {
    Device& device = *device_;
    // --- The spatial index, rebuilt from what the presentation published.
    //
    // A SECOND INDEX RATHER THAN THE PUBLISHER'S, AND THE REASON IS A FINDING RATHER THAN A
    // PREFERENCE. `SceneIndex::apply` leaves `SpatialEntry::gpu_slot` at zero — its own header says
    // so — and `SpatialIndex` has no way to set it afterwards, so every draw of a `SceneIndex`
    // frame carries `instance_slot == 0` and every instance would read instance row zero. On a
    // frame that is only ASSEMBLED that is invisible; on one that is RECORDED every prop in the
    // level lands on top of the first one. So this rebuilds the entries with `gpu_slot` set to the
    // publisher's own spatial slot, which is also the key `capture_surface` resolves through, and
    // the finding is reported to the owner of src/rendering/assembly/.
    const cy::rendering::assembly::SceneIndex& published = presentation.scene_index();
    const cy::rendering::SpatialIndex& source = published.index();
    delete device.index;
    device.index = new (std::nothrow) cy::rendering::SpatialIndex(*allocator_);
    if (device.index == nullptr) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "the capture's spatial index did not allocate");
    }
    if (Status sized = device.instances.resize(source.slot_count()); !sized) {
        return sized;
    }
    for (cy::u32 slot = 0; slot < source.slot_count(); ++slot) {
        const cy::rendering::SpatialEntry& entry = source.entry(slot);
        if (entry.stable_id == 0U) {
            continue;
        }
        cy::rendering::SpatialEntry copy = entry;
        copy.gpu_slot = slot;
        if (Expected<cy::u32, Error> made = device.index->insert(copy); !made) {
            return Status{cy::make_unexpected(made.error())};
        }
        // CAMERA-RELATIVE, which is what `InstanceTransform` is declared to be. The box the mesh
        // asset carries is already the right size, so the placement is a translation and the frame
        // has no world matrix in it anywhere.
        const Vec3 centre{(entry.bounds.min.x + entry.bounds.max.x) * 0.5F,
                          (entry.bounds.min.y + entry.bounds.max.y) * 0.5F,
                          (entry.bounds.min.z + entry.bounds.max.z) * 0.5F};
        InstanceTransform& transform = device.instances[slot];
        transform = InstanceTransform{};
        transform.row0[0] = 1.0F;
        transform.row1[1] = 1.0F;
        transform.row2[2] = 1.0F;
        transform.row0[3] = centre.x - view_state.eye.x;
        transform.row1[3] = centre.y - view_state.eye.y;
        transform.row2[3] = centre.z - view_state.eye.z;
        transform.tint[0] = 1.0F;
        transform.tint[1] = 1.0F;
        transform.tint[2] = 1.0F;
        transform.tint[3] = 1.0F;
    }

    if (Status copied = device.lights.resize(presentation.lights().size()); !copied) {
        return copied;
    }
    for (usize which = 0; which < device.lights.size(); ++which) {
        device.lights[which] = presentation.lights()[which];
    }

    return cy::ok();
}

Status FrameCapture::attach_particles(
    cy::u32 frame_slot, Span<const cy::rendering::particles::ParticleInstance> particles,
    Span<const char* const> readout, const Mat4& relative_view, f32 aspect,
    f32 vertical_fov) noexcept {
    Device& device = *device_;
    composed_.clear();
    for (const cy::rendering::particles::ParticleInstance& particle : particles) {
        if (Status pushed = composed_.push_back(particle); !pushed) {
            return pushed;
        }
    }
    if (Status written =
            append_readout(composed_, readout, transpose(relative_view), vertical_fov, aspect);
        !written) {
        return written;
    }
    if (Status uploaded = device.effect.upload(frame_slot, composed_.span()); !uploaded) {
        return uploaded;
    }
    return device.recorder.add_extension(device.effect.extension());
}

void FrameCapture::collect(CaptureReport& out) noexcept {
    Device& device = *device_;
    const cy::rendering::pipeline::RecorderReport& recorded = device.recorder.report();
    out.passes = recorded.passes;
    out.prepass_draws = recorded.prepass_draws;
    out.opaque_draws = recorded.opaque_draws;
    out.transparent_draws = recorded.transparent_draws;
    out.skipped_draws = recorded.skipped_draws;
    out.extensions_run = recorded.extensions_run;
    out.uploaded_bytes = recorded.uploaded_bytes;
    out.particles_drawn = device.effect.report().particles;
    out.particles_dropped = device.effect.report().dropped;
    out.validation_errors = device.validation_errors;
    for (const cy::u32 texel : pixels_.span()) {
        if ((texel & 0xFFU) > 16U || ((texel >> 8U) & 0xFFU) > 16U ||
            ((texel >> 16U) & 0xFFU) > 16U) {
            ++out.lit_texels;
        }
    }
    if (previous_.size() == pixels_.size()) {
        for (usize index = 0; index < pixels_.size(); ++index) {
            const cy::u32 a = pixels_[index];
            const cy::u32 b = previous_[index];
            for (cy::u32 channel = 0; channel < 4U; ++channel) {
                const auto left = static_cast<cy::i32>((a >> (channel * 8U)) & 0xFFU);
                const auto right = static_cast<cy::i32>((b >> (channel * 8U)) & 0xFFU);
                if (left - right > 1 || right - left > 1) {
                    ++out.differing_texels;
                    break;
                }
            }
        }
    }
    if (Status kept = previous_.resize(pixels_.size()); kept) {
        for (usize index = 0; index < pixels_.size(); ++index) {
            previous_[index] = pixels_[index];
        }
    }
}

Status FrameCapture::save(const char* path, CaptureReport& out) noexcept {
    cy::render_test::Image image(*allocator_);
    if (!cy::render_test::adopt(image, pixels_.span(), width_, height_)) {
        return cy::fail(cy::ErrorCode::Internal, "the capture could not be adopted as an image");
    }
    if (Status written = cy::render_test::write_png(path, image); !written) {
        return written;
    }
    out.captured = true;
    std::fprintf(stderr, "wrote %s (%ux%u)\n", path, width_, height_);
    return cy::ok();
}

Status FrameCapture::shoot(const Presentation& presentation,
                           Span<const cy::rendering::particles::ParticleInstance> particles,
                           Span<const char* const> readout, CaptureMode mode, const char* path,
                           CaptureReport& out) noexcept {
    out = CaptureReport{};
    if (!available_ || device_ == nullptr || !device_->described) {
        return cy::ok();
    }
    Device& device = *device_;
    rhi::Device& handle = *device.handle.value();
    out.device = true;

    const ViewState view_state = presentation.last_view();
    if (Status rebuilt = rebuild_scene(presentation, view_state); !rebuilt) {
        return rebuilt;
    }

    const Expected<cy::u32, Error> began = handle.begin_frame();
    if (!began) {
        return Status{cy::make_unexpected(began.error())};
    }
    const cy::u32 slot = *began;

    device.graph.reset();
    const f32 aspect = static_cast<f32>(width_) / static_cast<f32>(height_);
    const Mat4 projection =
        cy::perspective_reversed_z(view_state.vertical_fov, aspect, 0.1F, 400.0F);
    const Mat4 world_view = cy::look_at(view_state.eye, view_state.target, Vec3{0.0F, 1.0F, 0.0F});
    // The camera-relative view: the same rotation with the eye at the origin. The instances above
    // are already rebased, so this is the matrix the shader multiplies them by.
    const Vec3 forward{view_state.target.x - view_state.eye.x,
                       view_state.target.y - view_state.eye.y,
                       view_state.target.z - view_state.eye.z};
    const Mat4 relative_view = cy::look_at(Vec3{0.0F, 0.0F, 0.0F}, forward, Vec3{0.0F, 1.0F, 0.0F});

    device.recorder.clear_extensions();
    // RESET EVERY SHOOT, not only inside `bind()`. `bind()` is not called in `Assembled` mode, so
    // the control frame reported the PREVIOUS capture's five stages and its draws — numbers from a
    // frame recorded thirty-eight ticks earlier, presented as the control's own.
    device.recorder.reset_report();
    device.effect.reset_report();
    if (mode == CaptureMode::RecordedWithParticles) {
        if (Status attached = attach_particles(slot, particles, readout, relative_view, aspect,
                                               view_state.vertical_fov);
            !attached) {
            return attached;
        }
    }

    AssemblyView view;
    view.fov_y_radians = view_state.vertical_fov;
    view.view = world_view;
    view.projection = projection;
    view.cull.frustum = cy::Frustum::from_view_projection(projection * world_view);
    view.cull.camera_position = view_state.eye;
    const f32 length =
        std::sqrt((forward.x * forward.x) + (forward.y * forward.y) + (forward.z * forward.z));
    view.cull.camera_forward =
        length > 1e-4F ? Vec3{forward.x / length, forward.y / length, forward.z / length}
                       : Vec3{0.0F, 0.0F, -1.0F};
    view.cull.fov_y_radians = view.fov_y_radians;
    view.lights = device.lights.span();
    view.sun_direction = view_state.sun;

    cy::rendering::TextureRequest request;
    request.name = "slice output";
    request.format = kOutputFormat;
    request.width = width_;
    request.height = height_;
    request.extra_usage =
        rhi::TextureUsage::TransferSource | rhi::TextureUsage::TransferDestination;
    view.output = device.graph.import_texture(request, device.output, rhi::ImageLayout::Undefined);

    FrameSinks sinks;
    if (mode == CaptureMode::Assembled) {
        // THE CONTROL, and it is exactly what this sample supplied for the whole of M8.b: a surface
        // query and no record callbacks at all. The frame is still assembled, compiled, barriered
        // and executed; nothing draws into it.
        sinks.surfaces = &capture_surface;
        sinks.surfaces_user = &device;
    } else {
        if (Status bound = device.recorder.bind(device.assembly); !bound) {
            return bound;
        }
        sinks = device.recorder.sinks();
        sinks.surfaces = &capture_surface;
        sinks.surfaces_user = &device;
    }

    AssemblyReport assembled;
    if (Status made = device.assembly.assemble(*device.index, view, sinks, device.graph, assembled);
        !made) {
        return made;
    }

    if (mode != CaptureMode::Assembled) {
        GlobalsData globals;
        globals.exposure_stops = kExposureStops;
        const FrameUpload upload =
            upload_for(device.assembly, assembled, projection * relative_view, relative_view,
                       device.instances.span(), globals, device.material_offsets);
        if (Status uploaded = device.bindings.upload(slot, upload); !uploaded) {
            return uploaded;
        }
    }

    Readback readback;
    readback.output = device.assembly.resources().output;
    readback.buffer = device.readback;
    readback.width = width_;
    readback.height = height_;
    const cy::u64 bytes = cy::u64{width_} * cy::u64{height_} * sizeof(cy::u32);
    if (mode == CaptureMode::Assembled && readback.output != cy::rendering::kInvalidResource) {
        // See `record_clear`: nothing in this frame writes the output, and the image is imported.
        device.clear.output = readback.output;
        device.clear.buffer = device.zeros;
        device.clear.width = width_;
        device.clear.height = height_;
        // A WRITE TO AN IMPORTED RESOURCE IS A CULLING ROOT, which is what keeps this pass alive
        // in a frame where nothing else touches the output.
        device.graph.add_pass("control clear", rhi::QueueKind::Graphics)
            .write(readback.output, rhi::Access::TransferWrite)
            .record(&record_clear, &device.clear);
    }
    if (readback.output != cy::rendering::kInvalidResource) {
        // A WRITE TO AN IMPORTED RESOURCE IS A CULLING ROOT, and that is the whole reason this pass
        // survives: the graph drops a pass whose output nothing consumes, and a capture that
        // declared only a READ came back uniformly zero with the rest of the run green. The
        // pipeline suite records the same finding.
        cy::rendering::BufferRequest capture_request;
        capture_request.name = "slice capture";
        capture_request.size = bytes;
        capture_request.extra_usage = rhi::BufferUsage::TransferDestination;
        const cy::rendering::ResourceId destination =
            device.graph.import_buffer(capture_request, device.readback);
        device.graph.add_pass("capture", rhi::QueueKind::Graphics)
            .read(readback.output, rhi::Access::TransferRead)
            .write(destination, rhi::Access::TransferWrite)
            .record(&record_readback, &readback);
        device.graph.add_pass("capture host", rhi::QueueKind::Graphics)
            .read(destination, rhi::Access::HostRead)
            .side_effect();
    }

    cy::rendering::GraphExecutor executor(*allocator_, handle);
    Status executed = device.assembly.execute(executor, device.graph, assembled);
    if (executed) {
        executed = handle.wait_idle();
    }
    if (executed && readback.output != cy::rendering::kInvalidResource) {
        executed = read_pixels();
    }
    executor.release();
    if (Status ended = handle.end_frame(); !ended && executed) {
        return ended;
    }
    if (!executed) {
        return executed;
    }

    collect(out);
    if (path != nullptr) {
        return save(path, out);
    }
    return cy::ok();
}

// --- What the slice asks for
// ----------------------------------------------------------------------

Status Slice::shoot(u32 tick) noexcept {
    if (capture_ == nullptr || !capture_->available()) {
        return cy::ok();
    }
    const u32 last = options_.ticks == 0U ? 0U : options_.ticks - 1U;
    const u32 wanted = options_.capture_tick == 0U ? last : options_.capture_tick;
    // THE FRAME THE CUT IS MID-BLEND ON. The cinematic starts at `cut_start_tick`, cuts at its own
    // frame 36 and blends for half a second at the simulation's rate, so frame 51 is the middle of
    // the overlap — which is what task 5.5 asks a picture of, "mid-blend, not at either end".
    const u32 blend = options_.cut_start_tick + 51U;
    if (tick != wanted && tick != blend) {
        return cy::ok();
    }

    Span<const cy::rendering::particles::ParticleInstance> particles;
    if (spectacle_ != nullptr) {
        particles = spectacle_->particles();
    }
    const SpectacleReport& spectacle = spectacle_report();

    // THE READOUT IS THE BUDGET, AND THE SLICE PRINTS THE SAME NUMBERS AS `key = value` LINES, so
    // the picture is a second view of a measurement rather than the only record of it.
    char lines[5][48];
    (void)std::snprintf(lines[0], sizeof(lines[0]), "CYBERENGINE M8.C VERTICAL SLICE");
    (void)std::snprintf(lines[1], sizeof(lines[1]), "PARTICLES %u DROPPED %u EFFECTS %u",
                        spectacle.vfx_live_particles, spectacle.vfx_dropped,
                        spectacle.effects_played);
    (void)std::snprintf(
        lines[2], sizeof(lines[2]), "DISPATCH %u OF %u BUDGET %u%%",
        spectacle.vfx_dispatches_merged, spectacle.vfx_dispatches_unmerged,
        static_cast<u32>(((spectacle_ == nullptr ? 0.0 : spectacle_->particles_us()) * 100.0) /
                         Budgets::kSpectacleUs));
    (void)std::snprintf(lines[3], sizeof(lines[3]), "AGENTS %u DRAWS %u TICK %u", report_.agents,
                        report_.frame_draws, tick);
    (void)std::snprintf(lines[4], sizeof(lines[4]), "CUT WIDE %u%% TIGHT %u%% FOV %u",
                        static_cast<u32>(spectacle.cut_wide_weight * 100.0F),
                        static_cast<u32>(spectacle.cut_tight_weight * 100.0F),
                        static_cast<u32>(spectacle.cut_fov * 1000.0F));
    const char* readout[5] = {lines[0], lines[1], lines[2], lines[3], lines[4]};

    char path[512];
    if (tick == blend && tick != wanted) {
        (void)std::snprintf(path, sizeof(path), "%s-cut.png", options_.capture_prefix);
        CaptureReport cut;
        return capture_->shoot(*presentation_, particles, Span<const char* const>(readout, 5),
                               CaptureMode::RecordedWithParticles, path, cut);
    }

    // THE CONTROL FIRST, so that the recorded frame's `differing_texels` is measured against it.
    // Two calls of one function with one argument changed is the only arrangement in which the
    // difference between the two pictures is the record callbacks and nothing else.
    (void)std::snprintf(path, sizeof(path), "%s-no-callbacks.png", options_.capture_prefix);
    if (Status control =
            capture_->shoot(*presentation_, {}, {}, CaptureMode::Assembled, path, control_report_);
        !control) {
        return control;
    }
    (void)std::snprintf(path, sizeof(path), "%s.png", options_.capture_prefix);
    return capture_->shoot(*presentation_, particles, Span<const char* const>(readout, 5),
                           CaptureMode::RecordedWithParticles, path, capture_report_);
}

}  // namespace cy::sample::slice
