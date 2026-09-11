// The frame `render.vfx` photographs. M8.c section 2. See vfx_scene.h for what it proves.

#include "vfx_scene.h"

#include "effects.h"

#include <cy/core/math/projection.h>

#include <cstdio>

namespace cy::vfx_test {
namespace {

inline constexpr rhi::Format kOutputFormat = rhi::Format::Rgba8Unorm;
inline constexpr u64 kReadbackBytes = u64{kSceneWidth} * u64{kSceneHeight} * sizeof(u32);

/// This scene has no meshes, so no draw ever asks. The lookup exists because `FrameRecorder::bind`
/// requires a complete geometry source — the mesh table is the caller's and this layer holds no
/// copy of it — and answering false is the honest answer for "this draw has nothing to draw".
bool no_geometry(const render::DrawItem& /*item*/, const GpuDrawInstance& /*instance*/,
                 void* /*user*/, DrawGeometry& /*out*/) noexcept {
    return false;
}

struct Readback {
    ResourceId output = kInvalidResource;
    rhi::BufferHandle buffer;
};

void record_readback(const PassContext& context, void* user) noexcept {
    auto* readback = static_cast<Readback*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{kSceneWidth, kSceneHeight, 1};
    context.commands->copy_texture_to_buffer(context.executor->texture(readback->output),
                                             readback->buffer,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
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

}  // namespace

VfxScene::VfxScene(Allocator& allocator) noexcept
    : allocator_(&allocator),
      sink_(allocator),
      cook_(allocator),
      world_(allocator),
      records_(allocator),
      assembly_(allocator),
      index_(allocator),
      graph_(allocator),
      instances_(allocator),
      lights_(allocator),
      pixels_(allocator) {}

VfxScene::~VfxScene() {
    release();
}

Status VfxScene::build(rhi::Device& device, u32 instances) noexcept {
    device_ = &device;

    vfx::CompileOptions options;
    auto compiled = cook_plume(*allocator_, sink_, cook_, options, 1, 384);
    if (!compiled) {
        return make_unexpected(compiled.error());
    }
    system_ = Expected<vfx::CompiledSystem, Error>(std::move(compiled.value()));

    vfx::WorldDescription world;
    world.pool_bytes = 8ULL * 1024ULL * 1024ULL;
    world.max_instances = 64;
    if (Status made = world_.initialize(world); !made) {
        return made;
    }
    for (u32 which = 0; which < instances; ++which) {
        vfx::EffectSpawn spawn;
        // A row of plumes across the view, in front of the camera. Camera-relative from here on:
        // the camera sits at the origin looking down -Z, which is the arrangement the frame's own
        // conventions want and the reason nothing here builds a world matrix.
        const f32 across = static_cast<f32>(which) - ((static_cast<f32>(instances) - 1.0F) * 0.5F);
        spawn.position = Vec3{across * 1.45F, -2.1F, -5.6F - (static_cast<f32>(which % 3U) * 1.1F)};
        spawn.scale = 1.0F;
        if (Expected<vfx::EffectHandle, Error> played = world_.play(system_.value(), spawn);
            !played.has_value()) {
            return make_unexpected(played.error());
        }
    }

    assembly::AssemblyDescription description;
    description.width = kSceneWidth;
    description.height = kSceneHeight;
    description.near_plane = 0.1F;
    description.far_plane = 120.0F;
    description.clusters = ClusterGridConfig{32, 24, 32};
    description.material_capacity = 1;
    description.max_draws = 8;
    description.max_instances = 8;
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
    Expected<ClusterGrid, Error> grid =
        make_cluster_grid(description.clusters, kSceneWidth, kSceneHeight, description.near_plane,
                          description.far_plane);
    if (!grid.has_value()) {
        return make_unexpected(grid.error());
    }
    const BindingCapacity capacity = BindingCapacity::for_grid(
        *grid, description.max_draws, description.max_instances, description.material_capacity, 8);
    if (Status made = bindings_.initialize(device, pipelines_, capacity); !made) {
        return made;
    }
    if (Status made = recorder_.initialize(pipelines_, bindings_); !made) {
        return made;
    }
    if (Status made = effect_.initialize(device, pipelines_, kRingCapacity); !made) {
        return made;
    }
    if (Status made = create_dummy_geometry(device); !made) {
        return made;
    }

    GeometrySource geometry;
    geometry.streams[kPositionStream] = streams_[0];
    geometry.streams[kNormalStream] = streams_[1];
    geometry.streams[kUvStream] = streams_[2];
    geometry.geometry = &no_geometry;
    geometry.user = this;
    recorder_.set_geometry(geometry);

    projection_ =
        perspective_reversed_z(0.9F, static_cast<f32>(kSceneWidth) / static_cast<f32>(kSceneHeight),
                               description.near_plane, description.far_plane);
    view_ = look_at(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.0F, 1.0F, 0.0F});

    Expected<rhi::BufferHandle, Error> readback =
        make_upload(device, "vfx readback", kReadbackBytes, rhi::BufferUsage::TransferDestination);
    if (!readback.has_value()) {
        return make_unexpected(readback.error());
    }
    readback_ = *readback;

    rhi::TextureDescription output;
    output.name = "vfx output";
    output.format = kOutputFormat;
    output.extent = rhi::Extent3D{kSceneWidth, kSceneHeight, 1};
    output.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSource;
    Expected<rhi::TextureHandle, Error> created = device.create_texture(output);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    output_ = *created;
    built_ = true;
    return ok();
}

Status VfxScene::create_dummy_geometry(rhi::Device& device) noexcept {
    // Four bytes each: the recorder requires three non-null stream buffers, and this scene has no
    // geometry to put in them. Declared rather than faked — `no_geometry` above is what actually
    // answers, and it answers false.
    static constexpr const char* kNames[3] = {"vfx unused positions", "vfx unused normals",
                                              "vfx unused uvs"};
    for (u32 which = 0; which < 3U; ++which) {
        Expected<rhi::BufferHandle, Error> made =
            make_upload(device, kNames[which], 16, rhi::BufferUsage::Vertex);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        streams_[which] = *made;
    }
    Expected<rhi::BufferHandle, Error> made =
        make_upload(device, "vfx unused indices", 16, rhi::BufferUsage::Index);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    indices_ = *made;
    return ok();
}

Status VfxScene::simulate(f32 dt) noexcept {
    if (Status stepped = world_.step(dt, steps_); !stepped) {
        return stepped;
    }
    return publish_sprites(world_, Vec3{0.0F, 0.0F, 0.0F}, kRingCapacity, records_, published_);
}

Status VfxScene::render(assembly::AssemblyReport& out) noexcept {
    if (!built_) {
        return fail(ErrorCode::Unavailable, "vfx scene: build() was not called");
    }
    rhi::Device& device = *device_;
    Expected<u32, Error> began = device.begin_frame();
    if (!began.has_value()) {
        return make_unexpected(began.error());
    }
    const u32 slot = *began;

    recorder_.clear_extensions();
    effect_.reset_report();
    if (Status uploaded = effect_.upload(slot, records_.span()); !uploaded) {
        return uploaded;
    }
    if (Status added = recorder_.add_extension(effect_.extension()); !added) {
        return added;
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

    TextureRequest request;
    request.name = "vfx output";
    request.format = kOutputFormat;
    request.width = kSceneWidth;
    request.height = kSceneHeight;
    request.extra_usage = rhi::TextureUsage::TransferSource;
    view.output = graph_.import_texture(request, output_, rhi::ImageLayout::Undefined);

    if (Status bound = recorder_.bind(assembly_); !bound) {
        return bound;
    }
    FrameSinks sinks = recorder_.sinks();
    if (Status assembled = assembly_.assemble(index_, view, sinks, graph_, out); !assembled) {
        return assembled;
    }

    GlobalsData globals;
    // The exposure the pipeline suite measured for a physically-lit frame. A particle's colour is a
    // RADIANCE — see src/rendering/particles/README.md — so the same number applies here, and zero
    // stops would photograph the effect as a white rectangle.
    globals.exposure_stops = -11.4F;
    const FrameUpload upload = upload_for(assembly_, out, projection_ * view_, view_,
                                          instances_.span(), globals, material_offsets_);
    if (Status uploaded = bindings_.upload(slot, upload); !uploaded) {
        return uploaded;
    }

    Readback readback;
    readback.output = assembly_.resources().output;
    readback.buffer = readback_;
    if (read_back_ && readback.output != kInvalidResource) {
        // A WRITE TO AN IMPORTED RESOURCE IS A CULLING ROOT. The pipeline suite records what
        // happens without one: the graph drops the capture pass and every picture comes back
        // uniformly zero, with the rest of the suite green.
        BufferRequest capture_request;
        capture_request.name = "vfx capture";
        capture_request.size = kReadbackBytes;
        capture_request.extra_usage = rhi::BufferUsage::TransferDestination;
        const ResourceId destination = graph_.import_buffer(capture_request, readback_);
        graph_.add_pass("capture", rhi::QueueKind::Graphics)
            .read(readback.output, rhi::Access::TransferRead)
            .write(destination, rhi::Access::TransferWrite)
            .record(&record_readback, &readback);
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

Status VfxScene::read_pixels() noexcept {
    if (Status sized = pixels_.resize(usize{kSceneWidth} * usize{kSceneHeight}); !sized) {
        return sized;
    }
    const auto* mapped = static_cast<const u32*>(device_->buffer_mapped_pointer(readback_));
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "vfx scene: the readback buffer is not mapped");
    }
    for (usize index = 0; index < pixels_.size(); ++index) {
        pixels_[index] = mapped[index];
    }
    return ok();
}

u32 VfxScene::differing_texels(Span<const u32> other) const noexcept {
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

void VfxScene::release() noexcept {
    if (device_ == nullptr) {
        return;
    }
    (void)device_->wait_idle();
    effect_.shutdown();
    bindings_.shutdown();
    pipelines_.shutdown();
    world_.shutdown();
    if (!output_.is_null()) {
        device_->destroy_texture(output_);
        output_ = rhi::TextureHandle{};
    }
    rhi::BufferHandle* buffers[] = {&streams_[0], &streams_[1], &streams_[2], &indices_,
                                    &readback_};
    for (rhi::BufferHandle* handle : buffers) {
        if (!handle->is_null()) {
            device_->destroy_buffer(*handle);
            *handle = rhi::BufferHandle{};
        }
    }
    device_ = nullptr;
    built_ = false;
}

}  // namespace cy::vfx_test
