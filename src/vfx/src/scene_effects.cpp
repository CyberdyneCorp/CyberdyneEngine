// SPDX-License-Identifier: MIT
#include <cy/vfx/scene_effects.h>

#include <cmath>
#include <string>

namespace cy::vfx {
namespace {

namespace ser = scene::serialization;

[[nodiscard]] const ser::WorldTypeDecl* effect_type(const ser::World& scene) noexcept {
    for (const ser::WorldTypeDecl& type : scene.types()) {
        if (scene.text(type.name) == "cy::vfx::Effect") {
            return &type;
        }
    }
    return nullptr;
}

[[nodiscard]] std::string_view value_text(const ser::World& scene,
                                          const ser::WorldValue& value) noexcept {
    const Span<const u8> bytes = scene.blob(value);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] const ser::WorldFieldDecl* declaration(const ser::WorldTypeDecl& type,
                                                     u64 field) noexcept {
    return type.find(field);
}

[[nodiscard]] Expected<std::string_view, Error> asset_of(const ser::World& scene,
                                                         const ser::WorldTypeDecl& type,
                                                         const ser::WorldComponent& component,
                                                         bool& enabled) noexcept {
    std::string_view asset;
    enabled = true;
    for (const ser::WorldField& field : component.fields()) {
        const ser::WorldFieldDecl* declared = declaration(type, field.file_field);
        if (declared == nullptr) {
            return fail(ErrorCode::InvalidArgument, "vfx scene: undeclared effect field");
        }
        const std::string_view name = scene.text(declared->name);
        if (name == "asset") {
            if (field.value.kind != ser::WorldValueKind::Text) {
                return fail(ErrorCode::InvalidArgument, "vfx scene: effect asset must be text");
            }
            asset = value_text(scene, field.value);
        } else if (name == "enabled") {
            if (field.value.kind != ser::WorldValueKind::Bool) {
                return fail(ErrorCode::InvalidArgument,
                            "vfx scene: effect enabled must be boolean");
            }
            enabled = field.value.integer != 0;
        }
    }
    if (asset.empty()) {
        return fail(ErrorCode::InvalidArgument, "vfx scene: effect has no asset path");
    }
    return asset;
}

[[nodiscard]] std::string field_name(const ParameterDecl& parameter) {
    std::string name = parameter.emitter.is_empty() ? "system." : "emitter.";
    if (!parameter.emitter.is_empty()) {
        name += parameter.emitter.text();
        name += '.';
    }
    name += parameter.name.text();
    name += '.';
    name += parameter.type.text();
    return name;
}

[[nodiscard]] const ParameterDecl* parameter_for(const CompiledSystem& system,
                                                 std::string_view field) {
    for (const ParameterDecl& parameter : system.parameters()) {
        if (field_name(parameter) == field) {
            return &parameter;
        }
    }
    return nullptr;
}

[[nodiscard]] Expected<u32, Error> parameter_value(const ser::WorldValue& value,
                                                   const ParameterDecl& parameter,
                                                   f32 out[4]) noexcept {
    const std::string_view kind = parameter.type.text();
    u32 lanes = 0;
    if (kind == "float" && value.kind == ser::WorldValueKind::Float) {
        lanes = 1;
        out[0] = value.lanes[0];
    } else if (kind == "vec2" && value.kind == ser::WorldValueKind::Vec2) {
        lanes = 2;
    } else if (kind == "vec3" && value.kind == ser::WorldValueKind::Vec3) {
        lanes = 3;
    } else if (kind == "vec4" && value.kind == ser::WorldValueKind::Vec4) {
        lanes = 4;
    } else if (kind == "int" && value.kind == ser::WorldValueKind::Int) {
        lanes = 1;
        out[0] = static_cast<f32>(value.integer);
    } else if (kind == "bool" && value.kind == ser::WorldValueKind::Bool) {
        lanes = 1;
        out[0] = value.integer != 0 ? 1.0F : 0.0F;
    } else {
        return fail(ErrorCode::InvalidArgument,
                    "vfx scene: override type differs from its parameter");
    }
    if (lanes > 1) {
        for (u32 lane = 0; lane < lanes; ++lane) {
            out[lane] = value.lanes[lane];
        }
    }
    for (u32 lane = 0; lane < lanes; ++lane) {
        if (!std::isfinite(out[lane])) {
            return fail(ErrorCode::InvalidArgument, "vfx scene: non-finite parameter override");
        }
    }
    return lanes;
}

[[nodiscard]] Status apply_overrides(const ser::World& scene, const ser::WorldTypeDecl& type,
                                     const ser::WorldComponent& component,
                                     const CompiledSystem& system, SimulationWorld& simulation,
                                     EffectHandle effect) noexcept {
    for (const ser::WorldField& field : component.fields()) {
        const ser::WorldFieldDecl* declared = declaration(type, field.file_field);
        if (declared == nullptr) {
            return fail(ErrorCode::InvalidArgument, "vfx scene: undeclared effect field");
        }
        const std::string_view name = scene.text(declared->name);
        if (name == "asset" || name == "enabled") {
            continue;
        }
        const ParameterDecl* parameter = parameter_for(system, name);
        if (parameter == nullptr) {
            return fail(ErrorCode::NotFound, "vfx scene: unknown parameter override");
        }
        if (!parameter->exposed) {
            return fail(ErrorCode::InvalidArgument, "vfx scene: folded parameter override");
        }
        f32 values[4] = {};
        auto lanes = parameter_value(field.value, *parameter, values);
        if (!lanes) {
            return make_unexpected(lanes.error());
        }
        const Status set = parameter->emitter.is_empty()
                               ? simulation.set_parameter(effect, parameter->name, {values, *lanes})
                               : simulation.set_parameter(effect, parameter->emitter,
                                                          parameter->name, {values, *lanes});
        if (!set) {
            return set;
        }
    }
    return ok();
}

}  // namespace

std::vector<SceneEffects::ParameterSnapshot> SceneEffects::snapshot_of(
    const ser::World& scene, const ser::WorldTypeDecl& type, const ser::WorldComponent& component) {
    std::vector<ParameterSnapshot> values;
    for (const ser::WorldField& field : component.fields()) {
        const ser::WorldFieldDecl* declared = type.find(field.file_field);
        if (declared == nullptr) {
            continue;
        }
        const std::string_view name = scene.text(declared->name);
        if (name == "asset" || name == "enabled") {
            continue;
        }
        ParameterSnapshot value;
        value.field = field.file_field;
        value.kind = field.value.kind;
        value.integer = field.value.integer;
        for (u32 lane = 0; lane < 4; ++lane) {
            value.lanes[lane] = field.value.lanes[lane];
        }
        values.push_back(value);
    }
    return values;
}

void SceneEffects::clear(SimulationWorld& simulation) noexcept {
    for (const Binding& binding : bindings_) {
        (void)simulation.stop(binding.effect, false);
    }
    bindings_.clear();
}

EffectHandle SceneEffects::find(u64 node_identity) const noexcept {
    for (const Binding& binding : bindings_) {
        if (binding.node == node_identity) {
            return binding.effect;
        }
    }
    return kInvalidEffect;
}

Status SceneEffects::load(const ser::World& scene, SimulationWorld& simulation,
                          SceneSystemResolver resolve, void* context) noexcept {
    clear(simulation);
    if (resolve == nullptr) {
        return fail(ErrorCode::InvalidArgument, "vfx scene: no cooked-system resolver");
    }
    const ser::WorldTypeDecl* type = effect_type(scene);
    if (type == nullptr) {
        return ok();
    }
    for (const ser::WorldNode& node : scene.nodes()) {
        const ser::WorldComponent* component = node.live ? node.find(type->file_type) : nullptr;
        if (component == nullptr) {
            continue;
        }
        bool enabled = true;
        auto asset = asset_of(scene, *type, *component, enabled);
        if (!asset) {
            clear(simulation);
            return make_unexpected(asset.error());
        }
        if (!enabled) {
            continue;
        }
        Transform transform;
        if (!ser::transform_of(scene, node, transform)) {
            clear(simulation);
            return fail(ErrorCode::InvalidArgument, "vfx scene: effect has no transform");
        }
        auto system = resolve(*asset, context);
        if (!system || *system == nullptr) {
            clear(simulation);
            return system ? fail(ErrorCode::NotFound, "vfx scene: cooked system was not found")
                          : make_unexpected(system.error());
        }
        EffectSpawn spawn;
        spawn.position = transform.translation;
        auto effect = simulation.play(**system, spawn);
        if (!effect) {
            clear(simulation);
            return make_unexpected(effect.error());
        }
        if (Status applied =
                apply_overrides(scene, *type, *component, **system, simulation, *effect);
            !applied) {
            (void)simulation.stop(*effect, false);
            clear(simulation);
            return applied;
        }
        if (Status pushed = bindings_.push_back({node.identity, *effect, std::string(*asset),
                                                 snapshot_of(scene, *type, *component)});
            !pushed) {
            (void)simulation.stop(*effect, false);
            clear(simulation);
            return pushed;
        }
    }
    return ok();
}

bool SceneEffects::same_instances(const ser::World& scene) const noexcept {
    const ser::WorldTypeDecl* type = effect_type(scene);
    if (type == nullptr) {
        return bindings_.empty();
    }
    usize index = 0;
    for (const ser::WorldNode& node : scene.nodes()) {
        const ser::WorldComponent* component = node.live ? node.find(type->file_type) : nullptr;
        if (component == nullptr) {
            continue;
        }
        bool enabled = true;
        auto asset = asset_of(scene, *type, *component, enabled);
        if (!asset) {
            return false;
        }
        if (!enabled) {
            continue;
        }
        if (index >= bindings_.size() || bindings_[index].node != node.identity ||
            bindings_[index].asset != *asset) {
            return false;
        }
        ++index;
    }
    return index == bindings_.size();
}

Status SceneEffects::refresh(const ser::World& scene, SimulationWorld& simulation) noexcept {
    if (!same_instances(scene)) {
        return fail(ErrorCode::InvalidArgument, "vfx scene: effect assets or entities changed");
    }
    const ser::WorldTypeDecl* type = effect_type(scene);
    if (type == nullptr) {
        return ok();
    }
    for (Binding& binding : bindings_) {
        const u32 index = scene.index_of(binding.node);
        const ser::WorldNode& node = scene.nodes()[index];
        const ser::WorldComponent* component = node.find(type->file_type);
        const EffectInstance* instance = simulation.find(binding.effect);
        if (component == nullptr || instance == nullptr) {
            return fail(ErrorCode::NotFound, "vfx scene: playing effect entity was removed");
        }
        auto parameters = snapshot_of(scene, *type, *component);
        if (parameters != binding.parameters) {
            if (Status reset = simulation.reset_parameters(binding.effect); !reset) {
                return reset;
            }
            if (Status applied = apply_overrides(scene, *type, *component, *instance->system,
                                                 simulation, binding.effect);
                !applied) {
                return applied;
            }
            binding.parameters = std::move(parameters);
        }
    }
    return update_transforms(scene, simulation);
}

Status SceneEffects::update_transforms(const ser::World& scene,
                                       SimulationWorld& simulation) noexcept {
    for (const Binding& binding : bindings_) {
        const u32 index = scene.index_of(binding.node);
        if (index == ser::WorldNode::kNoParent || !scene.nodes()[index].live) {
            return fail(ErrorCode::NotFound, "vfx scene: playing effect entity was removed");
        }
        Transform transform;
        if (!ser::transform_of(scene, scene.nodes()[index], transform)) {
            return fail(ErrorCode::InvalidArgument, "vfx scene: effect has no transform");
        }
        if (Status moved = simulation.set_transform(binding.effect, transform.translation);
            !moved) {
            return moved;
        }
    }
    return ok();
}

}  // namespace cy::vfx
