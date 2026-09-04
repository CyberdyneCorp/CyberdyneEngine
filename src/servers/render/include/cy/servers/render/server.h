#pragma once
// The handle-based render server. Task 4.1.1.
//
// `rendering-architecture` — "Handle-based render server": `RenderServer` "SHALL own all renderable
// state and expose it through generational handles, **with no knowledge of entities, nodes, or
// scripts**", and it "SHALL produce a frame without an ECS world or scene tree existing".
//
// ================================================================================================
// THE THING TO UNDERSTAND FIRST: WHAT THIS SERVER IS *NOT* ALLOWED TO SEE
// ================================================================================================
//
// `src/servers/` is layer 2. `src/backends/` is layer 3 and `src/rendering/` is layer 4, so this
// file may not include `cy/backends/rhi/**` or `cy/rendering/**` — there is no device here, no
// image format the RHI named, no command buffer and no render graph. The layer checker fails the
// build over it, and that is the correct arrangement rather than an obstacle:
//
//   * "a test drives RenderServer directly with handles and produces a frame without an ECS world
//     or scene tree existing" is TRUE OF THIS HEADER, not of a mock. There is nothing to mock.
//   * the model — scenes, views, instances, meshes, materials, the GPU scene's records — is
//     testable with no GPU, no window and no job system.
//   * the modules that DO own a device (src/rendering/) consume this one, and the direction cannot
//     invert by accident.
//
// It also fixes the shape of the wiring. `cy::runtime::ServerRegistry` lives at layer 5 and holds
// `runtime::Server*`; this class cannot derive from that interface, so the runtime adapts it — and
// the four methods below (`backend_name`, `initialize`, `shutdown`, `is_null_backend`) are
// deliberately the four `runtime::Server` declares, with the same signatures, so the adapter is
// four forwarding lines and no decisions. See README.md.
//
// ================================================================================================
// WHAT IS STORED HERE AND WHAT IS NOT
// ================================================================================================
//
// Twenty object families are named in `handles.h`, because a family that arrives later with a
// different handle spelling is a family every caller learns twice. EIGHT of them have storage at
// M3: textures, samplers, meshes, materials, lights, scenes, views and instances. The rest —
// skeletons, effects, probes, decals, GI volumes, lightmaps, occluders, cameras, canvases,
// environments, post-process settings — are handle types with no pool, and creating one fails with
// `NotImplemented` naming the milestone that adds it. That is honest in a way an empty pool is not:
// a caller gets an error rather than a handle to nothing.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/handle_pool.h>
#include <cy/core/memory/hash_map.h>
#include <cy/servers/render/debug_draw.h>
#include <cy/servers/render/gpu_scene.h>
#include <cy/servers/render/handles.h>
#include <cy/servers/render/mesh.h>
#include <cy/servers/render/model.h>
#include <cy/servers/render/snapshot.h>
#include <cy/servers/render/sort.h>
#include <cy/servers/render/statistics.h>

namespace cy::render {

/// A texture as the server holds it. The bytes live on the device and are the concern of the module
/// that owns one; what is here is the description and the residency accounting.
struct TextureRecord {
    Name name;
    TextureFormat format = TextureFormat::Undefined;
    TextureUsageClass usage_class = TextureUsageClass::Color;
    u32 width = 0;
    u32 height = 0;
    u16 mip_levels = 1;
    u16 array_layers = 1;
    /// What the full mip chain costs, for the memory report. Computed from the description rather
    /// than asked of a device, so the number exists before anything is uploaded.
    u64 bytes = 0;
};

struct SamplerRecord {
    Name name;
    bool linear_filter = true;
    bool anisotropic = true;
    /// Reversed-Z again: a shadow comparison sampler compares `Greater`. Recorded rather than
    /// derived so a validation pass can say which samplers are comparison samplers.
    bool comparison = false;
};

/// A material, as far as the render server is concerned.
///
/// The full material model — parameters, texture slots, the standard material, validation — is
/// `cy::rendering::material` at layer 4. What is here is what SORTING and DRAWING need, and nothing
/// else: the program to bind, the parameter block to index, and the three properties that decide
/// which bucket a draw falls in.
struct MaterialRecord {
    Name name;
    ShaderHandle shader;
    /// The compiled program's identity — the most significant state a sort key groups by.
    u32 program = 0;
    /// Index into the GPU material table, carried into `GpuInstance::material` and used by the
    /// shader to reach the parameters with no per-object descriptor binding.
    ///
    /// ASSIGNED BY `create_material()`, not by the caller: it is a dense index into a shared table
    /// and two owners of the allocation would eventually collide. The material module (layer 4)
    /// reads it back and places its parameter block there.
    u32 table_index = 0;
    BlendMode blend = BlendMode::Opaque;
    ShadingModel model = ShadingModel::Lit;
    bool two_sided = false;
    bool casts_shadow = true;
};

/// A scene: its GPU instance store, its lights and its environment.
struct Scene {
    SceneDescription desc;
    GpuScene gpu;
    /// The producer the server itself publishes extracted instances under. Every other producer
    /// registers its own.
    ProducerId extract_producer = kInvalidProducer;
    Array<LightHandle> lights;
    /// stable id -> instance handle bits, for this scene. A LOOKUP ONLY: the map is never iterated,
    /// because a hash map's iteration order is exactly the kind of thing design.md §6 forbids
    /// anything ordered from depending on.
    HashMap<u64, u64> by_stable_id;

    Scene(Allocator& allocator, const SceneDescription& description) noexcept
        : desc(description), gpu(allocator), lights(allocator), by_stable_id(allocator) {}
};

/// What one instance costs the server beyond its GPU record: the slot it reserved and the handle
/// bookkeeping.
struct InstanceRecord {
    Instance instance;
    SceneHandle scene;
    InstanceRange range;
};

/// The sort-relevant properties of a mesh, indexed by mesh slot. See `RenderServer::mesh_info_`.
struct MeshDrawInfo {
    u32 surface_count = 0;
    u64 triangles = 0;
    bool live = false;
};

/// The sort-relevant properties of a material, indexed by GPU material table index.
struct MaterialDrawInfo {
    u32 program = 0;
    BlendMode blend = BlendMode::Opaque;
    bool live = false;
};

/// What a host sizes a server with, before `initialize()`.
///
/// WHY THIS IS A STRUCT AND NOT FOUR ARGUMENTS TO `initialize()`. `initialize()` has to keep the
/// signature `runtime::Server` declares — the adapter forwards to it and adds nothing (see the
/// header comment) — so a capacity cannot arrive through it. It arrives before, through
/// `configure()`, and the default is what a host that never calls `configure()` gets.
///
/// THE DEFAULT IS SIZED FOR A GAME AND IS TOO BIG FOR A TEST, WHICH IS THE REASON THIS EXISTS.
/// The debug primitive store is two buffers of fixed-capacity records reserved once (debug_draw.h:
/// "no allocation per primitive"), and at the default that is around 850 KiB of storage that
/// `initialize()` constructs. A unit case that builds a server per test case was paying it per
/// case, which is milliseconds at `-O0` and is the whole of a `unit` budget — a cost that measures
/// the default rather than the behaviour under test. A suite sets a capacity that fits what it
/// submits, and the number stops being a hidden constant of the server.
struct RenderServerConfig {
    /// Debug primitives one frame may hold, per buffer. Beyond it a submission is dropped and
    /// counted — see `DebugDrawList::dropped()`.
    u32 debug_primitive_capacity = 4096;
    u32 debug_label_capacity = 256;
};

/// Dependency invalidation. `rendering-architecture`: "WHEN a material's shader is destroyed THEN
/// dependent cached pipelines and descriptor sets SHALL be invalidated through a
/// dependency-tracking mechanism, not left dangling."
///
/// A dependency is recorded as a pair of handle bit patterns, so one table covers every family
/// without a template per pair. The renderer registers an observer and is told what died and what
/// depended on it; the server itself caches no pipeline and has none to invalidate, which is
/// exactly why the mechanism is a callback rather than a hard-coded sweep.
using InvalidationFn = void (*)(u64 destroyed_bits, u64 dependent_bits, void* user) noexcept;

/// The render server.
///
/// NOT THREAD-SAFE, and deliberately: it is mutated from the frame's prepare stage and read from
/// the stages after it, which is one thread. The one interface that is safe from workers is
/// `debug_draw()`, and it says so.
class RenderServer {
public:
    explicit RenderServer(Allocator& allocator) noexcept;
    ~RenderServer();

    RenderServer(const RenderServer&) = delete;
    RenderServer& operator=(const RenderServer&) = delete;

    // --- The four methods `runtime::Server` declares -----------------------------------------
    //
    // Same names, same signatures, same meanings. See the header comment: the runtime's adapter
    // forwards to these and adds nothing.

    [[nodiscard]] const char* backend_name() const noexcept { return backend_name_; }
    [[nodiscard]] Status initialize() noexcept;
    void shutdown() noexcept;
    [[nodiscard]] bool is_null_backend() const noexcept { return null_backend_; }

    /// Name the backend that is driving this server, for the diagnostic that says "asked for
    /// vulkan, ran null". Set by whoever wired the device before `initialize()`.
    void set_backend(const char* name, bool is_null) noexcept;

    /// Size the server. Refused once `initialize()` has run: the debug store is reserved there and
    /// a capacity that changed underneath it would either strand the reservation or reallocate a
    /// buffer a worker thread may be writing into.
    [[nodiscard]] Status configure(const RenderServerConfig& config) noexcept;
    [[nodiscard]] const RenderServerConfig& configuration() const noexcept { return config_; }

    // --- Resources ---------------------------------------------------------------------------

    [[nodiscard]] Expected<TextureHandle, Error> create_texture(
        const TextureRecord& record) noexcept;
    void destroy_texture(TextureHandle handle) noexcept;
    [[nodiscard]] const TextureRecord* texture(TextureHandle handle) const noexcept;

    [[nodiscard]] Expected<SamplerHandle, Error> create_sampler(
        const SamplerRecord& record) noexcept;
    void destroy_sampler(SamplerHandle handle) noexcept;
    [[nodiscard]] const SamplerRecord* sampler(SamplerHandle handle) const noexcept;

    /// Create a mesh. `surfaces` and `lods` are copied — the server owns its model, and a caller
    /// that kept the arrays would be a second owner of the same description.
    [[nodiscard]] Expected<MeshHandle, Error> create_mesh(const MeshDescription& desc,
                                                          Span<const MeshSurface> surfaces,
                                                          Span<const MeshLod> lods) noexcept;
    void destroy_mesh(MeshHandle handle) noexcept;
    [[nodiscard]] const Mesh* mesh(MeshHandle handle) const noexcept;

    [[nodiscard]] Expected<MaterialHandle, Error> create_material(
        const MaterialRecord& record) noexcept;
    void destroy_material(MaterialHandle handle) noexcept;
    [[nodiscard]] const MaterialRecord* material(MaterialHandle handle) const noexcept;

    [[nodiscard]] Expected<LightHandle, Error> create_light(const LightDescription& desc) noexcept;
    void destroy_light(LightHandle handle) noexcept;
    [[nodiscard]] const LightDescription* light(LightHandle handle) const noexcept;

    // --- Scenes, views and instances ---------------------------------------------------------

    [[nodiscard]] Expected<SceneHandle, Error> create_scene(const SceneDescription& desc) noexcept;
    void destroy_scene(SceneHandle handle) noexcept;
    [[nodiscard]] Scene* scene(SceneHandle handle) noexcept;
    [[nodiscard]] const Scene* scene(SceneHandle handle) const noexcept;

    [[nodiscard]] Expected<ViewHandle, Error> create_view(const ViewDescription& desc) noexcept;
    void destroy_view(ViewHandle handle) noexcept;
    [[nodiscard]] View* view(ViewHandle handle) noexcept;
    [[nodiscard]] const View* view(ViewHandle handle) const noexcept;
    /// Every live view, in creation order. The frame renders all of them: there is no main view.
    [[nodiscard]] Span<const ViewHandle> views() const noexcept { return view_order_.span(); }

    /// Place a renderable into a scene. Reserves one GPU scene slot and writes its record.
    ///
    /// Fails when `desc.stable_id` is zero: see `InstanceDescription::stable_id`. An instance with
    /// no stable identity is an instance whose draw order is publication order, and refusing it at
    /// creation is the only place the mistake is cheap.
    [[nodiscard]] Expected<InstanceHandle, Error> create_instance(
        SceneHandle scene, const InstanceDescription& desc) noexcept;
    void destroy_instance(InstanceHandle handle) noexcept;
    [[nodiscard]] const InstanceRecord* instance(InstanceHandle handle) const noexcept;
    /// The instance published under a stable id, or a null handle. This is how a snapshot's
    /// incremental update finds what it is updating without carrying handles across the boundary.
    [[nodiscard]] InstanceHandle find_instance(SceneHandle scene, u64 stable_id) const noexcept;

    /// Move an instance and republish its record. The previous transform is kept, so motion vectors
    /// and previous bounds are a frame apart rather than zero.
    [[nodiscard]] Status set_instance_transform(InstanceHandle handle,
                                                const Transform& transform) noexcept;
    [[nodiscard]] Status set_instance_visible(InstanceHandle handle, bool visible) noexcept;
    [[nodiscard]] Status set_instance_material(InstanceHandle handle,
                                               MaterialHandle material) noexcept;

    // --- The snapshot ------------------------------------------------------------------------

    /// Apply a published snapshot to a scene. Task 4.1.2's consuming half.
    ///
    /// Creates instances it has not seen, updates the ones it has, destroys the ones the snapshot
    /// removed, and blends the two placements with `alpha`. Incremental in, incremental out: the
    /// work is proportional to `snapshot.changed.size()` and not to the scene.
    [[nodiscard]] Status apply_snapshot(SceneHandle scene, const RenderSnapshot& snapshot,
                                        f32 alpha) noexcept;

    // --- Drawing -----------------------------------------------------------------------------

    /// Collect the draws a view produces, sorted.
    ///
    /// A LINEAR SCAN OVER THE GPU SCENE, which is the shape `virtual-geometry` requires of every
    /// culler — "Instance culling SHALL read the GPU scene, so virtual geometry does not traverse
    /// ECS entities or maintain its own instance list". The spatial index and the GPU-driven
    /// compaction that replace the scan are `rendering-culling-and-lod`'s (task 4.4.3) and M6's;
    /// what they replace is the *scan*, not the interface, and this remains the correct fallback on
    /// a device without them.
    ///
    /// The result is sorted by `sort_draws()`, so it is the same list for the same scene state
    /// whatever order the instances were published in.
    [[nodiscard]] Status collect_draws(SceneHandle scene, const View& view, Array<DrawItem>& out,
                                       ViewStatistics& stats) const noexcept;

    // --- Dependencies ------------------------------------------------------------------------

    /// Record that `dependent` stops being valid when `producer` is destroyed.
    [[nodiscard]] Status add_dependency(u64 producer_bits, u64 dependent_bits) noexcept;
    void set_invalidation_observer(InvalidationFn observer, void* user) noexcept;
    /// Notify and forget every dependent of `producer`. Called by each `destroy_*`; public because
    /// a module above that caches pipelines keyed on a handle the server does not own calls it too.
    void invalidate_dependents(u64 producer_bits) noexcept;

    // --- Reporting ---------------------------------------------------------------------------

    [[nodiscard]] DebugDrawList& debug_draw() noexcept { return debug_draw_; }
    [[nodiscard]] const DebugDrawList& debug_draw() const noexcept { return debug_draw_; }

    [[nodiscard]] FrameStatistics& frame_statistics() noexcept { return frame_stats_; }
    [[nodiscard]] const FrameStatistics& frame_statistics() const noexcept { return frame_stats_; }

    /// Recompute the memory and occupancy report. Called once a frame, naming the scene whose GPU
    /// store the occupancy comes from; a null handle reports resource memory and no occupancy.
    ///
    /// Separate from the counters so a report is a snapshot rather than a running total that
    /// drifts, and so a caller can choose not to pay for it.
    void refresh_statistics(SceneHandle scene) noexcept;

    [[nodiscard]] u32 live_instances() const noexcept { return instances_.size(); }
    [[nodiscard]] u32 live_scenes() const noexcept { return scenes_.size(); }
    [[nodiscard]] u32 live_views() const noexcept { return views_.size(); }

private:
    struct Dependency {
        u64 producer = 0;
        u64 dependent = 0;
    };

    /// The sort key one visible record produces, from the material's bucket and the view depth.
    /// Separate from the cull loop so the loop is a cull and the key is a key — and so a pipeline
    /// that wants a key without walking a scene has one to call.
    [[nodiscard]] u64 sort_key_for(const GpuInstance& record, f32 view_depth) const noexcept;

    /// One draw item per surface of the record's mesh, all carrying the same key. Instancing merges
    /// them afterwards (`merge_instanced_draws`); emitting them separately is what lets it.
    [[nodiscard]] Status emit_draws(const GpuInstance& record, u32 slot, u64 key,
                                    Array<DrawItem>& out) const noexcept;

    /// Write an instance's record into its scene's GPU store and mark it dirty. One function so
    /// that every path that changes an instance publishes the same fields.
    [[nodiscard]] Status publish_instance(Scene& scene, InstanceRecord& record) noexcept;

    Allocator* allocator_;
    RenderServerConfig config_;
    const char* backend_name_ = "null";
    bool null_backend_ = true;
    bool initialized_ = false;

    HandlePool<TextureRecord, RenderTextureTag> textures_;
    HandlePool<SamplerRecord, RenderSamplerTag> samplers_;
    HandlePool<Mesh, RenderMeshTag> meshes_;
    HandlePool<MaterialRecord, RenderMaterialTag> materials_;
    HandlePool<LightDescription, RenderLightTag> lights_;
    HandlePool<Scene, RenderSceneTag> scenes_;
    HandlePool<View, RenderViewTag> views_;
    HandlePool<InstanceRecord, RenderInstanceTag> instances_;

    /// Creation order, so `views()` is stable and does not depend on a pool's free list.
    Array<ViewHandle> view_order_;

    // --- The two dense side tables, and why they exist ---------------------------------------
    //
    // A `GpuInstance` carries SLOT INDICES, not handles: a compute shader has no generation to
    // check, and requirement 2 in gpu_scene.h is that a consumer can read a record without looking
    // anything up. So the CPU path — which does need the program to sort by and the surface count
    // to emit draws for — bridges back with two dense arrays indexed by those same numbers. The
    // alternative is a hash lookup per instance per view, which would be the frame's hottest map
    // and would put a hash map on the path design.md §6 is about.
    //
    // Both are maintained by `create_*`/`destroy_*` and are the reason those functions are the only
    // way to register a mesh or a material.

    /// Indexed by the mesh handle's slot index (`GpuInstance::mesh`).
    Array<MeshDrawInfo> mesh_info_;
    /// Indexed by the GPU material table index (`GpuInstance::material`), which the server assigns.
    Array<MaterialDrawInfo> material_info_;
    /// Table indices returned by `destroy_material`, reused before the table grows.
    Array<u32> free_material_slots_;

    Array<Dependency> dependencies_;
    InvalidationFn invalidation_ = nullptr;
    void* invalidation_user_ = nullptr;

    /// Running totals for the memory report. Maintained by `create_*`/`destroy_*` rather than
    /// summed on demand, because a handle pool has no iteration and adding one so a report could
    /// walk it would put a second traversal order into the server.
    u64 texture_bytes_ = 0;
    u64 mesh_bytes_ = 0;

    DebugDrawList debug_draw_;
    FrameStatistics frame_stats_;
};

}  // namespace cy::render
