// The render server: handle pools, the scene/view/instance model, snapshot application and the
// sorted draw list. See cy/servers/render/server.h.

#include <cy/servers/render/server.h>

#include <cy/core/base/assert.h>
#include <cy/core/math/projection.h>

namespace cy::render {
namespace {

constexpr u32 kPoolChunk = 256;

/// Handle pools in a render server are touched from one thread (see the class comment), so the
/// thread-safe flag is off: it buys a mutex on every create and destroy to serve nobody.
constexpr bool kPoolThreadSafe = false;

/// Grow a dense side table so `index` is addressable, filling the gap with dead entries.
template <class T>
[[nodiscard]] Status ensure_slot(Array<T>& table, u32 index) noexcept {
    if (index < table.size()) {
        return ok();
    }
    return table.resize(static_cast<usize>(index) + 1U);
}

}  // namespace

// --- Construction ---------------------------------------------------------------------------

RenderServer::RenderServer(Allocator& allocator) noexcept
    : allocator_(&allocator),
      textures_(MemoryDomain::Renderer, "render.textures", kPoolChunk, kPoolThreadSafe),
      samplers_(MemoryDomain::Renderer, "render.samplers", kPoolChunk, kPoolThreadSafe),
      meshes_(MemoryDomain::Renderer, "render.meshes", kPoolChunk, kPoolThreadSafe),
      materials_(MemoryDomain::Renderer, "render.materials", kPoolChunk, kPoolThreadSafe),
      lights_(MemoryDomain::Renderer, "render.lights", kPoolChunk, kPoolThreadSafe),
      scenes_(MemoryDomain::Renderer, "render.scenes", 8, kPoolThreadSafe),
      views_(MemoryDomain::Renderer, "render.views", 32, kPoolThreadSafe),
      instances_(MemoryDomain::Renderer, "render.instances", 1024, kPoolThreadSafe),
      view_order_(allocator),
      mesh_info_(allocator),
      material_info_(allocator),
      free_material_slots_(allocator),
      dependencies_(allocator),
      debug_draw_(allocator) {
    textures_.set_allocator(allocator);
    samplers_.set_allocator(allocator);
    meshes_.set_allocator(allocator);
    materials_.set_allocator(allocator);
    lights_.set_allocator(allocator);
    scenes_.set_allocator(allocator);
    views_.set_allocator(allocator);
    instances_.set_allocator(allocator);
}

RenderServer::~RenderServer() {
    shutdown();
}

void RenderServer::set_backend(const char* name, bool is_null) noexcept {
    backend_name_ = (name != nullptr) ? name : "null";
    null_backend_ = is_null;
}

Status RenderServer::configure(const RenderServerConfig& config) noexcept {
    if (initialized_) {
        return fail(ErrorCode::PermissionDenied,
                    "the render server is already initialized; the debug store is reserved at "
                    "initialize() and resizing it under a submitting thread is not something a "
                    "caller can be told to be careful about");
    }
    config_ = config;
    return ok();
}

Status RenderServer::initialize() noexcept {
    if (initialized_) {
        return ok();
    }
    if (Status started =
            debug_draw_.initialize(config_.debug_primitive_capacity, config_.debug_label_capacity);
        !started) {
        return started;
    }
    frame_stats_.reset();
    initialized_ = true;
    return ok();
}

void RenderServer::shutdown() noexcept {
    if (!initialized_) {
        return;
    }
    view_order_.clear();
    mesh_info_.clear();
    material_info_.clear();
    free_material_slots_.clear();
    dependencies_.clear();
    initialized_ = false;
}

// --- Resources ------------------------------------------------------------------------------

Expected<TextureHandle, Error> RenderServer::create_texture(const TextureRecord& record) noexcept {
    TextureRecord stored = record;
    if (stored.mip_levels == 0) {
        stored.mip_levels = static_cast<u16>(full_mip_count(stored.width, stored.height));
    }
    // The size is computed from the description rather than asked of a device: the number has to
    // exist before anything is uploaded, and this server has no device to ask.
    stored.bytes =
        texture_mip_chain_byte_size(stored.format, stored.width, stored.height, stored.mip_levels) *
        (stored.array_layers != 0 ? stored.array_layers : 1U);
    Expected<TextureHandle, Error> handle = textures_.create(stored);
    if (handle) {
        texture_bytes_ += stored.bytes;
    }
    return handle;
}

void RenderServer::destroy_texture(TextureHandle handle) noexcept {
    if (const TextureRecord* record = textures_.resolve(handle); record != nullptr) {
        texture_bytes_ -= record->bytes;
    }
    invalidate_dependents(handle.bits());
    (void)textures_.destroy(handle);
}

const TextureRecord* RenderServer::texture(TextureHandle handle) const noexcept {
    return textures_.resolve(handle);
}

Expected<SamplerHandle, Error> RenderServer::create_sampler(const SamplerRecord& record) noexcept {
    return samplers_.create(record);
}

void RenderServer::destroy_sampler(SamplerHandle handle) noexcept {
    invalidate_dependents(handle.bits());
    (void)samplers_.destroy(handle);
}

const SamplerRecord* RenderServer::sampler(SamplerHandle handle) const noexcept {
    return samplers_.resolve(handle);
}

Expected<MeshHandle, Error> RenderServer::create_mesh(const MeshDescription& desc,
                                                      Span<const MeshSurface> surfaces,
                                                      Span<const MeshLod> lods) noexcept {
    if (surfaces.empty()) {
        return fail(ErrorCode::InvalidArgument, "a mesh with no surfaces draws nothing");
    }
    Expected<MeshHandle, Error> handle = meshes_.create(*allocator_);
    if (!handle) {
        return handle;
    }
    Mesh* stored = meshes_.resolve(*handle);
    if (stored == nullptr) {
        // Unreachable: the pool just returned this handle. Checked rather than asserted because the
        // compiler cannot see that and -Wnull-dereference is an error here, and a return is a
        // cheaper answer than an assertion that is compiled out of two of the four profiles.
        return fail(ErrorCode::Internal, "the mesh pool returned a handle it cannot resolve");
    }
    stored->desc = desc;
    // The index width follows from the vertex count and is never taken from the caller: the rule is
    // in one place (`index_width_for`) and an importer that got it wrong would produce a mesh that
    // draws garbage past vertex 65536.
    stored->desc.index_width = index_width_for(desc.vertex_count);
    if (Status copied = stored->surfaces.append(surfaces); !copied) {
        (void)meshes_.destroy(*handle);
        return make_unexpected(copied.error());
    }
    if (Status copied = stored->lods.append(lods); !copied) {
        (void)meshes_.destroy(*handle);
        return make_unexpected(copied.error());
    }

    MeshDrawInfo info;
    info.surface_count = static_cast<u32>(surfaces.size());
    info.live = true;
    for (const MeshSurface& surface : surfaces) {
        info.triangles += surface.index_count / 3U;
    }
    if (Status sized = ensure_slot(mesh_info_, handle->index()); !sized) {
        (void)meshes_.destroy(*handle);
        return make_unexpected(sized.error());
    }
    mesh_info_[handle->index()] = info;
    mesh_bytes_ += mesh_byte_size(stored->desc);
    return handle;
}

void RenderServer::destroy_mesh(MeshHandle handle) noexcept {
    if (const Mesh* stored = meshes_.resolve(handle); stored != nullptr) {
        mesh_bytes_ -= mesh_byte_size(stored->desc);
    }
    if (handle.index() < mesh_info_.size()) {
        mesh_info_[handle.index()] = MeshDrawInfo{};
    }
    invalidate_dependents(handle.bits());
    (void)meshes_.destroy(handle);
}

const Mesh* RenderServer::mesh(MeshHandle handle) const noexcept {
    return meshes_.resolve(handle);
}

Expected<MaterialHandle, Error> RenderServer::create_material(
    const MaterialRecord& record) noexcept {
    // The table index is the server's to assign — see the field's comment. Reusing a freed index
    // before growing keeps the table dense, which is what makes the side lookup an array.
    u32 table_index = 0;
    if (!free_material_slots_.empty()) {
        table_index = free_material_slots_.back();
        free_material_slots_.pop_back();
    } else {
        table_index = static_cast<u32>(material_info_.size());
        if (Status sized = ensure_slot(material_info_, table_index); !sized) {
            return make_unexpected(sized.error());
        }
    }

    MaterialRecord stored = record;
    stored.table_index = table_index;
    Expected<MaterialHandle, Error> handle = materials_.create(stored);
    if (!handle) {
        (void)free_material_slots_.push_back(table_index);
        return handle;
    }
    material_info_[table_index] = MaterialDrawInfo{stored.program, stored.blend, true};

    if (stored.shader) {
        // "WHEN a material's shader is destroyed THEN dependent cached pipelines and descriptor
        // sets SHALL be invalidated" — recorded here, at the moment the dependency is created,
        // because that is the only moment anybody knows about it.
        if (Status recorded = add_dependency(stored.shader.bits(), handle->bits()); !recorded) {
            (void)materials_.destroy(*handle);
            (void)free_material_slots_.push_back(table_index);
            return make_unexpected(recorded.error());
        }
    }
    return handle;
}

void RenderServer::destroy_material(MaterialHandle handle) noexcept {
    const MaterialRecord* record = materials_.resolve(handle);
    if (record != nullptr && record->table_index < material_info_.size()) {
        material_info_[record->table_index] = MaterialDrawInfo{};
        (void)free_material_slots_.push_back(record->table_index);
    }
    invalidate_dependents(handle.bits());
    (void)materials_.destroy(handle);
}

const MaterialRecord* RenderServer::material(MaterialHandle handle) const noexcept {
    return materials_.resolve(handle);
}

Expected<LightHandle, Error> RenderServer::create_light(const LightDescription& desc) noexcept {
    if (desc.stable_id == 0) {
        return fail(
            ErrorCode::InvalidArgument,
            "a light needs a stable identity: a shadow atlas page and a deterministic light "
            "order both key off it, and zero would make both depend on creation order");
    }
    return lights_.create(desc);
}

void RenderServer::destroy_light(LightHandle handle) noexcept {
    invalidate_dependents(handle.bits());
    (void)lights_.destroy(handle);
}

const LightDescription* RenderServer::light(LightHandle handle) const noexcept {
    return lights_.resolve(handle);
}

// --- Scenes and views -----------------------------------------------------------------------

Expected<SceneHandle, Error> RenderServer::create_scene(const SceneDescription& desc) noexcept {
    Expected<SceneHandle, Error> handle = scenes_.create(*allocator_, desc);
    if (!handle) {
        return handle;
    }
    Scene* stored = scenes_.resolve(*handle);
    if (Status started = stored->gpu.initialize(desc.instance_capacity); !started) {
        (void)scenes_.destroy(*handle);
        return make_unexpected(started.error());
    }
    Expected<ProducerId, Error> producer =
        stored->gpu.register_producer("extract", ProducerKind::Extract, Residency::Cpu);
    if (!producer) {
        (void)scenes_.destroy(*handle);
        return make_unexpected(producer.error());
    }
    stored->extract_producer = *producer;
    return handle;
}

void RenderServer::destroy_scene(SceneHandle handle) noexcept {
    invalidate_dependents(handle.bits());
    (void)scenes_.destroy(handle);
}

Scene* RenderServer::scene(SceneHandle handle) noexcept {
    return scenes_.resolve(handle);
}

const Scene* RenderServer::scene(SceneHandle handle) const noexcept {
    return scenes_.resolve(handle);
}

Expected<ViewHandle, Error> RenderServer::create_view(const ViewDescription& desc) noexcept {
    View built;
    built.desc = desc;
    built.refresh();
    Expected<ViewHandle, Error> handle = views_.create(built);
    if (!handle) {
        return handle;
    }
    if (Status pushed = view_order_.push_back(*handle); !pushed) {
        (void)views_.destroy(*handle);
        return make_unexpected(pushed.error());
    }
    return handle;
}

void RenderServer::destroy_view(ViewHandle handle) noexcept {
    for (usize index = 0; index < view_order_.size(); ++index) {
        if (view_order_[index] == handle) {
            // erase() rather than remove_unordered(): `views()` is creation order, and a swap would
            // make the render order of the remaining views depend on which one was destroyed.
            view_order_.erase(index);
            break;
        }
    }
    invalidate_dependents(handle.bits());
    (void)views_.destroy(handle);
}

View* RenderServer::view(ViewHandle handle) noexcept {
    return views_.resolve(handle);
}

const View* RenderServer::view(ViewHandle handle) const noexcept {
    return views_.resolve(handle);
}

// --- Instances ------------------------------------------------------------------------------

Status RenderServer::publish_instance(Scene& scene, InstanceRecord& record) noexcept {
    Expected<Span<GpuInstance>, Error> slots =
        scene.gpu.writable(scene.extract_producer, record.range);
    if (!slots) {
        return Status{make_unexpected(slots.error())};
    }
    GpuInstance& gpu = (*slots)[0];

    const InstanceDescription& desc = record.instance.desc;
    if (record.instance.has_previous) {
        write_previous_transform(gpu, record.instance.previous_transform);
    } else {
        write_previous_transform(gpu, desc.transform);
    }
    write_transform(gpu, desc.transform);
    write_bounds(gpu, world_bounds_of(desc.transform, desc.local_bounds));

    gpu.mesh = desc.mesh.index();
    const MaterialRecord* material_record = materials_.resolve(desc.material);
    gpu.material = (material_record != nullptr) ? material_record->table_index : 0U;
    gpu.lod_chain = desc.mesh.index();
    gpu.layer_mask = desc.visible ? desc.layer_mask : kNoLayers;
    gpu.importance = desc.importance;
    gpu.set_stable_id(desc.stable_id);

    u32 flags = kInstanceActive;
    flags |= desc.visible ? kInstanceVisible : 0U;
    flags |= desc.casts_shadow ? kInstanceCastsShadow : 0U;
    flags |= desc.receives_shadow ? kInstanceReceivesShadow : 0U;
    flags |= desc.two_sided ? kInstanceTwoSided : 0U;
    if (record.instance.has_previous &&
        !nearly_equal(record.instance.previous_transform, desc.transform)) {
        flags |= kInstanceMoved;
    }
    gpu.flags = flags;

    return scene.gpu.mark_dirty(record.range);
}

Expected<InstanceHandle, Error> RenderServer::create_instance(
    SceneHandle scene_handle, const InstanceDescription& desc) noexcept {
    Scene* target = scenes_.resolve(scene_handle);
    if (target == nullptr) {
        return fail(ErrorCode::NotFound, "no such scene");
    }
    if (desc.stable_id == 0) {
        return fail(
            ErrorCode::InvalidArgument,
            "an instance needs a stable identity: without one its draw order is publication "
            "order, which is what deterministic submission forbids (design.md section 6)");
    }
    if (target->by_stable_id.contains(desc.stable_id)) {
        return fail(
            ErrorCode::AlreadyExists,
            "an instance with that stable identity is already in this scene; two would make "
            "their relative draw order depend on which was published first");
    }

    Expected<InstanceRange, Error> range = target->gpu.reserve(target->extract_producer, 1);
    if (!range) {
        return make_unexpected(range.error());
    }

    InstanceRecord record;
    record.instance.desc = desc;
    record.instance.scene = scene_handle;
    record.instance.gpu_slot = range->first;
    record.scene = scene_handle;
    record.range = *range;

    Expected<InstanceHandle, Error> handle = instances_.create(record);
    if (!handle) {
        (void)target->gpu.release(target->extract_producer, *range);
        return handle;
    }
    InstanceRecord* stored = instances_.resolve(*handle);
    if (stored == nullptr) {
        (void)target->gpu.release(target->extract_producer, *range);
        return fail(ErrorCode::Internal, "the instance pool returned a handle it cannot resolve");
    }
    if (Status published = publish_instance(*target, *stored); !published) {
        (void)instances_.destroy(*handle);
        (void)target->gpu.release(target->extract_producer, *range);
        return make_unexpected(published.error());
    }
    if (!target->by_stable_id.insert(desc.stable_id, handle->bits())) {
        (void)instances_.destroy(*handle);
        (void)target->gpu.release(target->extract_producer, *range);
        return fail(ErrorCode::OutOfMemory, "could not record the instance's stable identity");
    }
    return handle;
}

void RenderServer::destroy_instance(InstanceHandle handle) noexcept {
    InstanceRecord* record = instances_.resolve(handle);
    if (record == nullptr) {
        return;
    }
    Scene* target = scenes_.resolve(record->scene);
    if (target != nullptr) {
        (void)target->by_stable_id.remove(record->instance.desc.stable_id);
        // Releasing clears the records and marks them dirty, which is the whole of "removed without
        // requiring a full rebuild": the GPU sees an inactive record on the next upload.
        (void)target->gpu.release(target->extract_producer, record->range);
    }
    (void)instances_.destroy(handle);
}

const InstanceRecord* RenderServer::instance(InstanceHandle handle) const noexcept {
    return instances_.resolve(handle);
}

InstanceHandle RenderServer::find_instance(SceneHandle scene_handle, u64 stable_id) const noexcept {
    const Scene* target = scenes_.resolve(scene_handle);
    if (target == nullptr) {
        return {};
    }
    const u64* bits = target->by_stable_id.find(stable_id);
    return (bits != nullptr) ? InstanceHandle::from_bits(*bits) : InstanceHandle{};
}

Status RenderServer::set_instance_transform(InstanceHandle handle,
                                            const Transform& transform) noexcept {
    InstanceRecord* record = instances_.resolve(handle);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "no such instance");
    }
    Scene* target = scenes_.resolve(record->scene);
    if (target == nullptr) {
        return fail(ErrorCode::NotFound, "the instance's scene is gone");
    }
    record->instance.previous_transform = record->instance.desc.transform;
    record->instance.has_previous = true;
    record->instance.desc.transform = transform;
    return publish_instance(*target, *record);
}

Status RenderServer::set_instance_visible(InstanceHandle handle, bool visible) noexcept {
    InstanceRecord* record = instances_.resolve(handle);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "no such instance");
    }
    Scene* target = scenes_.resolve(record->scene);
    if (target == nullptr) {
        return fail(ErrorCode::NotFound, "the instance's scene is gone");
    }
    record->instance.desc.visible = visible;
    return publish_instance(*target, *record);
}

Status RenderServer::set_instance_material(InstanceHandle handle,
                                           MaterialHandle material_handle) noexcept {
    InstanceRecord* record = instances_.resolve(handle);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "no such instance");
    }
    if (materials_.resolve(material_handle) == nullptr) {
        return fail(ErrorCode::NotFound, "no such material");
    }
    Scene* target = scenes_.resolve(record->scene);
    if (target == nullptr) {
        return fail(ErrorCode::NotFound, "the instance's scene is gone");
    }
    record->instance.desc.material = material_handle;
    return publish_instance(*target, *record);
}

// --- The snapshot ---------------------------------------------------------------------------

Status RenderServer::apply_snapshot(SceneHandle scene_handle, const RenderSnapshot& snapshot,
                                    f32 alpha) noexcept {
    Scene* target = scenes_.resolve(scene_handle);
    if (target == nullptr) {
        return fail(ErrorCode::NotFound, "no such scene");
    }

    for (u64 stable_id : snapshot.removed) {
        const InstanceHandle handle = find_instance(scene_handle, stable_id);
        if (handle) {
            destroy_instance(handle);
        }
    }

    // The boolean set the snapshot carries as `InstanceFlagBits`, applied the same way on the
    // create and the update path — one lambda so the two cannot drift, which is exactly how an
    // instance ends up visible after an update and invisible after a create.
    const auto apply_flags = [](InstanceDescription& desc, u32 flags) noexcept {
        desc.visible = (flags & kInstanceVisible) != 0U;
        desc.casts_shadow = (flags & kInstanceCastsShadow) != 0U;
        desc.receives_shadow = (flags & kInstanceReceivesShadow) != 0U;
        desc.two_sided = (flags & kInstanceTwoSided) != 0U;
    };

    for (const InstanceSnapshot& change : snapshot.changed) {
        // The blend happens here rather than at extraction: the alpha is not known at the commit
        // boundary, and one snapshot is commonly rendered at one alpha by several views. See
        // snapshot.h.
        const Transform placement = resolve_transform(change.previous_transform, change.transform,
                                                      alpha, change.teleported);
        const InstanceHandle existing = find_instance(scene_handle, change.stable_id);
        InstanceRecord* record = existing ? instances_.resolve(existing) : nullptr;
        if (record != nullptr) {
            record->instance.previous_transform = record->instance.desc.transform;
            record->instance.has_previous = !change.teleported;
            record->instance.desc.transform = placement;
            record->instance.desc.mesh = change.mesh;
            record->instance.desc.material = change.material;
            record->instance.desc.local_bounds = change.local_bounds;
            record->instance.desc.layer_mask = change.layer_mask;
            record->instance.desc.importance = change.importance;
            record->instance.desc.lod_bias = change.lod_bias;
            apply_flags(record->instance.desc, change.flags);
            if (Status published = publish_instance(*target, *record); !published) {
                return published;
            }
            continue;
        }
        InstanceDescription desc;
        desc.mesh = change.mesh;
        desc.material = change.material;
        desc.transform = placement;
        desc.local_bounds = change.local_bounds;
        desc.layer_mask = change.layer_mask;
        desc.importance = change.importance;
        desc.lod_bias = change.lod_bias;
        apply_flags(desc, change.flags);
        desc.stable_id = change.stable_id;
        Expected<InstanceHandle, Error> created = create_instance(scene_handle, desc);
        if (!created) {
            return Status{make_unexpected(created.error())};
        }
    }

    target->desc.environment = snapshot.environment;
    return ok();
}

// --- Drawing --------------------------------------------------------------------------------

namespace {

/// The two tests that need nothing but the record and the view's mask: is this slot live and
/// visible, and is it on a layer this view draws.
///
/// A free function because it is exactly the test a GPU cull will run in a compute shader — the CPU
/// path and the device path have to agree, and the way to keep them agreeing is for the CPU's to be
/// one readable expression rather than three conditions inside a loop.
[[nodiscard]] bool instance_is_drawable(const GpuInstance& record, LayerMask view_mask) noexcept {
    if (!record.active() || (record.flags & kInstanceVisible) == 0U) {
        return false;
    }
    return (record.layer_mask & view_mask) != 0U;
}

[[nodiscard]] Sphere world_sphere_of(const GpuInstance& record) noexcept {
    return Sphere{Vec3{record.bounds_center[0], record.bounds_center[1], record.bounds_center[2]},
                  record.bounds_radius};
}

}  // namespace

u64 RenderServer::sort_key_for(const GpuInstance& record, f32 view_depth) const noexcept {
    const MaterialDrawInfo material_info = (record.material < material_info_.size())
                                               ? material_info_[record.material]
                                               : MaterialDrawInfo{};
    DrawKeyInputs key_inputs;
    key_inputs.layer = sort_layer_for(material_info.blend);
    key_inputs.pipeline = material_info.program;
    key_inputs.material = record.material;
    key_inputs.mesh = record.mesh;
    key_inputs.view_depth = view_depth;
    return make_sort_key(key_inputs);
}

Status RenderServer::emit_draws(const GpuInstance& record, u32 slot, u64 key,
                                Array<DrawItem>& out) const noexcept {
    const MeshDrawInfo mesh_info =
        (record.mesh < mesh_info_.size()) ? mesh_info_[record.mesh] : MeshDrawInfo{};
    // A mesh the server has no record of still draws once: an instance that vanished because its
    // mesh table entry was missing would be a rendering bug diagnosed by its absence.
    const u32 surface_count = (mesh_info.surface_count != 0U) ? mesh_info.surface_count : 1U;

    for (u32 surface = 0; surface < surface_count; ++surface) {
        DrawItem item;
        item.key = key;
        item.stable_id = record.stable_id();
        item.instance_slot = slot;
        item.surface = surface;
        if (Status pushed = out.push_back(item); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status RenderServer::collect_draws(SceneHandle scene_handle, const View& view_state,
                                   Array<DrawItem>& out, ViewStatistics& stats) const noexcept {
    const Scene* target = scenes_.resolve(scene_handle);
    if (target == nullptr) {
        return fail(ErrorCode::NotFound, "no such scene");
    }
    out.clear();
    stats = ViewStatistics{};
    stats.name = view_state.desc.name;
    stats.purpose = view_state.desc.purpose;

    const Span<const GpuInstance> records = target->gpu.instances();
    const u32 bound = target->gpu.high_water();
    const Vec3 eye = view_state.camera_relative_origin;
    const Vec3 forward = view_state.desc.camera.forward();

    for (u32 slot = 0; slot < bound; ++slot) {
        const GpuInstance& record = records[slot];
        if (!instance_is_drawable(record, view_state.desc.layer_mask)) {
            continue;
        }
        ++stats.instances_considered;

        const Sphere bounds = world_sphere_of(record);
        if (!view_state.frustum.intersects(bounds)) {
            continue;
        }
        ++stats.instances_visible;

        // Depth along the view's forward axis. Stable — it is a function of the instance's world
        // position and the camera's, both of which are inputs — and never of an iteration order.
        const f32 depth = dot(bounds.center - eye, forward);
        if (Status emitted = emit_draws(record, slot, sort_key_for(record, depth), out); !emitted) {
            return emitted;
        }
        stats.triangles +=
            (record.mesh < mesh_info_.size()) ? mesh_info_[record.mesh].triangles : 0ULL;
    }

    sort_draws(out.span());
    stats.draw_calls = static_cast<u32>(out.size());
    return ok();
}

// --- Dependencies ---------------------------------------------------------------------------
//
// `rendering-architecture`: "WHEN a material's shader is destroyed THEN dependent cached pipelines
// and descriptor sets SHALL be invalidated through a dependency-tracking mechanism, not left
// dangling."
//
// A dependency is a PAIR OF HANDLE BIT PATTERNS, so one table covers every family without a
// template per pair — and so a caller above may record a dependency on a handle this server does
// not own (a shader is the shader system's). The server caches no pipeline and has none to
// invalidate itself, which is exactly why the mechanism is a callback rather than a hard-coded
// sweep: what needs invalidating lives above.

Status RenderServer::add_dependency(u64 producer_bits, u64 dependent_bits) noexcept {
    return dependencies_.push_back(Dependency{producer_bits, dependent_bits});
}

void RenderServer::set_invalidation_observer(InvalidationFn observer, void* user) noexcept {
    invalidation_ = observer;
    invalidation_user_ = user;
}

void RenderServer::invalidate_dependents(u64 producer_bits) noexcept {
    // Backwards, so removing an entry does not move one this loop has yet to visit —
    // `remove_unordered` swaps the last element into the gap, and a forward loop would skip it.
    //
    // FORGOTTEN AS IT FIRES. A dependency that survived the destruction of the thing it described
    // would notify again the next time a recycled handle happened to match, which is the shape of
    // an invalidation storm nobody can trace to its source.
    for (usize index = dependencies_.size(); index > 0; --index) {
        const usize at = index - 1U;
        if (dependencies_[at].producer != producer_bits) {
            continue;
        }
        const u64 dependent = dependencies_[at].dependent;
        dependencies_.remove_unordered(at);
        if (invalidation_ != nullptr) {
            invalidation_(producer_bits, dependent, invalidation_user_);
        }
    }
}

void RenderServer::refresh_statistics(SceneHandle scene_handle) noexcept {
    // Textures and meshes are what the server knows the size of. Render targets, buffers and
    // acceleration structures belong to whoever owns a device and are written by it — deliberately
    // NOT zeroed here, so that "this server does not account for it" and "there is none of it" stay
    // different answers rather than both being nought written by two writers.
    frame_stats_.memory_bytes[static_cast<u32>(MemoryCategory::Textures)] = texture_bytes_;
    frame_stats_.memory_bytes[static_cast<u32>(MemoryCategory::Meshes)] = mesh_bytes_;

    const Scene* target = scenes_.resolve(scene_handle);
    if (target == nullptr) {
        frame_stats_.gpu_scene_capacity = 0;
        frame_stats_.gpu_scene_reserved = 0;
        return;
    }
    const GpuSceneStatistics gpu = target->gpu.statistics();
    frame_stats_.gpu_scene_capacity = gpu.capacity;
    frame_stats_.gpu_scene_reserved = gpu.reserved_slots;
}

}  // namespace cy::render
