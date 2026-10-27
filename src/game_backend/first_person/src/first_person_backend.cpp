// SPDX-License-Identifier: MIT
#include <cy/game_backend/first_person_backend.h>

#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>

namespace cy::game_backend {
namespace {

[[nodiscard]] Vec3 vec3_of(const float (&values)[3]) noexcept {
    return Vec3{values[0], values[1], values[2]};
}

void write3(Vec3 value, float (&out)[3]) noexcept {
    out[0] = value.x;
    out[1] = value.y;
    out[2] = value.z;
}

}  // namespace

camera::FirstPersonSettings settings_of(const CyFirstPersonDesc& desc) noexcept {
    camera::FirstPersonSettings settings;
    settings.look.sensitivity = desc.look_sensitivity;
    settings.look.aim_sensitivity = desc.aim_sensitivity;
    settings.look.invert_y = (desc.flags & CY_FIRST_PERSON_INVERT_Y) != 0U;
    settings.look.pitch_limit_radians = desc.pitch_limit;
    settings.aim.hip_lens.gameplay.vertical_fov_radians = desc.hip_fov;
    settings.aim.aim_lens.gameplay.vertical_fov_radians = desc.aim_fov;
    settings.aim.hip_viewmodel_fov_radians = desc.hip_viewmodel_fov;
    settings.aim.aim_viewmodel_fov_radians = desc.aim_viewmodel_fov;
    settings.aim.half_life_seconds = desc.aim_half_life;
    settings.aim.hip_offset = vec3_of(desc.hip_offset);
    settings.aim.aim_offset = vec3_of(desc.aim_offset);
    settings.bob.stride_metres = desc.bob_stride;
    settings.bob.vertical_metres = desc.bob_vertical;
    settings.bob.lateral_metres = desc.bob_lateral;
    settings.bob.reference_speed = desc.bob_reference_speed;
    settings.recoil.frequency_hz = desc.recoil_frequency;
    settings.recoil.damping_ratio = desc.recoil_damping;
    settings.recoil.camera_share = desc.recoil_camera_share;
    settings.motion_scale = 1.0F - desc.motion_reduction;
    return settings;
}

rendering::viewmodel::ViewmodelSettings viewmodel_of(const CyViewmodelDesc& desc,
                                                     f32 fov_now) noexcept {
    rendering::viewmodel::ViewmodelSettings settings;
    if (desc.layers == 0U) {
        return settings;
    }
    settings.layers = desc.layers;
    settings.vertical_fov_radians = desc.fov > 0.0F ? desc.fov : fov_now;
    settings.near_plane = desc.near_plane;
    settings.far_plane = desc.far_plane;
    settings.depth_fraction = desc.depth_fraction;
    settings.shadows.receive = (desc.flags & CY_VIEWMODEL_IGNORE_SHADOWS) == 0U;
    settings.shadows.cast = (desc.flags & CY_VIEWMODEL_CAST_SHADOWS) != 0U;
    settings.shadows.contact = (desc.flags & CY_VIEWMODEL_CONTACT_SHADOWS) != 0U;
    return settings;
}

FirstPersonAdapter::FirstPersonAdapter(Allocator& allocator) noexcept : slots_(allocator) {}

Status FirstPersonAdapter::create(u32 capacity) noexcept {
    capacity_ = capacity;
    return slots_.reserve(capacity);
}

u32 FirstPersonAdapter::cameras() const noexcept {
    return static_cast<u32>(slots_.size());
}

FirstPersonAdapter::Slot* FirstPersonAdapter::find(CyCamera camera) noexcept {
    for (Slot& slot : slots_) {
        if (slot.camera == camera) {
            return &slot;
        }
    }
    return nullptr;
}

const FirstPersonAdapter::Slot* FirstPersonAdapter::find(CyCamera camera) const noexcept {
    for (const Slot& slot : slots_) {
        if (slot.camera == camera) {
            return &slot;
        }
    }
    return nullptr;
}

void FirstPersonAdapter::advance(f32 seconds) noexcept {
    for (Slot& slot : slots_) {
        camera::FirstPersonInput input = slot.pending;
        input.delta_seconds = seconds;
        (void)slot.behaviours.advance(input);
        // The look is a delta and is spent; the speed and the flags hold until the game says
        // otherwise, as a held key does.
        slot.pending.look = Vec2{0.0F, 0.0F};
    }
}

const camera::FirstPersonPose* FirstPersonAdapter::pose(CyCamera camera) const noexcept {
    const Slot* slot = find(camera);
    return slot != nullptr ? &slot->behaviours.pose() : nullptr;
}

rendering::viewmodel::ViewmodelSettings FirstPersonAdapter::viewmodel(
    CyCamera camera) const noexcept {
    const Slot* slot = find(camera);
    if (slot == nullptr) {
        return rendering::viewmodel::ViewmodelSettings{};
    }
    return viewmodel_of(slot->weapon, slot->behaviours.pose().viewmodel_fov_radians);
}

CyResult FirstPersonAdapter::configure(CyCamera camera, const CyFirstPersonDesc& desc) noexcept {
    if (Slot* slot = find(camera); slot != nullptr) {
        // NEW SETTINGS, THE SAME STATE: a sensitivity changed in the options menu does not snap the
        // look back to where the camera started.
        slot->behaviours.set_settings(settings_of(desc));
        return CY_RESULT_OK;
    }
    if (slots_.size() >= capacity_) {
        return abi::report(CY_RESULT_OUT_OF_RANGE,
                           "every first-person slot is taken; release a camera first");
    }
    Slot slot;
    slot.camera = camera;
    slot.behaviours = camera::FirstPersonCamera(settings_of(desc));
    if (Status added = slots_.push_back(slot); !added) {
        return abi::report(CY_RESULT_OUT_OF_MEMORY, added.error().message);
    }
    return CY_RESULT_OK;
}

CyResult FirstPersonAdapter::input(CyCamera camera, const CyFirstPersonInput& input) noexcept {
    Slot* slot = find(camera);
    if (slot == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND, "the camera has no first-person behaviours");
    }
    slot->pending.look.x += input.look[0];
    slot->pending.look.y += input.look[1];
    slot->pending.move_speed = input.move_speed;
    slot->pending.aiming = (input.flags & CY_FIRST_PERSON_AIMING) != 0U;
    slot->pending.grounded = (input.flags & CY_FIRST_PERSON_AIRBORNE) == 0U;
    return CY_RESULT_OK;
}

CyResult FirstPersonAdapter::kick(CyCamera camera, f32 pitch, f32 yaw) noexcept {
    Slot* slot = find(camera);
    if (slot == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND, "the camera has no first-person behaviours");
    }
    slot->behaviours.kick(camera::RecoilImpulse{pitch, yaw});
    return CY_RESULT_OK;
}

CyResult FirstPersonAdapter::reset(CyCamera camera, f32 yaw, f32 pitch) noexcept {
    Slot* slot = find(camera);
    if (slot == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND, "the camera has no first-person behaviours");
    }
    slot->behaviours.reset(yaw, pitch);
    slot->pending = camera::FirstPersonInput{};
    return CY_RESULT_OK;
}

CyResult FirstPersonAdapter::state(CyCamera camera, CyFirstPersonState& out) const noexcept {
    const Slot* slot = find(camera);
    if (slot == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND, "the camera has no first-person behaviours");
    }
    const camera::FirstPersonPose& pose = slot->behaviours.pose();
    out.yaw = pose.yaw;
    out.pitch = pose.pitch;
    out.recoil[0] = pose.recoil.x;
    out.recoil[1] = pose.recoil.y;
    out.aim = pose.aim;
    out.fov = pose.lens.vertical_fov_radians();
    out.viewmodel_fov = pose.viewmodel_fov_radians;
    write3(pose.eye_offset, out.eye_offset);
    write3(pose.weapon.translation, out.weapon.position);
    out.weapon.rotation[0] = pose.weapon.rotation.x;
    out.weapon.rotation[1] = pose.weapon.rotation.y;
    out.weapon.rotation[2] = pose.weapon.rotation.z;
    out.weapon.rotation[3] = pose.weapon.rotation.w;
    write3(pose.aims.view_aim, out.view_aim);
    write3(pose.aims.weapon_aim, out.weapon_aim);
    return CY_RESULT_OK;
}

CyResult FirstPersonAdapter::release(CyCamera camera) noexcept {
    for (usize index = 0; index < slots_.size(); ++index) {
        if (slots_[index].camera == camera) {
            slots_.erase(index);
            return CY_RESULT_OK;
        }
    }
    return abi::report(CY_RESULT_NOT_FOUND, "the camera has no first-person behaviours");
}

CyResult FirstPersonAdapter::configure_viewmodel(CyCamera camera,
                                                 const CyViewmodelDesc& desc) noexcept {
    Slot* slot = find(camera);
    if (slot == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND,
                           "a weapon view belongs to a camera with first-person behaviours");
    }
    slot->weapon = desc;
    return CY_RESULT_OK;
}

CyResult FirstPersonAdapter::viewmodel(CyCamera camera, CyViewmodelDesc& out) const noexcept {
    const Slot* slot = find(camera);
    if (slot == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND, "the camera has no first-person behaviours");
    }
    const u32 size = out.struct_size;
    out = slot->weapon;
    out.struct_size = size;
    // The lens it draws with NOW, which follows the sights when it was set to follow them.
    if (out.layers != 0U) {
        out.fov = viewmodel_of(slot->weapon, slot->behaviours.pose().viewmodel_fov_radians)
                      .vertical_fov_radians;
    }
    return CY_RESULT_OK;
}

void bind_first_person(cy::abi::Host& host, FirstPersonAdapter* adapter) noexcept {
    host.game.first_person = adapter;
}

}  // namespace cy::game_backend
