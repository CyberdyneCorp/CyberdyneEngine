// The extract stage. See cy/rendering/scene/extract.h.

#include <cy/rendering/scene/extract.h>

#include <cy/ecs/query.h>

#include <utility>

namespace cy::rendering {
namespace {

using determinism::PresentationContext;

/// The witness extraction reads presentation fields with.
///
/// EXTRACTION IS PRESENTATION CODE, and this constant is where that is stated once. A presentation
/// reader may read anything (`determinism::may_read`), which is what lets this file read both an
/// authored `MeshRenderer::visible` and an interpolated `InterpolatedTransform::previous`. The
/// direction that stays closed is the other one: nothing here writes back into the world.
constexpr PresentationContext kPresentation{};

/// The columns one chunk offers the extractor, resolved once per chunk rather than per row.
///
/// `previous` is empty when the chunk's archetype has no `InterpolatedTransform` — the opt-in
/// component a node marked interpolatable carries — and `placement_of` is the one place that
/// absence is turned into an answer, so no caller repeats the test.
struct ChunkColumns {
    Span<const Entity> entities;
    Span<const scene::WorldTransform> transforms;
    Span<const scene::InterpolatedTransform> previous;
};

/// Both placements and the teleport flag, for one row.
struct Placement {
    Transform current;
    Transform previous;
    bool teleported = false;
};

[[nodiscard]] ChunkColumns columns_of(ecs::QueryChunk& chunk,
                                      const scene::SceneComponents& scene) noexcept {
    ChunkColumns columns;
    columns.entities = chunk.entities();
    columns.transforms = chunk.read<scene::WorldTransform>(scene.world_transform);
    if (chunk.has(scene.interpolated_transform)) {
        columns.previous = chunk.read<scene::InterpolatedTransform>(scene.interpolated_transform);
    }
    return columns;
}

/// A node that did not opt into interpolation has no previous placement, so its two transforms are
/// equal — which `resolve_transform` blends to the same answer at every alpha rather than to a
/// smear from the origin.
[[nodiscard]] Placement placement_of(const ChunkColumns& columns, u32 row) noexcept {
    Placement placement;
    placement.current = columns.transforms[row].value;
    if (columns.previous.empty()) {
        placement.previous = placement.current;
        return placement;
    }
    placement.previous = columns.previous[row].previous.read(kPresentation);
    placement.teleported = columns.previous[row].teleport.read(kPresentation);
    return placement;
}

/// The three terms every one of the extractor's queries selects: the subsystem's component, the
/// placement, and the optional interpolation history. Written once because three copies of it would
/// be three places for a term to be forgotten — and a forgotten `optional()` is a chunk whose
/// previous transform silently becomes its current one.
[[nodiscard]] Status describe_query(ecs::QueryDesc& desc, ecs::ComponentTypeId component,
                                    const scene::SceneComponents& scene) noexcept {
    if (Status declared = desc.read(component); !declared) {
        return declared;
    }
    if (Status declared = desc.read(scene.world_transform); !declared) {
        return declared;
    }
    return desc.optional(scene.interpolated_transform);
}

/// One instance record, built from a row. Pure: everything it reads is in its arguments, which is
/// what keeps the chunk loop above it a loop rather than a second copy of this.
[[nodiscard]] render::InstanceSnapshot instance_of(const ChunkColumns& columns,
                                                   const MeshRenderer& renderer, u32 row) noexcept {
    const Placement placement = placement_of(columns, row);
    render::InstanceSnapshot instance;
    instance.stable_id = columns.entities[row].bits();
    instance.mesh = renderer.mesh;
    instance.material = renderer.material;
    instance.transform = placement.current;
    instance.previous_transform = placement.previous;
    instance.teleported = placement.teleported;
    instance.local_bounds = renderer.local_bounds;
    instance.layer_mask = renderer.layer_mask;
    instance.importance = renderer.importance.read(kPresentation);
    instance.lod_bias = renderer.lod_bias;
    instance.flags = render::kInstanceActive | (renderer.visible ? render::kInstanceVisible : 0U) |
                     (renderer.casts_shadow ? render::kInstanceCastsShadow : 0U) |
                     (renderer.receives_shadow ? render::kInstanceReceivesShadow : 0U) |
                     (renderer.two_sided ? render::kInstanceTwoSided : 0U);
    return instance;
}

[[nodiscard]] render::CameraSnapshot camera_of(const ChunkColumns& columns, const Camera& camera,
                                               u32 row) noexcept {
    const Placement placement = placement_of(columns, row);
    render::CameraSnapshot published;
    published.stable_id = columns.entities[row].bits();
    published.transform = placement.current;
    published.previous_transform = placement.previous;
    published.teleported = placement.teleported;
    published.projection = camera.projection;
    published.viewport = camera.viewport;
    published.layer_mask = camera.layer_mask;
    published.importance = camera.importance;
    // Zero means "no history of its own", so the entity's identity is the fallback — a camera that
    // never set one still reprojects against itself rather than against whichever other view also
    // carried zero.
    published.history_id = (camera.history_id != 0) ? camera.history_id : published.stable_id;
    return published;
}

[[nodiscard]] render::LightSnapshot light_of(const ChunkColumns& columns, const LightSource& light,
                                             u32 row) noexcept {
    const Placement placement = placement_of(columns, row);
    render::LightSnapshot published;
    published.desc.kind = light.kind;
    published.desc.transform = placement.current;
    published.desc.color[0] = light.color[0];
    published.desc.color[1] = light.color[1];
    published.desc.color[2] = light.color[2];
    published.desc.intensity = light.intensity;
    published.desc.range = light.range;
    published.desc.inner_cone_radians = light.inner_cone_radians;
    published.desc.outer_cone_radians = light.outer_cone_radians;
    published.desc.layer_mask = light.layer_mask;
    published.desc.casts_shadow = light.casts_shadow;
    // The identity a shadow atlas page and the deterministic light order key off. The entity's, for
    // the same reason an instance's is: it is stable across two runs and the slot a light lands in
    // is not.
    published.desc.stable_id = columns.entities[row].bits();
    published.previous_transform = placement.previous;
    published.teleported = placement.teleported;
    return published;
}

/// Insertion point for `stable_id` in a sorted array, by binary search.
[[nodiscard]] usize lower_bound(Span<const u64> sorted, u64 value) noexcept {
    usize low = 0;
    usize high = sorted.size();
    while (low < high) {
        const usize middle = low + ((high - low) / 2U);
        if (sorted[middle] < value) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return low;
}

}  // namespace

SnapshotExtractor::SnapshotExtractor(Allocator& allocator, World& world,
                                     const RenderComponents& render_components,
                                     const scene::SceneComponents& scene_components,
                                     render::SnapshotBuffer& buffer) noexcept
    : allocator_(&allocator),
      world_(&world),
      render_(render_components),
      scene_(scene_components),
      buffer_(&buffer),
      published_(allocator) {}

void SnapshotExtractor::reset() noexcept {
    published_.clear();
    last_version_ = 0;
    last_structural_ = 0;
    extracted_once_ = false;
    stats_ = ExtractStatistics{};
}

bool SnapshotExtractor::remember(u64 stable_id) noexcept {
    const usize at = lower_bound(published_.span(), stable_id);
    if (at < published_.size() && published_[at] == stable_id) {
        return true;
    }
    // Append, then shift the tail right. Entities are created with ascending indices, so the common
    // case appends at the end and shifts nothing; the sorted order is what makes the removal list
    // deterministic without a second sort (see the header).
    if (!published_.push_back(stable_id).has_value()) {
        return false;
    }
    for (usize index = published_.size() - 1; index > at; --index) {
        published_[index] = published_[index - 1U];
    }
    published_[at] = stable_id;
    return true;
}

Status SnapshotExtractor::on_commit(const determinism::CommitRecord& record) noexcept {
    if (!render_.registered() || scene_.world_transform == ecs::kInvalidComponent) {
        return fail(ErrorCode::InvalidArgument,
                    "the renderer's or the scene's components are not registered in this world; "
                    "extraction over an invalid component id would publish an empty snapshot and "
                    "report success");
    }

    render::RenderSnapshot& snapshot = buffer_->writable();
    snapshot.state_version = record.state_version;
    snapshot.point = record.point;
    snapshot.environment = environment_;
    stats_ = ExtractStatistics{};

    if (Status extracted = extract_instances(snapshot); !extracted) {
        return extracted;
    }
    if (Status swept = sweep_removed(snapshot); !swept) {
        return swept;
    }
    if (Status cameras = extract_cameras(snapshot); !cameras) {
        return cameras;
    }
    if (Status lights = extract_lights(snapshot); !lights) {
        return lights;
    }

    snapshot.examined = stats_.examined;
    last_structural_ = world_->structural_changes();
    last_version_ = world_->version();
    extracted_once_ = true;
    // ADVANCE THE VERSION, DELIBERATELY. Change detection compares a chunk's version against the
    // baseline this extraction just recorded, and a write that happened at the same version would
    // compare equal and be missed. Advancing here makes extraction a stage boundary in the sense
    // `ecs-core` means one: everything written after it carries a strictly greater version, so
    // nothing written between two extractions can be skipped by the filter.
    (void)world_->advance_version();

    (void)buffer_->publish();
    return ok();
}

Status SnapshotExtractor::extract_instances(render::RenderSnapshot& snapshot) noexcept {
    ecs::QueryDesc desc(*allocator_);
    if (Status declared = describe_query(desc, render_.mesh_renderer, scene_); !declared) {
        return declared;
    }

    ecs::Query query(*world_, std::move(desc));
    const u64 since = last_version_;
    const bool filter = extracted_once_;
    Status failure = ok();

    Status iterated = query.for_each_chunk([&](ecs::QueryChunk& chunk) noexcept {
        // `for_each_chunk` has no way to stop early, so a failed allocation is remembered and every
        // later chunk returns immediately. Reported rather than swallowed: a snapshot missing half
        // its instances must not look like a snapshot of a world with half as many.
        if (!failure) {
            return;
        }
        // The change filter, over THREE components rather than the one `filter_changed()` takes: a
        // chunk is re-extracted when the placement, the renderable, or the interpolation history
        // changed. The third is not decoration — `Node::teleport()` writes only the teleport flag,
        // and a filter that ignored it would publish the smear the flag exists to suppress.
        if (filter && chunk.version(scene_.world_transform) <= since &&
            chunk.version(render_.mesh_renderer) <= since &&
            chunk.version(scene_.interpolated_transform) <= since) {
            ++stats_.chunks_skipped;
            return;
        }
        ++stats_.chunks_visited;
        failure = publish_chunk(chunk, snapshot);
    });

    if (!iterated) {
        return iterated;
    }
    return failure;
}

Status SnapshotExtractor::publish_chunk(ecs::QueryChunk& chunk,
                                        render::RenderSnapshot& snapshot) noexcept {
    const ChunkColumns columns = columns_of(chunk, scene_);
    const Span<const MeshRenderer> renderers = chunk.read<MeshRenderer>(render_.mesh_renderer);

    for (u32 row = 0; row < chunk.count(); ++row) {
        ++stats_.examined;
        const render::InstanceSnapshot instance = instance_of(columns, renderers[row], row);
        if (Status pushed = snapshot.changed.push_back(instance); !pushed) {
            return pushed;
        }
        if (!remember(instance.stable_id)) {
            return fail(ErrorCode::OutOfMemory, "the published set could not grow");
        }
        ++stats_.published;
    }
    return ok();
}

Status SnapshotExtractor::sweep_removed(render::RenderSnapshot& snapshot) noexcept {
    // Only on a tick that created or destroyed something. See the header: without an event log in
    // the ECS this is the honest check, and skipping it on the ticks where nothing structural
    // happened is what keeps a moving world proportional to what moved.
    if (extracted_once_ && world_->structural_changes() == last_structural_) {
        return ok();
    }
    stats_.swept = true;

    usize kept = 0;
    for (const u64 stable_id : published_) {
        const Entity entity = Entity::from_bits(stable_id);
        // Two ways to stop being renderable, and both are removals: the entity died, or it lost its
        // `MeshRenderer` while staying alive. A consumer told about only the first would keep
        // drawing the second.
        if (world_->is_alive(entity) && world_->has(entity, render_.mesh_renderer)) {
            published_[kept] = stable_id;
            ++kept;
            continue;
        }
        if (Status pushed = snapshot.removed.push_back(stable_id); !pushed) {
            return pushed;
        }
        ++stats_.removed;
    }
    return published_.resize(kept);
}

Status SnapshotExtractor::extract_cameras(render::RenderSnapshot& snapshot) noexcept {
    ecs::QueryDesc desc(*allocator_);
    if (Status declared = describe_query(desc, render_.camera, scene_); !declared) {
        return declared;
    }

    ecs::Query query(*world_, std::move(desc));
    Status failure = ok();
    Status iterated = query.for_each_chunk([&](ecs::QueryChunk& chunk) noexcept {
        if (!failure) {
            return;
        }
        const ChunkColumns columns = columns_of(chunk, scene_);
        const Span<const Camera> cameras = chunk.read<Camera>(render_.camera);
        for (u32 row = 0; row < chunk.count(); ++row) {
            if (!cameras[row].enabled) {
                continue;
            }
            if (Status pushed = snapshot.cameras.push_back(camera_of(columns, cameras[row], row));
                !pushed) {
                failure = pushed;
                return;
            }
            ++stats_.cameras;
        }
    });
    if (!iterated) {
        return iterated;
    }
    return failure;
}

Status SnapshotExtractor::extract_lights(render::RenderSnapshot& snapshot) noexcept {
    ecs::QueryDesc desc(*allocator_);
    if (Status declared = describe_query(desc, render_.light_source, scene_); !declared) {
        return declared;
    }

    ecs::Query query(*world_, std::move(desc));
    Status failure = ok();
    Status iterated = query.for_each_chunk([&](ecs::QueryChunk& chunk) noexcept {
        if (!failure) {
            return;
        }
        const ChunkColumns columns = columns_of(chunk, scene_);
        const Span<const LightSource> lights = chunk.read<LightSource>(render_.light_source);
        for (u32 row = 0; row < chunk.count(); ++row) {
            if (!lights[row].enabled) {
                continue;
            }
            if (Status pushed = snapshot.lights.push_back(light_of(columns, lights[row], row));
                !pushed) {
                failure = pushed;
                return;
            }
            ++stats_.lights;
        }
    });
    if (!iterated) {
        return iterated;
    }
    return failure;
}

}  // namespace cy::rendering
