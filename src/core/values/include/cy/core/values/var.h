#pragma once
// `Var` — the dynamic value. Task 1.3.1.
//
// `core-type-system` — "Dynamic value type": a tagged dynamic value used **only at boundaries** —
// a scripting call, serialized text, an editor edit, a network payload — and never for per-entity
// runtime storage. Gameplay data is a typed component field. A `Var` appears when a value crosses
// to script, to disk or to the wire, and it stops existing on the other side.
//
// THE SHAPE, AND WHY IT IS 24 BYTES. There is no RTTI, so the value carries its own tag: one byte
// of `VarType` and sixteen bytes of payload, aligned to eight. Sixteen is the number that makes the
// engine's small values free — every vector, every integer colour, a quaternion, a handle, an
// entity id and a 128-bit asset id are stored inline and copied by copying the object. The shapes
// that do not fit — a string, a byte buffer, an array, a dictionary, Mat3, Mat4, Transform, Aabb
// and a `Callable` — live in a reference-counted block, so copying one of those is an atomic
// increment and mutating a shared one detaches first. That is the copy-on-write the specification
// asks for, and it is why passing a `Var` by value costs three words.
//
// COERCION IS EXPLICIT. There is no implicit conversion between a `Var` and anything, in either
// direction: `as_int()` on a value holding a float fails rather than truncating, and
// `coerce_to_int()` performs the conversion and reports whether it narrowed. A dynamic value that
// silently narrows is the defect this API exists to make impossible.
//
// THE HEAP. Blocks are allocated through `detail::var_block_alloc`, which is one function in
// var.cpp. `core-memory-and-containers` (task 2.x) owns the pool the specification names; this is
// the seam it plugs into, and until it lands the allocation is a `new` and the counters in
// `values_diagnostics()` report how many blocks are live.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/values/asset_id.h>
#include <cy/core/values/handle.h>
#include <cy/core/values/name.h>
#include <cy/core/values/payload.h>

#include <span>
#include <string_view>
#include <vector>

namespace cy {

class Callable;
class Var;
class VarArray;
class VarDict;

/// The kinds a `Var` covers. The list is `core-type-system`'s, in its order; the numeric values are
/// not an interchange format — a serializer writes a `TypeId` from the identity manifest, not this.
enum class VarType : u8 {
    Nil = 0,
    Bool,
    Int,    ///< i64
    Float,  ///< f64
    String,
    Vec2,
    Vec3,
    Vec4,
    IVec2,
    IVec3,
    IVec4,
    Quat,
    Mat3,
    Mat4,
    Transform,
    Color,
    Aabb,
    Rect,
    Plane,
    Handle,
    EntityId,
    AssetId,
    Array,
    Dict,
    Bytes,
    Callable,
};

/// The enumerator's own spelling, for a diagnostic. Never null.
[[nodiscard]] const char* var_type_name(VarType type) noexcept;

/// Whether a kind is stored in a reference-counted block rather than inline.
[[nodiscard]] bool var_type_is_heap(VarType type) noexcept;

namespace detail {
struct VarBlock;
}  // namespace detail

class Var {
public:
    /// The nil value. A default-constructed `Var` holds nothing and allocates nothing.
    Var() noexcept = default;
    ~Var();

    Var(const Var& other) noexcept;
    Var(Var&& other) noexcept;
    Var& operator=(const Var& other) noexcept;
    Var& operator=(Var&& other) noexcept;

    // --- Construction ----------------------------------------------------------------------------
    //
    // Named factories rather than converting constructors, for two reasons. `Var(0)` against
    // overloads taking bool, i64 and f64 is ambiguous — every one of the three is a conversion of
    // the same rank — and a converting constructor would make a `Var` appear wherever one is
    // accepted, which is exactly the implicit boundary crossing this type is meant to be visible
    // at.

    [[nodiscard]] static Var nil() noexcept { return {}; }
    [[nodiscard]] static Var from_bool(bool value) noexcept;
    [[nodiscard]] static Var from_int(i64 value) noexcept;
    [[nodiscard]] static Var from_float(f64 value) noexcept;

    [[nodiscard]] static Var from_vec2(VarVec2 value) noexcept;
    [[nodiscard]] static Var from_vec3(VarVec3 value) noexcept;
    [[nodiscard]] static Var from_vec4(VarVec4 value) noexcept;
    [[nodiscard]] static Var from_ivec2(VarIVec2 value) noexcept;
    [[nodiscard]] static Var from_ivec3(VarIVec3 value) noexcept;
    [[nodiscard]] static Var from_ivec4(VarIVec4 value) noexcept;
    [[nodiscard]] static Var from_quat(VarQuat value) noexcept;
    [[nodiscard]] static Var from_color(VarColor value) noexcept;
    [[nodiscard]] static Var from_rect(VarRect value) noexcept;
    [[nodiscard]] static Var from_plane(VarPlane value) noexcept;

    [[nodiscard]] static Var from_mat3(const VarMat3& value) noexcept;
    [[nodiscard]] static Var from_mat4(const VarMat4& value) noexcept;
    [[nodiscard]] static Var from_transform(const VarTransform& value) noexcept;
    [[nodiscard]] static Var from_aabb(const VarAabb& value) noexcept;

    [[nodiscard]] static Var from_handle(AnyHandle value) noexcept;
    template <class Tag>
    [[nodiscard]] static Var from_handle(cy::Handle<Tag> value) noexcept {
        return from_handle(to_any(value));
    }
    [[nodiscard]] static Var from_entity(cy::EntityId value) noexcept;
    [[nodiscard]] static Var from_asset(cy::AssetId value) noexcept;

    /// Copies the text into the block. A `Var` never points at storage it does not own — the
    /// lifetime of a value that crossed a boundary has nothing to do with the lifetime of whatever
    /// produced it.
    [[nodiscard]] static Var from_string(std::string_view text) noexcept;
    [[nodiscard]] static Var from_bytes(const u8* data, usize size) noexcept;
    [[nodiscard]] static Var from_name(cy::Name name) noexcept;

    [[nodiscard]] static Var empty_array() noexcept;
    [[nodiscard]] static Var empty_dict() noexcept;
    [[nodiscard]] static Var from_array(const Var* values, usize count) noexcept;
    [[nodiscard]] static Var from_callable(const cy::Callable& callable) noexcept;

    // --- Inspection ------------------------------------------------------------------------------

    [[nodiscard]] VarType type() const noexcept { return type_; }
    [[nodiscard]] bool is_nil() const noexcept { return type_ == VarType::Nil; }
    [[nodiscard]] bool is(VarType type) const noexcept { return type_ == type; }
    [[nodiscard]] const char* type_name() const noexcept { return var_type_name(type_); }

    /// Whether this value shares its block with another. False for every inline kind. Exposed
    /// because "did that copy allocate?" is a question a test should be able to answer.
    [[nodiscard]] bool is_shared() const noexcept;

    // --- Typed access ----------------------------------------------------------------------------
    //
    // Every one of these fails on a type mismatch rather than converting. The error names both the
    // kind that was asked for and the kind that was there, because "expected Int, found String" is
    // the whole of what a caller at a boundary needs to report.

    [[nodiscard]] Expected<bool, Error> as_bool() const noexcept;
    [[nodiscard]] Expected<i64, Error> as_int() const noexcept;
    [[nodiscard]] Expected<f64, Error> as_float() const noexcept;

    [[nodiscard]] Expected<VarVec2, Error> as_vec2() const noexcept;
    [[nodiscard]] Expected<VarVec3, Error> as_vec3() const noexcept;
    [[nodiscard]] Expected<VarVec4, Error> as_vec4() const noexcept;
    [[nodiscard]] Expected<VarIVec2, Error> as_ivec2() const noexcept;
    [[nodiscard]] Expected<VarIVec3, Error> as_ivec3() const noexcept;
    [[nodiscard]] Expected<VarIVec4, Error> as_ivec4() const noexcept;
    [[nodiscard]] Expected<VarQuat, Error> as_quat() const noexcept;
    [[nodiscard]] Expected<VarColor, Error> as_color() const noexcept;
    [[nodiscard]] Expected<VarRect, Error> as_rect() const noexcept;
    [[nodiscard]] Expected<VarPlane, Error> as_plane() const noexcept;

    [[nodiscard]] Expected<VarMat3, Error> as_mat3() const noexcept;
    [[nodiscard]] Expected<VarMat4, Error> as_mat4() const noexcept;
    [[nodiscard]] Expected<VarTransform, Error> as_transform() const noexcept;
    [[nodiscard]] Expected<VarAabb, Error> as_aabb() const noexcept;

    [[nodiscard]] Expected<AnyHandle, Error> as_handle() const noexcept;
    template <class Tag>
    [[nodiscard]] Expected<cy::Handle<Tag>, Error> as_handle() const noexcept {
        const Expected<AnyHandle, Error> erased = as_handle();
        if (!erased) {
            return make_unexpected(erased.error());
        }
        return from_any<Tag>(*erased);
    }
    [[nodiscard]] Expected<cy::EntityId, Error> as_entity() const noexcept;
    [[nodiscard]] Expected<cy::AssetId, Error> as_asset() const noexcept;

    /// The text, valid while this `Var` — or another sharing its block — is alive. NUL-terminated.
    [[nodiscard]] Expected<std::string_view, Error> as_string() const noexcept;

    /// The bytes, valid on the same terms as `as_string()`. `size` is set even when the buffer is
    /// empty, so a caller does not have to distinguish empty from absent.
    [[nodiscard]] Expected<const u8*, Error> as_bytes(usize& size) const noexcept;

    /// The array's elements. Null when this value is not an array; `var_array()` below is the
    /// spelling that hands back a span once `Var` is a complete type.
    [[nodiscard]] const Var* array_data() const noexcept;
    [[nodiscard]] usize array_size() const noexcept;

    [[nodiscard]] const VarDict* dict() const noexcept;
    [[nodiscard]] const cy::Callable* callable() const noexcept;

    // --- Mutation, with copy-on-write ------------------------------------------------------------
    //
    // Each detaches from a shared block first, so a mutation through one `Var` is never visible
    // through a copy that was taken before it. A detach is counted, so a hot loop that is
    // accidentally detaching on every iteration is visible in `values_diagnostics()`.

    [[nodiscard]] Expected<VarArray*, Error> array_mut() noexcept;
    [[nodiscard]] Expected<VarDict*, Error> dict_mut() noexcept;

    friend bool operator==(const Var& a, const Var& b) noexcept;
    friend bool operator!=(const Var& a, const Var& b) noexcept { return !(a == b); }

private:
    void retain() noexcept;
    void release() noexcept;
    [[nodiscard]] detail::VarBlock* block() const noexcept;
    void set_block(detail::VarBlock* block, VarType type) noexcept;
    [[nodiscard]] Status detach() noexcept;

    /// Read an inline payload of type `T`, or fail naming both kinds.
    template <class T>
    [[nodiscard]] Expected<T, Error> read_inline(VarType expected) const noexcept;

    template <class T>
    [[nodiscard]] static Var make_inline(VarType type, const T& value) noexcept;

    /// The payload, as bytes. Bytes rather than a union: every kind stored here is trivially
    /// copyable, so a memcpy in and a memcpy out is the whole of the access, and there is then no
    /// active-member rule to get wrong across an assignment that changes the kind.
    alignas(8) unsigned char storage_[16] = {};
    VarType type_ = VarType::Nil;
};

static_assert(sizeof(Var) == 24, "a Var is a tag and sixteen bytes of payload; it is copied a lot");

/// An array of values. Boundary-sized: an argument list, a serialized sequence, a script array.
///
/// Defined after `Var` because it holds `Var`s by value, which needs the complete type. It is a
/// thin wrapper over a `std::vector` rather than the vector itself, so that when
/// `core-memory-and-containers` lands its allocator-aware sequence container (task 2.4) the change
/// is to this class and not to every caller.
class VarArray {
public:
    VarArray() noexcept = default;

    [[nodiscard]] usize size() const noexcept { return items_.size(); }
    [[nodiscard]] bool is_empty() const noexcept { return items_.empty(); }
    [[nodiscard]] const Var* data() const noexcept { return items_.data(); }
    [[nodiscard]] Var* data() noexcept { return items_.data(); }

    /// Bounds are the caller's responsibility, as they are for any sequence container; an index
    /// that came from outside the process is checked by whatever accepted it, not here.
    [[nodiscard]] const Var& operator[](usize index) const noexcept { return items_[index]; }
    [[nodiscard]] Var& operator[](usize index) noexcept { return items_[index]; }

    Status push(const Var& value) noexcept;
    Status resize(usize count) noexcept;
    void clear() noexcept { items_.clear(); }

    [[nodiscard]] const Var* begin() const noexcept { return items_.data(); }
    [[nodiscard]] const Var* end() const noexcept { return items_.data() + items_.size(); }
    [[nodiscard]] Var* begin() noexcept { return items_.data(); }
    [[nodiscard]] Var* end() noexcept { return items_.data() + items_.size(); }

    friend bool operator==(const VarArray& a, const VarArray& b) noexcept;

private:
    std::vector<Var> items_;
};

/// One named entry of a `VarDict`.
struct VarDictEntry {
    cy::Name key;
    Var value;
};

/// A dictionary keyed by interned `Name`.
///
/// Entries are kept in **insertion order** and looked up linearly. That is the right structure for
/// what a boundary dictionary actually is — a handful of named fields on their way to or from a
/// script call, an editor edit or a serializer — and a `Name` comparison is one integer compare, so
/// a linear scan of a dozen entries is a handful of instructions.
///
/// Insertion order is also the only *deterministic* order available: a hash order would make a
/// serialized dictionary's field order depend on the allocator, and an order by `Name` index would
/// make it depend on what the process happened to intern first.
class VarDict {
public:
    VarDict() noexcept = default;

    [[nodiscard]] usize size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool is_empty() const noexcept { return entries_.empty(); }

    [[nodiscard]] const Var* find(cy::Name key) const noexcept;
    [[nodiscard]] Var* find(cy::Name key) noexcept;
    [[nodiscard]] bool contains(cy::Name key) const noexcept { return find(key) != nullptr; }

    /// Insert or replace. Replacing keeps the entry's original position, so setting a field twice
    /// does not reorder a dictionary that is about to be written out.
    Status set(cy::Name key, const Var& value) noexcept;
    bool erase(cy::Name key) noexcept;
    void clear() noexcept { entries_.clear(); }

    [[nodiscard]] const VarDictEntry* begin() const noexcept { return entries_.data(); }
    [[nodiscard]] const VarDictEntry* end() const noexcept {
        return entries_.data() + entries_.size();
    }

    friend bool operator==(const VarDict& a, const VarDict& b) noexcept;

private:
    std::vector<VarDictEntry> entries_;
};

/// The array's elements as a span, once `Var` is complete. Empty when the value is not an array.
[[nodiscard]] std::span<const Var> var_array(const Var& value) noexcept;

// --- Explicit coercion
// ----------------------------------------------------------------------------
//
// `core-type-system` — "Type coercion is explicit": assigning a `Var` holding a Float to an Int
// field goes through an API that *reports narrowing*, never silently. `narrowed` is the report: the
// conversion still happened and the value is still returned, so a caller that has decided narrowing
// is acceptable does not have to reimplement the conversion to get it.

struct IntCoercion {
    i64 value = 0;
    bool narrowed = false;  ///< a fractional part was discarded, or the magnitude did not fit
};

struct FloatCoercion {
    f64 value = 0.0;
    bool narrowed = false;  ///< the integer was not exactly representable as an f64
};

[[nodiscard]] Expected<IntCoercion, Error> coerce_to_int(const Var& value) noexcept;
[[nodiscard]] Expected<FloatCoercion, Error> coerce_to_float(const Var& value) noexcept;
/// Nil is false, Bool is itself, Int and Float are non-zero. Nothing else coerces: the truthiness
/// of a string or of an empty array is a language's convention, not the engine's.
[[nodiscard]] Expected<bool, Error> coerce_to_bool(const Var& value) noexcept;

}  // namespace cy
