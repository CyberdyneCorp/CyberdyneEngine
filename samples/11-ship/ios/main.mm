// SPDX-License-Identifier: MIT
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include <cy/backends/rhi-metal/backend.h>
#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/backends/rhi/device.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/quat.h>
#include <cy/core/memory/domain.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/platform/ios_display_server.h>
#include <cy/platform/ios_platform.h>
#include <cy/platform/ios_touch_input_source.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/skinning/skin_pass.h>
#include <cy/servers/input/server.h>
#include <cy/servers/render/geometry/skin_dispatch.h>
#include <cy/vfx/budget.h>
#include <cy/vfx/gpu/gpu_pass.h>
#include <cy/vfx/gpu_layout.h>

#include "mobile_vfx_effect.h"
#include "shaders/mobile_vfx_kernel.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

namespace {

constexpr float kMobileRenderScale = 0.42F;
constexpr int kTerrainOctaves = 3;
constexpr int kTerrainMarchSteps = 32;

constexpr char kWorldShader[] = R"msl(
#include <metal_stdlib>
using namespace metal;

struct Raster { float4 position [[position]]; float2 uv; };
struct Frame { float time; float aspect; float2 pad; };

vertex Raster world_vertex(uint id [[vertex_id]]) {
    const float2 p[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    Raster out;
    out.position = float4(p[id], 0.0, 1.0);
    out.uv = p[id] * 0.5 + 0.5;
    return out;
}

float hash21(float2 p) {
    p = fract(p * float2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

float smooth_noise(float2 p) {
    const float2 cell = floor(p);
    float2 local = fract(p);
    local = local * local * (3.0 - 2.0 * local);
    const float low = mix(hash21(cell), hash21(cell + float2(1,0)), local.x);
    const float high = mix(hash21(cell + float2(0,1)), hash21(cell + 1.0), local.x);
    return mix(low, high, local.y) * 2.0 - 1.0;
}

float terrain(float2 p) {
    float h = 0.0;
    float a = 0.55;
    for (int i = 0; i < 3; ++i) {
        h += a * smooth_noise(p);
        p = float2(p.x * 1.81 - p.y * 0.34, p.x * 0.34 + p.y * 1.81) + 7.13;
        a *= 0.48;
    }
    return h * 0.72;
}

float map_world(float3 p) { return p.y - terrain(p.xz); }

float3 normal_at(float3 p) {
    const float e = 0.02;
    return normalize(float3(map_world(p + float3(e,0,0)) - map_world(p - float3(e,0,0)),
                            2.0 * e,
                            map_world(p + float3(0,0,e)) - map_world(p - float3(0,0,e))));
}

fragment float4 world_fragment(Raster in [[stage_in]], constant Frame& frame [[buffer(0)]]) {
    const float day = fract(frame.time / 28.0);
    const float angle = day * 6.2831853 - 1.5707963;
    const float3 sun = normalize(float3(cos(angle), sin(angle), -0.35));
    const float daylight = smoothstep(-0.13, 0.16, sun.y);
    const float2 screen = float2((in.uv.x * 2.0 - 1.0) * frame.aspect,
                                 in.uv.y * 2.0 - 1.0);
    const float3 camera = float3(sin(frame.time * 0.035) * 3.0, 2.1,
                                 5.8 + cos(frame.time * 0.035) * 3.0);
    const float3 target = float3(0.0, 0.15, 0.0);
    const float3 forward = normalize(target - camera);
    const float3 right = normalize(cross(forward, float3(0,1,0)));
    const float3 up = cross(right, forward);
    const float3 ray = normalize(forward * 1.55 + right * screen.x + up * screen.y);

    const float3 day_sky = mix(float3(0.62,0.77,0.92), float3(0.08,0.32,0.68), in.uv.y);
    const float stars = step(0.997, hash21(floor(in.uv * 520.0)));
    float3 color = mix(float3(0.006,0.012,0.04) + stars * 0.65, day_sky, daylight);
    const float sun_disk = pow(max(dot(ray, sun), 0.0), 900.0);
    color += float3(1.0,0.67,0.28) * sun_disk * (0.4 + daylight);

    float distance = 0.0;
    float3 point = camera;
    bool hit = false;
    for (int step = 0; step < 32; ++step) {
        point = camera + ray * distance;
        const float d = map_world(point);
        if (d < 0.006) { hit = true; break; }
        distance += clamp(d * 0.50, 0.03, 0.40);
        if (distance > 28.0) break;
    }
    if (hit) {
        const float3 n = normal_at(point);
        const float diffuse = max(dot(n, sun), 0.0);
        const float snow = smoothstep(0.58, 1.08, point.y) * smoothstep(0.52, 0.9, n.y);
        const float rock = smoothstep(0.22, 0.72, 1.0 - n.y);
        float3 ground = mix(float3(0.06,0.20,0.055), float3(0.26,0.21,0.14), rock);
        ground = mix(ground, float3(0.78,0.84,0.88), snow);
        const float night_ambient = 0.035;
        color = ground * (night_ambient + daylight * (0.18 + diffuse * 1.05));
        color += float3(0.08,0.13,0.23) * max(n.y, 0.0) * (0.25 + daylight * 0.3);
        const float fog = 1.0 - exp(-distance * 0.055);
        color = mix(color, mix(float3(0.01,0.02,0.06), day_sky, daylight), fog);
    }
    color = color / (color + 1.0);
    color = pow(color, float3(1.0 / 2.2));
    return float4(color, 1.0);
}

struct CharacterPush {
    float scale;
    float aspect;
    float2 offset;
    float4 color;
};

struct CharacterInput { float3 position [[attribute(0)]]; };
struct CharacterRaster { float4 position [[position]]; float4 color; };

vertex CharacterRaster character_vertex(CharacterInput in [[stage_in]],
                                          constant CharacterPush& draw [[buffer(0)]]) {
    CharacterRaster out;
    out.position = float4(draw.offset.x + in.position.x * draw.scale / draw.aspect,
                          draw.offset.y + in.position.y * draw.scale, 0.1, 1.0);
    out.color = draw.color;
    return out;
}

fragment float4 character_fragment(CharacterRaster in [[stage_in]]) { return in.color; }

struct ParticleSet { device uint* particles; device uint* alive; };
struct ParticlePush {
    uint position_base_words;
    uint capacity;
    float aspect;
    float time;
};
struct ParticleRaster { float4 position [[position]]; float2 local; float alive; };

vertex ParticleRaster particle_vertex(uint vertex_id [[vertex_id]], uint instance_id [[instance_id]],
                                       constant ParticleSet* set [[buffer(0)]],
                                       constant ParticlePush& draw [[buffer(1)]]) {
    const float2 corners[6] = {float2(-1,-1), float2(1,-1), float2(1,1),
                               float2(-1,-1), float2(1,1), float2(-1,1)};
    ParticleRaster out;
    out.local = corners[vertex_id];
    out.alive = instance_id < draw.capacity && set->alive[instance_id] != 0u ? 1.0 : 0.0;
    if (out.alive == 0.0) {
        out.position = float4(2.0, 2.0, 0.0, 1.0);
        return out;
    }
    const uint base = draw.position_base_words + instance_id * 3u;
    const float3 p = float3(as_type<float>(set->particles[base]),
                            as_type<float>(set->particles[base + 1u]),
                            as_type<float>(set->particles[base + 2u]));
    const float pulse = 0.012 + 0.006 * sin(draw.time * 5.0 + float(instance_id));
    const float2 center = float2(-0.34 + p.x * 0.11 / draw.aspect,
                                  -0.66 + p.y * 0.11);
    out.position = float4(center + float2(corners[vertex_id].x / draw.aspect,
                                          corners[vertex_id].y) * pulse, 0.05, 1.0);
    return out;
}

fragment float4 particle_fragment(ParticleRaster in [[stage_in]]) {
    const float alpha = in.alive * smoothstep(1.0, 0.05, length(in.local));
    const float3 radiance = mix(float3(1.0, 0.16, 0.02), float3(1.0, 0.9, 0.32),
                                saturate(1.0 - length(in.local)));
    return float4(radiance * alpha, alpha);
}
)msl";

struct FrameConstants {
    float time;
    float aspect;
    float padding[2];
};

cy::Extent scaled_drawable(CGSize view_size, CGFloat native_scale) {
    const auto scaled = native_scale * static_cast<CGFloat>(kMobileRenderScale);
    return {static_cast<cy::i32>(std::lround(view_size.width * scaled)),
            static_cast<cy::i32>(std::lround(view_size.height * scaled))};
}

cy::Extent native_drawable(CGSize view_size, CGFloat native_scale) {
    return {static_cast<cy::i32>(std::lround(view_size.width * native_scale)),
            static_cast<cy::i32>(std::lround(view_size.height * native_scale))};
}

void report(const char* operation, const cy::Error& error) {
    std::fprintf(stderr, "CY_IOS_ERROR operation=%s code=%u message=%s\n", operation,
                 static_cast<unsigned>(error.code), error.message);
}

bool verify_ios_contract(cy::IosPlatform& platform, cy::IosDisplayServer& display,
                         cy::WindowId window, CAMetalLayer* layer, cy::input::InputServer& input,
                         cy::IosTouchInputSource& touch) {
    const auto child = platform.spawn_process(cy::ProcessOptions{});
    if (child || child.error().code != cy::ErrorCode::Unsupported) {
        std::fprintf(stderr, "CY_IOS_CONTRACT_FAIL subprocess\n");
        return false;
    }
    const auto second_window = display.create_window(cy::WindowDescription{});
    if (second_window || second_window.error().code != cy::ErrorCode::Unsupported) {
        std::fprintf(stderr, "CY_IOS_CONTRACT_FAIL second-window\n");
        return false;
    }
    const auto moved = display.set_window_position(window, {1, 1});
    const auto titled = display.set_window_title(window, "unsupported");
    const auto unsynchronised = display.set_window_vsync(window, cy::VSyncMode::Disabled);
    if (moved || titled || unsynchronised || moved.error().code != cy::ErrorCode::Unsupported ||
        titled.error().code != cy::ErrorCode::Unsupported ||
        unsynchronised.error().code != cy::ErrorCode::Unsupported) {
        std::fprintf(stderr, "CY_IOS_CONTRACT_FAIL desktop-window-operation\n");
        return false;
    }
    cy::SurfaceDescription surface_description;
    surface_description.api = cy::GraphicsApi::Metal;
    const auto surface = display.create_surface(window, surface_description);
    if (!surface || surface->handle != (__bridge void*)layer) {
        std::fprintf(stderr, "CY_IOS_CONTRACT_FAIL metal-surface\n");
        return false;
    }
    touch.began(0.25F, 0.75F, 0.5F, platform.monotonic_nanoseconds());
    if (input.pending().size() != 4) {
        std::fprintf(stderr, "CY_IOS_CONTRACT_FAIL touch-events=%zu\n", input.pending().size());
        return false;
    }
    input.resolve_tick(0, platform.monotonic_nanoseconds(), 1.0F / 60.0F);
    std::printf(
        "CY_IOS_CONTRACT platform=ios window=single desktop_ops=unsupported surface=metal "
        "touch_events=4\n");
    std::fflush(stdout);
    return true;
}

class MobileWorldRenderer {
public:
    ~MobileWorldRenderer() { shutdown(); }

    bool start(CAMetalLayer* layer, cy::Extent pixels) {
        allocator_ = &cy::system_allocator(cy::MemoryDomain::Gpu);
        (void)cy::rhi::metal::register_metal_backend();
        cy::rhi::DeviceDescription device_description;
        device_description.application_name = "Cyberdyne iOS Compute Scene";
        // This workload has four very small VFX dispatches followed immediately by graphics
        // consumers. Keeping them on the graphics queue avoids a cross-queue rendezvous on tile
        // GPUs while preserving the graph's compute-to-graphics dependencies.
        device_description.request_async_compute = false;
        cy::rhi::BackendSelection selection;
        auto created = cy::rhi::create_device(*allocator_, cy::rhi::metal::kMetalBackendName,
                                              device_description, selection);
        if (!created) {
            report("create-device", created.error());
            return false;
        }
        device_ = *created;

        cy::rhi::SwapchainDescription swapchain_description;
        swapchain_description.name = "iOS compute presentation";
        swapchain_description.native_surface = (__bridge void*)layer;
        swapchain_description.extent = {static_cast<cy::u32>(pixels.width),
                                        static_cast<cy::u32>(pixels.height)};
        auto swapchain = device_->create_swapchain(swapchain_description);
        if (!swapchain) {
            report("create-swapchain", swapchain.error());
            return false;
        }
        swapchain_ = *swapchain;
        auto acquired = device_->create_semaphore();
        auto rendered = device_->create_semaphore();
        if (!acquired || !rendered) {
            return false;
        }
        acquired_ = *acquired;
        rendered_ = *rendered;

        if (!create_graphics() || !create_character() || !create_vfx() || !reset_vfx()) {
            return false;
        }
        started_ = true;
        began_ = std::chrono::steady_clock::now();
        std::printf("CY_IOS_READY backend=%s device=%s size=%dx%d\n", selection.selected,
                    device_->capabilities().device_name(), pixels.width, pixels.height);
        std::fflush(stdout);
        return true;
    }

    bool resize(cy::Extent pixels) {
        if (!started_) {
            return false;
        }
        return device_
            ->resize_swapchain(swapchain_, {static_cast<cy::u32>(pixels.width),
                                            static_cast<cy::u32>(pixels.height)})
            .has_value();
    }

    bool draw() {
        if (!started_) {
            return false;
        }
        const float seconds = elapsed_seconds();
        auto frame = device_->begin_frame();
        if (!frame) {
            report("begin-frame", frame.error());
            return false;
        }
        auto& skin = skins_[*frame];
        if (!upload_animation(skin, seconds) || !step_vfx(seconds)) {
            (void)device_->end_frame();
            return false;
        }
        auto image = device_->acquire_next_image(swapchain_, acquired_, 1'000'000'000ULL);
        if (!image) {
            (void)device_->end_frame();
            return false;
        }

        cy::rendering::RenderGraph graph(*allocator_);
        if (const cy::Status status = skin.declare(graph); !status) {
            report("declare-skinning", status.error());
            return end_failed_frame();
        }
        if (const cy::Status status = vfx_.declare(graph); !status) {
            report("declare-vfx", status.error());
            return end_failed_frame();
        }

        const auto info = device_->swapchain_info(swapchain_);
        cy::rendering::TextureRequest target_request;
        target_request.name = "iOS compute presentation target";
        target_request.format = info.format;
        target_request.width = info.extent.width;
        target_request.height = info.extent.height;
        const cy::rendering::ResourceId target =
            graph.import_texture(target_request, device_->swapchain_texture(swapchain_, *image),
                                 cy::rhi::ImageUse::Undefined);

        DrawState state;
        state.renderer = this;
        state.skin = &skin;
        state.target = target;
        state.width = info.extent.width;
        state.height = info.extent.height;
        state.time = seconds;
        graph.add_pass("iOS terrain + skin + VFX", cy::rhi::QueueKind::Graphics)
            .read(skin.positions_resource(), cy::rhi::Access::VertexAttributeRead)
            .read(vfx_.particles_resource(), cy::rhi::Access::VertexStorageRead)
            .read(vfx_.alive_resource(), cy::rhi::Access::VertexStorageRead)
            .write(target, cy::rhi::Access::ColorAttachmentWrite)
            .record(&MobileWorldRenderer::record_scene, &state);
        graph.add_pass("iOS present", cy::rhi::QueueKind::Graphics)
            .read(target, cy::rhi::Access::Present)
            .side_effect();
        if (const cy::Status status = graph.status(); !status) {
            report("frame-graph", status.error());
            return end_failed_frame();
        }
        cy::rendering::GraphExecutor executor(*allocator_, *device_);
        cy::rendering::ExecuteOptions options;
        options.wait_acquire = acquired_;
        options.signal_present = rendered_;
        auto executed = executor.execute(graph, cy::rendering::CompileOptions{}, options);
        if (!executed) {
            report("execute-frame", executed.error());
            return end_failed_frame();
        }
        const bool presented = device_->present(swapchain_, *image, rendered_).has_value();
        (void)device_->end_frame();
        executor.release();
        if (presented) {
            ++frames_;
            if (!compute_reported_) {
                std::printf(
                    "CY_IOS_COMPUTE skin_vertices=%u skin_bones=%u vfx_capacity=%u "
                    "vfx_dispatches=%u gpu_particle_instances=%u "
                    "cpu_particle_readback=0\n",
                    character_vertices_, kCharacterBones, vfx_.block_capacity(),
                    vfx_.report().dispatches, vfx_.block_capacity());
                std::fflush(stdout);
                compute_reported_ = true;
            }
        }
        return presented;
    }

    cy::u64 frames() const { return frames_; }

private:
    static constexpr cy::u32 kCharacterBones = 5;
    static constexpr cy::u32 kVfxCapacity = 512;

    struct CharacterPush {
        float scale = 0.38F;
        float aspect = 1.0F;
        float offset[2] = {0.45F, -0.68F};
        float color[4] = {0.18F, 0.92F, 1.0F, 1.0F};
    };
    struct ParticlePush {
        cy::u32 position_base_words = 0;
        cy::u32 capacity = 0;
        float aspect = 1.0F;
        float time = 0.0F;
    };
    struct DrawState {
        MobileWorldRenderer* renderer = nullptr;
        cy::rendering::skinning::SkinPass* skin = nullptr;
        cy::rendering::ResourceId target = cy::rendering::kInvalidResource;
        cy::u32 width = 0;
        cy::u32 height = 0;
        float time = 0.0F;
    };

    static void append_rectangle(std::vector<cy::Vec3>& positions,
                                 std::vector<cy::render::geometry::GpuSkinInfluence>& influences,
                                 float left, float bottom, float right, float top, cy::u8 bone) {
        const cy::Vec3 a{left, bottom, 0.0F};
        const cy::Vec3 b{right, bottom, 0.0F};
        const cy::Vec3 c{right, top, 0.0F};
        const cy::Vec3 d{left, top, 0.0F};
        const cy::Vec3 vertices[] = {a, b, c, a, c, d};
        const cy::u8 indices[4] = {bone, 0, 0, 0};
        const cy::u8 weights[4] = {255, 0, 0, 0};
        const auto influence = cy::render::geometry::skin_influence(indices, weights);
        for (const cy::Vec3 vertex : vertices) {
            positions.push_back(vertex);
            influences.push_back(influence);
        }
    }

    static cy::Mat4 rotate_about(cy::Vec3 pivot, float radians) {
        return cy::Mat4::from_translation(pivot) *
               cy::Mat4::from_quat(cy::Quat::from_axis_angle(cy::kAxisZ, radians)) *
               cy::Mat4::from_translation(pivot * -1.0F);
    }

    bool create_graphics() {
        const auto make_shader = [this](cy::rhi::ShaderStage stage, const char* entry,
                                        cy::rhi::ShaderModuleHandle& output) {
            cy::rhi::ShaderModuleDescription shader;
            shader.name = entry;
            shader.native = {reinterpret_cast<const cy::u8*>(kWorldShader),
                             sizeof(kWorldShader) - 1};
            shader.native_format = cy::rhi::ShaderFormat::Msl;
            shader.stage = stage;
            shader.entry_point = entry;
            auto module = device_->create_shader_module(shader);
            if (!module) {
                report(entry, module.error());
                return false;
            }
            output = *module;
            return true;
        };
        if (!make_shader(cy::rhi::ShaderStage::Vertex, "world_vertex", world_vertex_) ||
            !make_shader(cy::rhi::ShaderStage::Fragment, "world_fragment", world_fragment_) ||
            !make_shader(cy::rhi::ShaderStage::Vertex, "character_vertex", character_vertex_) ||
            !make_shader(cy::rhi::ShaderStage::Fragment, "character_fragment",
                         character_fragment_) ||
            !make_shader(cy::rhi::ShaderStage::Vertex, "particle_vertex", particle_vertex_) ||
            !make_shader(cy::rhi::ShaderStage::Fragment, "particle_fragment", particle_fragment_)) {
            return false;
        }
        const auto info = device_->swapchain_info(swapchain_);
        cy::rhi::ColorAttachmentState opaque_color{.format = info.format};

        cy::rhi::PushConstantRange world_range{cy::rhi::ShaderStage::Fragment, 0,
                                               sizeof(FrameConstants)};
        cy::rhi::PipelineLayoutDescription world_layout;
        world_layout.name = "iOS world layout";
        world_layout.push_constants = {&world_range, 1};
        auto made_world_layout = device_->create_pipeline_layout(world_layout);
        if (!made_world_layout) {
            report("world-layout", made_world_layout.error());
            return false;
        }
        world_layout_ = *made_world_layout;
        cy::rhi::GraphicsPipelineDescription world;
        world.name = "iOS open world";
        world.layout = world_layout_;
        world.vertex_shader = world_vertex_;
        world.fragment_shader = world_fragment_;
        world.rasterisation.cull_mode = cy::rhi::CullMode::None;
        world.color_attachments = {&opaque_color, 1};
        auto made_world = device_->create_graphics_pipeline(world);
        if (!made_world) {
            report("world-pipeline", made_world.error());
            return false;
        }
        world_pipeline_ = *made_world;

        cy::rhi::PushConstantRange character_range{
            cy::rhi::ShaderStage::Vertex | cy::rhi::ShaderStage::Fragment, 0,
            sizeof(CharacterPush)};
        cy::rhi::PipelineLayoutDescription character_layout;
        character_layout.name = "iOS character layout";
        character_layout.push_constants = {&character_range, 1};
        auto made_character_layout = device_->create_pipeline_layout(character_layout);
        if (!made_character_layout) {
            report("character-layout", made_character_layout.error());
            return false;
        }
        character_layout_ = *made_character_layout;
        const cy::rhi::VertexBinding character_binding{0, sizeof(cy::Vec3),
                                                       cy::rhi::VertexInputRate::PerVertex};
        const cy::rhi::VertexAttribute character_attribute{0, 0, cy::rhi::Format::Rgb32Sfloat, 0};
        cy::rhi::GraphicsPipelineDescription character;
        character.name = "iOS GPU-skinned character";
        character.layout = character_layout_;
        character.vertex_shader = character_vertex_;
        character.fragment_shader = character_fragment_;
        character.vertex_bindings = {&character_binding, 1};
        character.vertex_attributes = {&character_attribute, 1};
        character.rasterisation.cull_mode = cy::rhi::CullMode::None;
        character.color_attachments = {&opaque_color, 1};
        auto made_character = device_->create_graphics_pipeline(character);
        if (!made_character) {
            report("character-pipeline", made_character.error());
            return false;
        }
        character_pipeline_ = *made_character;

        const cy::rhi::DescriptorBinding particle_bindings[2] = {
            {0, cy::rhi::DescriptorKind::StorageBuffer, 1, cy::rhi::ShaderStage::Vertex, false},
            {1, cy::rhi::DescriptorKind::StorageBuffer, 1, cy::rhi::ShaderStage::Vertex, false},
        };
        cy::rhi::DescriptorSetLayoutDescription particle_set;
        particle_set.name = "iOS VFX render inputs";
        particle_set.bindings = {particle_bindings, 2};
        auto made_particle_set = device_->create_descriptor_set_layout(particle_set);
        if (!made_particle_set) {
            report("particle-set-layout", made_particle_set.error());
            return false;
        }
        particle_set_layout_ = *made_particle_set;
        cy::rhi::PushConstantRange particle_range{
            cy::rhi::ShaderStage::Vertex | cy::rhi::ShaderStage::Fragment, 0, sizeof(ParticlePush)};
        cy::rhi::PipelineLayoutDescription particle_layout;
        particle_layout.name = "iOS VFX draw layout";
        particle_layout.set_layouts = {&particle_set_layout_, 1};
        particle_layout.push_constants = {&particle_range, 1};
        auto made_particle_layout = device_->create_pipeline_layout(particle_layout);
        if (!made_particle_layout) {
            report("particle-layout", made_particle_layout.error());
            return false;
        }
        particle_layout_ = *made_particle_layout;
        cy::rhi::ColorAttachmentState particle_color{.format = info.format};
        particle_color.blend_enable = true;
        particle_color.source_color = cy::rhi::BlendFactor::One;
        particle_color.destination_color = cy::rhi::BlendFactor::OneMinusSourceAlpha;
        particle_color.source_alpha = cy::rhi::BlendFactor::One;
        particle_color.destination_alpha = cy::rhi::BlendFactor::OneMinusSourceAlpha;
        cy::rhi::GraphicsPipelineDescription particle;
        particle.name = "iOS GPU VFX";
        particle.layout = particle_layout_;
        particle.vertex_shader = particle_vertex_;
        particle.fragment_shader = particle_fragment_;
        particle.rasterisation.cull_mode = cy::rhi::CullMode::None;
        particle.color_attachments = {&particle_color, 1};
        auto made_particle = device_->create_graphics_pipeline(particle);
        if (!made_particle) {
            report("particle-pipeline", made_particle.error());
            return false;
        }
        particle_pipeline_ = *made_particle;
        return true;
    }

    bool create_character() {
        std::vector<cy::Vec3> positions;
        std::vector<cy::render::geometry::GpuSkinInfluence> influences;
        positions.reserve(42);
        influences.reserve(42);
        append_rectangle(positions, influences, -0.34F, 0.82F, 0.34F, 1.52F, 0);
        append_rectangle(positions, influences, -0.23F, 1.52F, 0.23F, 1.96F, 0);
        append_rectangle(positions, influences, -0.25F, 0.02F, -0.03F, 0.90F, 1);
        append_rectangle(positions, influences, 0.03F, 0.02F, 0.25F, 0.90F, 2);
        append_rectangle(positions, influences, -0.72F, 0.92F, -0.30F, 1.40F, 3);
        append_rectangle(positions, influences, 0.30F, 0.92F, 0.72F, 1.40F, 4);
        character_vertices_ = static_cast<cy::u32>(positions.size());
        cy::rendering::skinning::SkinPassDescription description;
        description.max_vertices = character_vertices_;
        description.max_bones = kCharacterBones;
        description.with_frames = false;
        for (auto& skin : skins_) {
            if (const cy::Status status = skin.create(*allocator_, *device_, description); !status) {
                report("create-skinning", status.error());
                return false;
            }
            if (const cy::Status status =
                    skin.upload_mesh({positions.data(), positions.size()}, {},
                                     {influences.data(), influences.size()});
                !status) {
                report("upload-character", status.error());
                return false;
            }
        }
        skin_description_.vertex_count = character_vertices_;
        skin_description_.bone_count = kCharacterBones;
        skin_description_.retained_bones = kCharacterBones;
        skin_description_.source = cy::render::geometry::PoseSource::UploadedPerSkin;
        return true;
    }

    bool create_vfx() {
        cy::graph::DiagnosticSink sink(*allocator_);
        cy::vfx::CompileReport compile_report(*allocator_);
        auto cooked = cy::ship_ios::cook_plume(*allocator_, sink, compile_report,
                                               cy::vfx::CompileOptions{}, 1, kVfxCapacity);
        if (!cooked) {
            report("cook-mobile-vfx", cooked.error());
            return false;
        }
        vfx_system_.emplace(std::move(*cooked));
        cy::vfx::gpu::GpuPassDescription description;
        description.async_compute = false;
        description.read_back = false;
        description.kernel.msl = {reinterpret_cast<const cy::u8*>(cy::vfx::gpu::kMobileVfxMsl),
                                  sizeof(cy::vfx::gpu::kMobileVfxMsl) - 1};
        description.kernel.msl_entry_point = cy::vfx::kVfxKernelEntryPoint;
        if (const cy::Status status = vfx_.create(*allocator_, *device_, *vfx_system_, description);
            !status) {
            report("create-vfx", status.error());
            return false;
        }
        const auto generated = vfx_.generated_source();
        if (generated.size() != cy::ship_ios::kMobileVfxSlangBytes ||
            cy::ship_ios::source_hash(generated) != cy::ship_ios::kMobileVfxSlangHash) {
            std::fprintf(stderr,
                         "CY_IOS_ERROR operation=stale-mobile-vfx-kernel generated_bytes=%zu\n",
                         generated.size());
            return false;
        }
        const auto& emitter = vfx_system_->emitters()[0];
        const auto* position = emitter.layout().find(cy::Name::intern("position"));
        if (position == nullptr || position->components != 3 ||
            position->precision != cy::vfx::Precision::Float32) {
            std::fprintf(stderr, "CY_IOS_ERROR operation=vfx-position-layout\n");
            return false;
        }
        particle_position_base_words_ =
            cy::vfx::gpu_array_base_words(emitter.layout(), *position, kVfxCapacity);
        auto set = device_->allocate_descriptor_set(particle_set_layout_, false);
        if (!set) {
            report("allocate-particle-set", set.error());
            return false;
        }
        particle_set_ = *set;
        cy::rhi::DescriptorWrite writes[2] = {};
        writes[0].binding = 0;
        writes[0].kind = cy::rhi::DescriptorKind::StorageBuffer;
        writes[0].buffer = vfx_.particles_buffer();
        writes[1].binding = 1;
        writes[1].kind = cy::rhi::DescriptorKind::StorageBuffer;
        writes[1].buffer = vfx_.alive_buffer();
        if (const cy::Status status = device_->update_descriptor_set(particle_set_, {writes, 2});
            !status) {
            report("write-particle-set", status.error());
            return false;
        }
        return true;
    }

    bool reset_vfx() {
        if (!device_->begin_frame()) {
            return false;
        }
        cy::rendering::RenderGraph graph(*allocator_);
        if (const cy::Status status = vfx_.declare_reset(graph); !status) {
            report("reset-vfx", status.error());
            (void)device_->end_frame();
            return false;
        }
        cy::rendering::GraphExecutor executor(*allocator_, *device_);
        auto executed = executor.execute(graph, cy::rendering::CompileOptions{}, {});
        if (!executed) {
            report("execute-vfx-reset", executed.error());
            (void)device_->end_frame();
            return false;
        }
        (void)device_->wait_idle();
        (void)device_->end_frame();
        executor.release();
        return true;
    }

    bool upload_animation(cy::rendering::skinning::SkinPass& skin, float seconds) {
        const float walk = std::sin(seconds * 3.4F) * 0.55F;
        std::array<cy::Mat4, kCharacterBones> pose = {
            cy::Mat4::identity(),
            rotate_about({-0.14F, 0.90F, 0.0F}, walk),
            rotate_about({0.14F, 0.90F, 0.0F}, -walk),
            rotate_about({-0.34F, 1.42F, 0.0F}, -walk * 0.75F),
            rotate_about({0.34F, 1.42F, 0.0F}, walk * 0.75F),
        };
        const cy::Status status =
            skin.upload(skin_description_, {pose.data(), pose.size()}, frames_);
        if (!status) {
            report("upload-walk-pose", status.error());
        }
        return status.has_value();
    }

    bool step_vfx(float seconds) {
        float parameters[8] = {};
        for (cy::usize index = 0; index < vfx_system_->parameters().size(); ++index) {
            for (cy::u32 component = 0; component < 4; ++component) {
                parameters[index * 4 + component] =
                    vfx_system_->parameters()[index].value[component];
            }
        }
        cy::vfx::BudgetController controller;
        cy::vfx::gpu::GpuStepInputs inputs;
        inputs.dt = 1.0F / 60.0F;
        inputs.emitter_age = seconds;
        inputs.levers =
            controller.levers_for(vfx_system_->importance(), vfx_system_->scalability());
        inputs.levers.sorted = false;
        inputs.reserved_particles = vfx_system_->scalability().reserved_particles;
        if (!vfx_parameters_uploaded_) {
            inputs.parameters = {parameters, vfx_system_->parameters().size() * 4};
        }
        const cy::Status status = vfx_.step(inputs);
        if (!status) {
            report("step-vfx", status.error());
        }
        vfx_parameters_uploaded_ = status.has_value();
        return status.has_value();
    }

    static void record_scene(const cy::rendering::PassContext& context, void* user) noexcept {
        auto* state = static_cast<DrawState*>(user);
        MobileWorldRenderer& self = *state->renderer;
        cy::rhi::RenderAttachment attachment;
        attachment.view = context.executor->view(state->target);
        attachment.load = cy::rhi::LoadOp::Clear;
        attachment.store = cy::rhi::StoreOp::Store;
        cy::rhi::RenderingInfo rendering;
        rendering.render_area = {0, 0, state->width, state->height};
        rendering.color_attachments = {&attachment, 1};
        context.commands->begin_rendering(rendering);
        context.commands->set_viewport(
            {0, 0, static_cast<float>(state->width), static_cast<float>(state->height), 0, 1});
        context.commands->set_scissor({0, 0, state->width, state->height});
        const float aspect = static_cast<float>(state->width) / static_cast<float>(state->height);

        context.commands->bind_graphics_pipeline(self.world_pipeline_);
        const FrameConstants frame{state->time, aspect, {0, 0}};
        context.commands->push_constants(self.world_layout_, cy::rhi::ShaderStage::Fragment, 0,
                                         {reinterpret_cast<const cy::u8*>(&frame), sizeof(frame)});
        context.commands->draw(3, 1, 0, 0);

        context.commands->bind_graphics_pipeline(self.character_pipeline_);
        const cy::u64 vertex_offset = 0;
        const auto character_buffer = state->skin->output_positions();
        context.commands->bind_vertex_buffers(0, {&character_buffer, 1}, {&vertex_offset, 1});
        CharacterPush character;
        character.aspect = aspect;
        context.commands->push_constants(
            self.character_layout_, cy::rhi::ShaderStage::Vertex | cy::rhi::ShaderStage::Fragment,
            0, {reinterpret_cast<const cy::u8*>(&character), sizeof(character)});
        context.commands->draw(self.character_vertices_, 1, state->skin->vertex_offset(), 0);

        context.commands->bind_graphics_pipeline(self.particle_pipeline_);
        context.commands->bind_descriptor_sets(self.particle_layout_, 0, {&self.particle_set_, 1});
        ParticlePush particles;
        particles.position_base_words = self.particle_position_base_words_;
        particles.capacity = self.vfx_.block_capacity();
        particles.aspect = aspect;
        particles.time = state->time;
        context.commands->push_constants(
            self.particle_layout_, cy::rhi::ShaderStage::Vertex | cy::rhi::ShaderStage::Fragment, 0,
            {reinterpret_cast<const cy::u8*>(&particles), sizeof(particles)});
        context.commands->draw(6, self.vfx_.block_capacity(), 0, 0);
        context.commands->end_rendering();
    }

    float elapsed_seconds() const {
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - began_).count();
    }

    bool end_failed_frame() {
        (void)device_->wait_idle();
        (void)device_->end_frame();
        return false;
    }

    void shutdown() {
        if (device_ == nullptr) {
            return;
        }
        (void)device_->wait_idle();
        vfx_.destroy();
        for (auto& skin : skins_) {
            skin.destroy();
        }
        if (!particle_pipeline_.is_null()) {
            device_->destroy_graphics_pipeline(particle_pipeline_);
        }
        if (!character_pipeline_.is_null()) {
            device_->destroy_graphics_pipeline(character_pipeline_);
        }
        if (!world_pipeline_.is_null()) {
            device_->destroy_graphics_pipeline(world_pipeline_);
        }
        if (!particle_layout_.is_null()) {
            device_->destroy_pipeline_layout(particle_layout_);
        }
        if (!character_layout_.is_null()) {
            device_->destroy_pipeline_layout(character_layout_);
        }
        if (!world_layout_.is_null()) {
            device_->destroy_pipeline_layout(world_layout_);
        }
        if (!particle_set_layout_.is_null()) {
            device_->destroy_descriptor_set_layout(particle_set_layout_);
        }
        const cy::rhi::ShaderModuleHandle shaders[] = {particle_fragment_,  particle_vertex_,
                                                       character_fragment_, character_vertex_,
                                                       world_fragment_,     world_vertex_};
        for (const auto shader : shaders) {
            if (!shader.is_null()) {
                device_->destroy_shader_module(shader);
            }
        }
        if (!rendered_.is_null()) {
            device_->destroy_semaphore(rendered_);
        }
        if (!acquired_.is_null()) {
            device_->destroy_semaphore(acquired_);
        }
        if (!swapchain_.is_null()) {
            device_->destroy_swapchain(swapchain_);
        }
        vfx_system_.reset();
        cy::rhi::destroy_device(*allocator_, device_);
        device_ = nullptr;
    }

    cy::Allocator* allocator_ = nullptr;
    cy::rhi::Device* device_ = nullptr;
    cy::rhi::SwapchainHandle swapchain_{};
    cy::rhi::SemaphoreHandle acquired_{};
    cy::rhi::SemaphoreHandle rendered_{};
    cy::rhi::ShaderModuleHandle world_vertex_{};
    cy::rhi::ShaderModuleHandle world_fragment_{};
    cy::rhi::ShaderModuleHandle character_vertex_{};
    cy::rhi::ShaderModuleHandle character_fragment_{};
    cy::rhi::ShaderModuleHandle particle_vertex_{};
    cy::rhi::ShaderModuleHandle particle_fragment_{};
    cy::rhi::DescriptorSetLayoutHandle particle_set_layout_{};
    cy::rhi::DescriptorSetHandle particle_set_{};
    cy::rhi::PipelineLayoutHandle world_layout_{};
    cy::rhi::PipelineLayoutHandle character_layout_{};
    cy::rhi::PipelineLayoutHandle particle_layout_{};
    cy::rhi::GraphicsPipelineHandle world_pipeline_{};
    cy::rhi::GraphicsPipelineHandle character_pipeline_{};
    cy::rhi::GraphicsPipelineHandle particle_pipeline_{};
    std::array<cy::rendering::skinning::SkinPass, cy::rhi::kDefaultFramesInFlight> skins_;
    cy::render::geometry::SkinningDescriptor skin_description_{};
    cy::vfx::gpu::VfxGpuPass vfx_;
    std::optional<cy::vfx::CompiledSystem> vfx_system_;
    std::chrono::steady_clock::time_point began_{};
    cy::u64 frames_ = 0;
    cy::u32 character_vertices_ = 0;
    cy::u32 particle_position_base_words_ = 0;
    bool compute_reported_ = false;
    bool vfx_parameters_uploaded_ = false;
    bool started_ = false;
};

}  // namespace

@protocol CyberdyneTouchSink
- (void)touchBeganAt:(CGPoint)point pressure:(CGFloat)pressure;
- (void)touchMovedTo:(CGPoint)point pressure:(CGFloat)pressure;
- (void)touchEndedAt:(CGPoint)point;
@end

@interface CyberdyneMetalView : UIView
@property(nonatomic, weak) id<CyberdyneTouchSink> touchSink;
@end
@implementation CyberdyneMetalView
+ (Class)layerClass {
    return CAMetalLayer.class;
}
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* touch = touches.anyObject;
    const CGFloat pressure =
        touch.maximumPossibleForce > 0 ? touch.force / touch.maximumPossibleForce : 1.0;
    [self.touchSink touchBeganAt:[touch locationInView:self] pressure:pressure];
    (void)event;
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* touch = touches.anyObject;
    const CGFloat pressure =
        touch.maximumPossibleForce > 0 ? touch.force / touch.maximumPossibleForce : 1.0;
    [self.touchSink touchMovedTo:[touch locationInView:self] pressure:pressure];
    (void)event;
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self.touchSink touchEndedAt:[touches.anyObject locationInView:self]];
    (void)event;
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self touchesEnded:touches withEvent:event];
}
@end

@interface CyberdyneViewController : UIViewController <CyberdyneTouchSink>
@end

@implementation CyberdyneViewController {
    cy::IosPlatform _platform;
    cy::IosDisplayServer _display;
    std::optional<cy::input::InputServer> _input;
    cy::IosTouchInputSource _touch;
    MobileWorldRenderer _renderer;
    CADisplayLink* _displayLink;
    UILabel* _stats;
    CFTimeInterval _sampleStart;
    cy::u64 _sampleFrames;
}

- (void)loadView {
    CyberdyneMetalView* view =
        [[CyberdyneMetalView alloc] initWithFrame:UIScreen.mainScreen.bounds];
    view.touchSink = self;
    self.view = view;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    CAMetalLayer* layer = (CAMetalLayer*)self.view.layer;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
    layer.framebufferOnly = NO;
    const CGFloat scale = UIScreen.mainScreen.nativeScale;
    self.view.contentScaleFactor = scale;
    const CGSize size = self.view.bounds.size;
    const cy::Extent native_pixels = native_drawable(size, scale);
    const cy::Extent pixels = scaled_drawable(size, scale);
    layer.drawableSize = CGSizeMake(pixels.width, pixels.height);
    (void)_display.initialise((__bridge void*)self.view, (__bridge void*)layer, pixels,
                              static_cast<cy::f32>(scale),
                              UIScreen.mainScreen.maximumFramesPerSecond);
    cy::WindowDescription window_description;
    window_description.mode = cy::WindowMode::Fullscreen;
    window_description.flags = cy::WindowFlags::HighDpi;
    const auto window = _display.create_window(window_description);
    _input.emplace(cy::system_allocator(cy::MemoryDomain::Engine));
    if (_input->initialize()) {
        _input->set_backend("ios-uikit", false);
        (void)_touch.attach(*_input, _platform.monotonic_nanoseconds());
    }
    if (!window || !_input ||
        !verify_ios_contract(_platform, _display, *window, layer, *_input, _touch)) {
        std::fprintf(stderr, "CY_IOS_ERROR operation=platform-contract\n");
        std::abort();
    }
    if (!_renderer.start(layer, pixels)) {
        std::abort();
    }
    std::printf(
        "CY_IOS_QUALITY native=%dx%d drawable=%dx%d scale=%.3f terrain_octaves=%d march_steps=%d\n",
        native_pixels.width, native_pixels.height, pixels.width, pixels.height,
        static_cast<double>(kMobileRenderScale), kTerrainOctaves, kTerrainMarchSteps);
    std::fflush(stdout);

    _stats = [[UILabel alloc] initWithFrame:CGRectMake(18, 18, 360, 72)];
    _stats.textColor = UIColor.whiteColor;
    _stats.backgroundColor = [UIColor colorWithWhite:0 alpha:0.45];
    _stats.font = [UIFont monospacedSystemFontOfSize:14 weight:UIFontWeightSemibold];
    _stats.numberOfLines = 3;
    _stats.text =
        @"Cyberdyne iOS • Metal\nTerrain • GPU skinning • GPU VFX\n42% render scale • measuring…";
    [self.view addSubview:_stats];

    _displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(drawFrame:)];
    _displayLink.preferredFrameRateRange = CAFrameRateRangeMake(60.0, 60.0, 60.0);
    [_displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    _sampleStart = CACurrentMediaTime();
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CAMetalLayer* layer = (CAMetalLayer*)self.view.layer;
    const CGFloat scale = self.view.contentScaleFactor;
    const cy::Extent pixels = scaled_drawable(self.view.bounds.size, scale);
    const CGSize wanted = CGSizeMake(pixels.width, pixels.height);
    if (!CGSizeEqualToSize(layer.drawableSize, wanted)) {
        layer.drawableSize = wanted;
        _display.update_metrics(pixels, static_cast<cy::f32>(scale),
                                UIScreen.mainScreen.maximumFramesPerSecond);
        _renderer.resize(pixels);
    }
}

- (void)drawFrame:(CADisplayLink*)link {
    if (_renderer.draw()) {
        ++_sampleFrames;
    }
    if (_input) {
        _input->resolve_tick(_renderer.frames(), _platform.monotonic_nanoseconds(),
                             static_cast<cy::f32>(link.duration));
    }
    const CFTimeInterval elapsed = CACurrentMediaTime() - _sampleStart;
    if (elapsed >= 1.0) {
        const double fps = static_cast<double>(_sampleFrames) / elapsed;
        _stats.text = [NSString
            stringWithFormat:
                @"Cyberdyne iOS • Metal\nTerrain • skinning • VFX\n%.1f FPS • 42%% render scale",
                fps];
        std::printf("CY_IOS_FPS fps=%.2f frames=%llu seconds=%.3f\n", fps,
                    static_cast<unsigned long long>(_sampleFrames), elapsed);
        std::fflush(stdout);
        _sampleFrames = 0;
        _sampleStart = CACurrentMediaTime();
    }
    (void)link;
}

- (CGPoint)normaliseTouch:(CGPoint)point {
    const CGSize size = self.view.bounds.size;
    return CGPointMake(size.width > 0 ? point.x / size.width : 0,
                       size.height > 0 ? point.y / size.height : 0);
}
- (void)touchBeganAt:(CGPoint)point pressure:(CGFloat)pressure {
    const CGPoint p = [self normaliseTouch:point];
    _touch.began(static_cast<cy::f32>(p.x), static_cast<cy::f32>(p.y),
                 static_cast<cy::f32>(pressure), _platform.monotonic_nanoseconds());
}
- (void)touchMovedTo:(CGPoint)point pressure:(CGFloat)pressure {
    const CGPoint p = [self normaliseTouch:point];
    _touch.moved(static_cast<cy::f32>(p.x), static_cast<cy::f32>(p.y),
                 static_cast<cy::f32>(pressure), _platform.monotonic_nanoseconds());
}
- (void)touchEndedAt:(CGPoint)point {
    const CGPoint p = [self normaliseTouch:point];
    _touch.ended(static_cast<cy::f32>(p.x), static_cast<cy::f32>(p.y),
                 _platform.monotonic_nanoseconds());
}

- (void)dealloc {
    _touch.detach(_platform.monotonic_nanoseconds());
    if (_input) {
        _input->shutdown();
    }
}

- (BOOL)prefersStatusBarHidden {
    return YES;
}
- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return UIInterfaceOrientationMaskLandscape;
}
@end

@interface CyberdyneSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation CyberdyneSceneDelegate
- (void)scene:(UIScene*)scene
    willConnectToSession:(UISceneSession*)session
                 options:(UISceneConnectionOptions*)connectionOptions {
    UIWindowScene* windowScene = (UIWindowScene*)scene;
    self.window = [[UIWindow alloc] initWithWindowScene:windowScene];
    self.window.rootViewController = [CyberdyneViewController new];
    [self.window makeKeyAndVisible];
    (void)session;
    (void)connectionOptions;
}
@end

@interface CyberdyneAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation CyberdyneAppDelegate
- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)options {
    (void)application;
    (void)options;
    return YES;
}
- (UISceneConfiguration*)application:(UIApplication*)application
    configurationForConnectingSceneSession:(UISceneSession*)session
                                   options:(UISceneConnectionOptions*)connectionOptions {
    (void)application;
    (void)session;
    (void)connectionOptions;
    return [[UISceneConfiguration alloc] initWithName:@"Default Configuration"
                                          sessionRole:UIWindowSceneSessionRoleApplication];
}
@end

int main(int argc, char** argv) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(CyberdyneAppDelegate.class));
    }
}
