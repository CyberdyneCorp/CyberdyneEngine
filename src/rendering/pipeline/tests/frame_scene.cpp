// The scene both pipeline suites render, and the harness that renders it. M8.c tasks 1b.1 and 1b.2.

#include "frame_scene.h"

#include <cy/rendering/material/standard.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>

namespace cy::pipeline_test {
namespace {

/// One opaque surface per instance, whose material is the instance's own slot modulo the table.
/// `FrameSinks::surfaces` null would give one surface per instance too, with material == slot —
/// this one exists so several materials are exercised rather than one.
Span<const DrawSurface> query_surfaces(const VisibleInstance& instance, void* user) noexcept {
    auto* storage = static_cast<DrawSurface*>(user);
    storage->pipeline = 0;
    storage->material = instance.gpu_slot % kMaterialCount;
    storage->mesh = 1;
    storage->surface = 0;
    storage->blend = render::BlendMode::Opaque;
    return {storage, 1};
}

/// Where a draw's geometry is. One mesh in this scene, so the answer is the same for every draw —
/// which is the honest shape of the seam: the LOOKUP is the caller's, not the data.
bool cube_geometry(const render::DrawItem& /*item*/, const GpuDrawInstance& /*instance*/,
                   void* user, DrawGeometry& out) noexcept {
    auto* scene = static_cast<FrameScene*>(user);
    out.indices = scene->index_buffer();
    out.wide_indices = false;
    out.index_count = 36;
    out.first_index = 0;
    out.vertex_offset = 0;
    return true;
}

[[nodiscard]] Status upload_bytes(rhi::Device& device, rhi::BufferHandle buffer, const void* source,
                                  u64 bytes) noexcept {
    void* mapped = device.buffer_mapped_pointer(buffer);
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "pipeline test: a geometry buffer is not mapped");
    }
    const auto* from = static_cast<const u8*>(source);
    auto* to = static_cast<u8*>(mapped);
    for (u64 index = 0; index < bytes; ++index) {
        to[index] = from[index];
    }
    return ok();
}

[[nodiscard]] Expected<rhi::BufferHandle, Error> make_upload(rhi::Device& device, const char* name,
                                                             u64 bytes,
                                                             rhi::BufferUsage usage) noexcept {
    rhi::BufferDescription description;
    description.name = name;
    description.size = bytes;
    description.usage = usage;
    description.memory = rhi::MemoryUse::Upload;
    return device.create_buffer(description);
}

/// The display-referred format the frame's output is imported in. `Rgba8Unorm` rather than
/// `Rgba8Srgb`: `cy/fullscreen.slang`'s resolve does not apply the transfer function because a
/// swapchain is normally sRGB and the hardware applies it on write, so a UNORM capture is the one
/// that reads back the numbers the shader produced.
inline constexpr rhi::Format kOutputFormat = rhi::Format::Rgba8Unorm;

/// The readback pass: the frame's output into a host-visible buffer.
struct Readback {
    ResourceId output = kInvalidResource;
    rhi::BufferHandle buffer;
};

/// The size of one capture, in bytes.
inline constexpr u64 kReadbackBytes = u64{kWidth} * u64{kHeight} * sizeof(u32);

void record_readback(const PassContext& context, void* user) noexcept {
    auto* readback = static_cast<Readback*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{kWidth, kHeight, 1};
    context.commands->copy_texture_to_buffer(context.executor->texture(readback->output),
                                             readback->buffer,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

}  // namespace

Status FrameScene::create_geometry(rhi::Device& device) noexcept {
    struct Request {
        const char* name;
        const void* source;
        u64 bytes;
        rhi::BufferUsage usage;
        rhi::BufferHandle* out;
    };
    const Request requests[] = {
        {"cube positions", mesh_.positions, sizeof(mesh_.positions), rhi::BufferUsage::Vertex,
         &positions_},
        {"cube normals", mesh_.normals, sizeof(mesh_.normals), rhi::BufferUsage::Vertex, &normals_},
        {"cube uvs", mesh_.uvs, sizeof(mesh_.uvs), rhi::BufferUsage::Vertex, &uvs_},
        {"cube indices", mesh_.indices, sizeof(mesh_.indices), rhi::BufferUsage::Index, &indices_},
    };
    for (const Request& request : requests) {
        Expected<rhi::BufferHandle, Error> made =
            make_upload(device, request.name, request.bytes, request.usage);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        *request.out = *made;
        if (Status written = upload_bytes(device, *request.out, request.source, request.bytes);
            !written) {
            return written;
        }
    }
    return ok();
}

Status FrameScene::create_materials() noexcept {
    if (Status described =
            describe_standard_material(program_, Name::intern("standard"),
                                       render::ShadingModel::Lit, render::BlendMode::Opaque);
        !described) {
        return described;
    }
    const StandardParameters ids;
    // THE OFFSETS ARE DERIVED, WHICH IS THE POINT. `cy/frame.slang` reads base colour, roughness,
    // metallic and emission out of the GPU material table at word offsets it is TOLD, and this is
    // where they are looked up. A hardcoded number here would be a second description of a layout
    // `describe_standard_material` already decides.
    const ParameterId wanted[4] = {ids.base_color_factor, ids.roughness_factor, ids.metallic_factor,
                                   ids.emission_color};
    for (u32 index = 0; index < 4U; ++index) {
        const MaterialParameter* parameter = program_.find(wanted[index]);
        if (parameter == nullptr) {
            return fail(ErrorCode::NotFound,
                        "pipeline test: the standard material lacks a parameter the frame reads");
        }
        material_offsets_[index] = parameter->offset / 4U;
    }

    MaterialTable& table = assembly_.materials();
    static constexpr f32 kColors[kMaterialCount][3] = {
        {0.82F, 0.24F, 0.18F}, {0.20F, 0.62F, 0.85F}, {0.90F, 0.78F, 0.32F}, {0.35F, 0.80F, 0.45F}};
    for (u32 slot = 0; slot < kMaterialCount; ++slot) {
        Expected<u32, Error> allocated = table.allocate();
        if (!allocated.has_value()) {
            return make_unexpected(allocated.error());
        }
        if (Status defaults = apply_standard_defaults(program_, table, *allocated, ids);
            !defaults) {
            return defaults;
        }
        const Vec4 colour{kColors[slot][0], kColors[slot][1], kColors[slot][2], 1.0F};
        if (Status set = table.set_color(program_, *allocated, ids.base_color_factor, colour);
            !set) {
            return set;
        }
        if (Status set = table.set_float(program_, *allocated, ids.roughness_factor,
                                         0.25F + (0.2F * static_cast<f32>(slot)));
            !set) {
            return set;
        }
        if (Status set = table.set_float(program_, *allocated, ids.metallic_factor,
                                         slot == 2U ? 1.0F : 0.0F);
            !set) {
            return set;
        }
    }
    return ok();
}

Status FrameScene::fill_index() noexcept {
    if (Status sized = instances_.resize(kInstanceCount); !sized) {
        return sized;
    }
    for (u32 which = 0; which < kInstanceCount; ++which) {
        // Eleven boxes on a ring the camera looks into, and one wide slab under them so the frame
        // has a floor for the point lights to fall on. A scene with nothing but floating boxes
        // photographs as floating boxes, and a reader cannot tell a shading defect from the layout.
        const bool floor = which == 0;
        const f32 angle = static_cast<f32>(which) * 0.5711986643F;
        const f32 radius = 2.1F + (static_cast<f32>(which % 3U) * 0.55F);
        const Vec3 centre =
            floor ? Vec3{0.0F, -1.9F, -8.0F}
                  : Vec3{radius * std::cos(angle), -1.1F + (0.42F * static_cast<f32>(which % 4U)),
                         -6.4F - (radius * std::sin(angle) * 0.9F)};
        const f32 half = floor ? 6.0F : 0.34F + (0.09F * static_cast<f32>(which % 5U));

        SpatialEntry entry;
        entry.bounds = Aabb::from_center_extents(centre, Vec3{half, floor ? 0.125F : half, half});
        entry.stable_id = 900U + which;
        entry.gpu_slot = which;
        // The half-diagonal of a cube of this half-extent: sqrt(3), which is what bounds a box
        // by a sphere and is spelled from the standard library rather than as a literal.
        entry.radius = half * std::numbers::sqrt3_v<f32>;
        if (Expected<u32, Error> slot = index_.insert(entry); !slot.has_value()) {
            return make_unexpected(slot.error());
        }

        // The instance's placement, model to CAMERA-RELATIVE. The camera is at the origin looking
        // down -Z in this scene, so the camera-relative position IS the position — which is the
        // arrangement design.md §3 wants and the reason there is no world matrix anywhere here.
        InstanceTransform& transform = instances_[which];
        const f32 scale = half * 2.0F;
        transform.row0[0] = scale;
        transform.row1[1] = floor ? 0.25F : scale;
        transform.row2[2] = scale;
        transform.row0[3] = centre.x;
        transform.row1[3] = centre.y;
        transform.row2[3] = centre.z;
        transform.tint[0] = 1.0F;
        transform.tint[1] = 1.0F;
        transform.tint[2] = 1.0F;
        transform.tint[3] = 1.0F;
    }

    if (Status sized = lights_.resize(3); !sized) {
        return sized;
    }
    lights_[0].kind = render::LightKind::Directional;
    lights_[0].intensity = 22000.0F;
    // THE DIRECTION IS THE TRANSFORM'S FORWARD, and an identity rotation means (0, 0, -1): a sun
    // that travels straight away from the camera and leaves every horizontal surface unlit. The sun
    // here comes from above and behind the camera's right shoulder, which is what puts light on
    // both the boxes' faces and the floor.
    lights_[0].transform = Transform::from_translation(Vec3{0.0F, 12.0F, 4.0F});
    lights_[0].transform.rotation =
        Quat::look_rotation(normalize(Vec3{-0.28F, -0.82F, -0.50F}), Vec3{0.0F, 1.0F, 0.0F});
    lights_[0].color[0] = 1.0F;
    lights_[0].color[1] = 0.96F;
    lights_[0].color[2] = 0.88F;
    lights_[0].stable_id = 1;
    for (u32 which = 1; which < 3U; ++which) {
        lights_[which].kind = render::LightKind::Point;
        lights_[which].intensity = 9000.0F;
        lights_[which].range = 16.0F;
        lights_[which].transform =
            Transform::from_translation(Vec3{which == 1U ? -3.2F : 3.2F, 1.4F, -5.4F});
        lights_[which].color[0] = which == 1U ? 0.4F : 1.0F;
        lights_[which].color[1] = which == 1U ? 0.7F : 0.5F;
        lights_[which].color[2] = which == 1U ? 1.0F : 0.3F;
        lights_[which].stable_id = 1 + which;
    }
    return ok();
}

Status FrameScene::fill_particles() noexcept {
    if (Status sized = particles_.resize(kParticleCount); !sized) {
        return sized;
    }
    // A plume: a deterministic pseudo-random spray, so the picture is the same every run and a
    // difference between two captures is a difference in the renderer.
    u32 state = 0x9E3779B9U;
    const auto next = [&state]() noexcept {
        state = (state * 1664525U) + 1013904223U;
        return static_cast<f32>(state >> 8U) / static_cast<f32>(1U << 24U);
    };
    for (u32 which = 0; which < kParticleCount; ++which) {
        const f32 age = static_cast<f32>(which) / static_cast<f32>(kParticleCount);
        const f32 spread = 0.25F + (age * 1.1F);
        particles_[which].position[0] = -0.75F + ((next() - 0.5F) * spread);
        particles_[which].position[1] = -1.5F + (age * 2.2F) + ((next() - 0.5F) * 0.15F);
        // In FRONT of the boxes rather than among them: the effect is what the picture is of, and a
        // plume behind an opaque wall photographs as an opaque wall.
        particles_[which].position[2] = -3.4F + ((next() - 0.5F) * spread);
        particles_[which].size = 0.07F + ((1.0F - age) * 0.17F);
        // RADIANCE, NOT A COLOUR. The frame's colour target is scene-referred and the tonemap is
        // downstream, so a particle's emission is in the same physical range as a light's — a
        // colour of 1 would be invisible at the exposure a 22 000 lux sun is viewed through, and
        // that is a real property of a physically-based frame rather than a fudge factor.
        const f32 glow = 13000.0F * (1.0F - (age * 0.75F));
        particles_[which].color[0] = glow;
        particles_[which].color[1] = glow * (0.30F + (0.45F * (1.0F - age)));
        particles_[which].color[2] = glow * (0.05F + (0.12F * (1.0F - age)));
        particles_[which].color[3] = 0.85F * (1.0F - age) * (1.0F - age);
    }
    return ok();
}

Status FrameScene::build(rhi::Device& device) noexcept {
    device_ = &device;

    AssemblyDescription description;
    description.width = kWidth;
    description.height = kHeight;
    description.near_plane = 0.1F;
    description.far_plane = 120.0F;
    description.clusters = ClusterGridConfig{32, 24, 32};
    description.material_capacity = kMaterialCount;
    description.max_draws = 64;
    description.max_instances = 64;
    // The CPU cull path, deliberately: the device dispatch produces the same `CullResults` and this
    // suite is about what is RECORDED rather than about where the cull ran.
    description.gpu_culling = false;
    if (Status made = assembly_.initialize(description); !made) {
        return made;
    }
    if (Status attached = assembly_.attach_device(device); !attached) {
        return attached;
    }

    PipelineSetup setup;
    setup.color_format = description.color_format;
    setup.depth_format = description.depth_format;
    setup.output_format = kOutputFormat;
    if (Status made = pipelines_.initialize(device, setup); !made) {
        return made;
    }

    Expected<ClusterGrid, Error> grid = make_cluster_grid(
        description.clusters, kWidth, kHeight, description.near_plane, description.far_plane);
    if (!grid.has_value()) {
        return make_unexpected(grid.error());
    }
    const BindingCapacity capacity = BindingCapacity::for_grid(
        *grid, description.max_draws, description.max_instances, description.material_capacity, 16);
    if (Status made = bindings_.initialize(device, pipelines_, capacity); !made) {
        return made;
    }
    if (Status made = recorder_.initialize(pipelines_, bindings_); !made) {
        return made;
    }
    if (Status made = effect_.initialize(device, pipelines_, kParticleCount); !made) {
        return made;
    }
    if (Status made = create_geometry(device); !made) {
        return made;
    }
    if (Status made = create_materials(); !made) {
        return made;
    }
    if (Status made = fill_index(); !made) {
        return made;
    }
    if (Status made = fill_particles(); !made) {
        return made;
    }

    GeometrySource geometry;
    geometry.streams[kPositionStream] = positions_;
    geometry.streams[kNormalStream] = normals_;
    geometry.streams[kUvStream] = uvs_;
    geometry.geometry = &cube_geometry;
    geometry.user = this;
    recorder_.set_geometry(geometry);

    projection_ = perspective_reversed_z(0.9F, static_cast<f32>(kWidth) / static_cast<f32>(kHeight),
                                         description.near_plane, description.far_plane);
    view_ = look_at(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.0F, 1.0F, 0.0F});

    Expected<rhi::BufferHandle, Error> readback = make_upload(
        device, "pipeline readback", kReadbackBytes, rhi::BufferUsage::TransferDestination);
    if (!readback.has_value()) {
        return make_unexpected(readback.error());
    }
    readback_ = *readback;

    rhi::TextureDescription output;
    output.name = "pipeline output";
    output.format = kOutputFormat;
    output.extent = rhi::Extent3D{kWidth, kHeight, 1};
    output.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSource;
    Expected<rhi::TextureHandle, Error> created = device.create_texture(output);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    output_ = *created;
    built_ = true;
    return ok();
}

Status FrameScene::read_pixels() noexcept {
    if (Status sized = pixels_.resize(usize{kWidth} * usize{kHeight}); !sized) {
        return sized;
    }
    const auto* mapped = static_cast<const u32*>(device_->buffer_mapped_pointer(readback_));
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "pipeline test: the readback buffer is not mapped");
    }
    for (usize index = 0; index < pixels_.size(); ++index) {
        pixels_[index] = mapped[index];
    }
    return ok();
}

Status FrameScene::render(RecordMode mode, AssemblyReport& out) noexcept {
    if (!built_) {
        return fail(ErrorCode::Unavailable, "pipeline test: build() was not called");
    }
    rhi::Device& device = *device_;
    Expected<u32, Error> began = device.begin_frame();
    if (!began.has_value()) {
        return make_unexpected(began.error());
    }
    const u32 slot = *began;

    recorder_.clear_extensions();
    effect_.reset_report();
    if (mode == RecordMode::CallbacksAndParticles) {
        if (Status uploaded = effect_.upload(slot, particles_.span()); !uploaded) {
            return uploaded;
        }
        if (Status added = recorder_.add_extension(effect_.extension()); !added) {
            return added;
        }
    }

    graph_.reset();
    assembly::AssemblyView view;
    view.fov_y_radians = 0.9F;
    view.view = view_;
    view.projection = projection_;
    view.cull.frustum = Frustum::from_view_projection(projection_ * view_);
    view.cull.camera_position = Vec3{0.0F, 0.0F, 0.0F};
    view.cull.camera_forward = Vec3{0.0F, 0.0F, -1.0F};
    view.cull.fov_y_radians = view.fov_y_radians;
    view.lights = lights_.span();
    view.sun_direction = Vec3{0.35F, 0.86F, 0.37F};

    // IMPORTED, not frame-owned, and `Undefined` every frame because the resolve clears it: the
    // graph derives the transition from whatever the previous frame left it in, and "the contents
    // are discarded" is the truth here rather than a shortcut.
    TextureRequest request;
    request.name = "pipeline output";
    request.format = kOutputFormat;
    request.width = kWidth;
    request.height = kHeight;
    request.extra_usage = rhi::TextureUsage::TransferSource;
    view.output = graph_.import_texture(request, output_, rhi::ImageLayout::Undefined);

    DrawSurface surface;
    FrameSinks sinks;
    if (mode == RecordMode::None) {
        // THE CONTROL. This is exactly what every caller in the tree supplied before M8.c: a
        // surface query and no record callbacks at all.
        sinks.surfaces = &query_surfaces;
        sinks.surfaces_user = &surface;
    } else {
        if (Status bound = recorder_.bind(assembly_); !bound) {
            return bound;
        }
        sinks = recorder_.sinks();
        sinks.surfaces = &query_surfaces;
        sinks.surfaces_user = &surface;
    }

    if (Status assembled = assembly_.assemble(index_, view, sinks, graph_, out); !assembled) {
        return assembled;
    }

    if (mode != RecordMode::None) {
        // The upload happens after `assemble` because the light records and the cluster lists are
        // its outputs, and before `execute` because the record callbacks read the sets it writes.
        GlobalsData globals;
        // EV-like exposure, and it has to be here rather than in the shader: the frame's colour
        // target holds illuminance in physical units — a 22 000 lux sun on a 0.8 albedo surface is
        // a few thousand nits — and `cy/fullscreen.slang`'s resolve divides by 2^-stops before it
        // tonemaps. Zero stops photographs a physically-lit scene as a white rectangle, which is
        // exactly what the first run of this suite produced.
        globals.exposure_stops = -11.4F;
        const FrameUpload upload = upload_for(assembly_, out, projection_ * view_, view_,
                                              instances_.span(), globals, material_offsets_);
        if (getenv("CY_PIPELINE_DEBUG") != nullptr && !upload.lights.empty()) {
            const GpuLight& light = upload.lights[0];
            std::fprintf(stderr, "light0 intensity=%f colour=(%f %f %f) kind=%u\n",
                         static_cast<double>(light.intensity), static_cast<double>(light.color[0]),
                         static_cast<double>(light.color[1]), static_cast<double>(light.color[2]),
                         light.kind);
            const auto* words = reinterpret_cast<const f32*>(upload.materials.data());
            std::fprintf(stderr,
                         "material0 base=(%f %f %f) rough=%f metal=%f offsets=%u %u %u %u\n",
                         static_cast<double>(words[material_offsets_[0]]),
                         static_cast<double>(words[material_offsets_[0] + 1]),
                         static_cast<double>(words[material_offsets_[0] + 2]),
                         static_cast<double>(words[material_offsets_[1]]),
                         static_cast<double>(words[material_offsets_[2]]), material_offsets_[0],
                         material_offsets_[1], material_offsets_[2], material_offsets_[3]);
        }
        if (Status uploaded = bindings_.upload(slot, upload); !uploaded) {
            return uploaded;
        }
    }

    Readback readback;
    readback.output = assembly_.resources().output;
    readback.buffer = readback_;
    if (read_back_ && readback.output != kInvalidResource) {
        // THE DESTINATION IS IMPORTED AND THE WRITE IS DECLARED, and both halves are load-bearing.
        // `rhi-and-render-graph`'s culling rule drops a pass whose output nothing consumes, and the
        // first version of this declared a READ of the output and no write at all — so the graph
        // culled the capture, the copy never happened, and every committed picture was uniformly
        // zero. A write to an imported resource is a culling root, which is what makes this
        // survive.
        BufferRequest capture_request;
        capture_request.name = "pipeline capture";
        capture_request.size = kReadbackBytes;
        capture_request.extra_usage = rhi::BufferUsage::TransferDestination;
        const ResourceId destination = graph_.import_buffer(capture_request, readback_);
        graph_.add_pass("capture", rhi::QueueKind::Graphics)
            .read(readback.output, rhi::Access::TransferRead)
            .write(destination, rhi::Access::TransferWrite)
            .record(&record_readback, &readback);
        // The host boundary declared as a dependency, so the graph emits the transfer-to-host
        // barrier rather than this suite relying on coherent memory and a fence. M3 spike, 6f.
        graph_.add_pass("capture host", rhi::QueueKind::Graphics)
            .read(destination, rhi::Access::HostRead)
            .side_effect();
    }

    GraphExecutor executor(*allocator_, device);
    Status executed = assembly_.execute(executor, graph_, out);
    if (executed) {
        executed = device.wait_idle();
    }
    if (executed && read_back_ && readback.output != kInvalidResource) {
        executed = read_pixels();
    }
    executor.release();
    if (Status ended = device.end_frame(); !ended && executed) {
        return ended;
    }
    return executed;
}

u32 FrameScene::differing_texels(Span<const u32> other) const noexcept {
    if (other.size() != pixels_.size()) {
        return static_cast<u32>(pixels_.size() > other.size() ? pixels_.size() : other.size());
    }
    u32 differing = 0;
    for (usize index = 0; index < pixels_.size(); ++index) {
        const u32 a = pixels_[index];
        const u32 b = other[index];
        for (u32 channel = 0; channel < 4U; ++channel) {
            const auto left = static_cast<i32>((a >> (channel * 8U)) & 0xFFU);
            const auto right = static_cast<i32>((b >> (channel * 8U)) & 0xFFU);
            if (left - right > 1 || right - left > 1) {
                ++differing;
                break;
            }
        }
    }
    return differing;
}

void FrameScene::release() noexcept {
    if (device_ == nullptr) {
        return;
    }
    (void)device_->wait_idle();
    effect_.shutdown();
    bindings_.shutdown();
    pipelines_.shutdown();
    if (!output_.is_null()) {
        device_->destroy_texture(output_);
        output_ = rhi::TextureHandle{};
    }
    rhi::BufferHandle* buffers[] = {&positions_, &normals_, &uvs_, &indices_, &readback_};
    for (rhi::BufferHandle* handle : buffers) {
        if (!handle->is_null()) {
            device_->destroy_buffer(*handle);
            *handle = rhi::BufferHandle{};
        }
    }
    device_ = nullptr;
    built_ = false;
}

}  // namespace cy::pipeline_test
