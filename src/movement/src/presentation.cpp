// SPDX-License-Identifier: MIT
// The authoritative transform and its presentation sync. See include/cy/movement/components.h.
//
// The authoritative half (`publish_units`) is integer-only. `sync_presentation` is the one float
// path that runs every tick, and it is presentation: it converts with `detmath::to_f32_relative`
// (design §7.2) and writes the scene, and nothing on the authoritative side reads what it writes.

#include <cy/core/detmath/convert.h>
#include <cy/ecs/query.h>
#include <cy/movement/components.h>
#include <cy/scene/tree.h>

#include <utility>
#include <vector>

namespace cy::movement {

Expected<ecs::ComponentTypeId, Error> register_authoritative_transform(ecs::World& world) noexcept {
    if (const ecs::ComponentInfo* existing =
            world.components().find(kAuthoritativeTransformComponentName);
        existing != nullptr) {
        return existing->id;
    }
    return world.components().register_builtin(kAuthoritativeTransformComponentName,
                                               static_cast<u32>(sizeof(AuthoritativeTransform)),
                                               static_cast<u32>(alignof(AuthoritativeTransform)));
}

Expected<PublishReport, Error> publish_units(const KinematicMover& mover, ecs::World& world,
                                             ecs::ComponentTypeId component) noexcept {
    PublishReport report;
    for (u32 unit = 0; unit < mover.size(); ++unit) {
        const ecs::Entity entity = ecs::Entity::from_bits(mover.entity(unit));
        if (!mover.active(unit) || !world.is_alive(entity) || !world.has(entity, component)) {
            ++report.skipped;
            continue;
        }
        if (Status written =
                world.set(entity, component, AuthoritativeTransform{mover.transform(unit)});
            !written) {
            return make_unexpected(written.error());
        }
        ++report.written;
    }
    return report;
}

Expected<PresentationSyncReport, Error> sync_presentation(scene::SceneTree& tree,
                                                          ecs::ComponentTypeId component,
                                                          detmath::FixedVec3 origin) noexcept {
    ecs::World& world = tree.world();
    ecs::QueryDesc desc(world.allocator());
    if (Status read = desc.read(component); !read) {
        return make_unexpected(read.error());
    }
    ecs::Query query(world, std::move(desc));

    // Gathered first and written after: the scene write marks dirty state up the hierarchy, and the
    // query holds the world in its iterating state while it runs.
    std::vector<std::pair<ecs::Entity, detmath::FixedTransform>> placements;
    if (Status walked = query.for_each_chunk([&](ecs::QueryChunk& chunk) noexcept {
            const Span<const ecs::Entity> entities = chunk.entities();
            const Span<const AuthoritativeTransform> values =
                chunk.read<AuthoritativeTransform>(component);
            for (usize row = 0; row < entities.size(); ++row) {
                placements.emplace_back(entities[row], values[row].value);
            }
        });
        !walked) {
        return make_unexpected(walked.error());
    }

    PresentationSyncReport report;
    const detmath::Fixed zero = detmath::Fixed::zero();
    for (const auto& [entity, placement] : placements) {
        const scene::Node node = tree.node(entity);
        if (!node.valid()) {
            ++report.without_node;
            continue;
        }
        Transform presented = Transform::identity();
        presented.translation = Vec3{detmath::to_f32_relative(placement.translation.x, origin.x),
                                     detmath::to_f32_relative(placement.translation.y, origin.y),
                                     detmath::to_f32_relative(placement.translation.z, origin.z)};
        presented.rotation = Quat{detmath::to_f32_relative(placement.rotation.x, zero),
                                  detmath::to_f32_relative(placement.rotation.y, zero),
                                  detmath::to_f32_relative(placement.rotation.z, zero),
                                  detmath::to_f32_relative(placement.rotation.w, zero)};
        if (Status written = node.set_local_transform(presented); !written) {
            return make_unexpected(written.error());
        }
        ++report.synced;
    }
    return report;
}

}  // namespace cy::movement
