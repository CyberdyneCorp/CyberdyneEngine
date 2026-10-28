// SPDX-License-Identifier: MIT
#include <cy/game_backend/decals_backend.h>

#include <cy/abi/errors.h>
#include <cy/core/math/quat.h>

#include <cmath>

namespace cy::game_backend {
namespace {

[[nodiscard]] Quat rotation_of(const CyPose& pose) noexcept {
    const Quat rotation{pose.rotation[0], pose.rotation[1], pose.rotation[2], pose.rotation[3]};
    const f32 norm = (rotation.x * rotation.x) + (rotation.y * rotation.y) +
                     (rotation.z * rotation.z) + (rotation.w * rotation.w);
    // An all-zero quaternion is the identity, as cy_abi.h says of every `CyPose`.
    if (!(norm > 1.0e-12F)) {
        return Quat::identity();
    }
    const f32 scale = 1.0F / std::sqrt(norm);
    return Quat{rotation.x * scale, rotation.y * scale, rotation.z * scale, rotation.w * scale};
}

}  // namespace

rendering::DecalInstance decal_of(const CyDecalDesc& desc, u32 material_index,
                                  bool emissive) noexcept {
    const rendering::DecalInstance defaults;
    rendering::DecalInstance decal;
    const Quat rotation = rotation_of(desc.pose);
    decal.center = Vec3{desc.pose.position[0], desc.pose.position[1], desc.pose.position[2]};
    decal.half_extent = Vec3{desc.half_extent[0], desc.half_extent[1], desc.half_extent[2]};
    decal.axis_x = rotation * Vec3{1.0F, 0.0F, 0.0F};
    decal.axis_y = rotation * Vec3{0.0F, 1.0F, 0.0F};
    decal.axis_z = rotation * Vec3{0.0F, 0.0F, 1.0F};
    decal.material_index = material_index;
    decal.sort_order = desc.sort_order;
    decal.fade_angle_radians =
        desc.fade_angle > 0.0F ? desc.fade_angle : defaults.fade_angle_radians;
    decal.fade_start = desc.fade_start > 0.0F ? desc.fade_start : defaults.fade_start;
    decal.fade_end = desc.fade_end > 0.0F ? desc.fade_end : defaults.fade_end;
    decal.importance = desc.importance;
    decal.weights.emission = emissive ? 1.0F : 0.0F;
    return decal;
}

DecalsAdapter::DecalsAdapter(Allocator& allocator, rendering::DecalBudget& budget) noexcept
    : budget_(&budget), materials_(allocator) {}

Status DecalsAdapter::add_material(const char* name, u32 index, bool emissive) noexcept {
    if (name == nullptr || name[0] == '\0') {
        return fail(ErrorCode::InvalidArgument, "a decal material needs a name");
    }
    const Name interned = Name::intern(name);
    for (const Material& material : materials_) {
        if (material.name == interned) {
            return fail(ErrorCode::AlreadyExists,
                        "a decal material is already registered under that name");
        }
    }
    return materials_.push_back(Material{interned, index, emissive});
}

CyResult DecalsAdapter::find_material(const char* name, CyDecalMaterial& out) const noexcept {
    const Name wanted = Name::intern(name);
    for (usize index = 0; index < materials_.size(); ++index) {
        if (materials_[index].name == wanted) {
            out = static_cast<CyDecalMaterial>(index) + 1U;
            return CY_RESULT_OK;
        }
    }
    return abi::report(CY_RESULT_NOT_FOUND, "no decal material is registered under that name");
}

CyResult DecalsAdapter::spawn(const CyDecalDesc& desc, CyDecal& out) noexcept {
    if (desc.material == CY_DECAL_MATERIAL_NULL || desc.material > materials_.size()) {
        return abi::report(CY_RESULT_NOT_FOUND, "a decal material this host did not hand out");
    }
    const Material& material = materials_[desc.material - 1U];
    const usize evictions_before = budget_->evictions().size();
    const Expected<u64, Error> made =
        budget_->spawn(decal_of(desc, material.index, material.emissive), now_);
    if (!made) {
        // A budget the arbiter gave no decals is UNAVAILABLE, as an unbound one is.
        return abi::report(made.error().code == ErrorCode::Unavailable ? CY_RESULT_UNAVAILABLE
                                                                       : CY_RESULT_INTERNAL,
                           made.error().message);
    }
    ++spawned_;
    evicted_ += budget_->evictions().size() - evictions_before;
    out = *made;
    return CY_RESULT_OK;
}

CyResult DecalsAdapter::status(CyDecalStatus& out) const noexcept {
    out.live = static_cast<u32>(budget_->size());
    out.capacity = budget_->capacity();
    out.spawned = spawned_;
    out.evicted = evicted_;
    return CY_RESULT_OK;
}

void bind_decals(cy::abi::Host& host, DecalsAdapter* adapter) noexcept {
    host.game.decals = adapter;
}

}  // namespace cy::game_backend
