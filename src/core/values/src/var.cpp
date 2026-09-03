// `Var` — construction, the reference-counted heap, copy-on-write and coercion. Task 1.3.1.

#include <cy/core/values/var.h>

#include <cy/core/base/assert.h>

#include <cy/core/values/callable.h>

#include "counters.h"
#include "var_block.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace cy {
namespace {

using detail::VarBlock;

/// The exact number of payload bytes a kind occupies inline. Equality and hashing compare this many
/// rather than all sixteen, so a kind smaller than the payload is not compared against whatever the
/// tail happens to hold. (`make_inline` zeroes the tail, so the two agree — but a comparison that
/// depends on that is a comparison that breaks the first time someone writes a payload another
/// way.)
usize inline_size(VarType type) noexcept {
    switch (type) {
        case VarType::Nil:
            return 0;
        case VarType::Bool:
            return sizeof(bool);
        case VarType::Int:
            return sizeof(i64);
        case VarType::Float:
            return sizeof(f64);
        case VarType::Vec2:
            return sizeof(VarVec2);
        case VarType::Vec3:
            return sizeof(VarVec3);
        case VarType::Vec4:
            return sizeof(VarVec4);
        case VarType::IVec2:
            return sizeof(VarIVec2);
        case VarType::IVec3:
            return sizeof(VarIVec3);
        case VarType::IVec4:
            return sizeof(VarIVec4);
        case VarType::Quat:
            return sizeof(VarQuat);
        case VarType::Color:
            return sizeof(VarColor);
        case VarType::Rect:
            return sizeof(VarRect);
        case VarType::Plane:
            return sizeof(VarPlane);
        case VarType::Handle:
            return sizeof(AnyHandle);
        case VarType::EntityId:
            return sizeof(EntityId);
        case VarType::AssetId:
            return sizeof(AssetId);
        default:
            return 0;  // a heap kind: the payload is a block pointer, compared by content
    }
}

/// Allocate a block of the derived type `T`, with its kind already set. Null on allocation failure,
/// which every caller turns into a nil `Var` — a boundary value that could not be built is nil, and
/// the failure is visible in the block counters rather than as a crash halfway through a
/// serializer.
template <class T>
T* allocate_block(VarType type) noexcept {
    T* block = new (std::nothrow) T();
    if (block == nullptr) {
        return nullptr;
    }
    block->type = type;
    values::detail::bump(values::detail::counters().var_blocks_allocated);
    return block;
}

void destroy_block(VarBlock* block) noexcept {
    // One switch, in one place. Deleting through the base pointer is what a virtual destructor
    // would be for, and a virtual destructor would put a vtable pointer in every block.
    switch (block->type) {
        case VarType::String:
            delete static_cast<detail::StringBlock*>(block);
            break;
        case VarType::Bytes:
            delete static_cast<detail::BytesBlock*>(block);
            break;
        case VarType::Array:
            delete static_cast<detail::ArrayBlock*>(block);
            break;
        case VarType::Dict:
            delete static_cast<detail::DictBlock*>(block);
            break;
        case VarType::Mat3:
            delete static_cast<detail::Mat3Block*>(block);
            break;
        case VarType::Mat4:
            delete static_cast<detail::Mat4Block*>(block);
            break;
        case VarType::Transform:
            delete static_cast<detail::TransformBlock*>(block);
            break;
        case VarType::Aabb:
            delete static_cast<detail::AabbBlock*>(block);
            break;
        case VarType::Callable:
            delete static_cast<detail::CallableBlock*>(block);
            break;
        default:
            // Unreachable: only a heap kind ever owns a block, and every heap kind is above.
            CY_ASSERT_MSG(false, "Var block with an inline kind");
            break;
    }
    values::detail::bump(values::detail::counters().var_blocks_freed);
}

/// Copy a block's payload into a fresh block of the same kind. The copy-on-write step.
VarBlock* clone_block(const VarBlock* block) noexcept {
    switch (block->type) {
        case VarType::String: {
            auto* copy = allocate_block<detail::StringBlock>(VarType::String);
            if (copy != nullptr) {
                copy->text = static_cast<const detail::StringBlock*>(block)->text;
            }
            return copy;
        }
        case VarType::Bytes: {
            auto* copy = allocate_block<detail::BytesBlock>(VarType::Bytes);
            if (copy != nullptr) {
                copy->data = static_cast<const detail::BytesBlock*>(block)->data;
            }
            return copy;
        }
        case VarType::Array: {
            auto* copy = allocate_block<detail::ArrayBlock>(VarType::Array);
            if (copy != nullptr) {
                copy->items = static_cast<const detail::ArrayBlock*>(block)->items;
            }
            return copy;
        }
        case VarType::Dict: {
            auto* copy = allocate_block<detail::DictBlock>(VarType::Dict);
            if (copy != nullptr) {
                copy->dict = static_cast<const detail::DictBlock*>(block)->dict;
            }
            return copy;
        }
        default:
            // Mat3, Mat4, Transform, Aabb and Callable are immutable through the Var interface:
            // there is no accessor that hands out a mutable reference, so nothing ever detaches
            // one and this is not reached.
            CY_ASSERT_MSG(false, "Var kind has no mutable accessor and cannot need a detach");
            return nullptr;
    }
}

}  // namespace

const char* var_type_name(VarType type) noexcept {
    switch (type) {
        case VarType::Nil:
            return "Nil";
        case VarType::Bool:
            return "Bool";
        case VarType::Int:
            return "Int";
        case VarType::Float:
            return "Float";
        case VarType::String:
            return "String";
        case VarType::Vec2:
            return "Vec2";
        case VarType::Vec3:
            return "Vec3";
        case VarType::Vec4:
            return "Vec4";
        case VarType::IVec2:
            return "IVec2";
        case VarType::IVec3:
            return "IVec3";
        case VarType::IVec4:
            return "IVec4";
        case VarType::Quat:
            return "Quat";
        case VarType::Mat3:
            return "Mat3";
        case VarType::Mat4:
            return "Mat4";
        case VarType::Transform:
            return "Transform";
        case VarType::Color:
            return "Color";
        case VarType::Aabb:
            return "Aabb";
        case VarType::Rect:
            return "Rect";
        case VarType::Plane:
            return "Plane";
        case VarType::Handle:
            return "Handle";
        case VarType::EntityId:
            return "EntityId";
        case VarType::AssetId:
            return "AssetId";
        case VarType::Array:
            return "Array";
        case VarType::Dict:
            return "Dict";
        case VarType::Bytes:
            return "Bytes";
        case VarType::Callable:
            return "Callable";
    }
    return "Unknown";
}

bool var_type_is_heap(VarType type) noexcept {
    switch (type) {
        case VarType::String:
        case VarType::Bytes:
        case VarType::Array:
        case VarType::Dict:
        case VarType::Mat3:
        case VarType::Mat4:
        case VarType::Transform:
        case VarType::Aabb:
        case VarType::Callable:
            return true;
        default:
            return false;
    }
}

// --- Lifetime
// -------------------------------------------------------------------------------------

VarBlock* Var::block() const noexcept {
    VarBlock* pointer = nullptr;
    std::memcpy(&pointer, storage_, sizeof(pointer));
    return pointer;
}

void Var::set_block(VarBlock* new_block, VarType type) noexcept {
    std::memset(storage_, 0, sizeof(storage_));
    if (new_block == nullptr) {
        type_ = VarType::Nil;
        return;
    }
    std::memcpy(storage_, &new_block, sizeof(new_block));
    type_ = type;
}

void Var::retain() noexcept {
    if (!var_type_is_heap(type_)) {
        return;
    }
    if (VarBlock* held = block(); held != nullptr) {
        held->refs.fetch_add(1, std::memory_order_relaxed);
    }
}

void Var::release() noexcept {
    if (!var_type_is_heap(type_)) {
        return;
    }
    VarBlock* held = block();
    if (held == nullptr) {
        return;
    }
    // Acquire-release on the decrement: the thread that destroys the block must see every write
    // another thread made before it dropped its reference.
    //
    // The textbook form of this is a release decrement followed by an acquire fence on the last
    // one, which is marginally cheaper on a weakly ordered machine. It is not used here because
    // ThreadSanitizer does not model std::atomic_thread_fence — GCC refuses to compile one under
    // -fsanitize=thread outright — and a reference count the race detector cannot follow would put
    // a permanent blind spot in exactly the suite `core-jobs-and-concurrency` points TSan at.
    if (held->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        destroy_block(held);
    }
}

Var::~Var() {
    release();
}

Var::Var(const Var& other) noexcept : type_(other.type_) {
    std::memcpy(storage_, other.storage_, sizeof(storage_));
    retain();
}

Var::Var(Var&& other) noexcept : type_(other.type_) {
    std::memcpy(storage_, other.storage_, sizeof(storage_));
    std::memset(other.storage_, 0, sizeof(other.storage_));
    other.type_ = VarType::Nil;
}

Var& Var::operator=(const Var& other) noexcept {
    if (this == &other) {
        return *this;
    }
    // Retain before release: self-assignment through two Vars sharing one block would otherwise
    // destroy the block between the two steps.
    Var copy(other);
    release();
    std::memcpy(storage_, copy.storage_, sizeof(storage_));
    type_ = copy.type_;
    std::memset(copy.storage_, 0, sizeof(copy.storage_));
    copy.type_ = VarType::Nil;
    return *this;
}

Var& Var::operator=(Var&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    release();
    std::memcpy(storage_, other.storage_, sizeof(storage_));
    type_ = other.type_;
    std::memset(other.storage_, 0, sizeof(other.storage_));
    other.type_ = VarType::Nil;
    return *this;
}

bool Var::is_shared() const noexcept {
    if (!var_type_is_heap(type_)) {
        return false;
    }
    const VarBlock* held = block();
    return held != nullptr && held->refs.load(std::memory_order_relaxed) > 1;
}

Status Var::detach() noexcept {
    VarBlock* held = block();
    if (held == nullptr) {
        return fail(ErrorCode::Internal, "Var holds a heap kind with no block");
    }
    if (held->refs.load(std::memory_order_acquire) == 1) {
        return ok();
    }
    VarBlock* copy = clone_block(held);
    if (copy == nullptr) {
        return fail(ErrorCode::OutOfMemory, "Var copy-on-write allocation failed");
    }
    release();
    set_block(copy, copy->type);
    values::detail::bump(values::detail::counters().var_blocks_detached);
    return ok();
}

// --- Inline construction and access
// ---------------------------------------------------------------

template <class T>
Var Var::make_inline(VarType type, const T& value) noexcept {
    static_assert(sizeof(T) <= sizeof(Var::storage_), "payload does not fit inline");
    static_assert(std::is_trivially_copyable_v<T>);
    Var result;
    std::memcpy(result.storage_, &value, sizeof(T));
    result.type_ = type;
    return result;
}

template <class T>
Expected<T, Error> Var::read_inline(VarType expected) const noexcept {
    static_assert(sizeof(T) <= sizeof(Var::storage_));
    static_assert(std::is_trivially_copyable_v<T>);
    if (type_ != expected) {
        return fail(ErrorCode::InvalidArgument, "Var holds a different kind");
    }
    T value{};
    std::memcpy(&value, storage_, sizeof(T));
    return value;
}

Var Var::from_bool(bool value) noexcept {
    return make_inline(VarType::Bool, value);
}
Var Var::from_int(i64 value) noexcept {
    return make_inline(VarType::Int, value);
}
Var Var::from_float(f64 value) noexcept {
    return make_inline(VarType::Float, value);
}
Var Var::from_vec2(VarVec2 value) noexcept {
    return make_inline(VarType::Vec2, value);
}
Var Var::from_vec3(VarVec3 value) noexcept {
    return make_inline(VarType::Vec3, value);
}
Var Var::from_vec4(VarVec4 value) noexcept {
    return make_inline(VarType::Vec4, value);
}
Var Var::from_ivec2(VarIVec2 value) noexcept {
    return make_inline(VarType::IVec2, value);
}
Var Var::from_ivec3(VarIVec3 value) noexcept {
    return make_inline(VarType::IVec3, value);
}
Var Var::from_ivec4(VarIVec4 value) noexcept {
    return make_inline(VarType::IVec4, value);
}
Var Var::from_quat(VarQuat value) noexcept {
    return make_inline(VarType::Quat, value);
}
Var Var::from_color(VarColor value) noexcept {
    return make_inline(VarType::Color, value);
}
Var Var::from_rect(VarRect value) noexcept {
    return make_inline(VarType::Rect, value);
}
Var Var::from_plane(VarPlane value) noexcept {
    return make_inline(VarType::Plane, value);
}
Var Var::from_handle(AnyHandle value) noexcept {
    return make_inline(VarType::Handle, value);
}
Var Var::from_entity(EntityId value) noexcept {
    return make_inline(VarType::EntityId, value);
}
Var Var::from_asset(AssetId value) noexcept {
    return make_inline(VarType::AssetId, value);
}

Expected<bool, Error> Var::as_bool() const noexcept {
    return read_inline<bool>(VarType::Bool);
}
Expected<i64, Error> Var::as_int() const noexcept {
    return read_inline<i64>(VarType::Int);
}
Expected<f64, Error> Var::as_float() const noexcept {
    return read_inline<f64>(VarType::Float);
}
Expected<VarVec2, Error> Var::as_vec2() const noexcept {
    return read_inline<VarVec2>(VarType::Vec2);
}
Expected<VarVec3, Error> Var::as_vec3() const noexcept {
    return read_inline<VarVec3>(VarType::Vec3);
}
Expected<VarVec4, Error> Var::as_vec4() const noexcept {
    return read_inline<VarVec4>(VarType::Vec4);
}
Expected<VarIVec2, Error> Var::as_ivec2() const noexcept {
    return read_inline<VarIVec2>(VarType::IVec2);
}
Expected<VarIVec3, Error> Var::as_ivec3() const noexcept {
    return read_inline<VarIVec3>(VarType::IVec3);
}
Expected<VarIVec4, Error> Var::as_ivec4() const noexcept {
    return read_inline<VarIVec4>(VarType::IVec4);
}
Expected<VarQuat, Error> Var::as_quat() const noexcept {
    return read_inline<VarQuat>(VarType::Quat);
}
Expected<VarColor, Error> Var::as_color() const noexcept {
    return read_inline<VarColor>(VarType::Color);
}
Expected<VarRect, Error> Var::as_rect() const noexcept {
    return read_inline<VarRect>(VarType::Rect);
}
Expected<VarPlane, Error> Var::as_plane() const noexcept {
    return read_inline<VarPlane>(VarType::Plane);
}
Expected<AnyHandle, Error> Var::as_handle() const noexcept {
    return read_inline<AnyHandle>(VarType::Handle);
}
Expected<EntityId, Error> Var::as_entity() const noexcept {
    return read_inline<EntityId>(VarType::EntityId);
}
Expected<AssetId, Error> Var::as_asset() const noexcept {
    return read_inline<AssetId>(VarType::AssetId);
}

// --- Heap kinds
// -----------------------------------------------------------------------------------

Var Var::from_string(std::string_view text) noexcept {
    auto* block = allocate_block<detail::StringBlock>(VarType::String);
    if (block == nullptr) {
        return Var();
    }
    block->text.assign(text.data(), text.size());
    Var result;
    result.set_block(block, VarType::String);
    return result;
}

Var Var::from_name(Name name) noexcept {
    return from_string(name.text());
}

Var Var::from_bytes(const u8* data, usize size) noexcept {
    auto* block = allocate_block<detail::BytesBlock>(VarType::Bytes);
    if (block == nullptr) {
        return Var();
    }
    if (data != nullptr && size > 0) {
        block->data.assign(data, data + size);
    }
    Var result;
    result.set_block(block, VarType::Bytes);
    return result;
}

Var Var::empty_array() noexcept {
    auto* block = allocate_block<detail::ArrayBlock>(VarType::Array);
    if (block == nullptr) {
        return Var();
    }
    Var result;
    result.set_block(block, VarType::Array);
    return result;
}

Var Var::from_array(const Var* values, usize count) noexcept {
    Var result = empty_array();
    if (result.is_nil()) {
        return result;
    }
    Expected<VarArray*, Error> items = result.array_mut();
    if (!items) {
        return Var();
    }
    for (usize i = 0; i < count; ++i) {
        if (!(*items)->push(values[i])) {
            return Var();
        }
    }
    return result;
}

Var Var::empty_dict() noexcept {
    auto* block = allocate_block<detail::DictBlock>(VarType::Dict);
    if (block == nullptr) {
        return Var();
    }
    Var result;
    result.set_block(block, VarType::Dict);
    return result;
}

Var Var::from_mat3(const VarMat3& value) noexcept {
    auto* block = allocate_block<detail::Mat3Block>(VarType::Mat3);
    if (block == nullptr) {
        return Var();
    }
    block->value = value;
    Var result;
    result.set_block(block, VarType::Mat3);
    return result;
}

Var Var::from_mat4(const VarMat4& value) noexcept {
    auto* block = allocate_block<detail::Mat4Block>(VarType::Mat4);
    if (block == nullptr) {
        return Var();
    }
    block->value = value;
    Var result;
    result.set_block(block, VarType::Mat4);
    return result;
}

Var Var::from_transform(const VarTransform& value) noexcept {
    auto* block = allocate_block<detail::TransformBlock>(VarType::Transform);
    if (block == nullptr) {
        return Var();
    }
    block->value = value;
    Var result;
    result.set_block(block, VarType::Transform);
    return result;
}

Var Var::from_aabb(const VarAabb& value) noexcept {
    auto* block = allocate_block<detail::AabbBlock>(VarType::Aabb);
    if (block == nullptr) {
        return Var();
    }
    block->value = value;
    Var result;
    result.set_block(block, VarType::Aabb);
    return result;
}

Var Var::from_callable(const Callable& callable) noexcept {
    auto* block = allocate_block<detail::CallableBlock>(VarType::Callable);
    if (block == nullptr) {
        return Var();
    }
    block->value = callable;
    Var result;
    result.set_block(block, VarType::Callable);
    return result;
}

Expected<VarMat3, Error> Var::as_mat3() const noexcept {
    if (type_ != VarType::Mat3) {
        return fail(ErrorCode::InvalidArgument, "Var holds a different kind");
    }
    return static_cast<const detail::Mat3Block*>(block())->value;
}

Expected<VarMat4, Error> Var::as_mat4() const noexcept {
    if (type_ != VarType::Mat4) {
        return fail(ErrorCode::InvalidArgument, "Var holds a different kind");
    }
    return static_cast<const detail::Mat4Block*>(block())->value;
}

Expected<VarTransform, Error> Var::as_transform() const noexcept {
    if (type_ != VarType::Transform) {
        return fail(ErrorCode::InvalidArgument, "Var holds a different kind");
    }
    return static_cast<const detail::TransformBlock*>(block())->value;
}

Expected<VarAabb, Error> Var::as_aabb() const noexcept {
    if (type_ != VarType::Aabb) {
        return fail(ErrorCode::InvalidArgument, "Var holds a different kind");
    }
    return static_cast<const detail::AabbBlock*>(block())->value;
}

Expected<std::string_view, Error> Var::as_string() const noexcept {
    if (type_ != VarType::String) {
        return fail(ErrorCode::InvalidArgument, "Var holds a different kind");
    }
    const auto* held = static_cast<const detail::StringBlock*>(block());
    return std::string_view{held->text};
}

Expected<const u8*, Error> Var::as_bytes(usize& size) const noexcept {
    size = 0;
    if (type_ != VarType::Bytes) {
        return fail(ErrorCode::InvalidArgument, "Var holds a different kind");
    }
    const auto* held = static_cast<const detail::BytesBlock*>(block());
    size = held->data.size();
    return held->data.data();
}

const Var* Var::array_data() const noexcept {
    if (type_ != VarType::Array) {
        return nullptr;
    }
    return static_cast<const detail::ArrayBlock*>(block())->items.data();
}

usize Var::array_size() const noexcept {
    if (type_ != VarType::Array) {
        return 0;
    }
    return static_cast<const detail::ArrayBlock*>(block())->items.size();
}

const VarDict* Var::dict() const noexcept {
    if (type_ != VarType::Dict) {
        return nullptr;
    }
    return &static_cast<const detail::DictBlock*>(block())->dict;
}

const Callable* Var::callable() const noexcept {
    if (type_ != VarType::Callable) {
        return nullptr;
    }
    return &static_cast<const detail::CallableBlock*>(block())->value;
}

Expected<VarArray*, Error> Var::array_mut() noexcept {
    if (type_ != VarType::Array) {
        return fail(ErrorCode::InvalidArgument, "Var does not hold an array");
    }
    if (Status detached = detach(); !detached) {
        return make_unexpected(detached.error());
    }
    return &static_cast<detail::ArrayBlock*>(block())->items;
}

Expected<VarDict*, Error> Var::dict_mut() noexcept {
    if (type_ != VarType::Dict) {
        return fail(ErrorCode::InvalidArgument, "Var does not hold a dictionary");
    }
    if (Status detached = detach(); !detached) {
        return make_unexpected(detached.error());
    }
    return &static_cast<detail::DictBlock*>(block())->dict;
}

std::span<const Var> var_array(const Var& value) noexcept {
    return std::span<const Var>{value.array_data(), value.array_size()};
}

// --- Equality
// --------------------------------------------------------------------------------------
//
// Bitwise over the payload, and structural for the heap kinds. Two `Var`s are equal when their
// representations are identical: NaN equals itself and 0.0 does not equal -0.0. That is the right
// rule for what this comparison is *for* — checking a value that crossed a boundary against the one
// that came back — and it is the rule the round-trip test asserts.

bool operator==(const Var& a, const Var& b) noexcept {
    if (a.type_ != b.type_) {
        return false;
    }
    if (!var_type_is_heap(a.type_)) {
        const usize size = inline_size(a.type_);
        return size == 0 || std::memcmp(a.storage_, b.storage_, size) == 0;
    }

    const VarBlock* left = a.block();
    const VarBlock* right = b.block();
    if (left == right) {
        return true;  // the same block: shared, and therefore identical
    }
    if (left == nullptr || right == nullptr) {
        return false;
    }

    switch (a.type_) {
        case VarType::String:
            return static_cast<const detail::StringBlock*>(left)->text ==
                   static_cast<const detail::StringBlock*>(right)->text;
        case VarType::Bytes:
            return static_cast<const detail::BytesBlock*>(left)->data ==
                   static_cast<const detail::BytesBlock*>(right)->data;
        case VarType::Array:
            return static_cast<const detail::ArrayBlock*>(left)->items ==
                   static_cast<const detail::ArrayBlock*>(right)->items;
        case VarType::Dict:
            return static_cast<const detail::DictBlock*>(left)->dict ==
                   static_cast<const detail::DictBlock*>(right)->dict;
        case VarType::Mat3:
            return std::memcmp(&static_cast<const detail::Mat3Block*>(left)->value,
                               &static_cast<const detail::Mat3Block*>(right)->value,
                               sizeof(VarMat3)) == 0;
        case VarType::Mat4:
            return std::memcmp(&static_cast<const detail::Mat4Block*>(left)->value,
                               &static_cast<const detail::Mat4Block*>(right)->value,
                               sizeof(VarMat4)) == 0;
        case VarType::Transform:
            return std::memcmp(&static_cast<const detail::TransformBlock*>(left)->value,
                               &static_cast<const detail::TransformBlock*>(right)->value,
                               sizeof(VarTransform)) == 0;
        case VarType::Aabb:
            return std::memcmp(&static_cast<const detail::AabbBlock*>(left)->value,
                               &static_cast<const detail::AabbBlock*>(right)->value,
                               sizeof(VarAabb)) == 0;
        case VarType::Callable:
            return static_cast<const detail::CallableBlock*>(left)->value ==
                   static_cast<const detail::CallableBlock*>(right)->value;
        default:
            return false;
    }
}

// --- VarArray and VarDict
// ---------------------------------------------------------------------------

Status VarArray::push(const Var& value) noexcept {
    // std::vector reports an allocation failure by throwing, which -fno-exceptions turns into a
    // terminate. Reserving first turns the common growth into something this function can report:
    // capacity() is checked, and the push itself then cannot need to allocate.
    if (items_.size() == items_.capacity()) {
        const usize next = items_.capacity() == 0 ? 4 : items_.capacity() * 2;
        items_.reserve(next);
        if (items_.capacity() < next) {
            return fail(ErrorCode::OutOfMemory, "VarArray growth failed");
        }
    }
    items_.push_back(value);
    return ok();
}

Status VarArray::resize(usize count) noexcept {
    items_.resize(count);
    if (items_.size() != count) {
        return fail(ErrorCode::OutOfMemory, "VarArray resize failed");
    }
    return ok();
}

bool operator==(const VarArray& a, const VarArray& b) noexcept {
    if (a.items_.size() != b.items_.size()) {
        return false;
    }
    for (usize i = 0; i < a.items_.size(); ++i) {
        if (!(a.items_[i] == b.items_[i])) {
            return false;
        }
    }
    return true;
}

const Var* VarDict::find(Name key) const noexcept {
    for (const VarDictEntry& entry : entries_) {
        if (entry.key == key) {
            return &entry.value;
        }
    }
    return nullptr;
}

Var* VarDict::find(Name key) noexcept {
    for (VarDictEntry& entry : entries_) {
        if (entry.key == key) {
            return &entry.value;
        }
    }
    return nullptr;
}

Status VarDict::set(Name key, const Var& value) noexcept {
    if (Var* existing = find(key); existing != nullptr) {
        *existing = value;
        return ok();
    }
    if (entries_.size() == entries_.capacity()) {
        const usize next = entries_.capacity() == 0 ? 4 : entries_.capacity() * 2;
        entries_.reserve(next);
        if (entries_.capacity() < next) {
            return fail(ErrorCode::OutOfMemory, "VarDict growth failed");
        }
    }
    entries_.push_back(VarDictEntry{key, value});
    return ok();
}

bool VarDict::erase(Name key) noexcept {
    for (usize i = 0; i < entries_.size(); ++i) {
        if (entries_[i].key == key) {
            entries_.erase(entries_.begin() + static_cast<isize>(i));
            return true;
        }
    }
    return false;
}

bool operator==(const VarDict& a, const VarDict& b) noexcept {
    if (a.entries_.size() != b.entries_.size()) {
        return false;
    }
    // Order-sensitive, because the order is part of what a dictionary carries across a boundary: a
    // serializer writes it, and a round trip that reordered fields is a round trip that changed the
    // document.
    for (usize i = 0; i < a.entries_.size(); ++i) {
        if (a.entries_[i].key != b.entries_[i].key ||
            !(a.entries_[i].value == b.entries_[i].value)) {
            return false;
        }
    }
    return true;
}

// --- Coercion
// ----------------------------------------------------------------------------------------

Expected<IntCoercion, Error> coerce_to_int(const Var& value) noexcept {
    switch (value.type()) {
        case VarType::Bool:
            return IntCoercion{*value.as_bool() ? 1 : 0, false};
        case VarType::Int:
            return IntCoercion{*value.as_int(), false};
        case VarType::Float: {
            const f64 source = *value.as_float();
            if (std::isnan(source) || std::isinf(source)) {
                return fail(ErrorCode::OutOfRange, "cannot coerce a non-finite Float to Int");
            }
            const f64 truncated = std::trunc(source);
            // 2^63 is not representable as an i64, so the upper bound is a strict less-than against
            // the f64 that 2^63 rounds to.
            constexpr f64 kUpper = 9223372036854775808.0;
            constexpr f64 kLower = -9223372036854775808.0;
            if (truncated >= kUpper || truncated < kLower) {
                return fail(ErrorCode::OutOfRange, "Float magnitude does not fit an Int");
            }
            return IntCoercion{static_cast<i64>(truncated), truncated != source};
        }
        default:
            return fail(ErrorCode::InvalidArgument, "kind does not coerce to Int");
    }
}

Expected<FloatCoercion, Error> coerce_to_float(const Var& value) noexcept {
    switch (value.type()) {
        case VarType::Bool:
            return FloatCoercion{*value.as_bool() ? 1.0 : 0.0, false};
        case VarType::Float:
            return FloatCoercion{*value.as_float(), false};
        case VarType::Int: {
            const i64 source = *value.as_int();
            const f64 converted = static_cast<f64>(source);
            // An i64 beyond 2^53 is not exactly representable; the round trip says so without a
            // magnitude test that would have to be written twice for the two signs.
            const bool exact = static_cast<i64>(converted) == source &&
                               converted >= -9223372036854775808.0 &&
                               converted < 9223372036854775808.0;
            return FloatCoercion{converted, !exact};
        }
        default:
            return fail(ErrorCode::InvalidArgument, "kind does not coerce to Float");
    }
}

Expected<bool, Error> coerce_to_bool(const Var& value) noexcept {
    switch (value.type()) {
        case VarType::Nil:
            return false;
        case VarType::Bool:
            return *value.as_bool();
        case VarType::Int:
            return *value.as_int() != 0;
        case VarType::Float:
            return *value.as_float() != 0.0;
        default:
            return fail(ErrorCode::InvalidArgument, "kind does not coerce to Bool");
    }
}

}  // namespace cy
