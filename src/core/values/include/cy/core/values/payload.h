#pragma once
// The math shapes a `Var` carries, and the seam that carries an engine math type across.
// Task 1.3.1.
//
// `core-type-system` lists Vec2/3/4, IVec2/3/4, Quat, Mat3, Mat4, Transform, Color, Aabb, Rect and
// Plane among the kinds a `Var` covers. `core-math` (task 3.1.1) owns the types those names refer
// to in engine code — the ones with operators, SIMD paths and a scalar reference.
//
// These are not those types, and they are deliberately spelled differently so they cannot be
// mistaken for them. They are the **boundary shapes**: plain aggregates of f32 and i32 with a
// stated layout, which is what a value crossing to script, to disk or to the wire actually is. A
// `Var` cannot hold `cy::Vec3` without values depending on math, and values sits below math in
// nothing but alphabetical order — the dependency would be legal and still wrong, because it would
// put a SIMD type's alignment inside a 16-byte dynamic value.
//
// `var_payload_cast` is the seam. An engine math type that is trivially copyable and the same size
// crosses in one memcpy, checked at compile time; when `core-math` lands, a `Var` round-trip for
// `cy::Vec3` is one call in each direction and no allocation.
//
// CONVENTIONS. design.md section 4 fixes them and `core-math` tests them: right-handed, Y-up, -Z
// forward, metres, seconds, radians, and **column-major matrices with column vectors**. So
// `VarMat4::columns[c][r]` — element `r` of column `c` — is the layout, and it is the layout a
// generated serializer and the editor will both read.

#include <cy/core/base/types.h>

#include <cstring>
#include <type_traits>

namespace cy {

struct VarVec2 {
    f32 x = 0.0f, y = 0.0f;
};
struct VarVec3 {
    f32 x = 0.0f, y = 0.0f, z = 0.0f;
};
struct VarVec4 {
    f32 x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};

struct VarIVec2 {
    i32 x = 0, y = 0;
};
struct VarIVec3 {
    i32 x = 0, y = 0, z = 0;
};
struct VarIVec4 {
    i32 x = 0, y = 0, z = 0, w = 0;
};

/// A rotation, stored x, y, z, w with w last — the order the GPU, glTF and every serializer in the
/// engine's path use. Identity is (0, 0, 0, 1).
struct VarQuat {
    f32 x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
};

/// Linear RGBA. Colour space is a property of the field, not of the value: an `AssetRef` to a
/// texture and a `Color` field both carry it as an attribute (`core-type-system`, field
/// attributes).
struct VarColor {
    f32 r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};

/// A 2D rectangle, position and size. Size is never negative; a normalised rectangle is the
/// caller's job, because the boundary value is whatever was authored.
struct VarRect {
    f32 x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
};

/// A plane in Hessian normal form: `dot(normal, p) + distance == 0`.
struct VarPlane {
    f32 nx = 0.0f, ny = 0.0f, nz = 0.0f, distance = 0.0f;
};

/// An axis-aligned box. 24 bytes, so it is one of the shapes a `Var` keeps on the heap.
struct VarAabb {
    VarVec3 min;
    VarVec3 max;
};

/// Column-major, column vectors: `columns[c][r]`.
struct VarMat3 {
    f32 columns[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
};
struct VarMat4 {
    f32 columns[4][4] = {{1.0f, 0.0f, 0.0f, 0.0f},
                         {0.0f, 1.0f, 0.0f, 0.0f},
                         {0.0f, 0.0f, 1.0f, 0.0f},
                         {0.0f, 0.0f, 0.0f, 1.0f}};
};

/// Translation, rotation, scale — the authored form, which is what crosses a boundary. A composed
/// matrix is derived from it and is not what a scene file stores.
struct VarTransform {
    VarVec3 translation;
    VarQuat rotation;
    VarVec3 scale{1.0f, 1.0f, 1.0f};
};

// The inline payload is 16 bytes (see var.h). These are the shapes that fit in it, checked here so
// that a change to one of them shows up as a failure at the definition rather than as a silently
// heap-allocated `Var` somewhere else.
static_assert(sizeof(VarVec4) <= 16 && sizeof(VarIVec4) <= 16 && sizeof(VarQuat) <= 16);
static_assert(sizeof(VarColor) <= 16 && sizeof(VarRect) <= 16 && sizeof(VarPlane) <= 16);
static_assert(sizeof(VarAabb) == 24, "Aabb does not fit the inline payload and lives on the heap");
static_assert(sizeof(VarMat3) == 36 && sizeof(VarMat4) == 64);

/// Carry a layout-compatible external type — an engine math type, a GPU-facing struct, a binding's
/// own vector — into a `Var` payload shape, or back out of one.
///
/// The three static_asserts are the whole of the check, and they are why this is a cast rather than
/// a reinterpret: a type that is not trivially copyable, or is a different size, or wants stricter
/// alignment than the payload gives it, fails to compile at the call site that tried.
template <class Payload, class T>
[[nodiscard]] Payload var_payload_cast(const T& value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>,
                  "only a trivially copyable type crosses into a Var payload");
    static_assert(sizeof(T) == sizeof(Payload),
                  "the external type and the Var payload shape must be the same size");
    static_assert(alignof(Payload) <= alignof(T) || alignof(Payload) <= alignof(std::max_align_t));
    Payload out{};
    // Through void*: both types have default member initialisers, so neither is *trivially* default
    // constructible, and -Wclass-memaccess objects to a raw memcpy between two such classes even
    // when both are trivially copyable — which is the property that actually matters here and is
    // asserted above.
    std::memcpy(static_cast<void*>(&out), static_cast<const void*>(&value), sizeof(out));
    return out;
}

}  // namespace cy
