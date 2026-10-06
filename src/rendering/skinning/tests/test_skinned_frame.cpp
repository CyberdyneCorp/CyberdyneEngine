// SPDX-License-Identifier: MIT
// Skinned meshes in the engine's forward frame, on a Vulkan device with validation and
// synchronisation validation on. `render.skinned_frame`, issue #76 stage 3.
//
// ================================================================================================
// THE SCENE
// ================================================================================================
//
// `pipeline_test::FrameScene` — the one scene the pipeline suites render — with one or more of its
// boxes replaced by a SKINNED LIMB: an octagonal tube with an elbow at its middle, two bones, the
// lower half bound to the first and the upper half to the second. The pose comes from a real
// `animation::PoseWorld`, the device pose buffer is filled from the world's dirty range by
// `skinning::SkinnedScene`, one compute pass skins every limb, and the frame draws the output
// through `FramePipelines`' skinned variants. Nothing here draws a skinned vertex with a pipeline
// of its own.
//
//   (a) with the skinned pipelines created and no skinned instance, the frame is BYTE-IDENTICAL to
//       `references/frame_scene_before_bloom.png`, which a build from before this change wrote;
//   (b) a bent limb, drawn by the frame, matches `references/skinned_frame.png` under the golden
//       rule, its vertices are `cpu_reference_skin`'s word for word, and the depth prepass, the
//       opaque pass and the shadow pass each drew it from the skinning output;
//   (c) with a directional shadow map: matches `references/skinned_frame_shadow.png`, and the map
//       itself changes when the limb bends — the shadow pass reads skinned positions;
//   (d) motion vectors: a still limb's are zero everywhere, and a moving forearm's are not, while
//       the upper arm's stay zero;
//   (e) the selection mask covers the limb where the frame drew it, which moves when it bends;
//   (f) three limbs are skinned by ONE pass of three dispatches;
//   (g) a frame in which one instance changed uploads only that instance's matrices.

#include "frame_scene.h"
#include "golden.h"
#include "skin_fixture.h"

#include <cy/animation/pose_world.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/selection/outline_pass.h>
#include <cy/rendering/skinning/frame_skinning.h>
#include <cy/rendering/skinning/skinned_scene.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace cy;
using cy::pipeline_test::FrameScene;
using cy::pipeline_test::FrameSceneHooks;
using cy::pipeline_test::kHeight;
using cy::pipeline_test::kWidth;
using cy::pipeline_test::RecordMode;
using cy::render::PackedNormalTangent;
using cy::render::geometry::GpuBoneMatrix;
using cy::render::geometry::GpuSkinInfluence;
using cy::rendering::skinning::SkinnedInstance;
using cy::rendering::skinning::SkinnedScene;

namespace {

constexpr u32 kPixels = kWidth * kHeight;
constexpr u32 kSides = 8;
constexpr u32 kRings = 4;
/// The rings' heights in the unit cube and the weight each gives the forearm's bone. The second
/// ring sits just below the elbow and belongs to the upper arm alone, so everything below it is
/// rigid.
constexpr f32 kRingHeights[kRings] = {-0.5F, -0.1F, 0.0F, 0.5F};
constexpr u8 kRingForearm[kRings] = {0, 0, 127, 255};
/// Side vertices, then a cap of `kSides` at each end: 48, the scene's whole UV stream.
constexpr u32 kLimbVertices = (kSides * kRings) + (kSides * 2U);
constexpr u32 kLimbIndices = (kSides * (kRings - 1U) * 6U) + ((kSides - 2U) * 3U * 2U);
constexpr u32 kBones = 2;
constexpr f32 kRadius = 0.14F;
constexpr u32 kMaxLimbs = 3;
/// The scene's set 0 texture slot the shadow map is bound at, and the map's size.
constexpr u32 kShadowSlot = 121;
constexpr u32 kShadowExtent = 512;
constexpr f32 kShadowRadius = 9.0F;
const Vec3 kShadowCentre{0.0F, -1.8F, -7.0F};
/// The limbs stand on the floor in front of the ring. Box 1 is the limb of every single-limb case.
constexpr f32 kLimbHalf = 0.9F;
const Vec3 kLimbCentres[kMaxLimbs] = {Vec3{-1.7F, -0.6F, -3.6F}, Vec3{1.9F, -0.6F, -3.6F},
                                      Vec3{0.1F, -0.2F, -5.0F}};
/// The ring's two boxes nearest the camera, moved out of the view so that no limb stands behind
/// one. Every other box keeps its place.
constexpr u32 kMovedBoxes[] = {8, 9};

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

// --- The limb ------------------------------------------------------------------------------------

/// An octagonal tube in the unit cube the scene's boxes are drawn from: y from -0.5 to 0.5, the
/// elbow at y = 0. The two lower rings are bone 0's, the top ring bone 1's, the elbow ring shared.
struct Limb {
    std::vector<Vec3> positions;
    std::vector<PackedNormalTangent> frames;
    std::vector<GpuSkinInfluence> influences;
    std::vector<u16> indices;

    Limb() {
        const f32 tau = 6.2831853F;
        const u8 bones[4] = {0, 1, 0, 0};
        for (u32 ring = 0; ring < kRings; ++ring) {
            const f32 y = kRingHeights[ring];
            const u8 upper = kRingForearm[ring];
            const u8 weights[4] = {static_cast<u8>(255U - upper), upper, 0, 0};
            for (u32 side = 0; side < kSides; ++side) {
                const f32 angle = tau * static_cast<f32>(side) / static_cast<f32>(kSides);
                const Vec3 normal{std::cos(angle), 0.0F, std::sin(angle)};
                positions.push_back(Vec3{normal.x * kRadius, y, normal.z * kRadius});
                frames.push_back(render::pack_normal_tangent(normal, Vec3{0, 1, 0}, 1.0F));
                influences.push_back(render::geometry::skin_influence(bones, weights));
            }
        }
        for (u32 cap = 0; cap < 2U; ++cap) {
            const f32 y = cap == 0 ? -0.5F : 0.5F;
            const Vec3 normal{0.0F, cap == 0 ? -1.0F : 1.0F, 0.0F};
            const u8 weights[4] = {static_cast<u8>(cap == 0 ? 255 : 0),
                                   static_cast<u8>(cap == 0 ? 0 : 255), 0, 0};
            for (u32 side = 0; side < kSides; ++side) {
                const f32 angle = tau * static_cast<f32>(side) / static_cast<f32>(kSides);
                positions.push_back(Vec3{std::cos(angle) * kRadius, y, std::sin(angle) * kRadius});
                frames.push_back(render::pack_normal_tangent(normal, Vec3{1, 0, 0}, 1.0F));
                influences.push_back(render::geometry::skin_influence(bones, weights));
            }
        }
        // Counter-clockwise seen from outside, which is the frame's front face.
        for (u32 ring = 0; ring + 1U < kRings; ++ring) {
            for (u32 side = 0; side < kSides; ++side) {
                const auto a = static_cast<u16>((ring * kSides) + side);
                const auto b = static_cast<u16>((ring * kSides) + ((side + 1U) % kSides));
                const auto c = static_cast<u16>(a + kSides);
                const auto d = static_cast<u16>(b + kSides);
                for (const u16 index : {a, c, b, b, c, d}) {
                    indices.push_back(index);
                }
            }
        }
        for (u32 cap = 0; cap < 2U; ++cap) {
            const auto first = static_cast<u16>((kSides * kRings) + (cap * kSides));
            for (u32 fan = 1; fan + 1U < kSides; ++fan) {
                const auto b = static_cast<u16>(first + fan);
                const auto c = static_cast<u16>(first + fan + 1U);
                if (cap == 0) {
                    for (const u16 index : {first, b, c}) {
                        indices.push_back(index);
                    }
                } else {
                    for (const u16 index : {first, c, b}) {
                        indices.push_back(index);
                    }
                }
            }
        }
    }
};

/// The skinning matrices of an elbow bent by `bend` radians toward +x: bone 0 at rest, bone 1
/// turned about the z axis through the elbow at the origin.
[[nodiscard]] std::vector<Mat4> limb_pose(f32 bend) noexcept {
    const f32 c = std::cos(-bend);
    const f32 s = std::sin(-bend);
    const Mat4 forearm =
        Mat4::from_columns(Vec4{c, s, 0.0F, 0.0F}, Vec4{-s, c, 0.0F, 0.0F},
                           Vec4{0.0F, 0.0F, 1.0F, 0.0F}, Vec4{0.0F, 0.0F, 0.0F, 1.0F});
    return {Mat4::identity(), forearm};
}

[[nodiscard]] f32 half_to_float(u16 half) noexcept {
    const u32 sign = (half >> 15U) & 1U;
    const u32 exponent = (half >> 10U) & 0x1FU;
    const u32 mantissa = half & 0x3FFU;
    f32 magnitude = 0.0F;
    if (exponent == 0) {
        magnitude = std::ldexp(static_cast<f32>(mantissa), -24);
    } else if (exponent == 31U) {
        magnitude = mantissa == 0 ? 65504.0F * 2.0F : 0.0F;
    } else {
        magnitude =
            std::ldexp(static_cast<f32>(mantissa | 0x400U), static_cast<i32>(exponent) - 25);
    }
    return sign != 0 ? -magnitude : magnitude;
}

[[nodiscard]] Mat4 shadow_to_clip() noexcept {
    const Vec3 travel = normalize(Vec3{0.45F, -0.72F, 0.55F});
    const Vec3 eye = kShadowCentre - (travel * (kShadowRadius * 2.0F));
    const Mat4 view = look_at(eye, kShadowCentre, Vec3{0.0F, 1.0F, 0.0F});
    const Mat4 projection = orthographic_reversed_z(-kShadowRadius, kShadowRadius, -kShadowRadius,
                                                    kShadowRadius, 0.1F, kShadowRadius * 4.0F);
    return projection * view;
}

// --- One run -------------------------------------------------------------------------------------

struct RunOptions {
    /// How many of boxes 1..3 are limbs. Zero is the frame with the skinned pipelines and no
    /// skinned instance.
    u32 limbs = 1;
    bool shadow = false;
    bool outline = false;
    /// The frame's skinned pipelines at all. Off is the scene as every suite before this one
    /// built it.
    bool skinned_pipelines = true;
    /// No sub-pixel jitter, so a still surface's motion vector is exactly zero frame to frame.
    bool pin_jitter = false;
};

class SkinnedRun;

struct RunState {
    SkinnedRun* run = nullptr;
};

class SkinnedRun {
public:
    SkinnedRun(skin_test::DeviceFixture& fixture, const RunOptions& options)
        : device_(&fixture.device()),
          options_(options),
          scene_(allocator()),
          skins_(allocator()),
          poses_(allocator()),
          highlights_(allocator()) {
        state_.run = this;
        FrameSceneHooks hooks;
        hooks.user = &state_;
        hooks.configure = &configure;
        hooks.pipelines = &pipelines;
        hooks.place_box = &place_box;
        hooks.geometry = &geometry;
        hooks.before_assemble = &before_assemble;
        hooks.before_upload = &before_upload;
        hooks.after_assemble = &after_assemble;
        scene_.set_hooks(hooks);
        const Status built = scene_.build(fixture.device());
        if (!built) {
            std::fprintf(stderr, "skinned frame: build failed: %s\n", built.error().message);
            return;
        }
        scene_.set_read_back(true);
        ready_ = create_skins() && create_targets();
        if (ready_ && options_.outline) {
            rendering::selection::OutlinePassDescription description;
            description.width = kWidth;
            description.height = kHeight;
            description.readback = true;
            ready_ = outline_.create(fixture.device(), scene_.pipelines(), description).has_value();
            ready_ =
                ready_ &&
                highlights_.select(901U, rendering::selection::HighlightColour{255, 150, 30, 255})
                    .has_value();
        }
    }

    ~SkinnedRun() {
        (void)device_->wait_idle();
        outline_.destroy();
        skins_.destroy();
        scene_.release();
        if (!shadow_view_.is_null()) {
            device_->destroy_texture_view(shadow_view_);
        }
        for (rhi::TextureHandle* texture : {&shadow_color_, &shadow_depth_}) {
            if (!texture->is_null()) {
                device_->destroy_texture(*texture);
            }
        }
        for (rhi::BufferHandle* buffer : {&velocity_buffer_, &shadow_buffer_, &limb_indices_}) {
            if (!buffer->is_null()) {
                device_->destroy_buffer(*buffer);
            }
        }
    }
    SkinnedRun(const SkinnedRun&) = delete;
    SkinnedRun& operator=(const SkinnedRun&) = delete;

    [[nodiscard]] bool ready() const noexcept { return ready_; }

    /// Publish `bend` for limb `which` into the pose world. Not uploaded until the next render.
    [[nodiscard]] bool pose(u32 which, f32 bend) {
        const std::vector<Mat4> matrices = limb_pose(bend);
        return poses_.publish(handles_[which], Span<const Mat4>(matrices.data(), matrices.size()))
            .has_value();
    }

    /// One frame: upload the dirty range, set every limb's pose offset, render.
    [[nodiscard]] bool render() {
        if (!ready_) {
            return false;
        }
        const Status uploaded =
            skins_.upload_poses(poses_.matrices(), poses_.upload_offset(), poses_.upload_size());
        if (!uploaded) {
            std::fprintf(stderr, "skinned frame: upload failed: %s\n", uploaded.error().message);
            return false;
        }
        poses_.clear_upload_range();
        for (u32 which = 0; which < options_.limbs; ++which) {
            if (!skins_.set_pose(instances_[which], poses_.matrix_offset(handles_[which]))) {
                return false;
            }
        }
        const Status rendered = scene_.render(RecordMode::Callbacks, report_);
        if (!rendered) {
            std::fprintf(stderr, "skinned frame: render failed: %s\n", rendered.error().message);
            return false;
        }
        ++frame_;
        pixels_.assign(scene_.pixels().begin(), scene_.pixels().end());
        read_velocity();
        read_shadow();
        if (options_.outline) {
            mask_.assign(kPixels, 0U);
            std::vector<f32> mask_depth(kPixels, 0.0F);
            std::vector<f32> scene_depth(kPixels, 0.0F);
            rendering::selection::OutlineReadback out;
            out.mask = Span<u32>(mask_.data(), mask_.size());
            out.mask_depth = Span<f32>(mask_depth.data(), mask_depth.size());
            out.scene_depth = Span<f32>(scene_depth.data(), scene_depth.size());
            return outline_.read_back(out).has_value();
        }
        return true;
    }

    [[nodiscard]] const std::vector<u32>& pixels() const { return pixels_; }
    [[nodiscard]] const std::vector<Vec2>& velocity() const { return velocity_; }
    [[nodiscard]] const std::vector<f32>& shadow() const { return shadow_; }
    [[nodiscard]] const std::vector<u32>& mask() const { return mask_; }
    [[nodiscard]] SkinnedScene& skins() { return skins_; }
    [[nodiscard]] animation::PoseWorld& poses() { return poses_; }
    [[nodiscard]] FrameScene& scene() { return scene_; }
    [[nodiscard]] SkinnedInstance instance(u32 which) const { return instances_[which]; }
    [[nodiscard]] animation::PoseHandle handle(u32 which) const { return handles_[which]; }
    [[nodiscard]] const Limb& limb() const { return limb_; }
    [[nodiscard]] u32 skin_passes() const { return skin_passes_; }

    // --- The hooks ----------------------------------------------------------------------------

    static void configure(rendering::assembly::AssemblyDescription& description,
                          void* user) noexcept {
        const SkinnedRun& run = *static_cast<RunState*>(user)->run;
        description.pin_jitter = run.options_.pin_jitter;
        description.selection_outlines = run.options_.outline;
    }

    static void pipelines(rendering::pipeline::PipelineSetup& setup, void* user) noexcept {
        setup.skinned = static_cast<RunState*>(user)->run->options_.skinned_pipelines;
    }

    static void place_box(u32 which, Vec3& centre, f32& half, void* user) noexcept {
        const SkinnedRun& run = *static_cast<RunState*>(user)->run;
        if (which >= 1U && which <= run.options_.limbs) {
            centre = kLimbCentres[which - 1U];
            half = kLimbHalf;
            return;
        }
        for (const u32 moved : kMovedBoxes) {
            if (which == moved && run.options_.limbs > 0U) {
                centre = Vec3{static_cast<f32>(which) * 3.0F, 0.0F, 60.0F};
            }
        }
    }

    static bool geometry(u32 which, rendering::pipeline::DrawGeometry& out, void* user) noexcept {
        SkinnedRun& run = *static_cast<RunState*>(user)->run;
        if (which < 1U || which > run.options_.limbs) {
            return false;
        }
        rendering::skinning::SkinnedDrawMesh mesh;
        mesh.indices = run.limb_indices_;
        mesh.index_count = kLimbIndices;
        // Every limb is posed before every frame these cases render, so each is skinned.
        return rendering::skinning::skinned_draw_geometry(run.skins_, run.instances_[which - 1U],
                                                          mesh, out);
    }

    static Status before_assemble(rendering::RenderGraph& graph,
                                  rendering::assembly::AssemblyView& view,
                                  rendering::assembly::FrameSinks& sinks, void* user) noexcept {
        SkinnedRun& run = *static_cast<RunState*>(user)->run;
        const auto before = static_cast<u32>(graph.pass_count());
        if (Status declared = run.skins_.declare(graph, run.frame_); !declared) {
            return declared;
        }
        run.skin_passes_ = static_cast<u32>(graph.pass_count()) - before;
        // EVERY PASS THAT DRAWS A SKINNED VERTEX DECLARES THE READ, so the graph orders it after
        // the skinning pass and derives the barrier.
        const Span<const rendering::ResourceId> reads = run.skins_.vertex_reads();
        for (const rendering::FramePassKind kind :
             {rendering::FramePassKind::DepthPrepass, rendering::FramePassKind::Shadow,
              rendering::FramePassKind::Opaque, rendering::FramePassKind::Transparent}) {
            sinks.passes[static_cast<usize>(kind)].vertex_reads = reads;
        }
        if (run.options_.shadow) {
            rendering::TextureRequest request;
            request.name = "skinned shadow map";
            request.format = rhi::Format::R32Sfloat;
            request.width = kShadowExtent;
            request.height = kShadowExtent;
            request.extra_usage = rhi::TextureUsage::TransferSource;
            view.shadow_color =
                graph.import_texture(request, run.shadow_color_, rhi::ImageUse::Undefined);
            request.name = "skinned shadow depth";
            request.format = rhi::Format::D32Sfloat;
            request.extra_usage = rhi::TextureUsage{};
            view.shadow_depth =
                graph.import_texture(request, run.shadow_depth_, rhi::ImageUse::Undefined);
            rendering::pipeline::FrameRecorder& recorder = run.scene_.recorder();
            recorder.set_shadow_targets(view.shadow_color, view.shadow_depth, kShadowExtent);
            const auto shadow = static_cast<usize>(rendering::FramePassKind::Shadow);
            sinks.passes[shadow] = recorder.sinks().passes[shadow];
            sinks.passes[shadow].vertex_reads = reads;
            run.shadow_resource_ = view.shadow_color;
            run.reads_[0] =
                rendering::FrameResourceRead{view.shadow_color, rhi::Access::FragmentSampledRead};
            sinks.passes[static_cast<usize>(rendering::FramePassKind::Opaque)].reads =
                Span<const rendering::FrameResourceRead>(run.reads_, 1);
        }
        if (run.options_.outline) {
            if (Status set = run.outline_.set_vertex_reads(reads); !set) {
                return set;
            }
            rendering::selection::OutlineSettings settings;
            if (Status set = run.outline_.set_highlights(&run.highlights_, settings); !set) {
                return set;
            }
            sinks.selection_outlines = run.outline_.stage(run.scene_.recorder());
        }
        return ok();
    }

    static Status before_upload(rendering::pipeline::FrameUpload& upload, void* user) noexcept {
        SkinnedRun& run = *static_cast<RunState*>(user)->run;
        if (!run.options_.shadow) {
            return ok();
        }
        u32 sun = static_cast<u32>(upload.lights.size());
        for (usize index = 0; index < upload.lights.size(); ++index) {
            if (upload.lights[index].kind == rendering::kGpuLightDirectional) {
                sun = static_cast<u32>(index);
            }
        }
        run.lights_.assign(upload.lights.begin(), upload.lights.end());
        if (sun < run.lights_.size()) {
            const Vec3 travel = normalize(Vec3{0.45F, -0.72F, 0.55F});
            run.lights_[sun].direction[0] = travel.x;
            run.lights_[sun].direction[1] = travel.y;
            run.lights_[sun].direction[2] = travel.z;
        }
        upload.lights = Span<const rendering::GpuLight>(run.lights_.data(), run.lights_.size());
        const Mat4 to_clip = shadow_to_clip();
        for (u32 row = 0; row < 4; ++row) {
            for (u32 column = 0; column < 4; ++column) {
                upload.view.shadow_to_clip[(row * 4U) + column] = to_clip.at(row, column);
            }
        }
        upload.view.shadow_control[0] = kShadowSlot;
        upload.view.shadow_control[1] = sun;
        upload.view.shadow_control[2] = kShadowExtent;
        upload.view.shadow_control[3] = 1U;
        const rendering::pipeline::MaterialTextureSlot slot{kShadowSlot, run.shadow_view_};
        return run.scene_.set_frame_textures(
            Span<const rendering::pipeline::MaterialTextureSlot>(&slot, 1));
    }

    static void record_copy(const rendering::PassContext& context, void* user) noexcept {
        auto* copy = static_cast<Copy*>(user);
        rhi::BufferTextureCopy region;
        region.texture_extent = rhi::Extent3D{copy->width, copy->height, 1};
        context.commands->copy_texture_to_buffer(context.executor->texture(copy->source),
                                                 copy->buffer,
                                                 Span<const rhi::BufferTextureCopy>(&region, 1));
    }

    static Status after_assemble(rendering::RenderGraph& graph,
                                 const rendering::FrameResources& resources, void* user) noexcept {
        SkinnedRun& run = *static_cast<RunState*>(user)->run;
        run.velocity_copy_ = Copy{resources.velocity, run.velocity_buffer_, kWidth, kHeight};
        run.declare_copy(graph, run.velocity_copy_, "skinned velocity capture", u64{kPixels} * 4U);
        if (run.options_.shadow && run.shadow_resource_ != rendering::kInvalidResource) {
            run.shadow_copy_ =
                Copy{run.shadow_resource_, run.shadow_buffer_, kShadowExtent, kShadowExtent};
            run.declare_copy(graph, run.shadow_copy_, "skinned shadow capture",
                             u64{kShadowExtent} * kShadowExtent * 4U);
        }
        return ok();
    }

private:
    struct Copy {
        rendering::ResourceId source = rendering::kInvalidResource;
        rhi::BufferHandle buffer;
        u32 width = 0;
        u32 height = 0;
    };

    void declare_copy(rendering::RenderGraph& graph, Copy& copy, const char* name, u64 bytes) {
        if (copy.source == rendering::kInvalidResource) {
            return;
        }
        rendering::BufferRequest request;
        request.name = name;
        request.size = bytes;
        request.extra_usage = rhi::BufferUsage::TransferDestination;
        const rendering::ResourceId destination = graph.import_buffer(request, copy.buffer);
        graph.add_pass(name, rhi::QueueKind::Graphics)
            .read(copy.source, rhi::Access::TransferRead)
            .write(destination, rhi::Access::TransferWrite)
            .record(&record_copy, &copy);
        graph.add_pass(name, rhi::QueueKind::Graphics)
            .read(destination, rhi::Access::HostRead)
            .side_effect();
    }

    [[nodiscard]] bool create_skins() {
        rendering::skinning::SkinnedSceneDescription description;
        description.max_mesh_vertices = kLimbVertices * 2U;
        description.max_instance_vertices = kLimbVertices * kMaxLimbs;
        description.max_pose_matrices = kBones * 2U * kMaxLimbs;
        description.max_meshes = 1;
        description.max_instances = kMaxLimbs;
        description.read_back = true;
        if (!skins_.create(*device_, description)) {
            return false;
        }
        rendering::skinning::SkinnedMeshDescription mesh;
        mesh.positions = Span<const Vec3>(limb_.positions.data(), limb_.positions.size());
        mesh.frames = Span<const PackedNormalTangent>(limb_.frames.data(), limb_.frames.size());
        mesh.influences =
            Span<const GpuSkinInfluence>(limb_.influences.data(), limb_.influences.size());
        mesh.bone_count = kBones;
        Expected<rendering::skinning::SkinnedMeshId, Error> added = skins_.add_mesh(mesh);
        if (!added.has_value()) {
            return false;
        }
        for (u32 which = 0; which < options_.limbs; ++which) {
            Expected<SkinnedInstance, Error> instance = skins_.add_instance(*added);
            Expected<animation::PoseHandle, Error> handle = poses_.add(kBones);
            if (!instance.has_value() || !handle.has_value()) {
                return false;
            }
            instances_[which] = *instance;
            handles_[which] = *handle;
            if (!pose(which, 0.0F)) {
                return false;
            }
        }
        rhi::BufferDescription indices;
        indices.name = "limb indices";
        indices.size = limb_.indices.size() * sizeof(u16);
        indices.usage = rhi::BufferUsage::Index;
        indices.memory = rhi::MemoryUse::Upload;
        Expected<rhi::BufferHandle, Error> made = device_->create_buffer(indices);
        if (!made.has_value()) {
            return false;
        }
        limb_indices_ = *made;
        std::memcpy(device_->buffer_mapped_pointer(limb_indices_), limb_.indices.data(),
                    indices.size);
        return true;
    }

    [[nodiscard]] bool create_targets() {
        rhi::BufferDescription readback;
        readback.name = "skinned velocity readback";
        readback.size = u64{kPixels} * 4U;
        readback.usage = rhi::BufferUsage::TransferDestination;
        readback.memory = rhi::MemoryUse::Readback;
        Expected<rhi::BufferHandle, Error> velocity = device_->create_buffer(readback);
        if (!velocity.has_value()) {
            return false;
        }
        velocity_buffer_ = *velocity;
        if (!options_.shadow) {
            return true;
        }
        readback.name = "skinned shadow readback";
        readback.size = u64{kShadowExtent} * kShadowExtent * 4U;
        Expected<rhi::BufferHandle, Error> shadow = device_->create_buffer(readback);
        if (!shadow.has_value()) {
            return false;
        }
        shadow_buffer_ = *shadow;
        rhi::TextureDescription texture;
        texture.name = "skinned shadow map";
        texture.format = rhi::Format::R32Sfloat;
        texture.extent = rhi::Extent3D{kShadowExtent, kShadowExtent, 1};
        texture.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled |
                        rhi::TextureUsage::TransferSource;
        Expected<rhi::TextureHandle, Error> color = device_->create_texture(texture);
        if (!color.has_value()) {
            return false;
        }
        shadow_color_ = *color;
        texture.name = "skinned shadow depth";
        texture.format = rhi::Format::D32Sfloat;
        texture.usage = rhi::TextureUsage::DepthStencilAttachment;
        Expected<rhi::TextureHandle, Error> depth = device_->create_texture(texture);
        if (!depth.has_value()) {
            return false;
        }
        shadow_depth_ = *depth;
        rhi::TextureViewDescription view;
        view.name = "skinned shadow map";
        view.texture = shadow_color_;
        Expected<rhi::TextureViewHandle, Error> made = device_->create_texture_view(view);
        if (!made.has_value()) {
            return false;
        }
        shadow_view_ = *made;
        return true;
    }

    void read_velocity() {
        velocity_.assign(kPixels, Vec2{});
        const auto* words =
            static_cast<const u16*>(device_->buffer_mapped_pointer(velocity_buffer_));
        if (words == nullptr) {
            return;
        }
        for (u32 pixel = 0; pixel < kPixels; ++pixel) {
            velocity_[pixel] =
                Vec2{half_to_float(words[pixel * 2U]), half_to_float(words[(pixel * 2U) + 1U])};
        }
    }

    void read_shadow() {
        shadow_.clear();
        if (!options_.shadow) {
            return;
        }
        const auto* texels =
            static_cast<const f32*>(device_->buffer_mapped_pointer(shadow_buffer_));
        if (texels != nullptr) {
            shadow_.assign(texels, texels + (static_cast<usize>(kShadowExtent) * kShadowExtent));
        }
    }

    rhi::Device* device_ = nullptr;
    RunOptions options_;
    FrameScene scene_;
    SkinnedScene skins_;
    animation::PoseWorld poses_;
    rendering::selection::HighlightSet highlights_;
    rendering::selection::OutlinePass outline_;
    Limb limb_;
    RunState state_{};
    SkinnedInstance instances_[kMaxLimbs];
    animation::PoseHandle handles_[kMaxLimbs];
    rhi::BufferHandle limb_indices_;
    rhi::BufferHandle velocity_buffer_;
    rhi::BufferHandle shadow_buffer_;
    rhi::TextureHandle shadow_color_;
    rhi::TextureHandle shadow_depth_;
    rhi::TextureViewHandle shadow_view_;
    rendering::ResourceId shadow_resource_ = rendering::kInvalidResource;
    rendering::FrameResourceRead reads_[1] = {};
    Copy velocity_copy_;
    Copy shadow_copy_;
    std::vector<rendering::GpuLight> lights_;
    rendering::assembly::AssemblyReport report_{};
    std::vector<u32> pixels_;
    std::vector<Vec2> velocity_;
    std::vector<f32> shadow_;
    std::vector<u32> mask_;
    u64 frame_ = 0;
    u32 skin_passes_ = 0;
    bool ready_ = false;
};

// --- References ----------------------------------------------------------------------------------

bool updating_references() noexcept {
    const char* value = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

void save(const char* name, const std::vector<u32>& texels) noexcept {
    render_test::Image image(allocator());
    if (!render_test::adopt(image, Span<const u32>(texels.data(), texels.size()), kWidth, kHeight)
             .has_value()) {
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

/// Compare against a committed reference. `exact` holds the frame to the bytes; otherwise the
/// golden rule (`compare` forgives one 8-bit step, and differences only on silhouette edges), so a
/// conformant driver other than the one that wrote the file passes.
void check_reference(const char* directory, const char* file, const std::vector<u32>& pixels,
                     bool exact) {
    char path[1024];
    (void)std::snprintf(path, sizeof(path), "%s/references/%s", directory, file);
    render_test::Image rendered(allocator());
    CY_REQUIRE(
        render_test::adopt(rendered, Span<const u32>(pixels.data(), pixels.size()), kWidth, kHeight)
            .has_value());
    if (updating_references() && !exact) {
        CY_CHECK(render_test::write_png(path, rendered).has_value());
        std::fprintf(stderr, "wrote %s — look at it, then commit it. This run FAILS on purpose.\n",
                     path);
        CY_TEST_FAIL_CHECK("references were regenerated; this mode never passes");
        return;
    }
    render_test::Image reference(allocator());
    const Status read = render_test::read_png(path, reference);
    if (!read) {
        std::fprintf(stderr, "%s: %s\n", path, read.error().message);
    }
    CY_REQUIRE(read.has_value());
    const render_test::Comparison comparison = render_test::compare(reference, rendered);
    std::fprintf(stderr,
                 "%s: %u differing (%u off an edge, %u edge texels), worst delta %u at (%u, %u)\n",
                 file, comparison.differing, comparison.differing_off_edge, comparison.edge_texels,
                 comparison.max_channel_delta, comparison.worst_x, comparison.worst_y);
    CY_REQUIRE(comparison.comparable);
    if (exact) {
        CY_CHECK_EQ(comparison.differing, 0U);
        CY_CHECK_EQ(comparison.max_channel_delta, 0U);
        return;
    }
    CY_CHECK_EQ(comparison.differing_off_edge, 0U);
    CY_CHECK_LE(comparison.differing, comparison.edge_texels);
}

[[nodiscard]] u32 differing(const std::vector<u32>& a, const std::vector<u32>& b) noexcept {
    u32 count = 0;
    for (usize index = 0; index < a.size() && index < b.size(); ++index) {
        count += a[index] != b[index] ? 1U : 0U;
    }
    return count;
}

/// The limb's pixels — those the selection mask covers — and their motion, in PIXELS.
struct LimbMotion {
    u32 covered = 0;
    /// Covered pixels moving by more than `kMovingPixels`, and the lowest screen row among them.
    u32 moving = 0;
    u32 lowest_moving_row = 0;
    /// The largest motion of a covered pixel at or below `row` — the upper arm's.
    f32 largest_below = 0.0F;
    f32 largest = 0.0F;
};

/// Half-float storage of a normalised coordinate near zero is far finer than this; a still
/// surface's vector is the jitter's rounding and nothing else.
constexpr f32 kStillPixels = 0.05F;
/// The ring is a tube, not a line: its near side projects a few rows below its centre.
constexpr f32 kRingMarginRows = 5.0F;
constexpr f32 kMovingPixels = 0.5F;

[[nodiscard]] LimbMotion limb_motion(const SkinnedRun& run, f32 row) noexcept {
    LimbMotion out;
    for (u32 pixel = 0; pixel < kPixels; ++pixel) {
        if (run.mask()[pixel] == 0U) {
            continue;
        }
        const Vec2 velocity = run.velocity()[pixel];
        const f32 pixels = std::hypot(velocity.x * static_cast<f32>(kWidth),
                                      velocity.y * static_cast<f32>(kHeight));
        const u32 y = pixel / kWidth;
        ++out.covered;
        out.largest = pixels > out.largest ? pixels : out.largest;
        if (static_cast<f32>(y) >= row) {
            out.largest_below = pixels > out.largest_below ? pixels : out.largest_below;
        }
        if (pixels > kMovingPixels) {
            ++out.moving;
            out.lowest_moving_row = y > out.lowest_moving_row ? y : out.lowest_moving_row;
        }
    }
    return out;
}

#define CY_SKINNED_FRAME_DEVICE(fixture)                              \
    skin_test::DeviceFixture fixture("cy_test_render_skinned_frame"); \
    if (!(fixture).has_gpu()) {                                       \
        (fixture).report_skip();                                      \
        return;                                                       \
    }

}  // namespace

CY_TEST_CASE("(a) skinned pipelines and no skinned instance: the frame before the change") {
    CY_SKINNED_FRAME_DEVICE(fixture)
    RunOptions options;
    options.limbs = 0;
    SkinnedRun run(fixture, options);
    CY_REQUIRE(run.ready());
    CY_REQUIRE(run.render());
    CY_CHECK_EQ(run.scene().recorded().skinned_draws, 0U);
    CY_CHECK_EQ(run.skins().stats().passes, 0U);
    CY_CHECK_EQ(run.skin_passes(), 0U);
    CY_CHECK_GT(run.scene().pipelines().created(), 0U);
    CY_CHECK_FALSE(run.scene()
                       .pipelines()
                       .skinned_pipeline(rendering::pipeline::FramePipelineKind::Opaque)
                       .is_null());
    save("skinned_frame_off.png", run.pixels());
    check_reference(CY_PIPELINE_TEST_DIR, "frame_scene_before_bloom.png", run.pixels(), true);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(b) a bent limb is skinned by the dispatch and drawn by the frame's own passes") {
    CY_SKINNED_FRAME_DEVICE(fixture)
    SkinnedRun run(fixture, RunOptions{});
    CY_REQUIRE(run.ready());
    CY_REQUIRE(run.pose(0, 1.1F));
    CY_REQUIRE(run.render());
    save("skinned_frame.png", run.pixels());

    // Drawn by the depth prepass and the opaque pass, from the skinning output.
    CY_CHECK_EQ(run.scene().recorded().skinned_draws, 2U);
    CY_CHECK_EQ(run.skins().stats().dispatches, 1U);

    // THE DISPATCH IS THE REFERENCE, word for word.
    Expected<render::geometry::GpuSkinConstants, Error> constants =
        run.skins().constants(run.instance(0));
    CY_REQUIRE(constants.has_value());
    const Span<const GpuBoneMatrix> pose = run.skins().pose_buffer();
    std::vector<GpuBoneMatrix> bones(pose.begin(), pose.end());
    std::vector<Vec3> positions(kLimbVertices * kMaxLimbs * 2U);
    std::vector<PackedNormalTangent> frames(positions.size());
    render::geometry::SkinInputs inputs;
    inputs.bones = Span<const GpuBoneMatrix>(bones.data(), bones.size());
    inputs.positions = Span<const Vec3>(run.limb().positions.data(), run.limb().positions.size());
    inputs.frames =
        Span<const PackedNormalTangent>(run.limb().frames.data(), run.limb().frames.size());
    inputs.influences =
        Span<const GpuSkinInfluence>(run.limb().influences.data(), run.limb().influences.size());
    render::geometry::GpuSkinConstants reference = *constants;
    reference.first_input_vertex = 0;
    CY_REQUIRE(
        render::geometry::cpu_reference_skin(
            reference, inputs,
            render::geometry::SkinOutputs{Span<Vec3>(positions.data(), positions.size()),
                                          Span<PackedNormalTangent>(frames.data(), frames.size())})
            .has_value());
    Expected<Span<const Vec3>, Error> skinned = run.skins().read_back_positions();
    Expected<Span<const PackedNormalTangent>, Error> skinned_frames =
        run.skins().read_back_frames();
    CY_REQUIRE(skinned.has_value());
    CY_REQUIRE(skinned_frames.has_value());
    u32 mismatched = 0;
    for (u32 vertex = 0; vertex < kLimbVertices; ++vertex) {
        const u32 at = constants->first_output_vertex + vertex;
        mismatched += std::memcmp(&(*skinned)[at], &positions[at], sizeof(Vec3)) != 0 ? 1U : 0U;
        mismatched +=
            std::memcmp(&(*skinned_frames)[at], &frames[at], sizeof(PackedNormalTangent)) != 0 ? 1U
                                                                                               : 0U;
    }
    CY_CHECK_EQ(mismatched, 0U);
    // The forearm moved: its top cap is no longer straight above the elbow.
    CY_CHECK_GT((*skinned)[constants->first_output_vertex + (kSides * kRings) + kSides].x, 0.2F);

    // A bent limb is a different picture from a straight one, and the frame draws it.
    SkinnedRun straight(fixture, RunOptions{});
    CY_REQUIRE(straight.ready());
    CY_REQUIRE(straight.render());
    CY_CHECK_GT(differing(run.pixels(), straight.pixels()), 200U);

    check_reference(CY_SKINNING_TEST_DIR, "skinned_frame.png", run.pixels(), false);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(c) the shadow pass draws the skinned limb into the directional map") {
    CY_SKINNED_FRAME_DEVICE(fixture)
    RunOptions options;
    options.shadow = true;
    SkinnedRun bent(fixture, options);
    CY_REQUIRE(bent.ready());
    CY_REQUIRE(bent.pose(0, 1.1F));
    CY_REQUIRE(bent.render());
    save("skinned_frame_shadow.png", bent.pixels());
    // Prepass, opaque and shadow.
    CY_CHECK_EQ(bent.scene().recorded().skinned_draws, 3U);

    SkinnedRun straight(fixture, options);
    CY_REQUIRE(straight.ready());
    CY_REQUIRE(straight.render());
    CY_REQUIRE_EQ(bent.shadow().size(), straight.shadow().size());
    u32 changed = 0;
    for (usize texel = 0; texel < bent.shadow().size(); ++texel) {
        changed += bent.shadow()[texel] != straight.shadow()[texel] ? 1U : 0U;
    }
    std::fprintf(stderr, "(c) shadow-map texels the bend changed: %u\n", changed);
    CY_CHECK_GT(changed, 20U);
    CY_CHECK_GT(differing(bent.pixels(), straight.pixels()), 200U);
    check_reference(CY_SKINNING_TEST_DIR, "skinned_frame_shadow.png", bent.pixels(), false);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(d) motion vectors: a still limb's are zero and a moving forearm's are not") {
    CY_SKINNED_FRAME_DEVICE(fixture)
    // The selection mask says which pixels are the limb; nothing else in the frame moves either.
    RunOptions options;
    options.pin_jitter = true;
    options.outline = true;
    SkinnedRun run(fixture, options);
    CY_REQUIRE(run.ready());
    // The rigid ring just below the elbow, on screen. Everything below it is the upper arm's
    // alone, bound to the bone that never moves.
    const Mat4 to_clip = run.scene().projection() * run.scene().view();
    const Vec3 ring = kLimbCentres[0] + Vec3{0.0F, kRingHeights[1] * kLimbHalf * 2.0F, 0.0F};
    const Vec4 projected = to_clip * Vec4{ring.x, ring.y, ring.z, 1.0F};
    const f32 rigid_row = (0.5F - (0.5F * projected.y / projected.w)) * static_cast<f32>(kHeight);

    CY_REQUIRE(run.pose(0, 0.6F));
    CY_REQUIRE(run.render());
    // The same pose published again: the limb stands still, and the frame reads last frame's
    // skinned positions from the other half of the output.
    CY_REQUIRE(run.pose(0, 0.6F));
    CY_REQUIRE(run.render());
    const LimbMotion still = limb_motion(run, rigid_row + kRingMarginRows);
    std::fprintf(stderr, "(d) still: %u covered, largest %.4f px\n", still.covered,
                 static_cast<double>(still.largest));
    CY_CHECK_GT(still.covered, 500U);
    CY_CHECK_LT(still.largest, kStillPixels);

    // The forearm turns further; the upper arm does not move at all.
    CY_REQUIRE(run.pose(0, 0.9F));
    CY_REQUIRE(run.render());
    const LimbMotion moving = limb_motion(run, rigid_row + kRingMarginRows);
    std::fprintf(stderr,
                 "(d) moving: %u of %u covered move, lowest moving row %u (rigid below %.1f), "
                 "largest %.2f px, below the rigid ring %.4f px\n",
                 moving.moving, moving.covered, moving.lowest_moving_row,
                 static_cast<double>(rigid_row), static_cast<double>(moving.largest),
                 static_cast<double>(moving.largest_below));
    CY_CHECK_GT(moving.moving, 200U);
    CY_CHECK_GT(moving.largest, 5.0F);
    CY_CHECK_LT(static_cast<f32>(moving.lowest_moving_row), rigid_row + kRingMarginRows);
    CY_CHECK_LT(moving.largest_below, kStillPixels);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(e) the selection mask covers the limb where the frame drew it") {
    CY_SKINNED_FRAME_DEVICE(fixture)
    RunOptions options;
    options.outline = true;
    SkinnedRun straight(fixture, options);
    CY_REQUIRE(straight.ready());
    CY_REQUIRE(straight.render());
    SkinnedRun bent(fixture, options);
    CY_REQUIRE(bent.ready());
    CY_REQUIRE(bent.pose(0, 1.1F));
    CY_REQUIRE(bent.render());
    u32 straight_covered = 0;
    u32 bent_covered = 0;
    u32 changed = 0;
    for (u32 pixel = 0; pixel < kPixels; ++pixel) {
        straight_covered += straight.mask()[pixel] != 0 ? 1U : 0U;
        bent_covered += bent.mask()[pixel] != 0 ? 1U : 0U;
        changed += (straight.mask()[pixel] != 0) != (bent.mask()[pixel] != 0) ? 1U : 0U;
    }
    std::fprintf(stderr, "(e) mask: straight %u, bent %u, changed %u\n", straight_covered,
                 bent_covered, changed);
    CY_CHECK_GT(straight_covered, 200U);
    CY_CHECK_GT(bent_covered, 200U);
    // A mask drawn from the rigid stream would be the cube, the same for both poses.
    CY_CHECK_GT(changed, 100U);
    save("skinned_frame_outline.png", bent.pixels());
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(f) three limbs are skinned by one pass of three dispatches") {
    CY_SKINNED_FRAME_DEVICE(fixture)
    RunOptions options;
    options.limbs = 3;
    SkinnedRun run(fixture, options);
    CY_REQUIRE(run.ready());
    CY_REQUIRE(run.pose(0, 0.4F));
    CY_REQUIRE(run.pose(1, 0.8F));
    CY_REQUIRE(run.pose(2, 1.2F));
    CY_REQUIRE(run.render());
    // One compute pass plus its read-back pair, and nothing else the scene declared.
    CY_CHECK_EQ(run.skins().stats().dispatches, 3U);
    CY_CHECK_EQ(run.skins().stats().passes, 3U);
    CY_CHECK_EQ(run.skin_passes(), 3U);
    CY_CHECK_EQ(run.scene().recorded().skinned_draws, 6U);
    save("skinned_frame_three.png", run.pixels());
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(g) a changed instance uploads its own matrices and nothing else") {
    CY_SKINNED_FRAME_DEVICE(fixture)
    RunOptions options;
    options.limbs = 3;
    SkinnedRun run(fixture, options);
    CY_REQUIRE(run.ready());
    // The first frame uploads the range the three creations published: from the first limb's
    // current half to the last's, which is the world's dirty range and not the whole array.
    const u32 first_range = run.poses().upload_size();
    CY_CHECK_LT(first_range, static_cast<u32>(run.poses().matrices().size()));
    CY_REQUIRE(run.render());
    CY_CHECK_EQ(run.skins().stats().uploaded_matrices, first_range);

    // A SENTINEL STAGED AND NEVER COMMITTED: limb 0's staging half holds garbage the world has not
    // published, so it is outside the dirty range. A full copy would carry it to the device.
    animation::PoseWorld& world = run.poses();
    Span<Mat4> staged = world.staging(run.handle(0));
    CY_REQUIRE_EQ(staged.size(), static_cast<usize>(kBones));
    const u32 staged_at = static_cast<u32>(staged.data() - world.matrices().data());
    for (Mat4& matrix : staged) {
        matrix = Mat4::from_translation(Vec3{99.0F, 99.0F, 99.0F});
    }
    CY_REQUIRE(run.pose(1, 0.7F));
    CY_CHECK_EQ(world.upload_size(), kBones);
    CY_REQUIRE(run.render());
    CY_CHECK_EQ(run.skins().stats().uploaded_matrices, kBones);
    const Span<const GpuBoneMatrix> device = run.skins().pose_buffer();
    for (u32 bone = 0; bone < kBones; ++bone) {
        const GpuBoneMatrix garbage =
            render::geometry::pack_bone_matrix(Mat4::from_translation(Vec3{99.0F, 99.0F, 99.0F}));
        CY_CHECK(std::memcmp(&device[staged_at + bone], &garbage, sizeof(GpuBoneMatrix)) != 0);
    }
    // Limb 1's current matrices are on the device.
    const u32 current = world.matrix_offset(run.handle(1));
    const GpuBoneMatrix expected = render::geometry::pack_bone_matrix(limb_pose(0.7F)[1]);
    CY_CHECK(std::memcmp(&device[current + 1U], &expected, sizeof(GpuBoneMatrix)) == 0);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
