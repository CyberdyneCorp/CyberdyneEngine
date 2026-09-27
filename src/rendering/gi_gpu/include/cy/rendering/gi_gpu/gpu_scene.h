// SPDX-License-Identifier: MIT
#pragma once
// The GI scene on the device: the distance field's clipmaps and the surface cards, uploaded
// incrementally. Issue #35, stage 1.
//
// ================================================================================================
// WHAT IS ON THE DEVICE, AND WHO DECIDES WHAT CHANGES
// ================================================================================================
//
//   page table   one word per brick of every clipmap level's window, toroidally addressed: the
//                slot in the brick pool, or empty. A brick outside the level's window is far.
//   brick pool   `DistanceField::brick_pool()`, slot for slot — 64 samples per brick.
//   cards        one record per surface-cache page: position, normal, albedo, emission.
//   card state   the page's outgoing radiance, validity and accumulated indirect.
//   card grid    `gi::CardGrid` over the live cards, the lookup a traced hit resolves through.
//
// THE HOST STAYS THE AUTHORITY. Nothing here solves a brick or allocates a card: `upload_field`
// applies `DistanceField::last_changes()` — the bricks the last `scroll_to` solved, which are the
// bricks the scene's invalidation causes reached (`IlluminationSystem::service_invalidations` turns
// a cause into `DistanceField::invalidate`) plus the band a scroll exposed — and `upload_cards`
// writes the pages whose `SurfacePage::revision` moved. A frame in which nothing moved uploads
// nothing, and `FieldUploadReport` / `CardUploadReport` are the counts that make that checkable. A
// mirror that missed a field generation rebuilds its table from `DistanceField::visit_bricks` and
// says so in `full`.
//
// ================================================================================================
// WHAT IT DOES NOT DO YET
// ================================================================================================
//
// Every buffer is host-visible and written in place, which is right for a suite that drains each
// frame and wrong for a renderer with frames in flight: stage 4 (the composite) needs a per-frame
// staging copy before this is used from a live frame. The shadow map is a depth array the host
// provides (`gi::ShadowMap`), not the frame's shadow cascade; binding the cascade is stage 4 too.
// See the module README.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/card_lighting.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/lighting.h>
#include <cy/rendering/gi/surface_cache.h>
#include <cy/rendering/graph/graph.h>

namespace cy::rendering::gi_gpu {

/// The most clipmap levels the constant block has room for.
inline constexpr u32 kMaxFieldLevels = 8;

/// How large a scene the device copy is sized for. One allocation per buffer, at creation: the
/// buffers do not grow, and an upload that would overflow one is refused by name.
struct GpuGiSceneDescription {
    u32 max_levels = 4;
    /// Bricks along each axis of a level's window: `ClipmapSettings::resolution / kBrickEdge`.
    u32 max_window_bricks = 16;
    u32 max_brick_slots = 8192;
    u32 max_cards = 4096;
    u32 max_lights = 16;
    u32 max_shadow_resolution = 256;
    /// The most pages one surface update may shade: the GI budget's `SurfaceCacheRate` ceiling.
    u32 max_selection = 1024;
    /// The most rays one `declare_trace` batch may carry.
    u32 max_rays = 4096;
};

/// What one `upload_field` wrote.
struct FieldUploadReport {
    /// The field generation the device copy now matches.
    u64 generation = 0;
    /// Bricks whose samples were copied into the pool.
    u32 bricks_uploaded = 0;
    /// Page-table entries written, including bricks found empty.
    u32 table_entries = 0;
    /// The table was rebuilt from every brick rather than from the last scroll's changes.
    bool full = false;
    u64 bytes = 0;
};

/// What one `upload_cards` wrote.
struct CardUploadReport {
    u32 cards_uploaded = 0;
    /// A card was allocated, released or moved, so the lookup grid was rebuilt.
    bool grid_rebuilt = false;
    u64 bytes = 0;
};

/// One ray of a trace batch, and its answer: `DistanceField::sphere_trace`'s arguments and result.
struct GpuTraceRay {
    Vec3 origin{0.0F, 0.0F, 0.0F};
    f32 max_distance = 0.0F;
    Vec3 direction{0.0F, 0.0F, -1.0F};
    f32 t_min = 0.0F;
};

struct GpuTraceHit {
    bool hit = false;
    bool exhausted = false;
    f32 t = 0.0F;
    f32 closest_approach_metres = 0.0F;
    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 normal{0.0F, 1.0F, 0.0F};
};

class GpuSurfaceShading;

class GpuGiScene {
public:
    GpuGiScene() = default;
    ~GpuGiScene();

    GpuGiScene(const GpuGiScene&) = delete;
    GpuGiScene& operator=(const GpuGiScene&) = delete;
    GpuGiScene(GpuGiScene&&) = delete;
    GpuGiScene& operator=(GpuGiScene&&) = delete;

    /// Compute, and a native shader format this module ships: SPIR-V or MSL.
    [[nodiscard]] static bool supported(const rhi::Device& device) noexcept;

    [[nodiscard]] Status create(Allocator& allocator, rhi::Device& device,
                                const GpuGiSceneDescription& desc) noexcept;
    void destroy() noexcept;

    // --- Stage 1: the scene representation ------------------------------------------------------

    /// Bring the device copy of `field` up to its current generation. Incremental when the copy is
    /// exactly one generation behind; a rebuild of the table otherwise. Refuses a field with more
    /// levels, a wider window or more brick slots than the scene was created for.
    [[nodiscard]] Expected<FieldUploadReport, Error> upload_field(
        const gi::DistanceField& field) noexcept;

    /// Write every page whose revision moved since the last call, and rebuild the lookup grid at
    /// `lookup_radius` when a card was allocated, released or moved.
    [[nodiscard]] Expected<CardUploadReport, Error> upload_cards(Span<const gi::SurfacePage> pages,
                                                                 f32 lookup_radius) noexcept;

    [[nodiscard]] Status upload_lights(Span<const gi::GiLight> lights) noexcept;
    /// The directional light's depth map, or null for none: every shadow ray then goes to the
    /// field, as `ShadowMapOccluder` with no map sends it to its fallback.
    [[nodiscard]] Status upload_shadow_map(const gi::ShadowMap* map) noexcept;
    void set_gather(const gi::CardGatherSettings& gather) noexcept;

    // --- A trace batch against the uploaded field -----------------------------------------------

    [[nodiscard]] Status set_rays(Span<const GpuTraceRay> rays) noexcept;
    /// Declare the batch `set_rays` wrote and the copy of its hits to the host.
    [[nodiscard]] Status declare_trace(RenderGraph& graph) noexcept;
    /// After the frame's fence. `out` is at most the batch.
    [[nodiscard]] Status read_back_hits(Span<GpuTraceHit> out) const noexcept;

    [[nodiscard]] const FieldUploadReport& last_field_upload() const noexcept {
        return field_report_;
    }
    [[nodiscard]] const CardUploadReport& last_card_upload() const noexcept { return card_report_; }
    [[nodiscard]] const GpuGiSceneDescription& description() const noexcept { return desc_; }

private:
    friend class GpuSurfaceShading;

    enum Binding : u32 {
        kConstants = 0,
        kPageTable,
        kBricks,
        kCards,
        kCardState,
        kGridRanges,
        kGridItems,
        kLights,
        kShadowDepths,
        kSelection,
        kResults,
        kRays,
        kHits,
        kBindingCount,
        kResultsReadback = kBindingCount,
        kHitsReadback,
        kBufferCount,
    };
    enum Pipeline : u32 {
        kTrace = 0,
        kShade,
        kCommit,
        kPipelineCount,
    };

    /// The push constant block every dispatch takes: `GiDispatch` in gi_gpu_common.slang.
    struct Dispatch {
        u32 count = 0;
        u32 frame = 0;
        u32 reserved[2] = {0, 0};
    };
    struct Recording {
        GpuGiScene* self = nullptr;
        Pipeline pipeline = kTrace;
        Dispatch dispatch{};
    };
    struct Copy {
        GpuGiScene* self = nullptr;
        Binding source = kHits;
        Binding destination = kHitsReadback;
        u64 bytes = 0;
    };

    [[nodiscard]] Status create_pipelines() noexcept;
    [[nodiscard]] Status create_buffers() noexcept;
    [[nodiscard]] Status write_descriptors() noexcept;
    [[nodiscard]] void* mapped(Binding binding) const noexcept;
    void write_constants() noexcept;
    void apply_brick(const gi::DistanceField& field, const gi::FieldBrickChange& change,
                     FieldUploadReport& report) noexcept;
    [[nodiscard]] ResourceId import(RenderGraph& graph, Binding binding) noexcept;

    static void record_dispatch(const PassContext& context, void* user) noexcept;
    static void record_copy(const PassContext& context, void* user) noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    GpuGiSceneDescription desc_{};

    rhi::ShaderModuleHandle shaders_[kPipelineCount];
    rhi::ComputePipelineHandle pipelines_[kPipelineCount];
    rhi::DescriptorSetLayoutHandle set_layout_;
    rhi::PipelineLayoutHandle pipeline_layout_;
    rhi::DescriptorSetHandle set_;
    rhi::BufferHandle buffers_[kBufferCount];
    u64 sizes_[kBufferCount] = {};

    /// `GpuGiConstants`: 32 uint4 words, the layout gi_gpu_common.slang states.
    u32 constants_[128] = {};

    u64 field_generation_ = 0;
    bool field_uploaded_ = false;
    u32 field_levels_ = 0;
    u32 field_window_ = 0;
    /// The revision of each card as last written, and whether it was live and where: a change of
    /// either of the last two is what rebuilds the grid.
    Array<u32> card_revisions_;
    Array<Vec3> card_positions_;
    Array<u8> card_live_;
    gi::CardGrid grid_;

    u32 ray_count_ = 0;
    Recording trace_{};
    Copy hits_copy_{};

    FieldUploadReport field_report_{};
    CardUploadReport card_report_{};
};

}  // namespace cy::rendering::gi_gpu
