#pragma once
// The Jolt include block, the type conversions, and the object-layer encoding every file here
// shares. Task 4.2.2.
//
// ONE PLACE THAT INCLUDES JOLT. `Jolt/Jolt.h` must be the first Jolt header in every translation
// unit — it defines the configuration macros the rest of the library reads — and a file that got
// the order wrong compiles into a different ABI than its neighbours. Including it from here makes
// the order impossible to get wrong.

#include <Jolt/Jolt.h>
// clang-format off
// Everything below must come after Jolt/Jolt.h. clang-format sorts includes within a block, so the
// block is fenced rather than reordered — the alternative is a blank line between every pair, which
// looks like an accident.
#include <Jolt/Core/JobSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/PhysicsSystem.h>
// clang-format on

#include <cy/core/base/types.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/transform.h>
#include <cy/core/math/vec.h>
#include <cy/servers/physics/types.h>

namespace cy::physics::jolt {

// --- Vector conversions -------------------------------------------------------------------------
//
// Written as free functions rather than as a reinterpret_cast: `JPH::Vec3` is sixteen bytes with a
// padding lane, `cy::Vec3` is twelve, and a cast between them would be correct on a good day and
// silently read one float past the end on every other one.

[[nodiscard]] inline JPH::Vec3 to_jolt(Vec3 v) noexcept {
    return {v.x, v.y, v.z};
}

[[nodiscard]] inline Vec3 from_jolt(JPH::Vec3Arg v) noexcept {
    return Vec3{v.GetX(), v.GetY(), v.GetZ()};
}

#if defined(JPH_DOUBLE_PRECISION)
/// `RVec3` is a distinct type only in a double-precision build. Guarded rather than written
/// unconditionally, because in single precision `RVec3Arg` IS `Vec3Arg` and the two overloads would
/// be one function defined twice — which is a compile error, and the right one: it says the
/// conversion is a property of the build, not of the call site.
[[nodiscard]] inline Vec3 from_jolt(JPH::RVec3Arg v) noexcept {
    return Vec3{static_cast<f32>(v.GetX()), static_cast<f32>(v.GetY()), static_cast<f32>(v.GetZ())};
}
#endif

[[nodiscard]] inline JPH::Quat to_jolt(const Quat& q) noexcept {
    return {q.x, q.y, q.z, q.w};
}

[[nodiscard]] inline Quat from_jolt(JPH::QuatArg q) noexcept {
    return Quat{q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
}

// --- Object layers -------------------------------------------------------------------------------
//
// `physics` — "Jolt as the 3D backend": "collision layers and masks map to Jolt's broad-phase
// layers and object layer filters".
//
// THE ENCODING, AND WHY IT IS NOT THE OBVIOUS ONE. Jolt needs two things out of a layer: which
// broad-phase tree a body lives in (moving or not — this is what makes a world of static geometry
// cheap), and whether two layers may interact at all. The engine's `CollisionFilter` answers the
// second and says nothing about the first, so an object layer here is the engine layer shifted up
// one bit with "is this body moving" in bit 0. Thirty-two engine layers become sixty-four object
// layers, which is well inside Jolt's 16-bit `ObjectLayer`.
//
// The engine's per-body MASK is not representable in an object layer at all — it is a property of
// the body, not of its layer — so it is applied in the contact listener's validate callback, where
// both bodies are in hand. See jolt_server.cpp.

inline constexpr JPH::uint kBroadPhaseNonMoving = 0;
inline constexpr JPH::uint kBroadPhaseMoving = 1;
inline constexpr JPH::uint kBroadPhaseLayerCount = 2;

[[nodiscard]] inline JPH::ObjectLayer object_layer(u8 layer, bool moving) noexcept {
    return static_cast<JPH::ObjectLayer>((static_cast<JPH::ObjectLayer>(layer) << 1U) |
                                         (moving ? 1U : 0U));
}

[[nodiscard]] inline u8 engine_layer(JPH::ObjectLayer layer) noexcept {
    return static_cast<u8>(layer >> 1U);
}

[[nodiscard]] inline bool layer_is_moving(JPH::ObjectLayer layer) noexcept {
    return (layer & 1U) != 0U;
}

/// Two broad-phase layers: everything static in one tree, everything that moves in the other.
class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override {
        return kBroadPhaseLayerCount;
    }

    [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>(
            layer_is_moving(layer) ? kBroadPhaseMoving : kBroadPhaseNonMoving));
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    [[nodiscard]] const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return static_cast<JPH::BroadPhaseLayer::Type>(layer) == kBroadPhaseMoving ? "moving"
                                                                                   : "non-moving";
    }
#endif
};

/// Which broad-phase trees an object layer has to be tested against. A static body is tested only
/// against the moving tree, which is what makes a world of static geometry cost nothing per step.
class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer,
                                     JPH::BroadPhaseLayer broad_phase) const override {
        if (layer_is_moving(layer)) {
            return true;
        }
        return static_cast<JPH::BroadPhaseLayer::Type>(broad_phase) == kBroadPhaseMoving;
    }
};

/// The project's collision matrix, as Jolt's object-layer pair filter.
///
/// The matrix half only. The per-body masks are the contact listener's, because an object layer
/// cannot carry them — see the encoding note above.
class ObjectPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    void set_matrix(const CollisionMatrix& matrix) noexcept { matrix_ = matrix; }

    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (!layer_is_moving(a) && !layer_is_moving(b)) {
            return false;
        }
        return matrix_.allows(engine_layer(a), engine_layer(b));
    }

private:
    CollisionMatrix matrix_;
};

}  // namespace cy::physics::jolt
