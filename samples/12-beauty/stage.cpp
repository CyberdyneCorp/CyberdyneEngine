// The device, the content on it, and the frame. M11.c section 7. See shot.h.

#include "shot.h"

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/pipeline.h>
#include <cy/backends/rhi/validation.h>
#include <cy/core/assets/file.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/import/texture.h>
#include <cy/rendering/assembly/capture_manifest.h>
#include <cy/rendering/assembly/frame_assembly.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/particles/particle_renderer.h>
#include <cy/rendering/particles/strip_renderer.h>
#include <cy/rendering/pipeline/frame_bindings.h>
#include <cy/rendering/pipeline/frame_pipelines.h>
#include <cy/rendering/pipeline/frame_recorder.h>
#include <cy/rendering/sky/celestial.h>
#include <cy/rendering/sky/clouds.h>
#include <cy/rendering/sky/composition.h>
#include <cy/rendering/sky/tables.h>

#if defined(CY_SAMPLE_BEAUTY_VULKAN)
#    include <cy/backends/rhi/vulkan/vulkan_backend.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <numbers>
#include <vector>

#include "embers.h"
#include "golden.h"

namespace cy::sample::beauty {
namespace {

using cy::rendering::FramePassKind;
using cy::rendering::kInvalidResource;
using cy::rendering::PassContext;
using cy::rendering::ResourceId;
using cy::rendering::assembly::AssemblyDescription;
using cy::rendering::assembly::AssemblyReport;
using cy::rendering::assembly::CaptureManifest;
using cy::rendering::assembly::CaptureProvenance;
using cy::rendering::assembly::CapturePurpose;
using cy::rendering::assembly::FrameAssembly;
using cy::rendering::assembly::FrameSinks;
using cy::rendering::particles::ParticleRenderer;
using cy::rendering::particles::StripRenderer;
using cy::rendering::pipeline::FrameBindings;
using cy::rendering::pipeline::FramePipelineKind;
using cy::rendering::pipeline::FramePipelines;

/// Linear HDR, because the post chain's exposure and tone curve are what this artefact is about.
constexpr rhi::Format kSceneFormat = rhi::Format::Rgba16Sfloat;
constexpr rhi::Format kOutputFormat = rhi::Format::Rgba8Unorm;
constexpr rhi::Format kDepthFormat = rhi::Format::D32Sfloat;
constexpr u32 kShadowExtent = 2048;
constexpr f32 kFarPlane = 400.0F;

[[nodiscard]] f64 now_millis() noexcept {
    return std::chrono::duration<f64, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Count every validation error and PRINT the first eight of them.
///
/// Counting alone is what samples/10-world does and it is enough for a number in a manifest; it is
/// not enough to fix one. A picture taken with validation errors in it is a picture of a bug, so
/// the first few are printed where somebody will see them and the rest are counted.
void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity != rhi::ValidationSeverity::Error) {
        return;
    }
    u32& errors = *static_cast<u32*>(user);
    ++errors;
    if (errors <= 8) {
        std::fprintf(stderr, "cy_sample_beauty: validation: %s\n", message);
    }
}

// --- Small vector maths, written out
// --------------------------------------------------------------
//
// Four explicit rows rather than a `Mat4`, and dot products rather than `mul`: the shader's own
// note gives the reason, and the two halves of a convention that can disagree are exactly the two
// halves this file and `beauty.slang` are.

[[nodiscard]] Vec3 subtract(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] Vec3 scale(Vec3 a, f32 k) noexcept {
    return Vec3{a.x * k, a.y * k, a.z * k};
}

[[nodiscard]] f32 dot3(Vec3 a, Vec3 b) noexcept {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

[[nodiscard]] Vec3 cross3(Vec3 a, Vec3 b) noexcept {
    return Vec3{(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

[[nodiscard]] Vec3 normalise(Vec3 a) noexcept {
    const f32 length = std::sqrt(dot3(a, a));
    return length > 1.0e-8F ? scale(a, 1.0F / length) : Vec3{0.0F, 0.0F, 1.0F};
}

/// The frame's constant block, laid out as `BeautyFrame` in beauty.slang.
struct FrameConstants {
    f32 view_projection[4][4] = {};
    f32 sun_to_clip[4][4] = {};
    f32 sun_direction[4] = {};
    f32 sun_color[4] = {};
    f32 ambient[4] = {};
    f32 eye[4] = {};
};

static_assert(sizeof(FrameConstants) == 192, "BeautyFrame is twelve float4");

/// The per-draw push block, laid out as `BeautyPush`.
struct SurfacePush {
    f32 normal_slot_bits = 0.0F;
    f32 uv_scale = 1.0F;
    f32 normal_strength = 1.0F;
    f32 data_slot_bits = 0.0F;
};

static_assert(sizeof(SurfacePush) == 16, "BeautyPush is one float4");

[[nodiscard]] f32 bit_cast_to_float(u32 value) noexcept {
    f32 result = 0.0F;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

}  // namespace

// ================================================================================================
// WHAT THE DEVICE HOLDS
// ================================================================================================

/// One run of the scene pass: a contiguous index range shaded by one material's own pipeline.
struct Stage::Batch {
    u32 first_index = 0;
    u32 index_count = 0;
    /// Which of the shot's materials shades it. Resolved to a pipeline and a set once those exist.
    u32 material = 0;
    rhi::GraphicsPipelineHandle pipeline;
    rhi::DescriptorSetHandle material_set;
    SurfacePush push;
};

struct Stage::Device {
    explicit Device(Allocator& allocator) noexcept
        : assembly(allocator),
          field(allocator),
          batches(allocator),
          textures(allocator),
          views(allocator),
          material_sets(allocator) {}

    Expected<rhi::Device*, Error> handle = fail(ErrorCode::Unavailable, "not created");
    rhi::BackendSelection selection{};
    u32 validation_errors = 0;

    FrameAssembly assembly;
    FramePipelines pipelines;
    FrameBindings bindings;

    // THE AIR. `field` owns the cooked system and the simulation world; `air` owns the sprite
    // pipeline and the ring the frame draws from. Both are members rather than locals because the
    // world holds a pointer INTO the compiled system and the renderer holds device handles, so
    // neither may outlive the other or the device.
    EmberField field;
    ParticleRenderer air;
    /// The motes' trails, drawn by `vfx-system`'s strip renderer in the same stage as the motes.
    StripRenderer trails;
    bool air_settled = false;

    rhi::BufferHandle vertices;
    rhi::BufferHandle indices;
    rhi::BufferHandle sky_vertices;
    rhi::BufferHandle sky_indices;
    rhi::BufferHandle frame_constants;
    rhi::BufferHandle readback;
    rhi::BufferHandle linear_readback;
    rhi::BufferHandle staging;

    rhi::TextureHandle shadow;
    rhi::TextureViewHandle shadow_view;
    rhi::ImageUse shadow_layout = rhi::ImageUse::Undefined;
    rhi::TextureHandle output;

    rhi::SamplerHandle material_sampler;
    rhi::SamplerHandle shadow_sampler;

    rhi::DescriptorSetLayoutHandle table_layout;
    rhi::DescriptorSetLayoutHandle shadow_layout_handle;
    rhi::DescriptorSetLayoutHandle view_layout;
    rhi::DescriptorSetLayoutHandle material_layout;
    rhi::DescriptorSetHandle table_set;
    rhi::DescriptorSetHandle shadow_set;
    rhi::DescriptorSetHandle view_set;
    rhi::PipelineLayoutHandle layout;

    rhi::ShaderModuleHandle shadow_vertex;
    rhi::ShaderModuleHandle sky_vertex;
    rhi::ShaderModuleHandle sky_fragment;
    rhi::GraphicsPipelineHandle shadow_pipeline;
    rhi::GraphicsPipelineHandle sky_pipeline;

    Array<Batch> batches;
    Array<rhi::TextureHandle> textures;
    Array<rhi::TextureViewHandle> views;
    Array<rhi::DescriptorSetHandle> material_sets;

    u32 sky_index_count = 0;
    u32 shadow_index_count = 0;
    bool frame_ready = false;
};

Stage::~Stage() {
    if (device_ != nullptr) {
        if (device_->handle.has_value()) {
            (void)device_->handle.value()->wait_idle();
            // THE FRAME'S OWN OBJECTS FIRST and before the device is destroyed: both hold device
            // handles, which is the contract every device-owning object in this tree states.
            device_->air.shutdown();
            device_->trails.shutdown();
            device_->bindings.shutdown();
            device_->pipelines.shutdown();
        }
        delete device_;
        device_ = nullptr;
    }
}

const char* Stage::absence() const noexcept {
    if (device_ == nullptr) {
        return "the stage was never opened";
    }
    const char* reason = device_->selection.reason;
    return (reason != nullptr && reason[0] != '\0')
               ? reason
               : "no backend reported a reason, which on this platform usually means the Vulkan "
                 "loader found no driver";
}

Status Stage::open(u32 width, u32 height, u32 supersample) noexcept {
    width_ = width * supersample;
    height_ = height * supersample;
    supersample_ = supersample;
    device_ = new (std::nothrow) Device(*allocator_);
    if (device_ == nullptr) {
        return fail(ErrorCode::OutOfMemory, "the stage did not allocate");
    }
#if defined(CY_SAMPLE_BEAUTY_VULKAN)
    (void)rhi::vulkan::register_vulkan_backend();
#endif
    (void)rhi::null::register_null_backend();

    rhi::DeviceDescription description;
    description.application_name = "cy_sample_beauty";
    // VALIDATION ON, and its errors counted rather than logged: the manifest publishes the number,
    // and a picture taken with validation errors in it is a picture of a bug.
    description.enable_validation = true;
    description.enable_synchronisation_validation = true;
    device_->handle = rhi::create_device(*allocator_, "vulkan", description, device_->selection);
    if (!device_->handle.has_value()) {
        return ok();
    }
    if (device_->handle.value()->capabilities().backend() != rhi::BackendKind::Vulkan) {
        return ok();
    }
    device_->handle.value()->set_validation_callback(&count_validation,
                                                     &device_->validation_errors);
    available_ = true;
    return ok();
}

// ================================================================================================
// JUNCTION 3 AND JUNCTION 4 — the textures are cooked, uploaded and BOUND
// ================================================================================================
//
// M11.c's spike found junction 3 PARTIAL ("names BC7, delivers uncompressed RGBA8") and junction 4
// ABSENT ("nothing in the tree binds the table; the spike had to write the binding"). Both are
// here.
//
// The cook is `cy::import::TextureImporter` — the importer `cy_import` drives, unchanged, with the
// BC7/BC5 encoder M11.c task 6.1b put behind it. What it produces is a payload with a sixteen-byte
// header, a mip count and the levels back to back, and `read_cooked` below is the reader for it.
//
// THE BINDING IS THE STANDARD LIBRARY'S OWN. `cy/material.slang` declares `cyMaterialTextures[]` at
// (set 0, binding 1) and `cyMaterialSampler` at binding 2, and set 0's layout below is those two.
// The RHI has a global bindless table of its own — `bind_texture_globally` — and it is STILL never
// placed in a pipeline layout by anything in this tree; this program allocates its own set rather
// than using it, and `m11c.toml` declares that as the gap it is.

namespace {

/// A cooked texture payload, read back.
struct CookedTexture {
    import::TextureFormat format = import::TextureFormat::Unknown;
    u32 width = 0;
    u32 height = 0;
    u32 mip_count = 0;
    bool encoded = false;
    bool srgb = false;
    Span<const u8> levels;
};

[[nodiscard]] u32 read_u32(const u8* bytes) noexcept {
    return static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8U) |
           (static_cast<u32>(bytes[2]) << 16U) | (static_cast<u32>(bytes[3]) << 24U);
}

[[nodiscard]] Expected<CookedTexture, Error> read_cooked(Span<const u8> payload) noexcept {
    if (payload.size() < 20) {
        return fail(ErrorCode::InvalidArgument, "a cooked texture shorter than its header");
    }
    CookedTexture cooked;
    cooked.format = static_cast<import::TextureFormat>(static_cast<u16>(payload[4]) |
                                                       (static_cast<u16>(payload[5]) << 8U));
    cooked.encoded = payload[6] != 0;
    cooked.srgb = payload[7] != 0;
    cooked.width = read_u32(payload.data() + 8);
    cooked.height = read_u32(payload.data() + 12);
    cooked.mip_count = read_u32(payload.data() + 16);
    cooked.levels = Span<const u8>(payload.data() + 20, payload.size() - 20);
    return cooked;
}

/// The device format for a cooked one. Refuses rather than guessing: a format this program cannot
/// map is a texture that would be sampled as something it is not.
[[nodiscard]] Expected<rhi::Format, Error> device_format_of(const CookedTexture& cooked) noexcept {
    switch (cooked.format) {
        case import::TextureFormat::BC7:
            return cooked.srgb ? rhi::Format::Bc7Srgb : rhi::Format::Bc7Unorm;
        case import::TextureFormat::BC5:
            return rhi::Format::Bc5Unorm;
        case import::TextureFormat::BC4:
            return rhi::Format::Bc4Unorm;
        case import::TextureFormat::RGBA8:
            return cooked.srgb ? rhi::Format::Rgba8Srgb : rhi::Format::Rgba8Unorm;
        default:
            return fail(ErrorCode::Unsupported,
                        "the importer produced a format this frame cannot bind; BC6H and ASTC have "
                        "no encoder in this build and RGBA8 with fewer than four channels has no "
                        "device format");
    }
}

/// How many bytes one mip level of a cooked payload occupies.
[[nodiscard]] usize level_bytes(const CookedTexture& cooked, u32 width, u32 height) noexcept {
    switch (cooked.format) {
        case import::TextureFormat::BC7:
        case import::TextureFormat::BC5:
            return static_cast<usize>((width + 3U) / 4U) * ((height + 3U) / 4U) * 16U;
        case import::TextureFormat::BC4:
            return static_cast<usize>((width + 3U) / 4U) * ((height + 3U) / 4U) * 8U;
        default:
            return static_cast<usize>(width) * height * 4U;
    }
}

/// One copy the upload pass records.
struct Copy {
    u32 texture = 0;
    u16 mip = 0;
    u64 offset = 0;
    u32 width = 0;
    u32 height = 0;
};

struct UploadState {
    const rendering::GraphExecutor* executor = nullptr;
    const Array<rhi::TextureHandle>* textures = nullptr;
    const std::vector<Copy>* copies = nullptr;
    rhi::BufferHandle staging;
};

void record_uploads(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<UploadState*>(user);
    for (const Copy& copy : *state->copies) {
        rhi::BufferTextureCopy region;
        region.buffer_offset = copy.offset;
        region.mip_level = copy.mip;
        region.texture_extent = rhi::Extent3D{copy.width, copy.height, 1};
        context.commands->copy_buffer_to_texture(state->staging, (*state->textures)[copy.texture],
                                                 Span<const rhi::BufferTextureCopy>(&region, 1));
    }
}

/// The importer's options for one of the three maps a material has.
[[nodiscard]] Status configure_usage(import::ImportOptions& options,
                                     const import::OptionsSchema& schema, const char* usage,
                                     bool srgb) noexcept {
    if (Status set = options.set(schema, "usage", import::OptionValue::of_enumeration(usage));
        !set) {
        return set;
    }
    return options.set(schema, "srgb", import::OptionValue::of_bool(srgb));
}

}  // namespace

Status Stage::cook_textures(Shot& shot, ShotReport& report) noexcept {
    rhi::Device& device = *device_->handle.value();
    Allocator& memory = system_allocator(MemoryDomain::Assets);
    const import::OptionsSchema schema = import::texture_options();

    std::vector<Copy> copies;
    Array<u8> staging_bytes(memory);

    const auto cook_one = [&](const std::string& path, const char* usage, bool srgb,
                              ShotMaterial::Cooked& out) -> Status {
        Array<u8> source(memory);
        if (Status read = assets::fs::read_whole(path.c_str(), source); !read) {
            std::fprintf(stderr, "cy_sample_beauty: cannot read %s\n", path.c_str());
            return read;
        }
        import::ImportOptions options;
        if (Status set = configure_usage(options, schema, usage, srgb); !set) {
            return set;
        }
        import::TextureImporter importer;
        import::ImportRequest request;
        auto normalised_path = assets::VirtualPath::normalise(path);
        if (!normalised_path) {
            return make_unexpected(normalised_path.error());
        }
        request.source = normalised_path.value();
        request.bytes = source.span();
        request.options = &options;
        import::ImportResult result;
        if (Status imported = importer.import(request, result); !imported) {
            std::fprintf(stderr, "cy_sample_beauty: %s: %s\n", path.c_str(),
                         imported.error().message);
            return imported;
        }
        for (const import::ImportDiagnostic& diagnostic : result.diagnostics()) {
            std::fprintf(stderr, "cy_sample_beauty: %s: %s: %s\n", path.c_str(), diagnostic.code,
                         diagnostic.detail);
        }
        if (result.assets().empty()) {
            return fail(ErrorCode::InvalidArgument, "the importer produced no cooked texture");
        }
        auto cooked = read_cooked(result.assets()[0].payload.span());
        if (!cooked) {
            return make_unexpected(cooked.error());
        }
        auto format = device_format_of(cooked.value());
        if (!format) {
            return make_unexpected(format.error());
        }

        rhi::TextureDescription description;
        description.name = "beauty material texture";
        description.format = format.value();
        description.extent = rhi::Extent3D{cooked.value().width, cooked.value().height, 1};
        description.mip_levels = static_cast<u16>(cooked.value().mip_count);
        description.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination;
        auto texture = device.create_texture(description);
        if (!texture) {
            return make_unexpected(texture.error());
        }
        rhi::TextureViewDescription view_description;
        view_description.name = "beauty material view";
        view_description.texture = *texture;
        view_description.range.mip_count = static_cast<u16>(cooked.value().mip_count);
        auto view = device.create_texture_view(view_description);
        if (!view) {
            return make_unexpected(view.error());
        }

        const u32 slot = static_cast<u32>(device_->textures.size());
        if (Status pushed = device_->textures.push_back(*texture); !pushed) {
            return pushed;
        }
        if (Status pushed = device_->views.push_back(*view); !pushed) {
            return pushed;
        }

        usize cursor = 0;
        u32 width = cooked.value().width;
        u32 height = cooked.value().height;
        for (u32 mip = 0; mip < cooked.value().mip_count; ++mip) {
            const usize bytes = level_bytes(cooked.value(), width, height);
            if (cursor + bytes > cooked.value().levels.size()) {
                return fail(ErrorCode::InvalidArgument,
                            "a cooked texture shorter than the levels its header declares");
            }
            // 16-byte aligned, which every block format's copy wants and no uncompressed one minds.
            while ((staging_bytes.size() % 16U) != 0) {
                if (Status pushed = staging_bytes.push_back(0); !pushed) {
                    return pushed;
                }
            }
            copies.push_back(Copy{slot, static_cast<u16>(mip),
                                  static_cast<u64>(staging_bytes.size()), width, height});
            if (Status appended = staging_bytes.append(
                    Span<const u8>(cooked.value().levels.data() + cursor, bytes));
                !appended) {
                return appended;
            }
            cursor += bytes;
            width = width > 1U ? width / 2U : 1U;
            height = height > 1U ? height / 2U : 1U;
        }

        out.width = cooked.value().width;
        out.height = cooked.value().height;
        out.mip_count = cooked.value().mip_count;
        out.format = static_cast<u32>(cooked.value().format);
        out.encoded = cooked.value().encoded;
        out.payload_bytes = result.assets()[0].payload.size();
        out.source_bytes = source.size();
        out.slot = slot;
        report.texture_source_bytes += source.size();
        report.texture_cooked_bytes += result.assets()[0].payload.size();
        ++report.textures;
        return ok();
    };

    for (ShotMaterial& material : shot.materials) {
        if (Status cooked = cook_one(material.albedo_path, "colour", true, material.albedo);
            !cooked) {
            return cooked;
        }
        if (Status cooked = cook_one(material.normal_path, "normal-map", false, material.normal);
            !cooked) {
            return cooked;
        }
        if (Status cooked = cook_one(material.data_path, "data", false, material.data); !cooked) {
            return cooked;
        }
    }

    rhi::BufferDescription staging;
    staging.name = "beauty texture staging";
    staging.size = staging_bytes.size();
    staging.usage = rhi::BufferUsage::TransferSource;
    staging.memory = rhi::MemoryUse::Upload;
    auto buffer = device.create_buffer(staging);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    device_->staging = *buffer;
    auto* mapped = static_cast<u8*>(device.buffer_mapped_pointer(device_->staging));
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "the staging buffer is not mapped");
    }
    std::memcpy(mapped, staging_bytes.data(), staging_bytes.size());

    // --- The residency pass, and it is what makes the layout right ------------------------------
    //
    // The uploads write every texture as a transfer destination and NOTHING IN THIS GRAPH READS
    // THEM, so without the second pass below the graph would leave them in the transfer layout and
    // the frame — a different graph, in which they are not resources at all — would sample them
    // there. Declaring the read is what makes the graph emit the transition, which is the same
    // argument samples/03-first-light's "host read" pass makes for a buffer.
    rendering::RenderGraph graph(*allocator_);
    std::vector<ResourceId> imported;
    for (const rhi::TextureHandle& texture : device_->textures) {
        // THE REQUEST'S FORMAT IS USED, and getting it wrong is a validation error rather than a
        // silent one: the graph creates the view it barriers from this description, and Vulkan
        // refuses a view whose format differs from its image's unless the image was created
        // mutable. Asking the device what it actually made is what keeps the two in step.
        const rhi::TextureDescription* description = device.texture_description(texture);
        if (description == nullptr) {
            return fail(ErrorCode::Internal, "a cooked texture has no description");
        }
        rendering::TextureRequest request;
        request.name = "beauty material texture";
        request.format = description->format;
        request.width = description->extent.width;
        request.height = description->extent.height;
        request.mip_levels = description->mip_levels;
        imported.push_back(graph.import_texture(request, texture, rhi::ImageUse::Undefined));
    }

    UploadState state;
    state.textures = &device_->textures;
    state.copies = &copies;
    state.staging = device_->staging;

    auto uploads = graph.add_pass("beauty texture uploads", rhi::QueueKind::Graphics);
    for (const ResourceId id : imported) {
        uploads.write(id, rhi::Access::TransferWrite);
    }
    uploads.record(&record_uploads, &state);

    auto residency = graph.add_pass("beauty texture residency", rhi::QueueKind::Graphics);
    for (const ResourceId id : imported) {
        residency.read(id, rhi::Access::FragmentSampledRead);
    }
    residency.side_effect();

    if (Status declared = graph.status(); !declared) {
        return declared;
    }
    if (const Expected<u32, Error> began = device.begin_frame(); !began) {
        return make_unexpected(began.error());
    }
    Status executed = ok();
    {
        rendering::GraphExecutor executor(*allocator_, device);
        state.executor = &executor;
        auto result =
            executor.execute(graph, rendering::CompileOptions{}, rendering::ExecuteOptions{});
        if (!result) {
            executed = make_unexpected(result.error());
        } else {
            executed = device.wait_idle();
        }
        executor.release();
    }
    if (Status ended = device.end_frame(); !ended && executed) {
        executed = ended;
    }
    return executed;
}

// ================================================================================================
// THE GEOMETRY, AND WHY IT IS BAKED
// ================================================================================================
//
// Every instance's transform is applied on the processor and the camera's position subtracted, so
// what reaches the device is one vertex buffer of camera-relative positions and one index buffer,
// sorted into a contiguous range per material. That costs a rebuild if the camera moves, which this
// artefact never does, and it buys two things worth more than that here: nothing in any shader
// holds a world coordinate (design.md §3) and the per-draw push block is SIXTEEN bytes, which every
// Vulkan device has — a model matrix would have taken it past the 128-byte guarantee alongside the
// frame's own rows.

Status Stage::build_geometry(const Shot& shot, ShotReport& report) noexcept {
    rhi::Device& device = *device_->handle.value();
    Allocator& memory = system_allocator(MemoryDomain::Assets);

    std::vector<std::pair<std::string, import::MeshData>> meshes;
    for (const auto& entry : shot.meshes) {
        std::string problem;
        auto mesh = load_primitive(entry.second.c_str(), problem);
        if (!mesh) {
            std::fprintf(stderr, "cy_sample_beauty: %s\n", problem.c_str());
            return make_unexpected(mesh.error());
        }
        meshes.emplace_back(entry.first, std::move(mesh.value()));
    }

    Array<Vertex> vertices(memory);
    Array<u32> indices(memory);

    // ONE BATCH PER INSTANCE, and the reason is in the push block rather than in the draw count.
    // `uv_scale` and `normal_strength` are per INSTANCE — the ground and a pillar are the same
    // stone at different scales, which is what a real scene does — and they travel in the
    // sixteen-byte push block. A batch per material would have had to carry one scale for all of
    // them, and the picture would have been tiled wrongly in a way nothing but a person's eye would
    // catch. Thirty-one draws is thirty-one pipeline binds, which at this scale costs nothing
    // measurable.
    for (const Instance& instance : shot.instances) {
        const import::MeshData* mesh = nullptr;
        for (const auto& entry : meshes) {
            if (entry.first == instance.mesh) {
                mesh = &entry.second;
            }
        }
        if (mesh == nullptr) {
            return fail(ErrorCode::NotFound, "an instance names a mesh that did not load");
        }
        u32 material_index = 0;
        for (usize index = 0; index < shot.materials.size(); ++index) {
            if (shot.materials[index].key == instance.material) {
                material_index = static_cast<u32>(index);
            }
        }

        Batch batch;
        batch.first_index = static_cast<u32>(indices.size());
        batch.material = material_index;
        batch.push.uv_scale = instance.uv_scale;
        batch.push.normal_strength = instance.normal_strength;
        batch.push.normal_slot_bits = bit_cast_to_float(shot.materials[material_index].normal.slot);
        batch.push.data_slot_bits = bit_cast_to_float(shot.materials[material_index].data.slot);

        const f32 radians = instance.yaw_degrees * std::numbers::pi_v<f32> / 180.0F;
        const f32 cosine = std::cos(radians);
        const f32 sine = std::sin(radians);
        const auto rotate = [cosine, sine](Vec3 value) noexcept {
            return Vec3{(value.x * cosine) + (value.z * sine), value.y,
                        (value.z * cosine) - (value.x * sine)};
        };
        const u32 base = static_cast<u32>(vertices.size());
        for (usize index = 0; index < mesh->positions.size(); ++index) {
            Vertex vertex;
            const Vec3 rotated = rotate(mesh->positions[index]);
            // THE SUBTRACTION, done in f64 for the reason design.md §3 gives: the difference is
            // small and narrowing it afterwards keeps every bit that matters, where narrowing first
            // would not.
            vertex.position =
                Vec3{static_cast<f32>(
                         (static_cast<f64>(rotated.x) + static_cast<f64>(instance.position.x)) -
                         static_cast<f64>(shot.camera_position.x)),
                     static_cast<f32>(
                         (static_cast<f64>(rotated.y) + static_cast<f64>(instance.position.y)) -
                         static_cast<f64>(shot.camera_position.y)),
                     static_cast<f32>(
                         (static_cast<f64>(rotated.z) + static_cast<f64>(instance.position.z)) -
                         static_cast<f64>(shot.camera_position.z))};
            vertex.normal = rotate(mesh->normals[index]);
            const Vec4 tangent = mesh->tangents[index];
            const Vec3 rotated_tangent = rotate(Vec3{tangent.x, tangent.y, tangent.z});
            vertex.tangent =
                Vec4{rotated_tangent.x, rotated_tangent.y, rotated_tangent.z, tangent.w};
            vertex.uv = mesh->uvs.empty() ? Vec2{0.0F, 0.0F} : mesh->uvs[index];
            if (Status pushed = vertices.push_back(vertex); !pushed) {
                return pushed;
            }
        }
        for (const u32 index : mesh->indices.span()) {
            if (Status pushed = indices.push_back(base + index); !pushed) {
                return pushed;
            }
        }
        batch.index_count = static_cast<u32>(indices.size()) - batch.first_index;
        if (Status pushed = device_->batches.push_back(batch); !pushed) {
            return pushed;
        }
        ++report.instances;
    }

    report.triangles = static_cast<u32>(indices.size() / 3);
    report.materials = static_cast<u32>(shot.materials.size());
    device_->shadow_index_count = static_cast<u32>(indices.size());

    const auto upload = [&device](rhi::BufferHandle handle, const void* bytes,
                                  u64 size) noexcept -> Status {
        void* mapped = device.buffer_mapped_pointer(handle);
        if (mapped == nullptr) {
            return fail(ErrorCode::Internal, "a geometry buffer is not mapped");
        }
        std::memcpy(mapped, bytes, size);
        return ok();
    };

    rhi::BufferDescription description;
    description.name = "beauty vertices";
    description.size = vertices.size() * sizeof(Vertex);
    description.usage = rhi::BufferUsage::Vertex;
    description.memory = rhi::MemoryUse::Upload;
    auto buffer = device.create_buffer(description);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    device_->vertices = *buffer;
    if (Status uploaded = upload(device_->vertices, vertices.data(), description.size); !uploaded) {
        return uploaded;
    }

    description.name = "beauty indices";
    description.size = indices.size() * sizeof(u32);
    description.usage = rhi::BufferUsage::Index;
    buffer = device.create_buffer(description);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    device_->indices = *buffer;
    return upload(device_->indices, indices.data(), description.size);
}

// ================================================================================================
// THE SKY, AND IT IS THE ENGINE'S
// ================================================================================================
//
// A dome of directions, every vertex one call to `rendering::sky::compose_sky()` — the engine's own
// atmosphere, its own multiple-scattering table and its own volumetric cloud march. Exactly what
// samples/10-world does, and for the same reason it gives: the MODEL is the shipped one and the
// RESOLUTION is a dome, because there is still no sky shader in this tree and a per-pixel march on
// the processor costs minutes a frame.
//
// `compose_sky_lighting` answers the sun's illuminance and the sky's irradiance from the SAME
// composition, which is what stops a scene lit for a clear noon from being drawn under a storm.

namespace {

struct SkyVertex {
    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 radiance{0.0F, 0.0F, 0.0F};
};

struct SkyBuild {
    Array<SkyVertex> vertices;
    Array<u32> indices;
    Vec3 sun_illuminance{0.0F, 0.0F, 0.0F};
    Vec3 sky_irradiance{0.0F, 0.0F, 0.0F};
    Vec3 sun_direction{0.0F, 1.0F, 0.0F};

    explicit SkyBuild(Allocator& allocator) noexcept : vertices(allocator), indices(allocator) {}
};

[[nodiscard]] Status build_sky(const Shot& shot, Allocator& allocator, SkyBuild& out) noexcept {
    rendering::sky::Atmosphere atmosphere;
    rendering::sky::AtmosphereTables tables;
    if (Status configured = tables.configure(rendering::sky::SkyTableQuality::Medium);
        !configured) {
        return configured;
    }
    if (auto built = tables.build(atmosphere); !built) {
        return make_unexpected(built.error());
    }

    // THE SUN IS PLACED BY THE SHOT AND ITS COLOUR IS THE ATMOSPHERE'S. A `CelestialState` carries
    // a direction; the radiance that reaches the ground from it is what `compose_sky_lighting`
    // integrates, and a colour typed into the shot file would be a second sun.
    const f32 elevation = shot.sun_elevation_degrees * std::numbers::pi_v<f32> / 180.0F;
    const f32 azimuth = shot.sun_azimuth_degrees * std::numbers::pi_v<f32> / 180.0F;
    rendering::sky::CelestialState celestial;
    celestial.sun.direction = Vec3{std::cos(elevation) * std::cos(azimuth), std::sin(elevation),
                                   std::cos(elevation) * std::sin(azimuth)};
    celestial.sun.above_horizon = shot.sun_elevation_degrees > 0.0F;
    // `direction` points FROM the surface TOWARDS the body, which is `celestial.h`'s stated
    // convention and what the fragment stage's `sunDirection` means too. Getting it backwards
    // "lights a scene from exactly the wrong side and looks plausible until a shadow is compared
    // with the sky" — that file's own words, and the reason this is not negated anywhere.
    out.sun_direction = celestial.sun.direction;

    rendering::sky::CloudWeatherMap map;
    if (Status configured = map.configure(40, 1'000.0F); !configured) {
        return configured;
    }
    if (Status generated =
            map.generate(shot.seed ^ 0xC10D5EULL, shot.cloud_cover, shot.cloud_density);
        !generated) {
        return generated;
    }
    rendering::sky::CloudField field;
    field.map = &map;
    field.layers = rendering::sky::default_cloud_layers();
    field.seed = shot.seed ^ 0xC10D5EULL;

    rendering::sky::CloudQuality quality;
    quality.steps = shot.cloud_steps;
    quality.light_steps = 6;
    quality.octaves = 4;
    quality.multiple_scattering = true;

    rendering::sky::SkyCompositionInputs inputs;
    inputs.atmosphere = &atmosphere;
    inputs.tables = &tables;
    inputs.celestial = &celestial;
    inputs.clouds = &field;
    inputs.cloud_quality = quality;
    inputs.time_seconds = 0.0;
    inputs.view = rendering::sky::planetary_view(atmosphere, world::WorldVec3d{0.0, 0.0, 0.0});

    // 96 x 192, which is 18 432 calls to `compose_sky` and about 170 ms on this host. At 48 x 96
    // the cloud deck's edges facet visibly across a quad — the dome IS the resolution of this sky
    // and there is still no sky shader in this tree, so the only way to soften it is more vertices.
    constexpr u32 kRings = 96;
    constexpr u32 kSegments = 192;
    if (Status sized = out.vertices.resize(static_cast<usize>(kRings + 1) * kSegments); !sized) {
        return sized;
    }
    for (u32 ring = 0; ring <= kRings; ++ring) {
        // Packed towards the horizon, where the atmosphere's gradient is steepest and a uniform
        // dome bands visibly.
        const f32 t = static_cast<f32>(ring) / static_cast<f32>(kRings);
        const f32 ring_elevation = ((1.0F - std::pow(t, 1.7F)) * 1.8850F) - 0.3140F;
        for (u32 segment = 0; segment < kSegments; ++segment) {
            const f32 ring_azimuth =
                6.28318F * static_cast<f32>(segment) / static_cast<f32>(kSegments);
            const Vec3 direction{std::cos(ring_elevation) * std::cos(ring_azimuth),
                                 std::sin(ring_elevation),
                                 std::cos(ring_elevation) * std::sin(ring_azimuth)};
            SkyVertex& vertex = out.vertices[(static_cast<usize>(ring) * kSegments) + segment];
            // The dome is drawn at the far plane's distance, camera-relative, so it is behind
            // everything and needs no depth trick.
            vertex.position = scale(direction, kFarPlane * 0.9F);
            vertex.radiance = rendering::sky::compose_sky(inputs, direction).radiance;
        }
    }
    for (u32 ring = 0; ring < kRings; ++ring) {
        for (u32 segment = 0; segment < kSegments; ++segment) {
            const u32 next = (segment + 1) % kSegments;
            const u32 a = (ring * kSegments) + segment;
            const u32 b = (ring * kSegments) + next;
            const u32 c = ((ring + 1) * kSegments) + segment;
            const u32 d = ((ring + 1) * kSegments) + next;
            const u32 order[6] = {a, c, b, b, c, d};
            for (const u32 index : order) {
                if (Status pushed = out.indices.push_back(index); !pushed) {
                    return pushed;
                }
            }
        }
    }

    const rendering::sky::SkyLighting lighting = rendering::sky::compose_sky_lighting(inputs, 16);
    out.sun_illuminance = lighting.sun_illuminance;
    // THE SKY'S MEAN RADIANCE AND NOT ITS IRRADIANCE, and the factor of pi between them is the
    // whole difference between a picture with a sun in it and a flat one. The fragment stage
    // multiplies this by the albedo directly, which is `albedo / pi * E` written the other way
    // round — the same Lambertian normalisation `diffuseLambert` applies to the sun's term.
    // Multiplying by the irradiance instead made the ambient term pi times too strong, the sun
    // invisible against it and every shadow in the frame a shade of the same grey.
    out.sky_irradiance = lighting.mean_sky_radiance;
    (void)allocator;
    return ok();
}

}  // namespace

// ================================================================================================
// THE PIPELINES, AND THE SET LAYOUT THE STANDARD LIBRARY DECLARES
// ================================================================================================
//
//   set 0   the material texture table and its sampler — (binding 1, binding 2), which is where
//           `cy/material.slang` puts them and not where this file decided to
//   set 1   the shadow map and its comparison sampler
//   set 2   the frame's constants
//   set 3   the material's own parameter block, which the generated prelude declares at binding 0
//
// FOUR SETS AND NOT THREE. `cy/backends/shader/reflection.h`'s convention is global/pass/view/draw,
// and a material instance IS the per-draw set; the prelude hard-codes 3 and this layout agrees with
// it rather than the other way round.

namespace {

/// How many slots the material table declares. Nine are written; the rest repeat slot zero, so no
/// descriptor in the array is left unwritten and `BindlessPartiallyBound` is not required.
constexpr u32 kTableSlots = 16;

/// The std140 layout slangc produces for `CyMaterialParams`, MEASURED with `spirv-dis` rather than
/// assumed:
///
///     OpMemberDecorate CyMaterialParams_std140 0 Offset 0    // float3 base_color
///     OpMemberDecorate CyMaterialParams_std140 1 Offset 12   // float  roughness
///     OpMemberDecorate CyMaterialParams_std140 2 Offset 16   // float  metallic
///     OpMemberDecorate CyMaterialParams_std140 3 Offset 32   // uint   textures[]
///     OpDecorate _arr_uint ArrayStride 16
///
/// THE ARRAY'S LENGTH IS PER MATERIAL, so the member after it is at a different offset for a
/// material with two textures than for one with three — which is why this is a byte layout written
/// out rather than a struct, and why `cy_material author`'s sidecar names the textures in order and
/// `main.cpp` refuses a signature that is not the one written here.
///
/// A PARAMETER'S VALUE IS THE AUTHORED DEFAULT. The block is zeroed and only the texture slots are
/// written: the compiler folds a parameter whose value never changes into the program, so what
/// reaches this buffer for `base_color`, `roughness` and `metallic` is read only where the compiler
/// kept them uniform — and overriding them here would be a material the `.cygraph` does not
/// describe.
inline constexpr usize kMaterialBlockBytes = 96;
inline constexpr usize kTextureArrayOffset = 32;
inline constexpr usize kTextureArrayStride = 16;

}  // namespace

Status Stage::create_pipelines(const Shot& shot) noexcept {
    rhi::Device& device = *device_->handle.value();

    // --- The sets -------------------------------------------------------------------------------
    const rhi::DescriptorBinding table_bindings[2] = {
        {1, rhi::DescriptorKind::SampledTexture, kTableSlots, rhi::ShaderStage::Fragment, false},
        {2, rhi::DescriptorKind::Sampler, 1, rhi::ShaderStage::Fragment, false},
    };
    rhi::DescriptorSetLayoutDescription table;
    table.name = "beauty material table";
    table.bindings = Span<const rhi::DescriptorBinding>(table_bindings, 2);
    auto table_layout = device.create_descriptor_set_layout(table);
    if (!table_layout) {
        return make_unexpected(table_layout.error());
    }
    device_->table_layout = *table_layout;

    const rhi::DescriptorBinding shadow_bindings[2] = {
        {0, rhi::DescriptorKind::SampledTexture, 1, rhi::ShaderStage::Fragment, false},
        {1, rhi::DescriptorKind::Sampler, 1, rhi::ShaderStage::Fragment, false},
    };
    rhi::DescriptorSetLayoutDescription shadow;
    shadow.name = "beauty shadow";
    shadow.bindings = Span<const rhi::DescriptorBinding>(shadow_bindings, 2);
    auto shadow_layout = device.create_descriptor_set_layout(shadow);
    if (!shadow_layout) {
        return make_unexpected(shadow_layout.error());
    }
    device_->shadow_layout_handle = *shadow_layout;

    const rhi::DescriptorBinding view_bindings[1] = {
        {0, rhi::DescriptorKind::UniformBuffer, 1,
         rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, false},
    };
    rhi::DescriptorSetLayoutDescription view;
    view.name = "beauty frame";
    view.bindings = Span<const rhi::DescriptorBinding>(view_bindings, 1);
    auto view_layout = device.create_descriptor_set_layout(view);
    if (!view_layout) {
        return make_unexpected(view_layout.error());
    }
    device_->view_layout = *view_layout;

    const rhi::DescriptorBinding material_bindings[1] = {
        {0, rhi::DescriptorKind::UniformBuffer, 1, rhi::ShaderStage::Fragment, false},
    };
    rhi::DescriptorSetLayoutDescription material;
    material.name = "beauty material parameters";
    material.bindings = Span<const rhi::DescriptorBinding>(material_bindings, 1);
    auto material_layout = device.create_descriptor_set_layout(material);
    if (!material_layout) {
        return make_unexpected(material_layout.error());
    }
    device_->material_layout = *material_layout;

    const rhi::DescriptorSetLayoutHandle sets[4] = {device_->table_layout,
                                                    device_->shadow_layout_handle,
                                                    device_->view_layout, device_->material_layout};
    const rhi::PushConstantRange range{rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
                                       sizeof(SurfacePush)};
    rhi::PipelineLayoutDescription layout;
    layout.name = "beauty layout";
    layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(sets, 4);
    layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    auto layout_handle = device.create_pipeline_layout(layout);
    if (!layout_handle) {
        return make_unexpected(layout_handle.error());
    }
    device_->layout = *layout_handle;

    // --- The samplers ---------------------------------------------------------------------------
    rhi::SamplerDescription material_sampler;
    material_sampler.name = "beauty material sampler";
    // ANISOTROPIC, and it is the single cheapest thing that makes a ground plane look like an
    // engine's rather than a diagram's: the floor is seen at a grazing angle across most of the
    // frame, which is exactly where trilinear alone turns a gravel texture into a grey smear.
    material_sampler.max_anisotropy = 16.0F;
    auto sampler = device.create_sampler(material_sampler);
    if (!sampler) {
        return make_unexpected(sampler.error());
    }
    device_->material_sampler = *sampler;

    rhi::SamplerDescription shadow_sampler;
    shadow_sampler.name = "beauty shadow sampler";
    shadow_sampler.address_u = rhi::AddressMode::ClampToEdge;
    shadow_sampler.address_v = rhi::AddressMode::ClampToEdge;
    shadow_sampler.compare_enable = true;
    // GreaterOrEqual, because the engine's depth is reversed: near is 1. `resources.h` says it.
    shadow_sampler.compare_op = rhi::CompareOp::GreaterOrEqual;
    sampler = device.create_sampler(shadow_sampler);
    if (!sampler) {
        return make_unexpected(sampler.error());
    }
    device_->shadow_sampler = *sampler;

    // --- The shadow map -------------------------------------------------------------------------
    rhi::TextureDescription shadow_map;
    shadow_map.name = "beauty sun shadow";
    shadow_map.format = kDepthFormat;
    shadow_map.extent = rhi::Extent3D{kShadowExtent, kShadowExtent, 1};
    shadow_map.usage = rhi::TextureUsage::DepthStencilAttachment | rhi::TextureUsage::Sampled;
    auto shadow_texture = device.create_texture(shadow_map);
    if (!shadow_texture) {
        return make_unexpected(shadow_texture.error());
    }
    device_->shadow = *shadow_texture;
    rhi::TextureViewDescription shadow_view;
    shadow_view.name = "beauty sun shadow view";
    shadow_view.texture = device_->shadow;
    auto shadow_view_handle = device.create_texture_view(shadow_view);
    if (!shadow_view_handle) {
        return make_unexpected(shadow_view_handle.error());
    }
    device_->shadow_view = *shadow_view_handle;

    // --- The sets, written ----------------------------------------------------------------------
    auto table_set = device.allocate_descriptor_set(device_->table_layout, false);
    if (!table_set) {
        return make_unexpected(table_set.error());
    }
    device_->table_set = *table_set;
    std::vector<rhi::DescriptorWrite> writes;
    for (u32 slot = 0; slot < kTableSlots; ++slot) {
        rhi::DescriptorWrite write;
        write.binding = 1;
        write.array_index = slot;
        write.kind = rhi::DescriptorKind::SampledTexture;
        write.texture_view = device_->views[slot < device_->views.size() ? slot : 0];
        writes.push_back(write);
    }
    rhi::DescriptorWrite sampler_write;
    sampler_write.binding = 2;
    sampler_write.kind = rhi::DescriptorKind::Sampler;
    sampler_write.sampler = device_->material_sampler;
    writes.push_back(sampler_write);
    if (Status written = device.update_descriptor_set(
            device_->table_set, Span<const rhi::DescriptorWrite>(writes.data(), writes.size()));
        !written) {
        return written;
    }

    auto shadow_set = device.allocate_descriptor_set(device_->shadow_layout_handle, false);
    if (!shadow_set) {
        return make_unexpected(shadow_set.error());
    }
    device_->shadow_set = *shadow_set;
    rhi::DescriptorWrite shadow_writes[2];
    shadow_writes[0].binding = 0;
    shadow_writes[0].kind = rhi::DescriptorKind::SampledTexture;
    shadow_writes[0].texture_view = device_->shadow_view;
    // SHADER READ ONLY and not DepthStencilReadOnly: the graph transitions a depth image declared
    // with `FragmentSampledRead` to the general shader-read layout, and a descriptor that named the
    // depth-specific one would disagree with it at submit time. Measured, not assumed — the
    // validation layer says which layout the command buffer expected and which the image was in.
    shadow_writes[0].use = rhi::ImageUse::SampledRead;
    shadow_writes[1].binding = 1;
    shadow_writes[1].kind = rhi::DescriptorKind::Sampler;
    shadow_writes[1].sampler = device_->shadow_sampler;
    if (Status written = device.update_descriptor_set(
            device_->shadow_set, Span<const rhi::DescriptorWrite>(shadow_writes, 2));
        !written) {
        return written;
    }

    rhi::BufferDescription constants;
    constants.name = "beauty frame constants";
    constants.size = sizeof(FrameConstants);
    constants.usage = rhi::BufferUsage::Uniform;
    constants.memory = rhi::MemoryUse::Upload;
    auto constants_buffer = device.create_buffer(constants);
    if (!constants_buffer) {
        return make_unexpected(constants_buffer.error());
    }
    device_->frame_constants = *constants_buffer;
    auto view_set = device.allocate_descriptor_set(device_->view_layout, false);
    if (!view_set) {
        return make_unexpected(view_set.error());
    }
    device_->view_set = *view_set;
    rhi::DescriptorWrite view_write;
    view_write.binding = 0;
    view_write.kind = rhi::DescriptorKind::UniformBuffer;
    view_write.buffer = device_->frame_constants;
    if (Status written = device.update_descriptor_set(
            device_->view_set, Span<const rhi::DescriptorWrite>(&view_write, 1));
        !written) {
        return written;
    }

    // --- One pipeline per material, out of that material's own compiled program ------------------
    const rhi::VertexBinding binding{0, sizeof(Vertex), rhi::VertexInputRate::PerVertex};
    const rhi::VertexAttribute attributes[4] = {
        {0, 0, rhi::Format::Rgb32Sfloat, 0},
        {1, 0, rhi::Format::Rgb32Sfloat, sizeof(Vec3)},
        {2, 0, rhi::Format::Rgba32Sfloat, sizeof(Vec3) * 2},
        {3, 0, rhi::Format::Rg32Sfloat, (sizeof(Vec3) * 2) + sizeof(Vec4)},
    };
    rhi::ColorAttachmentState colour;
    colour.format = kSceneFormat;

    for (usize index = 0; index < shot.materials.size(); ++index) {
        const ShotMaterial& entry = shot.materials[index];
        rhi::ShaderModuleDescription vertex;
        vertex.name = "beauty scene vertex";
        vertex.stage = rhi::ShaderStage::Vertex;
        vertex.entry_point = "sceneVertex";
        vertex.spirv = Span<const u32>(entry.spirv.data(), entry.spirv.size());
        auto vertex_module = device.create_shader_module(vertex);
        if (!vertex_module) {
            return make_unexpected(vertex_module.error());
        }
        rhi::ShaderModuleDescription fragment;
        fragment.name = "beauty scene fragment";
        fragment.stage = rhi::ShaderStage::Fragment;
        fragment.entry_point = "sceneFragment";
        fragment.spirv = Span<const u32>(entry.spirv.data(), entry.spirv.size());
        auto fragment_module = device.create_shader_module(fragment);
        if (!fragment_module) {
            return make_unexpected(fragment_module.error());
        }

        rhi::GraphicsPipelineDescription pipeline;
        pipeline.name = "beauty scene";
        pipeline.layout = device_->layout;
        pipeline.vertex_shader = *vertex_module;
        pipeline.fragment_shader = *fragment_module;
        pipeline.vertex_bindings = Span<const rhi::VertexBinding>(&binding, 1);
        pipeline.vertex_attributes = Span<const rhi::VertexAttribute>(attributes, 4);
        pipeline.color_attachments = Span<const rhi::ColorAttachmentState>(&colour, 1);
        // THE DEFAULT WINDING, which is the engine's `perspective_reversed_z` and `look_at`
        // composed with the primitive generator's own triangle order. Checked against a picture
        // rather than reasoned about: with `Clockwise` the ground plane disappears and every closed
        // solid still looks plausible, which is what makes a single-sided surface the only shape in
        // a scene that can tell you the convention is wrong.
        pipeline.rasterisation.cull_mode = rhi::CullMode::Back;
        pipeline.depth_stencil.format = kDepthFormat;
        pipeline.depth_stencil.depth_test_enable = true;
        pipeline.depth_stencil.depth_write_enable = true;
        auto created = device.create_graphics_pipeline(pipeline);
        if (!created) {
            return make_unexpected(created.error());
        }

        // The material's own parameter block. The DEFAULTS the authored graph declared are what the
        // frame uploads: a parameter this program set to something else would be a material the
        // `.cygraph` does not describe.
        u8 params[kMaterialBlockBytes] = {};
        // THE AUTHORED DEFAULTS, out of the sidecar and in the module's own declaration order:
        // base_color at 0 (three floats), roughness at 12, metallic at 16. A zeroed block draws a
        // black material however the graph was authored, which is what the first run of this
        // program produced and what the picture showed.
        const f32 base_color[3] = {entry.parameter_defaults[0].x, entry.parameter_defaults[0].y,
                                   entry.parameter_defaults[0].z};
        std::memcpy(params + 0, base_color, sizeof(base_color));
        const f32 roughness = entry.parameter_defaults[1].x;
        std::memcpy(params + 12, &roughness, sizeof(roughness));
        const f32 metallic = entry.parameter_defaults[2].x;
        std::memcpy(params + 16, &metallic, sizeof(metallic));

        const u32 slots[2] = {entry.albedo.slot, entry.data.slot};
        for (u32 slot = 0; slot < 2; ++slot) {
            std::memcpy(params + kTextureArrayOffset + (slot * kTextureArrayStride), &slots[slot],
                        sizeof(u32));
        }
        const u32 slot_count = static_cast<u32>(entry.textures.size());
        std::memcpy(params + kTextureArrayOffset + (2U * kTextureArrayStride), &slot_count,
                    sizeof(u32));
        rhi::BufferDescription block;
        block.name = "beauty material parameters";
        block.size = kMaterialBlockBytes;
        block.usage = rhi::BufferUsage::Uniform;
        block.memory = rhi::MemoryUse::Upload;
        auto block_buffer = device.create_buffer(block);
        if (!block_buffer) {
            return make_unexpected(block_buffer.error());
        }
        void* mapped = device.buffer_mapped_pointer(*block_buffer);
        if (mapped == nullptr) {
            return fail(ErrorCode::Internal, "the material parameter block is not mapped");
        }
        std::memcpy(mapped, params, sizeof(params));

        auto material_set = device.allocate_descriptor_set(device_->material_layout, false);
        if (!material_set) {
            return make_unexpected(material_set.error());
        }
        rhi::DescriptorWrite block_write;
        block_write.binding = 0;
        block_write.kind = rhi::DescriptorKind::UniformBuffer;
        block_write.buffer = *block_buffer;
        if (Status written = device.update_descriptor_set(
                *material_set, Span<const rhi::DescriptorWrite>(&block_write, 1));
            !written) {
            return written;
        }
        if (Status pushed = device_->material_sets.push_back(*material_set); !pushed) {
            return pushed;
        }
        for (Batch& batch : device_->batches.span()) {
            if (batch.material == index) {
                batch.pipeline = *created;
                batch.material_set = *material_set;
            }
        }
    }

    // --- The shadow and sky pipelines, from the first material's module --------------------------
    //
    // ONE MODULE, FIVE ENTRY POINTS. `beauty.slang` is compiled once per material and every copy
    // contains all five; the shadow and sky stages do not read a material at all, so taking them
    // from the first copy is taking them from any copy.
    const ShotMaterial& first = shot.materials.front();
    rhi::ShaderModuleDescription shadow_vertex;
    shadow_vertex.name = "beauty shadow vertex";
    shadow_vertex.stage = rhi::ShaderStage::Vertex;
    shadow_vertex.entry_point = "shadowVertex";
    shadow_vertex.spirv = Span<const u32>(first.spirv.data(), first.spirv.size());
    auto shadow_module = device.create_shader_module(shadow_vertex);
    if (!shadow_module) {
        return make_unexpected(shadow_module.error());
    }
    device_->shadow_vertex = *shadow_module;

    const rhi::VertexAttribute position_only[1] = {{0, 0, rhi::Format::Rgb32Sfloat, 0}};
    rhi::GraphicsPipelineDescription shadow_pipeline;
    shadow_pipeline.name = "beauty shadow";
    shadow_pipeline.layout = device_->layout;
    shadow_pipeline.vertex_shader = device_->shadow_vertex;
    shadow_pipeline.vertex_bindings = Span<const rhi::VertexBinding>(&binding, 1);
    shadow_pipeline.vertex_attributes = Span<const rhi::VertexAttribute>(position_only, 1);
    shadow_pipeline.depth_stencil.format = kDepthFormat;
    shadow_pipeline.depth_stencil.depth_test_enable = true;
    shadow_pipeline.depth_stencil.depth_write_enable = true;
    // FRONT FACES CULLED in the shadow pass: the occluder recorded is then the BACK of the object,
    // which is half a wall thickness further from the light and removes most of the acne a bias
    // would otherwise have to hide.
    // THE OUTWARD FACES ARE CULLED IN THE SHADOW PASS, and the winding is the OPPOSITE of the scene
    // pass's because the sun's orthographic matrix is built here and the scene's projection is
    // `core-math`'s — they do not agree about handedness, which is a fact about two matrices rather
    // than a preference.
    //
    // WHAT IT BUYS: the ground's only face points at the sky, so it writes nothing into the shadow
    // map and cannot shadow itself. At a sun 7.4 degrees above the horizon the light-space depth
    // across one shadow texel of a horizontal surface is about 0.19 m, and no constant bias
    // survives that. A closed object still records its far side, half a wall thickness behind its
    // lit one, which is where most of the remaining acne goes.
    shadow_pipeline.rasterisation.cull_mode = rhi::CullMode::Front;
    shadow_pipeline.rasterisation.front_face = rhi::FrontFace::Clockwise;
    auto shadow_created = device.create_graphics_pipeline(shadow_pipeline);
    if (!shadow_created) {
        return make_unexpected(shadow_created.error());
    }
    device_->shadow_pipeline = *shadow_created;

    rhi::ShaderModuleDescription sky_vertex;
    sky_vertex.name = "beauty sky vertex";
    sky_vertex.stage = rhi::ShaderStage::Vertex;
    sky_vertex.entry_point = "skyVertex";
    sky_vertex.spirv = Span<const u32>(first.spirv.data(), first.spirv.size());
    auto sky_vertex_module = device.create_shader_module(sky_vertex);
    if (!sky_vertex_module) {
        return make_unexpected(sky_vertex_module.error());
    }
    device_->sky_vertex = *sky_vertex_module;
    rhi::ShaderModuleDescription sky_fragment;
    sky_fragment.name = "beauty sky fragment";
    sky_fragment.stage = rhi::ShaderStage::Fragment;
    sky_fragment.entry_point = "skyFragment";
    sky_fragment.spirv = Span<const u32>(first.spirv.data(), first.spirv.size());
    auto sky_fragment_module = device.create_shader_module(sky_fragment);
    if (!sky_fragment_module) {
        return make_unexpected(sky_fragment_module.error());
    }
    device_->sky_fragment = *sky_fragment_module;

    const rhi::VertexBinding sky_binding{0, sizeof(SkyVertex), rhi::VertexInputRate::PerVertex};
    const rhi::VertexAttribute sky_attributes[2] = {
        {0, 0, rhi::Format::Rgb32Sfloat, 0},
        {1, 0, rhi::Format::Rgb32Sfloat, sizeof(Vec3)},
    };
    rhi::GraphicsPipelineDescription sky;
    sky.name = "beauty sky";
    sky.layout = device_->layout;
    sky.vertex_shader = device_->sky_vertex;
    sky.fragment_shader = device_->sky_fragment;
    sky.vertex_bindings = Span<const rhi::VertexBinding>(&sky_binding, 1);
    sky.vertex_attributes = Span<const rhi::VertexAttribute>(sky_attributes, 2);
    sky.color_attachments = Span<const rhi::ColorAttachmentState>(&colour, 1);
    sky.rasterisation.cull_mode = rhi::CullMode::None;
    sky.depth_stencil.format = kDepthFormat;
    // The dome is drawn AFTER the scene with depth testing on and depth writes off: it fills the
    // pixels nothing covered and cannot draw over anything nearer.
    sky.depth_stencil.depth_test_enable = true;
    sky.depth_stencil.depth_write_enable = false;
    auto sky_created = device.create_graphics_pipeline(sky);
    if (!sky_created) {
        return make_unexpected(sky_created.error());
    }
    device_->sky_pipeline = *sky_created;
    return ok();
}

// ================================================================================================
// THE FRAME
// ================================================================================================

namespace {

struct SceneState {
    const rendering::GraphExecutor* executor = nullptr;
    const Stage::Batch* batches = nullptr;
    usize batch_count = 0;
    rhi::PipelineLayoutHandle layout;
    rhi::DescriptorSetHandle table;
    rhi::DescriptorSetHandle shadow;
    rhi::DescriptorSetHandle view;
    rhi::BufferHandle vertices;
    rhi::BufferHandle indices;
    rhi::BufferHandle sky_vertices;
    rhi::BufferHandle sky_indices;
    rhi::GraphicsPipelineHandle sky_pipeline;
    u32 sky_index_count = 0;
    ResourceId color = kInvalidResource;
    ResourceId depth = kInvalidResource;
    u32 width = 0;
    u32 height = 0;
};

void bind_common(const PassContext& context, const SceneState& state) noexcept {
    const rhi::DescriptorSetHandle sets[3] = {state.table, state.shadow, state.view};
    context.commands->bind_descriptor_sets(state.layout, 0,
                                           Span<const rhi::DescriptorSetHandle>(sets, 3));
}

void record_scene(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<SceneState*>(user);

    rhi::RenderAttachment colour;
    colour.view = state->executor->view(state->color);
    colour.load = rhi::LoadOp::Clear;
    colour.store = rhi::StoreOp::Store;
    // BLACK, and not a stand-in sky: the dome covers every pixel the geometry does not, so a frame
    // in which black is visible is a frame in which the dome failed to draw. A plausible clear
    // colour would have hidden that.
    colour.clear.color[3] = 1.0F;

    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, state->width, state->height};
    info.color_attachments = Span<const rhi::RenderAttachment>(&colour, 1);
    info.depth_attachment.view = state->executor->view(state->depth);
    info.depth_attachment.load = rhi::LoadOp::Clear;
    info.depth_attachment.store = rhi::StoreOp::Store;
    info.depth_attachment.clear = rhi::reversed_z_depth_clear();

    context.commands->begin_rendering(info);
    context.commands->set_viewport(rhi::Viewport{0.0F, 0.0F, static_cast<f32>(state->width),
                                                 static_cast<f32>(state->height), 0.0F, 1.0F});
    context.commands->set_scissor(rhi::Rect2D{0, 0, state->width, state->height});
    bind_common(context, *state);

    const u64 offset = 0;
    for (usize index = 0; index < state->batch_count; ++index) {
        const Stage::Batch& batch = state->batches[index];
        context.commands->bind_graphics_pipeline(batch.pipeline);
        context.commands->bind_descriptor_sets(
            state->layout, 3, Span<const rhi::DescriptorSetHandle>(&batch.material_set, 1));
        context.commands->bind_vertex_buffers(0, Span<const rhi::BufferHandle>(&state->vertices, 1),
                                              Span<const u64>(&offset, 1));
        context.commands->bind_index_buffer(state->indices, 0, true);
        context.commands->push_constants(
            state->layout, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
            Span<const u8>(reinterpret_cast<const u8*>(&batch.push), sizeof(SurfacePush)));
        context.commands->draw_indexed(batch.index_count, 1, batch.first_index, 0, 0);
    }
    context.commands->end_rendering();
}

/// The sky dome, in the frame's own `Sky` stage — which is the thirteenth stage's fifth, and the
/// one `ForwardFrame` reserves for exactly this.
void record_sky(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<SceneState*>(user);
    rhi::RenderAttachment colour;
    colour.view = state->executor->view(state->color);
    colour.load = rhi::LoadOp::Load;
    colour.store = rhi::StoreOp::Store;

    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, state->width, state->height};
    info.color_attachments = Span<const rhi::RenderAttachment>(&colour, 1);
    info.depth_attachment.view = state->executor->view(state->depth);
    info.depth_attachment.load = rhi::LoadOp::Load;
    info.depth_attachment.store = rhi::StoreOp::Store;

    context.commands->begin_rendering(info);
    context.commands->set_viewport(rhi::Viewport{0.0F, 0.0F, static_cast<f32>(state->width),
                                                 static_cast<f32>(state->height), 0.0F, 1.0F});
    context.commands->set_scissor(rhi::Rect2D{0, 0, state->width, state->height});
    bind_common(context, *state);
    context.commands->bind_graphics_pipeline(state->sky_pipeline);
    const u64 offset = 0;
    context.commands->bind_vertex_buffers(0, Span<const rhi::BufferHandle>(&state->sky_vertices, 1),
                                          Span<const u64>(&offset, 1));
    context.commands->bind_index_buffer(state->sky_indices, 0, true);
    SurfacePush push;
    push.normal_slot_bits = bit_cast_to_float(0xFFFFFFFFU);
    push.data_slot_bits = bit_cast_to_float(0xFFFFFFFFU);
    context.commands->push_constants(
        state->layout, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&push), sizeof(SurfacePush)));
    context.commands->draw_indexed(state->sky_index_count, 1, 0, 0, 0);
    context.commands->end_rendering();
}

struct ShadowState {
    const rendering::GraphExecutor* executor = nullptr;
    ResourceId depth = kInvalidResource;
    rhi::PipelineLayoutHandle layout;
    rhi::DescriptorSetHandle table;
    rhi::DescriptorSetHandle shadow;
    rhi::DescriptorSetHandle view;
    rhi::GraphicsPipelineHandle pipeline;
    rhi::BufferHandle vertices;
    rhi::BufferHandle indices;
    u32 index_count = 0;
};

void record_shadow(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<ShadowState*>(user);
    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, kShadowExtent, kShadowExtent};
    info.depth_attachment.view = state->executor->view(state->depth);
    info.depth_attachment.load = rhi::LoadOp::Clear;
    info.depth_attachment.store = rhi::StoreOp::Store;
    info.depth_attachment.clear = rhi::reversed_z_depth_clear();

    context.commands->begin_rendering(info);
    context.commands->set_viewport(rhi::Viewport{0.0F, 0.0F, static_cast<f32>(kShadowExtent),
                                                 static_cast<f32>(kShadowExtent), 0.0F, 1.0F});
    context.commands->set_scissor(rhi::Rect2D{0, 0, kShadowExtent, kShadowExtent});
    const rhi::DescriptorSetHandle sets[3] = {state->table, state->shadow, state->view};
    context.commands->bind_descriptor_sets(state->layout, 0,
                                           Span<const rhi::DescriptorSetHandle>(sets, 3));
    context.commands->bind_graphics_pipeline(state->pipeline);
    const u64 offset = 0;
    context.commands->bind_vertex_buffers(0, Span<const rhi::BufferHandle>(&state->vertices, 1),
                                          Span<const u64>(&offset, 1));
    context.commands->bind_index_buffer(state->indices, 0, true);
    SurfacePush push;
    context.commands->push_constants(
        state->layout, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&push), sizeof(SurfacePush)));
    context.commands->draw_indexed(state->index_count, 1, 0, 0, 0);
    context.commands->end_rendering();
}

struct ResolveState {
    const rendering::GraphExecutor* executor = nullptr;
    FramePipelines* pipelines = nullptr;
    FrameBindings* bindings = nullptr;
    ResourceId scene = kInvalidResource;
    ResourceId output = kInvalidResource;
    u32 width = 0;
    u32 height = 0;
};

void record_resolve(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<ResolveState*>(user);
    if (Status bound = state->bindings->bind_scene_color(state->executor->view(state->scene));
        !bound) {
        return;
    }
    rhi::RenderAttachment colour;
    colour.view = state->executor->view(state->output);
    colour.load = rhi::LoadOp::Clear;
    colour.store = rhi::StoreOp::Store;
    colour.clear.color[3] = 1.0F;

    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, state->width, state->height};
    info.color_attachments = Span<const rhi::RenderAttachment>(&colour, 1);

    context.commands->begin_rendering(info);
    context.commands->set_viewport(rhi::Viewport{0.0F, 0.0F, static_cast<f32>(state->width),
                                                 static_cast<f32>(state->height), 0.0F, 1.0F});
    context.commands->set_scissor(rhi::Rect2D{0, 0, state->width, state->height});
    context.commands->bind_descriptor_sets(state->pipelines->layout(), 0, state->bindings->sets());
    context.commands->bind_graphics_pipeline(
        state->pipelines->pipeline(FramePipelineKind::Resolve));
    context.commands->draw(3, 1, 0, 0);
    context.commands->end_rendering();
}

/// THE AIR, in the frame's own TRANSPARENT stage. M11.c task 7.5, and `m11c:vfx-in-the-shot`.
///
/// It is the FRAME'S stage and not a pass of this program's. `ForwardFrame` declares
/// `FramePassKind::Transparent` between the sky and the resolve, reading the depth the opaque pass
/// wrote, and until this rung the beauty shot left that sink empty. Filling it is what puts a mote
/// BEHIND the pillar it is behind, and what grades the field through the same exposure and the same
/// tone curve as the stone — two things a separate pass of this program's own would each have had
/// to get right a second time.
///
/// The draw itself is `ParticleRenderer`'s, invoked through the `PassExtension` seam it publishes
/// rather than reimplemented here. This program has no `FrameRecorder` — it records its own
/// geometry out of its own buffers — so it builds the `ExtensionContext` the recorder would have
/// built. `recorder` stays null, which `record_particles` never reads; `inside_rendering` is true,
/// which it does.
struct AirState {
    const rendering::GraphExecutor* executor = nullptr;
    FramePipelines* pipelines = nullptr;
    FrameBindings* bindings = nullptr;
    ParticleRenderer* air = nullptr;
    /// The trails behind the motes. Drawn FIRST, so each mote's bright core composites over its own
    /// wake rather than under it.
    StripRenderer* trails = nullptr;
    ResourceId color = kInvalidResource;
    ResourceId depth = kInvalidResource;
    u32 width = 0;
    u32 height = 0;
};

void record_air(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<AirState*>(user);
    if (state->air == nullptr || !state->air->ready()) {
        return;
    }
    rhi::RenderAttachment colour;
    colour.view = state->executor->view(state->color);
    colour.load = rhi::LoadOp::Load;
    colour.store = rhi::StoreOp::Store;

    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, state->width, state->height};
    info.color_attachments = Span<const rhi::RenderAttachment>(&colour, 1);
    // LOADED AND NOT CLEARED, and the sprite pipeline does not write it: the graph declared this
    // target `DepthStencilAttachmentRead` for this stage, and a blended sprite that wrote depth
    // would occlude the sprite behind it and the field would stop reading as air.
    info.depth_attachment.view = state->executor->view(state->depth);
    info.depth_attachment.load = rhi::LoadOp::Load;
    info.depth_attachment.store = rhi::StoreOp::Store;

    context.commands->begin_rendering(info);
    context.commands->set_viewport(rhi::Viewport{0.0F, 0.0F, static_cast<f32>(state->width),
                                                 static_cast<f32>(state->height), 0.0F, 1.0F});
    context.commands->set_scissor(rhi::Rect2D{0, 0, state->width, state->height});
    // THE FRAME'S OWN SETS 0 AND 1, at the frame's own layout. `ParticleRenderer` reuses those two
    // layouts verbatim so that its pipeline layout is COMPATIBLE with this one — which is the seam
    // it exists to prove — and its own ring is set 2.
    context.commands->bind_descriptor_sets(state->pipelines->layout(), 0, state->bindings->sets());

    cy::rendering::pipeline::ExtensionContext air;
    air.commands = context.commands;
    air.executor = state->executor;
    air.kind = FramePassKind::Transparent;
    air.width = state->width;
    air.height = state->height;
    air.inside_rendering = true;
    if (state->trails != nullptr && state->trails->ready()) {
        const cy::rendering::pipeline::PassExtension wake = state->trails->extension();
        wake.record(air, wake.user);
    }
    const cy::rendering::pipeline::PassExtension extension = state->air->extension();
    extension.record(air, extension.user);
    context.commands->end_rendering();
}

struct ReadbackState {
    const rendering::GraphExecutor* executor = nullptr;
    ResourceId color = kInvalidResource;
    rhi::BufferHandle buffer;
    u32 width = 0;
    u32 height = 0;
};

void record_readback(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<ReadbackState*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{state->width, state->height, 1};
    context.commands->copy_texture_to_buffer(state->executor->texture(state->color), state->buffer,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

}  // namespace

Status Stage::stage_shot(Shot& shot, ShotReport& report) noexcept {
    if (!available_) {
        return fail(ErrorCode::Unavailable, "no graphics device answered");
    }
    rhi::Device& device = *device_->handle.value();
    const f64 mark = now_millis();

    if (Status cooked = cook_textures(shot, report); !cooked) {
        return cooked;
    }
    if (Status built = build_geometry(shot, report); !built) {
        return built;
    }

    const f64 sky_mark = now_millis();
    SkyBuild sky(*allocator_);
    if (Status built = build_sky(shot, *allocator_, sky); !built) {
        return built;
    }
    report.sky_ms = now_millis() - sky_mark;
    report.sun_illuminance = sky.sun_illuminance;
    report.sky_irradiance = sky.sky_irradiance;
    sun_direction_ = sky.sun_direction;
    sun_illuminance_ = sky.sun_illuminance;
    sky_irradiance_ = sky.sky_irradiance;

    const auto upload = [&device](rhi::BufferHandle handle, const void* bytes,
                                  u64 size) noexcept -> Status {
        void* mapped = device.buffer_mapped_pointer(handle);
        if (mapped == nullptr) {
            return fail(ErrorCode::Internal, "a sky buffer is not mapped");
        }
        std::memcpy(mapped, bytes, size);
        return ok();
    };

    rhi::BufferDescription description;
    description.name = "beauty sky vertices";
    description.size = sky.vertices.size() * sizeof(SkyVertex);
    description.usage = rhi::BufferUsage::Vertex;
    description.memory = rhi::MemoryUse::Upload;
    auto buffer = device.create_buffer(description);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    device_->sky_vertices = *buffer;
    if (Status uploaded = upload(device_->sky_vertices, sky.vertices.data(), description.size);
        !uploaded) {
        return uploaded;
    }
    description.name = "beauty sky indices";
    description.size = sky.indices.size() * sizeof(u32);
    description.usage = rhi::BufferUsage::Index;
    buffer = device.create_buffer(description);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    device_->sky_indices = *buffer;
    if (Status uploaded = upload(device_->sky_indices, sky.indices.data(), description.size);
        !uploaded) {
        return uploaded;
    }
    device_->sky_index_count = static_cast<u32>(sky.indices.size());

    if (Status made = create_pipelines(shot); !made) {
        return made;
    }

    rhi::BufferDescription readback;
    readback.name = "beauty colour readback";
    readback.size = static_cast<u64>(width_) * height_ * sizeof(u32);
    readback.usage = rhi::BufferUsage::TransferDestination;
    readback.memory = rhi::MemoryUse::Readback;
    buffer = device.create_buffer(readback);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    device_->readback = *buffer;

    readback.name = "beauty scene colour readback";
    readback.size = static_cast<u64>(width_) * height_ * 8U;  // Rgba16Sfloat
    buffer = device.create_buffer(readback);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    device_->linear_readback = *buffer;

    if (Status sized = pixels_.resize(static_cast<usize>(width_) * height_); !sized) {
        return sized;
    }

    rhi::TextureDescription output;
    output.name = "beauty output";
    output.format = kOutputFormat;
    output.extent = rhi::Extent3D{width_, height_, 1};
    output.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSource;
    auto created = device.create_texture(output);
    if (!created) {
        return make_unexpected(created.error());
    }
    device_->output = *created;

    report.build_ms = now_millis() - mark;
    report.supersample = supersample_;
    return ok();
}

// ================================================================================================
// THE TWO MATRICES, WRITTEN OUT
// ================================================================================================

namespace {

void write_rows(f32 out[4][4], const Mat4& matrix) noexcept {
    for (usize row = 0; row < 4; ++row) {
        const Vec4 values = matrix.row(row);
        out[row][0] = values.x;
        out[row][1] = values.y;
        out[row][2] = values.z;
        out[row][3] = values.w;
    }
}

/// The sun's orthographic projection, reversed-Z, centred on the scene.
///
/// One cascade and a fixed extent, both from the shot file. `virtual-shadows` and
/// `rendering-lighting-and-shadows` own the cascaded, cached, paged version; this is a sample's own
/// shadow map and the manifest says so rather than implying the engine's shadow cache drew it.
void write_sun_matrix(f32 out[4][4], Vec3 to_sun, Vec3 centre, f32 extent) noexcept {
    const Vec3 forward = normalise(scale(to_sun, -1.0F));  // the light's travel direction
    const Vec3 world_up{0.0F, 1.0F, 0.0F};
    Vec3 right = cross3(world_up, forward);
    if (dot3(right, right) < 1.0e-6F) {
        right = Vec3{1.0F, 0.0F, 0.0F};
    }
    right = normalise(right);
    const Vec3 up = normalise(cross3(forward, right));
    const f32 depth = extent * 2.0F;

    // s = dot(p - centre, right) / extent, t likewise, and z = (depth - dot(p - centre, forward)) /
    // (2 * depth) + 0.5 — which is 1 at the near plane and 0 at the far one, the engine's own
    // convention.
    out[0][0] = right.x / extent;
    out[0][1] = right.y / extent;
    out[0][2] = right.z / extent;
    out[0][3] = -dot3(centre, right) / extent;
    out[1][0] = up.x / extent;
    out[1][1] = up.y / extent;
    out[1][2] = up.z / extent;
    out[1][3] = -dot3(centre, up) / extent;
    out[2][0] = -forward.x / (2.0F * depth);
    out[2][1] = -forward.y / (2.0F * depth);
    out[2][2] = -forward.z / (2.0F * depth);
    out[2][3] = 0.5F + (dot3(centre, forward) / (2.0F * depth));
    out[3][0] = 0.0F;
    out[3][1] = 0.0F;
    out[3][2] = 0.0F;
    out[3][3] = 1.0F;
}

}  // namespace

Status Stage::create_frame() noexcept {
    rhi::Device& device = *device_->handle.value();

    AssemblyDescription description;
    description.width = width_;
    description.height = height_;
    description.near_plane = 0.08F;
    description.far_plane = kFarPlane;
    description.clusters = cy::rendering::ClusterGridConfig{16, 8, 24};
    description.color_format = kSceneFormat;
    description.depth_format = kDepthFormat;
    description.material_capacity = 4;
    description.max_draws = 64;
    description.max_instances = 64;
    description.gpu_culling = false;
    // NO DEPTH PREPASS: this program's geometry is not in the frame's draw list, so a declared
    // prepass would record nothing and the opaque pass's depth clear would be a write-after-write
    // the synchronisation validator reports. With the prepass off, `ForwardFrame` declares the
    // opaque pass as the depth writer, which is what this frame actually is.
    description.depth_prepass = false;
    description.sky = cy::rendering::sky::SkyTableQuality::Low;
    // PINNED, because a capture has to be reproducible.
    description.pin_jitter = true;
    if (Status made = device_->assembly.initialize(description); !made) {
        return made;
    }
    if (Status attached = device_->assembly.attach_device(device); !attached) {
        return attached;
    }

    cy::rendering::pipeline::PipelineSetup setup;
    setup.color_format = kSceneFormat;
    setup.depth_format = kDepthFormat;
    setup.output_format = kOutputFormat;
    setup.transparency = false;
    if (Status made = device_->pipelines.initialize(device, setup); !made) {
        return made;
    }

    Expected<cy::rendering::ClusterGrid, Error> grid = cy::rendering::make_cluster_grid(
        description.clusters, width_, height_, description.near_plane, description.far_plane);
    if (!grid.has_value()) {
        return make_unexpected(grid.error());
    }
    const cy::rendering::pipeline::BindingCapacity capacity =
        cy::rendering::pipeline::BindingCapacity::for_grid(*grid, description.max_draws,
                                                           description.max_instances,
                                                           description.material_capacity, 4);
    if (Status made = device_->bindings.initialize(device, device_->pipelines, capacity); !made) {
        return made;
    }

    // --- THE AIR ------------------------------------------------------------------------------
    //
    // Cooked and settled ONCE, here, and not per frame: `EmberField` is deterministic — every
    // random draw is a hash of the particle index and the stream — so one settle gives the same
    // field on every machine and on every run, which is what makes the published still
    // reproducible. `Stage::advance_air` is how the turntable moves it; the still never calls it.
    //
    // Published against `kShotEye`, which is the point the geometry was baked relative to. The
    // motes and the colonnade are then in ONE space, and the turntable can orbit without
    // republishing.
    if (Status made =
            device_->air.initialize(device, device_->pipelines, cy::sample::beauty::kEmberRing);
        !made) {
        return made;
    }
    if (Status made = device_->trails.initialize(device, device_->pipelines,
                                                 cy::sample::beauty::kEmberTrailRing);
        !made) {
        return made;
    }
    if (Status made = device_->field.build(); !made) {
        return made;
    }
    if (Status settled = device_->field.settle(cy::sample::beauty::kShotEye); !settled) {
        return settled;
    }
    device_->air_settled = true;
    device_->frame_ready = true;
    return ok();
}

Status Stage::advance_air(f32 dt) noexcept {
    if (device_ == nullptr || !device_->air_settled) {
        return fail(ErrorCode::Unavailable, "the air was never settled");
    }
    return device_->field.advance(cy::sample::beauty::kShotEye, dt);
}

Status Stage::render(const Shot& shot, const char* png_path, const char* linear_path,
                     ShotReport& report) noexcept {
    return render_from(shot, shot.camera_position, shot.camera_target, png_path, linear_path,
                       report);
}

Status Stage::render_from(const Shot& shot, Vec3 eye_world, Vec3 target_world, const char* png_path,
                          const char* linear_path, ShotReport& report) noexcept {
    if (!available_) {
        return fail(ErrorCode::Unavailable, "no graphics device answered");
    }
    rhi::Device& device = *device_->handle.value();
    if (!device_->frame_ready) {
        if (Status made = create_frame(); !made) {
            return made;
        }
    }

    // --- The constants, written once
    // --------------------------------------------------------------
    const f32 aspect = static_cast<f32>(width_) / static_cast<f32>(height_);
    const f32 fov_y =
        2.0F *
        std::atan(std::tan(shot.field_of_view_degrees * std::numbers::pi_v<f32> / 360.0F) / aspect);
    // THE GEOMETRY IS BAKED AGAINST THE SHOT'S OWN CAMERA and the rendering camera is expressed as
    // an offset from it. For the still they are the same point and `eye` is the origin; for a
    // turntable they are not, and that is what lets two hundred and forty frames share one vertex
    // buffer.
    const Vec3 eye = subtract(eye_world, shot.camera_position);
    const Vec3 target = subtract(target_world, shot.camera_position);
    const Mat4 projection = perspective_reversed_z(fov_y, aspect, shot.near_plane, kFarPlane);
    const Mat4 camera = look_at(eye, target);
    const Mat4 world_to_clip = projection * camera;

    FrameConstants constants;
    write_rows(constants.view_projection, world_to_clip);
    // The shadow volume is centred a little ahead of the camera, along the view direction, so the
    // 26 metres of extent the shot asks for are spent on what the frame can see.
    const Vec3 forward = normalise(subtract(target, eye));
    const Vec3 ahead = scale(forward, shot.shadow_extent_metres * 0.55F);
    const Vec3 centre = Vec3{eye.x + ahead.x, eye.y + ahead.y, eye.z + ahead.z};
    write_sun_matrix(constants.sun_to_clip, sun_direction_, centre, shot.shadow_extent_metres);
    constants.sun_direction[0] = sun_direction_.x;
    constants.sun_direction[1] = sun_direction_.y;
    constants.sun_direction[2] = sun_direction_.z;
    constants.sun_direction[3] = shot.shadow_bias;
    constants.sun_color[0] = sun_illuminance_.x;
    constants.sun_color[1] = sun_illuminance_.y;
    constants.sun_color[2] = sun_illuminance_.z;
    constants.sun_color[3] = 1.0F / static_cast<f32>(kShadowExtent);
    constants.ambient[0] = sky_irradiance_.x;
    constants.ambient[1] = sky_irradiance_.y;
    constants.ambient[2] = sky_irradiance_.z;
    // How far along the surface normal the shadow lookup is moved, in metres. Content, because it
    // is a property of the scene's scale and of how low its sun is.
    constants.ambient[3] = shot.shadow_normal_offset;
    constants.eye[0] = eye.x;
    constants.eye[1] = eye.y;
    constants.eye[2] = eye.z;
    void* mapped = device.buffer_mapped_pointer(device_->frame_constants);
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "the frame constants are not mapped");
    }
    std::memcpy(mapped, &constants, sizeof(constants));

    if (Status prepared = prepare_shadow(); !prepared) {
        return prepared;
    }

    const f64 mark = now_millis();
    const Expected<u32, Error> began = device.begin_frame();
    if (!began) {
        return make_unexpected(began.error());
    }
    const u32 slot = *began;

    // THE RING, WRITTEN INSIDE THE DEVICE FRAME and before the assembly executes, which is the
    // contract `ParticleRenderer::upload` states for the same reason `FrameBindings::upload` does:
    // the descriptor set is allocated out of this frame's pool.
    device_->air.reset_report();
    if (Status uploaded = device_->air.upload(slot, device_->field.records()); !uploaded) {
        (void)device.end_frame();
        return uploaded;
    }
    device_->trails.reset_report();
    if (Status uploaded = device_->trails.upload(slot, device_->field.trails()); !uploaded) {
        (void)device.end_frame();
        return uploaded;
    }

    cy::rendering::RenderGraph graph(*allocator_);

    // --- The light the assembly is told about, which is the same sun the picture is shaded by ----
    cy::render::LightDescription light;
    light.kind = cy::render::LightKind::Directional;
    light.transform = cy::Transform::identity();
    light.transform.rotation =
        cy::Quat::from_to(Vec3{0.0F, 0.0F, -1.0F}, normalise(scale(sun_direction_, -1.0F)));
    light.intensity = 100'000.0F;
    light.color[0] = sun_illuminance_.x;
    light.color[1] = sun_illuminance_.y;
    light.color[2] = sun_illuminance_.z;
    light.casts_shadow = true;
    light.stable_id = 1;

    cy::rendering::assembly::AssemblyView view;
    view.fov_y_radians = fov_y;
    view.projection = projection;
    view.view = camera;
    view.cull.frustum = cy::Frustum::from_view_projection(world_to_clip);
    view.cull.camera_position = eye;
    view.cull.camera_forward = forward;
    view.cull.fov_y_radians = fov_y;
    view.lights = Span<const cy::render::LightDescription>(&light, 1);
    view.sun_direction = normalise(scale(sun_direction_, -1.0F));

    cy::rendering::TextureRequest output_request;
    output_request.name = "beauty output";
    output_request.format = kOutputFormat;
    output_request.width = width_;
    output_request.height = height_;
    output_request.extra_usage = rhi::TextureUsage::TransferSource;
    view.output = graph.import_texture(output_request, device_->output, rhi::ImageUse::Undefined);

    SceneState scene;
    scene.batches = device_->batches.data();
    scene.batch_count = device_->batches.size();
    scene.layout = device_->layout;
    scene.table = device_->table_set;
    scene.shadow = device_->shadow_set;
    scene.view = device_->view_set;
    scene.vertices = device_->vertices;
    scene.indices = device_->indices;
    scene.sky_vertices = device_->sky_vertices;
    scene.sky_indices = device_->sky_indices;
    scene.sky_pipeline = device_->sky_pipeline;
    scene.sky_index_count = device_->sky_index_count;
    scene.width = width_;
    scene.height = height_;

    ResolveState resolve;
    resolve.pipelines = &device_->pipelines;
    resolve.bindings = &device_->bindings;
    resolve.width = width_;
    resolve.height = height_;

    AirState air;
    air.pipelines = &device_->pipelines;
    air.bindings = &device_->bindings;
    air.air = &device_->air;
    air.trails = &device_->trails;
    air.width = width_;
    air.height = height_;

    FrameSinks sinks;
    sinks.passes[static_cast<usize>(FramePassKind::Opaque)] =
        cy::rendering::FramePassCallback{&record_scene, &scene};
    sinks.passes[static_cast<usize>(FramePassKind::Sky)] =
        cy::rendering::FramePassCallback{&record_sky, &scene};
    sinks.passes[static_cast<usize>(FramePassKind::Transparent)] =
        cy::rendering::FramePassCallback{&record_air, &air};
    sinks.passes[static_cast<usize>(FramePassKind::PostProcess)] =
        cy::rendering::FramePassCallback{&record_resolve, &resolve};

    cy::rendering::SpatialIndex index(*allocator_);
    AssemblyReport assembly_report;
    if (Status assembled = device_->assembly.assemble(index, view, sinks, graph, assembly_report);
        !assembled) {
        (void)device.end_frame();
        return assembled;
    }
    report.frame_passes = assembly_report.passes_declared;
    report.post_stages = assembly_report.post_stages;

    const cy::rendering::FrameResources& resources = device_->assembly.resources();
    scene.color = resources.color;
    scene.depth = resources.depth;
    air.color = resources.color;
    air.depth = resources.depth;
    resolve.scene = resources.color;
    resolve.output = resources.output;

    // THE EXPOSURE IS CONTENT. `content/beauty/shot.cyshot` carries it and the resolve divides by
    // it before the tone curve; a number typed here would be a grade nobody could change without a
    // compiler.
    cy::rendering::pipeline::GlobalsData globals;
    globals.exposure_stops = shot.exposure_stops;
    const u32 material_offsets[4] = {0, 0, 0, 0};
    const cy::rendering::pipeline::FrameUpload upload = cy::rendering::pipeline::upload_for(
        device_->assembly, assembly_report, world_to_clip, camera,
        Span<const cy::rendering::pipeline::InstanceTransform>(), globals, material_offsets);
    if (Status uploaded = device_->bindings.upload(slot, upload); !uploaded) {
        (void)device.end_frame();
        return uploaded;
    }

    // --- TWO READBACKS OUT OF ONE FRAME, which is what makes the before/after pair honest --------
    //
    // Task 7.4 asks for "a before/after pair of the same frame, with the post chain and with none".
    // Two RUNS would have been two frames, and a reader would have to take on trust that nothing
    // else moved between them. This is one frame, one set of draws, one sun, read twice:
    //
    //   `resources.output`  the 8-bit image the tonemapping resolve wrote — exposure divided, the
    //                       tone curve applied, the transfer function applied.
    //   `resources.color`   the linear HDR scene colour the resolve READ, straight out of the
    //                       rasteriser, before any of that.
    //
    // The second is written to PNG with the display transfer and NOTHING ELSE — no exposure, no
    // curve — which is exactly "with none", and `write_linear_png` says so where it does it.
    ReadbackState readback;
    readback.color = resources.output;
    readback.buffer = device_->readback;
    readback.width = width_;
    readback.height = height_;

    cy::rendering::BufferRequest readback_request;
    readback_request.name = "beauty colour readback";
    readback_request.size = static_cast<u64>(width_) * height_ * sizeof(u32);
    readback_request.extra_usage = rhi::BufferUsage::TransferDestination;
    const ResourceId color_out = graph.import_buffer(readback_request, device_->readback);
    graph.add_pass("beauty readback", rhi::QueueKind::Graphics)
        .read(readback.color, rhi::Access::TransferRead)
        .write(color_out, rhi::Access::TransferWrite)
        .record(&record_readback, &readback);
    graph.add_pass("beauty host", rhi::QueueKind::Graphics)
        .read(color_out, rhi::Access::HostRead)
        .side_effect();

    ReadbackState linear;
    linear.color = resources.color;
    linear.buffer = device_->linear_readback;
    linear.width = width_;
    linear.height = height_;
    ResourceId linear_out = kInvalidResource;
    if (linear_path != nullptr) {
        cy::rendering::BufferRequest linear_request;
        linear_request.name = "beauty scene colour readback";
        linear_request.size = static_cast<u64>(width_) * height_ * 8U;
        linear_request.extra_usage = rhi::BufferUsage::TransferDestination;
        linear_out = graph.import_buffer(linear_request, device_->linear_readback);
        graph.add_pass("beauty scene readback", rhi::QueueKind::Graphics)
            .read(linear.color, rhi::Access::TransferRead)
            .write(linear_out, rhi::Access::TransferWrite)
            .record(&record_readback, &linear);
        graph.add_pass("beauty scene host", rhi::QueueKind::Graphics)
            .read(linear_out, rhi::Access::HostRead)
            .side_effect();
    }
    if (Status declared = graph.status(); !declared) {
        (void)device.end_frame();
        return declared;
    }

    Status frame = ok();
    {
        cy::rendering::GraphExecutor executor(*allocator_, device);
        scene.executor = &executor;
        air.executor = &executor;
        resolve.executor = &executor;
        readback.executor = &executor;
        linear.executor = &executor;
        frame = device_->assembly.execute(executor, graph, assembly_report);
        if (frame) {
            frame = device.wait_idle();
        }
        report.submit_ms = now_millis() - mark;
        if (frame && png_path != nullptr) {
            frame = write_png(png_path);
        }
        if (frame && linear_path != nullptr) {
            frame = write_linear_png(linear_path);
        }
        executor.release();
    }
    if (Status ended = device.end_frame(); !ended && frame) {
        frame = ended;
    }

    if (frame) {
        CaptureProvenance provenance;
        provenance.title = "samples/12-beauty";
        provenance.purpose = CapturePurpose::Publication;
        // NO ARBITER RUNS IN THIS PROGRAM, so nothing can degrade the frame between the assemble
        // and the capture. `capture_manifest` refuses a publication capture whose arbiter was free
        // to move, which is task 9.3's adversarial case and is why this is a fact rather than a
        // wish.
        provenance.arbiter_pinned = true;
        provenance.ev100 = -shot.exposure_stops;
        provenance.quality = cy::rendering::post_quality_preset(cy::rendering::QualityLevel::High);
        const Expected<CaptureManifest, Error> manifest = cy::rendering::assembly::capture_manifest(
            device_->assembly.description(), assembly_report, provenance);
        if (!manifest.has_value()) {
            return make_unexpected(manifest.error());
        }
        report.manifest = *manifest;
        report.manifest_valid = true;
    }
    // THE AIR, READ OFF THE RENDERER rather than off what this program published. The two are the
    // same number when the frame worked and they are different numbers when it did not — a ring
    // that overflowed, or a stage whose sink never ran — and the manifest publishes the renderer's.
    report.particles = device_->air.report().particles;
    report.particles_dropped = device_->air.report().dropped;
    report.particle_draws = device_->air.report().draws;
    report.trail_vertices = device_->trails.report().vertices;
    report.trail_strips = device_->trails.report().strips;
    report.trail_segments = device_->trails.report().segments;
    report.trail_draws = device_->trails.report().draws;
    report.trail_dropped = device_->trails.report().dropped;
    report.validation_errors = device_->validation_errors;
    return frame;
}

Status Stage::prepare_shadow() noexcept {
    rhi::Device& device = *device_->handle.value();
    cy::rendering::RenderGraph graph(*allocator_);

    cy::rendering::TextureRequest request;
    request.name = "beauty sun shadow";
    request.format = kDepthFormat;
    request.width = kShadowExtent;
    request.height = kShadowExtent;
    const ResourceId shadow =
        graph.import_texture(request, device_->shadow, device_->shadow_layout);

    ShadowState state;
    state.depth = shadow;
    state.layout = device_->layout;
    state.table = device_->table_set;
    state.shadow = device_->shadow_set;
    state.view = device_->view_set;
    state.pipeline = device_->shadow_pipeline;
    state.vertices = device_->vertices;
    state.indices = device_->indices;
    state.index_count = device_->shadow_index_count;

    graph.add_pass("beauty sun shadow", rhi::QueueKind::Graphics)
        .write(shadow, rhi::Access::DepthStencilAttachmentWrite)
        .record(&record_shadow, &state);
    // The reader that makes the layout right, exactly as the texture residency pass above does.
    graph.add_pass("beauty shadow residency", rhi::QueueKind::Graphics)
        .read(shadow, rhi::Access::FragmentSampledRead)
        .side_effect();
    if (Status declared = graph.status(); !declared) {
        return declared;
    }

    if (const Expected<u32, Error> began = device.begin_frame(); !began) {
        return make_unexpected(began.error());
    }
    Status executed = ok();
    {
        cy::rendering::GraphExecutor executor(*allocator_, device);
        state.executor = &executor;
        auto result = executor.execute(graph, cy::rendering::CompileOptions{},
                                       cy::rendering::ExecuteOptions{});
        if (!result) {
            executed = make_unexpected(result.error());
        } else {
            executed = device.wait_idle();
        }
        executor.release();
    }
    if (Status ended = device.end_frame(); !ended && executed) {
        executed = ended;
    }
    device_->shadow_layout = rhi::ImageUse::SampledRead;
    return executed;
}

Status Stage::write_png(const char* path) noexcept {
    rhi::Device& device = *device_->handle.value();
    const auto* bytes = static_cast<const u32*>(device.buffer_mapped_pointer(device_->readback));
    if (bytes == nullptr) {
        return fail(ErrorCode::Internal, "the readback buffer is not mapped");
    }
    for (usize index = 0; index < pixels_.size(); ++index) {
        pixels_[index] = bytes[index];
    }

    // THE DOWNSAMPLE, AND IT IS SUPERSAMPLING AND NOT ANTI-ALIASING. The frame is drawn at
    // `supersample` times the published resolution and box-filtered here. It is named in the
    // manifest as supersampling because that is what it is: `FramePassKind::Temporal` is declared
    // by the frame and nothing in this tree records it, so calling this TAA would put a stage in
    // the caption that no pass ran.
    const u32 out_width = width_ / supersample_;
    const u32 out_height = height_ / supersample_;
    Array<u32> filtered(*allocator_);
    if (Status sized = filtered.resize(static_cast<usize>(out_width) * out_height); !sized) {
        return sized;
    }
    const u32 taps = supersample_ * supersample_;
    for (u32 y = 0; y < out_height; ++y) {
        for (u32 x = 0; x < out_width; ++x) {
            u32 sums[4] = {};
            for (u32 sy = 0; sy < supersample_; ++sy) {
                for (u32 sx = 0; sx < supersample_; ++sx) {
                    const u32 texel =
                        pixels_[(static_cast<usize>((y * supersample_) + sy) * width_) +
                                static_cast<usize>(x * supersample_) + sx];
                    for (u32 channel = 0; channel < 4; ++channel) {
                        sums[channel] += (texel >> (channel * 8U)) & 0xFFU;
                    }
                }
            }
            u32 packed = 0;
            for (u32 channel = 0; channel < 4; ++channel) {
                packed |= ((sums[channel] + (taps / 2U)) / taps) << (channel * 8U);
            }
            filtered[(static_cast<usize>(y) * out_width) + x] = packed;
        }
    }
    render_test::Image image(*allocator_);
    if (Status adopted = render_test::adopt(image, filtered.span(), out_width, out_height);
        !adopted) {
        return adopted;
    }
    return render_test::write_png(path, image);
}

/// One IEEE 754 binary16 as a float. Fifteen lines rather than a dependency, and the only place in
/// this program that reads a half: the scene colour is `Rgba16Sfloat` because that is what the
/// frame's linear target is, and the before/after pair needs it on the processor.
namespace {

[[nodiscard]] f32 half_to_float(u16 bits) noexcept {
    const u32 sign = static_cast<u32>(bits >> 15U) << 31U;
    u32 exponent = (bits >> 10U) & 0x1FU;
    u32 mantissa = bits & 0x3FFU;
    if (exponent == 0) {
        if (mantissa == 0) {
            const u32 zero = sign;
            f32 value = 0.0F;
            std::memcpy(&value, &zero, sizeof(value));
            return value;
        }
        // A subnormal half: normalise it by hand.
        exponent = 1;
        while ((mantissa & 0x400U) == 0) {
            mantissa <<= 1U;
            --exponent;
        }
        mantissa &= 0x3FFU;
    } else if (exponent == 31) {
        exponent = 255 - 112;  // infinity or NaN, which a render target should not hold
    }
    const u32 assembled = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
    f32 value = 0.0F;
    std::memcpy(&value, &assembled, sizeof(value));
    return value;
}

}  // namespace

Status Stage::write_linear_png(const char* path) noexcept {
    rhi::Device& device = *device_->handle.value();
    const auto* halves =
        static_cast<const u16*>(device.buffer_mapped_pointer(device_->linear_readback));
    if (halves == nullptr) {
        return fail(ErrorCode::Internal, "the scene colour readback buffer is not mapped");
    }

    // THE DISPLAY TRANSFER AND NOTHING ELSE. No exposure divide and no tone curve: this is the
    // linear scene colour the resolve reads, clamped to the range a PNG can hold and gamma-encoded
    // so a monitor shows it. A bright highlight clips to white here and keeps its shape in the
    // tonemapped image beside it, and that difference IS the post chain.
    const u32 out_width = width_ / supersample_;
    const u32 out_height = height_ / supersample_;
    Array<u32> filtered(*allocator_);
    if (Status sized = filtered.resize(static_cast<usize>(out_width) * out_height); !sized) {
        return sized;
    }
    const f32 taps = static_cast<f32>(supersample_ * supersample_);
    for (u32 y = 0; y < out_height; ++y) {
        for (u32 x = 0; x < out_width; ++x) {
            f32 sums[3] = {};
            for (u32 sy = 0; sy < supersample_; ++sy) {
                for (u32 sx = 0; sx < supersample_; ++sx) {
                    const usize texel = ((static_cast<usize>((y * supersample_) + sy) * width_) +
                                         static_cast<usize>(x * supersample_) + sx) *
                                        4U;
                    for (u32 channel = 0; channel < 3; ++channel) {
                        sums[channel] += half_to_float(halves[texel + channel]);
                    }
                }
            }
            u32 packed = 0xFF000000U;
            for (u32 channel = 0; channel < 3; ++channel) {
                const f32 linear = std::fmin(std::fmax(sums[channel] / taps, 0.0F), 1.0F);
                const f32 encoded = std::pow(linear, 1.0F / 2.2F);
                packed |= static_cast<u32>(std::lround(encoded * 255.0F)) << (channel * 8U);
            }
            filtered[(static_cast<usize>(y) * out_width) + x] = packed;
        }
    }
    render_test::Image image(*allocator_);
    if (Status adopted = render_test::adopt(image, filtered.span(), out_width, out_height);
        !adopted) {
        return adopted;
    }
    return render_test::write_png(path, image);
}

Status Stage::write_manifest(const Shot& shot, const ShotReport& report,
                             const char* path) noexcept {
    if (!report.manifest_valid) {
        return fail(ErrorCode::InvalidArgument,
                    "no frame executed, so there is no stage list to publish");
    }
    std::FILE* file = std::fopen(path, "w");
    if (file == nullptr) {
        return fail(ErrorCode::Io, "the manifest could not be written");
    }

    // THE ENGINE'S OWN MANIFEST FIRST, verbatim, and this file's additions after it. The stage list
    // is copied out of `AssemblyReport` by `capture_manifest` and nothing here re-derives it: a
    // caption that listed a stage the frame did not run is exactly what that function exists to
    // make impossible.
    char text[4096] = {};
    const Expected<usize, Error> written =
        cy::rendering::assembly::write_capture_manifest(report.manifest, text, sizeof(text));
    if (!written.has_value()) {
        (void)std::fclose(file);
        return make_unexpected(written.error());
    }
    (void)std::fwrite(text, 1, *written, file);

    (void)std::fprintf(file, "\n# content — what was authored and what the renderer produced\n");
    (void)std::fprintf(file, "scene %s\n", shot.name.c_str());
    (void)std::fprintf(file, "instances %u\n", report.instances);
    (void)std::fprintf(file, "triangles %u\n", report.triangles);
    (void)std::fprintf(file, "materials %u  (all %u are textured; none is constants)\n",
                       report.materials, report.materials);
    (void)std::fprintf(file, "textures %u  source %llu bytes  cooked %llu bytes\n", report.textures,
                       static_cast<unsigned long long>(report.texture_source_bytes),
                       static_cast<unsigned long long>(report.texture_cooked_bytes));
    (void)std::fprintf(file, "supersample %ux\n", report.supersample);
    (void)std::fprintf(file,
                       "particles %u in %u draw(s), %u dropped  (courtyard_embers: 3 emitters, "
                       "authored in samples/12-beauty/embers.cpp, drawn in the frame's TRANSPARENT "
                       "stage)\n",
                       report.particles, report.particle_draws, report.particles_dropped);
    (void)std::fprintf(file,
                       "trails %u vertices in %u strip(s), %u segment(s), %u draw(s), %u dropped  "
                       "(the same motes through vfx-system's Trail renderer, drawn by "
                       "StripRenderer in the same stage)\n",
                       report.trail_vertices, report.trail_strips, report.trail_segments,
                       report.trail_draws, report.trail_dropped);
    (void)std::fprintf(file, "validation-errors %u\n", report.validation_errors);
    (void)std::fprintf(file, "sun-illuminance %.1f %.1f %.1f\n",
                       static_cast<double>(report.sun_illuminance.x),
                       static_cast<double>(report.sun_illuminance.y),
                       static_cast<double>(report.sun_illuminance.z));
    (void)std::fprintf(
        file, "sky-irradiance %.1f %.1f %.1f\n", static_cast<double>(report.sky_irradiance.x),
        static_cast<double>(report.sky_irradiance.y), static_cast<double>(report.sky_irradiance.z));
    (void)std::fprintf(file, "build-ms %.1f  sky-ms %.1f  submit-ms %.2f\n", report.build_ms,
                       report.sky_ms, report.submit_ms);

    for (const ShotMaterial& material : shot.materials) {
        (void)std::fprintf(file, "\nmaterial %s\n", material.key.c_str());
        (void)std::fprintf(file, "  authored %s\n", material.graph_path.c_str());
        (void)std::fprintf(file, "  cook-key 0x%016llx  entry %s\n",
                           static_cast<unsigned long long>(material.cook_key),
                           material.entry_point.c_str());
        const ShotMaterial::Cooked* maps[3] = {&material.albedo, &material.normal, &material.data};
        const char* names[3] = {"albedo", "normal", "data"};
        const std::string* paths[3] = {&material.albedo_path, &material.normal_path,
                                       &material.data_path};
        for (u32 index = 0; index < 3; ++index) {
            (void)std::fprintf(file, "  %-6s %s  %ux%u  %s  %u mips  slot %u  %llu -> %llu bytes\n",
                               names[index], paths[index]->c_str(), maps[index]->width,
                               maps[index]->height,
                               import::texture_format_name(
                                   static_cast<import::TextureFormat>(maps[index]->format)),
                               maps[index]->mip_count, maps[index]->slot,
                               static_cast<unsigned long long>(maps[index]->source_bytes),
                               static_cast<unsigned long long>(maps[index]->payload_bytes));
        }
        (void)std::fprintf(file,
                           "  block-compressed %s  (the normal map is sampled by the FRAME, not by "
                           "the material: CyClosure has no normal term)\n",
                           material.albedo.encoded ? "yes" : "no");
    }
    (void)std::fclose(file);
    return ok();
}

}  // namespace cy::sample::beauty
