// SPDX-License-Identifier: MIT
// The courtyard's pieces and the input. See level.h.

#include "level.h"

#include <cmath>
#include <utility>

namespace sample::tps {
namespace {

using cy::f32;
using cy::u32;
using cy::Vec3;

constexpr Vec3 kStone{0.62F, 0.59F, 0.53F};
constexpr Vec3 kCap{0.52F, 0.50F, 0.46F};

/// The stairs' numbers: eight risers up to the platform's top.
constexpr u32 kSteps = 8;
constexpr f32 kRiser = 0.18F;
constexpr f32 kTread = 0.32F;
constexpr f32 kStairsFront = 4.0F;
constexpr f32 kStairsHalfWidth = 1.5F;
constexpr f32 kPlatformTop = kRiser * static_cast<f32>(kSteps);
constexpr f32 kPlatformFront = kStairsFront - (kTread * static_cast<f32>(kSteps));
constexpr f32 kPlatformBack = -6.0F;
constexpr f32 kPlatformHalfWidth = 5.0F;
/// The ramp runs from the platform's east edge down to the courtyard.
constexpr f32 kRampFoot = 13.0F;
constexpr f32 kRampNorth = -5.5F;
constexpr f32 kRampSouth = -2.5F;
constexpr f32 kRampThickness = 0.3F;

[[nodiscard]] LevelPiece box(const char* name, Vec3 low, Vec3 high, const char* material,
                             Vec3 albedo = kStone) {
    LevelPiece piece;
    piece.name = name;
    piece.centre = (low + high) * 0.5F;
    piece.half = (high - low) * 0.5F;
    piece.material = material;
    piece.albedo = albedo;
    return piece;
}

/// The ramp: a slab whose top face runs from the platform's edge down to the courtyard.
[[nodiscard]] LevelPiece ramp() {
    const f32 run = kRampFoot - kPlatformHalfWidth;
    const f32 slope = std::atan2(kPlatformTop, run);
    const f32 length = std::sqrt((run * run) + (kPlatformTop * kPlatformTop));
    const Vec3 top_middle{(kPlatformHalfWidth + kRampFoot) * 0.5F, kPlatformTop * 0.5F,
                          (kRampNorth + kRampSouth) * 0.5F};
    // The slab's up, turned by the slope about +Z; its centre half its thickness below the top.
    const Vec3 up{std::sin(slope), std::cos(slope), 0.0F};
    LevelPiece piece;
    piece.name = "ramp";
    piece.centre = top_middle - (up * (kRampThickness * 0.5F));
    piece.half = Vec3{length * 0.5F, kRampThickness * 0.5F, (kRampSouth - kRampNorth) * 0.5F};
    piece.roll = -slope;
    piece.material = "flagstone";
    piece.albedo = kStone;
    return piece;
}

[[nodiscard]] std::vector<LevelPiece> build_pieces() {
    std::vector<LevelPiece> pieces;
    // The flagstones, as a collider only: the terrain draws them.
    // A centimetre over the terrain's flattened courtyard, so the character stands on the stone
    // and never on the terrain beneath it where the two would meet in one plane.
    LevelPiece floor = box(kFloorPiece, Vec3{-14.0F, -0.4F, -16.0F}, Vec3{14.0F, 0.01F, 14.0F}, "");
    pieces.push_back(floor);
    for (u32 step = 0; step < kSteps; ++step) {
        const f32 front = kStairsFront - (kTread * static_cast<f32>(step));
        pieces.push_back(box("stairs", Vec3{-kStairsHalfWidth, 0.0F, front - kTread},
                             Vec3{kStairsHalfWidth, kRiser * static_cast<f32>(step + 1U), front},
                             "flagstone"));
    }
    pieces.push_back(box("platform", Vec3{-kPlatformHalfWidth, 0.0F, kPlatformBack},
                         Vec3{kPlatformHalfWidth, kPlatformTop, kPlatformFront}, "flagstone"));
    pieces.push_back(box("parapet", Vec3{-kPlatformHalfWidth, kPlatformTop, kPlatformBack},
                         Vec3{-kPlatformHalfWidth + 0.35F, kPlatformTop + 0.6F, kPlatformFront},
                         "capstone", kCap));
    for (const f32 x : {-4.4F, 4.4F}) {
        pieces.push_back(box("pillar", Vec3{x - 0.3F, kPlatformTop, kPlatformFront - 0.7F},
                             Vec3{x + 0.3F, kPlatformTop + 2.4F, kPlatformFront - 0.1F},
                             "stone_wall"));
    }
    pieces.push_back(ramp());
    pieces.push_back(box("north_wall", Vec3{-7.0F, 0.0F, kPlatformBack - 0.6F},
                         Vec3{7.0F, 4.4F, kPlatformBack}, "stone_wall"));
    // The courtyard's walls, open at the arch (south) and at the ramp's foot (east).
    const struct {
        Vec3 low;
        Vec3 high;
    } walls[] = {
        {Vec3{-14.5F, 0.0F, -16.5F}, Vec3{14.5F, 1.6F, -16.0F}},
        {Vec3{-14.5F, 0.0F, -16.0F}, Vec3{-14.0F, 1.6F, 14.0F}},
        {Vec3{14.0F, 0.0F, -16.0F}, Vec3{14.5F, 1.6F, -6.0F}},
        {Vec3{14.0F, 0.0F, 0.0F}, Vec3{14.5F, 1.6F, 14.0F}},
        {Vec3{-14.5F, 0.0F, 14.0F}, Vec3{-2.4F, 1.6F, 14.5F}},
        {Vec3{2.4F, 0.0F, 14.0F}, Vec3{14.5F, 1.6F, 14.5F}},
    };
    for (const auto& wall : walls) {
        pieces.push_back(box("wall", wall.low, wall.high, "stone_wall"));
        pieces.push_back(box("coping", Vec3{wall.low.x - 0.05F, wall.high.y, wall.low.z - 0.05F},
                             Vec3{wall.high.x + 0.05F, wall.high.y + 0.12F, wall.high.z + 0.05F},
                             "capstone", kCap));
    }
    // The arch over the south gate.
    for (const f32 x : {-2.4F, 2.4F}) {
        pieces.push_back(
            box("arch", Vec3{x - 0.4F, 0.0F, 13.8F}, Vec3{x + 0.4F, 3.3F, 14.7F}, "stone_wall"));
    }
    pieces.push_back(
        box("arch", Vec3{-2.9F, 3.3F, 13.7F}, Vec3{2.9F, 3.9F, 14.8F}, "capstone", kCap));
    return pieces;
}

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

/// Four keys as one two-dimensional axis: x is left and right, y is back and forward.
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

}  // namespace

const std::vector<LevelPiece>& level_pieces() {
    static const std::vector<LevelPiece> kPieces = build_pieces();
    return kPieces;
}

cy::Status declare_input(cy::input::InputServer& input, cy::Allocator& allocator) noexcept {
    using cy::input::ActionValueType;
    const auto move = declare(input, kMoveAction, 1, ActionValueType::Axis2);
    const auto look = declare(input, kLookAction, 2, ActionValueType::Axis2);
    const auto jump = declare(input, kJumpAction, 3, ActionValueType::Digital);
    const auto sprint = declare(input, kSprintAction, 4, ActionValueType::Digital);
    if (!move || !look || !jump || !sprint) {
        return cy::fail(cy::ErrorCode::InvalidArgument, "an input action was refused");
    }
    if (cy::Status finalized = input.finalize_declarations(); !finalized) {
        return finalized;
    }
    using cy::input::Key;
    cy::input::MappingContext context(allocator);
    context.set_name(cy::Name::intern("third-person"));
    const cy::input::Binding bindings[] = {
        axis2_binding(*move, Key::A, Key::D, Key::S, Key::W),
        axis2_binding(*look, Key::Left, Key::Right, Key::Down, Key::Up),
        key_binding(*jump, Key::Space),
        key_binding(*sprint, Key::LeftShift),
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

}  // namespace sample::tps
