// Spawning. See cy/gameplay/play/spawn.h for the argument.

#include <cy/gameplay/play/spawn.h>
#include <cy/scene/node.h>
#include <cy/scene/propagation.h>

namespace cy::gameplay {

const char* spawn_policy_name(SpawnPolicy policy) noexcept {
    switch (policy) {
        case SpawnPolicy::ExactPosition:
            return "exact-position";
        case SpawnPolicy::SpawnPoint:
            return "spawn-point";
        case SpawnPolicy::NearestFree:
            return "nearest-free";
    }
    return "unknown";
}

Expected<u32, Error> SpawnService::add_point(Name name, const Transform& placement,
                                             u32 team) noexcept {
    SpawnPoint point;
    point.name = name;
    point.placement = placement;
    point.team = team;
    if (Status added = points_.push_back(point); !added) {
        return make_unexpected(added.error());
    }
    return static_cast<u32>(points_.size() - 1);
}

const SpawnPoint* SpawnService::point(u32 index) const noexcept {
    return index < points_.size() ? &points_[index] : nullptr;
}

u32 SpawnService::index_of(Name name) const noexcept {
    for (usize index = 0; index < points_.size(); ++index) {
        if (points_[index].name == name) {
            return static_cast<u32>(index);
        }
    }
    return kNoSpawnPoint;
}

Status SpawnService::reserve(u32 index, u64 now, u64 until_tick) noexcept {
    if (index >= points_.size()) {
        return fail(ErrorCode::NotFound, "spawn: no such point");
    }
    if (points_[index].reserved_until > now) {
        return fail(ErrorCode::Unavailable, "spawn: that point is already reserved");
    }
    points_[index].reserved_until = until_tick;
    ++statistics_.reservations;
    return ok();
}

Expected<SpawnResult, Error> SpawnService::select(const SpawnRequest& request,
                                                  u64 now) const noexcept {
    SpawnResult result;
    switch (request.policy) {
        case SpawnPolicy::ExactPosition:
            result.placement = request.placement;
            return result;

        case SpawnPolicy::SpawnPoint: {
            const u32 index = index_of(request.point);
            if (index == kNoSpawnPoint) {
                return make_unexpected(
                    fail(ErrorCode::NotFound, "spawn: no point of that name is registered")
                        .error());
            }
            if (points_[index].reserved_until > now) {
                return make_unexpected(
                    fail(ErrorCode::Unavailable, "spawn: that point is reserved").error());
            }
            result.placement = points_[index].placement;
            result.point = index;
            return result;
        }

        case SpawnPolicy::NearestFree:
            for (usize index = 0; index < points_.size(); ++index) {
                const SpawnPoint& point = points_[index];
                if (point.reserved_until > now) {
                    continue;
                }
                if (point.occupant.valid()) {
                    continue;
                }
                if (request.team != 0 && point.team != 0 && point.team != request.team) {
                    continue;
                }
                result.placement = point.placement;
                result.point = static_cast<u32>(index);
                return result;
            }
            return make_unexpected(
                fail(ErrorCode::Unavailable, "spawn: every registered point is taken").error());
    }
    return make_unexpected(fail(ErrorCode::InvalidArgument, "spawn: unknown policy").error());
}

Expected<SpawnResult, Error> SpawnService::spawn(const SpawnRequest& request, u64 now) noexcept {
    Expected<SpawnResult, Error> chosen = select(request, now);
    if (!chosen) {
        ++statistics_.refused;
        return chosen;
    }

    scene::Node parent = request.parent.valid() ? tree_->node(request.parent) : tree_->root();
    Expected<scene::Node, Error> made =
        tree_->create_node(request.name, parent, request.entity_template);
    if (!made) {
        ++statistics_.refused;
        return make_unexpected(made.error());
    }

    scene::LocalTransform local;
    local.value = chosen->placement;
    if (Status placed =
            tree_->world().set(made->entity(), tree_->components().local_transform, local);
        !placed) {
        return make_unexpected(placed.error());
    }
    // The dirty bit, not just the value: a spawned node whose subtree was never marked keeps the
    // template's `WorldTransform` until something else in its subtree moves.
    if (Status marked = scene::mark_transform_changed(*tree_, made->entity()); !marked) {
        return make_unexpected(marked.error());
    }

    chosen->entity = made->entity();
    if (chosen->point != kNoSpawnPoint) {
        points_[chosen->point].occupant = chosen->entity;
    }
    ++statistics_.spawned;
    return chosen;
}

Status SpawnService::spawn_many(const SpawnRequest& request, u32 count, Array<ecs::Entity>& out,
                                u64 now) noexcept {
    if (count == 0) {
        return ok();
    }
    if (Status reserved = out.reserve(out.size() + count); !reserved) {
        return reserved;
    }
    for (u32 index = 0; index < count; ++index) {
        Expected<SpawnResult, Error> one = spawn(request, now);
        if (!one) {
            return make_unexpected(one.error());
        }
        if (Status kept = out.push_back(one->entity); !kept) {
            return kept;
        }
    }
    ++statistics_.batches;
    return ok();
}

}  // namespace cy::gameplay
