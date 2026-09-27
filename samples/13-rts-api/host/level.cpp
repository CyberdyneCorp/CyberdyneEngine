// SPDX-License-Identifier: MIT
#include "level.h"

#include <utility>

namespace sample::rts {
namespace {

using cy::f32;
using cy::u32;
using cy::Vec3;

[[nodiscard]] cy::input::Binding key_binding(cy::input::ActionId action,
                                             cy::input::Key key) noexcept {
    cy::input::Binding binding;
    binding.action = action;
    binding.kind = cy::input::BindingKind::Simple;
    binding.component_count = 1;
    binding.components[0].control = cy::input::key_control(key);
    binding.components[0].weight = Vec3{1.0F, 0.0F, 0.0F};
    binding.trigger.kind = cy::input::TriggerKind::Down;
    return binding;
}

/// Four keys as one two-dimensional axis: x is left and right, y is down and up.
[[nodiscard]] cy::input::Binding axis2_binding(cy::input::ActionId action, cy::input::Key left,
                                               cy::input::Key right, cy::input::Key down,
                                               cy::input::Key up) noexcept {
    using cy::input::key_control;
    cy::input::Binding binding;
    binding.action = action;
    binding.kind = cy::input::BindingKind::Axis2D;
    binding.component_count = 4;
    binding.components[0] = {key_control(left), Vec3{-1.0F, 0.0F, 0.0F}};
    binding.components[1] = {key_control(right), Vec3{1.0F, 0.0F, 0.0F}};
    binding.components[2] = {key_control(down), Vec3{0.0F, -1.0F, 0.0F}};
    binding.components[3] = {key_control(up), Vec3{0.0F, 1.0F, 0.0F}};
    binding.trigger.kind = cy::input::TriggerKind::Down;
    return binding;
}

[[nodiscard]] cy::Expected<cy::input::ActionId, cy::Error> declare(
    cy::input::InputServer& input, const char* name, u32 stable,
    cy::input::ActionValueType type) noexcept {
    cy::input::ActionDeclaration declaration;
    declaration.name = cy::Name::intern(name);
    declaration.stable_id = cy::input::ActionStableId{stable};
    declaration.type = type;
    return input.actions().declare(declaration);
}

const cy::scene::NodeDesc kWorkerNodes[] = {
    cy::scene::NodeDesc{cy::Name::intern("Worker"), cy::scene::NodeDesc::kNoParent, cy::Name{},
                        cy::Transform::identity(), true, true, cy::Name{}, cy::Name{}},
};

}  // namespace

cy::navigation::NavTileData make_ground_tile(cy::Allocator& allocator) noexcept {
    cy::navigation::NavTileData data(allocator);
    data.coord = cy::navigation::TileCoord{0, 0, 0};
    const f32 step = kLevelSize / static_cast<f32>(kNavCells);
    for (u32 row = 0; row <= kNavCells; ++row) {
        for (u32 column = 0; column <= kNavCells; ++column) {
            (void)data.vertices().push_back(
                Vec3{static_cast<f32>(column) * step, 0.0F, static_cast<f32>(row) * step});
        }
    }
    const auto vertex_of = [](u32 row, u32 column) noexcept {
        return (row * (kNavCells + 1U)) + column;
    };
    for (u32 row = 0; row < kNavCells; ++row) {
        for (u32 column = 0; column < kNavCells; ++column) {
            cy::navigation::NavPoly poly;
            poly.first_corner = static_cast<u32>(data.corners().size());
            poly.corner_count = 4;
            (void)data.polys().push_back(poly);
            const u32 corners[4] = {vertex_of(row, column), vertex_of(row, column + 1U),
                                    vertex_of(row + 1U, column + 1U), vertex_of(row + 1U, column)};
            for (const u32 corner : corners) {
                (void)data.corners().push_back(corner);
            }
        }
    }
    data.finalise();
    return data;
}

cy::scene::SceneDescription worker_prefab() noexcept {
    return cy::scene::SceneDescription{cy::Name::intern(kWorkerPrefab),
                                       cy::Span<const cy::scene::NodeDesc>(kWorkerNodes)};
}

cy::Status make_click(cy::Array<f32>& out, u32 sample_rate) noexcept {
    const u32 frames = sample_rate / 5U;  // 200 ms
    if (cy::Status sized = out.resize(frames); !sized) {
        return sized;
    }
    // A fixed linear congruential sequence under an exponential decay: the same bytes everywhere.
    u32 state = 0x5EED0013U;
    f32 envelope = 0.5F;
    const f32 decay = 1.0F - (12.0F / static_cast<f32>(sample_rate));
    for (f32& sample : out) {
        state = (state * 1664525U) + 1013904223U;
        const f32 noise = (static_cast<f32>(state >> 8U) / 8388608.0F) - 1.0F;
        sample = noise * envelope;
        envelope *= decay;
    }
    return cy::ok();
}

cy::Status declare_input(cy::input::InputServer& input, cy::Allocator& allocator) noexcept {
    const auto pan = declare(input, kPanAction, 1, cy::input::ActionValueType::Axis2);
    if (!pan) {
        return cy::make_unexpected(pan.error());
    }
    const auto spawn = declare(input, kSpawnAction, 2, cy::input::ActionValueType::Digital);
    if (!spawn) {
        return cy::make_unexpected(spawn.error());
    }
    if (cy::Status finalized = input.finalize_declarations(); !finalized) {
        return finalized;
    }

    using cy::input::Key;
    cy::input::MappingContext context(allocator);
    context.set_name(cy::Name::intern("rts"));
    const cy::input::Binding bindings[] = {
        axis2_binding(*pan, Key::A, Key::D, Key::S, Key::W),
        axis2_binding(*pan, Key::Left, Key::Right, Key::Down, Key::Up),
        key_binding(*spawn, Key::B),
    };
    for (const cy::input::Binding& binding : bindings) {
        if (cy::Status added = context.add(binding); !added) {
            return added;
        }
    }
    const auto registered = input.register_context(std::move(context));
    if (!registered) {
        return cy::make_unexpected(registered.error());
    }
    const auto pushed = input.user(0).push_context(*registered, 0, cy::input::FocusLayer::Gameplay);
    if (!pushed) {
        return cy::make_unexpected(pushed.error());
    }
    return cy::ok();
}

}  // namespace sample::rts
