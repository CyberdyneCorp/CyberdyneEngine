// SPDX-License-Identifier: MIT
// cy/game_backend/illumination_backend.h — the GI adapter behind ABI 1.9. Issue #35.
//
// The contract is cy/abi/game/illumination.h; this is the half that knows CyberGI. One
// `gi::IlluminationSystem` — the one the host renders with — reached by entity and material name:
//
//   * OBJECTS BY ENTITY. The host registers each object it hands the GI (`add_object`) here with
//     `bind_object`: the entity a script names, the GI object id and the `Surfel::instance_id` its
//     cards carry. `gi_object_set_class` and an emission rule for an entity need it; an entity the
//     host never bound is NOT_FOUND, which is what a script asking about scenery the GI does not
//     hold should hear.
//   * MATERIALS BY NAME. `add_material` registers the `Surfel::material_id` a material's cards
//     carry under its name; a script names it by `CY_NAME_HASH` of the same text.
//   * VOLUMES BY HANDLE. A handle is a counter, never reused, so a destroyed volume's is NOT_FOUND.
//     Every change re-places the game's whole set (`set_volumes` under `GiVolumeOwner::Game`),
//     which retires exactly the probes in reach of the volumes that changed something placement
//     decides, and leaves the author's and the streamed cells' volumes as they are.
//   * EMISSION RULES BY (ENTITY, MATERIAL). Each call replaces the rule for its pair, AUTOMATIC
//     removes it, and the whole list is handed to `set_emitter_rules`, which classifies again only
//     the cells that have an emitter.
//
// Changes are stamped with the frame `begin_frame` last named. Not thread-safe: the GI is
// presentation, tuned from the game thread in N or U.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/abi/game/illumination.h>
#include <cy/abi/host.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/system.h>

namespace cy::game_backend {

/// `IlluminationBackend` over `cy::rendering::gi::IlluminationSystem`.
class IlluminationAdapter final : public abi::game::IlluminationBackend {
public:
    /// `system` is borrowed and outlives this.
    IlluminationAdapter(Allocator& allocator, rendering::gi::IlluminationSystem& system) noexcept;

    /// The frame the next changes are stamped with. Once per frame, before the frame's scripts.
    void begin_frame(u64 frame) noexcept { frame_ = frame; }

    /// Name an object the host registered with the GI: `object` is its `GiObjectDesc::id`, and
    /// `instance` the `Surfel::instance_id` of its cards. Refused for an entity already bound.
    [[nodiscard]] Status bind_object(CyEntity entity, u64 object, u32 instance) noexcept;
    /// Forget an entity; its emission rules go with it.
    void unbind_object(CyEntity entity) noexcept;
    /// Register the `Surfel::material_id` the cards of the material called `name` carry.
    [[nodiscard]] Status add_material(const char* name, u32 material_id) noexcept;

    /// THE HOST REBUILT THE GI under a running game (the editor's Play, after an edit that changed
    /// geometry): from now on the game's calls reach `system`, borrowed as the constructor's. The
    /// objects and materials bound to the old system are forgotten — bind them again on the new
    /// one — and the game's volumes and emission rules are kept, with their handles, for `restore`.
    void rebind(rendering::gi::IlluminationSystem& system) noexcept;
    /// Place the game's volumes (its own `GiVolumeOwner::Game` set) and its emission rules on the
    /// system again: after `rebind` and the bindings, so the GI the game tuned is the GI it sees.
    [[nodiscard]] Status restore() noexcept;

    CyResult settings(CyGiSettings& out) const noexcept override;
    CyResult set_settings(const CyGiSettings& settings) noexcept override;
    CyResult create_volume(const CyGiVolumeDesc& desc, CyGiVolume& out) noexcept override;
    CyResult update_volume(CyGiVolume volume, const CyGiVolumeDesc& desc) noexcept override;
    CyResult destroy_volume(CyGiVolume volume) noexcept override;
    CyResult set_object_class(CyEntity entity, u32 klass) noexcept override;
    CyResult object_class(CyEntity entity, u32& out) const noexcept override;
    CyResult set_emission(CyEntity entity, u64 material, u32 mode) noexcept override;
    CyResult sample_irradiance(const f32 (&position)[3], const f32* normal,
                               CyGiIrradiance& out) const noexcept override;
    CyResult status(CyGiStatus& out) const noexcept override;

    /// The volumes in the order they were created, as the GI holds them.
    [[nodiscard]] usize volume_count() const noexcept { return volumes_.size(); }

private:
    struct BoundObject {
        CyEntity entity = CY_ENTITY_NULL;
        u64 object = 0;
        u32 instance = 0;
    };
    struct Material {
        u64 hash = 0;
        u32 id = 0;
    };
    struct Volume {
        CyGiVolume handle = CY_GI_VOLUME_NULL;
        rendering::gi::GiVolume volume{};
    };
    struct Emission {
        CyEntity entity = CY_ENTITY_NULL;
        u64 material = 0;
        rendering::gi::EmitterOverride mode = rendering::gi::EmitterOverride::Automatic;
    };

    [[nodiscard]] const BoundObject* bound(CyEntity entity) const noexcept;
    [[nodiscard]] const Material* material(u64 hash) const noexcept;
    [[nodiscard]] Volume* find_volume(CyGiVolume handle) noexcept;
    [[nodiscard]] CyResult apply_volumes(Array<Volume>& next) noexcept;
    [[nodiscard]] CyResult apply_emission(Array<Emission>& next) noexcept;

    Allocator* allocator_;
    rendering::gi::IlluminationSystem* system_;
    u64 frame_ = 0;
    CyGiVolume next_volume_ = 1;
    Array<BoundObject> objects_;
    Array<Material> materials_;
    Array<Volume> volumes_;
    Array<Emission> emissions_;
};

/// The engine's GI volume for an ABI description: the parameters `sets` names taken from it, every
/// other one the neutral default, so a field whose bit is clear cannot be refused.
[[nodiscard]] rendering::gi::GiVolume gi_volume_from_abi(const CyGiVolumeDesc& desc,
                                                         u64 id) noexcept;

/// Bind `adapter` as `host.game.illumination`, or unbind with null.
void bind_illumination(cy::abi::Host& host, IlluminationAdapter* adapter) noexcept;

}  // namespace cy::game_backend
