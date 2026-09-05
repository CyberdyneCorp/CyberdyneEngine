// cy/abi/var.h — value marshalling across the boundary. Task 2.5.
//
// `native-abi`: "Dynamic values SHALL cross the ABI as `CyVar`: a tagged union with a type tag and
// a fixed-size payload, with values exceeding the payload heap-allocated and reference counted by
// the engine."
//
// --- WHY THIS IS NOT `cy::Var` ------------------------------------------------------------------
//
// `cy/core/values/var.h` already has a dynamic value, and it is the right one *inside* the engine.
// It cannot be this one: it is a C++ class with a destructor, a copy constructor and an allocator,
// and none of those cross a C boundary. `CyVar` is the POD projection of it — a subset of the
// kinds, a fixed 32-byte layout, and explicit reference counting instead of RAII. The numeric
// values of `CyVarType` are deliberately their own: `cy::VarType`'s comment already states that its
// numbers are not an interchange format, and this is the interchange format.
//
// --- OWNERSHIP IS ONE BIT, AND IT IS ON THE VALUE -----------------------------------------------
//
// A `CyVar` carrying `CY_VAR_FLAG_OWNED` holds one reference and the receiver must release it
// exactly once. A value without the flag owns nothing and releasing it is a no-op, so
// `var_release` is safe to call unconditionally — which is what the generated overlays do, and
// what makes "did this function return an owned value?" a question the value answers rather than
// the documentation.
//
// The reference count, the allocator and the owning host all live in a header *before* the payload
// bytes, which is why `var_release` needs no engine handle: everything it must know is reachable
// from the pointer inside the value. That is what lets a Swift `deinit` release a value without
// having kept the engine handle alive alongside it.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/abi/host.h>
#include <cy/core/base/types.h>

namespace cy::abi {

/// Bytes of bookkeeping in front of a heap payload. Stated here because src/abi/tests asserts it,
/// and because a change to it is a change to what `var_release` subtracts.
inline constexpr usize kVarBlobHeaderSize = 32;

/// A heap-backed value: `size` bytes copied out of `data`, one reference, owned by the caller.
///
/// Returns a nil value on allocation failure, having recorded the failure — the ABI's entries
/// return values rather than results, so "it failed" has to be readable from the value plus
/// `cy_get_last_error`. `data` may be null when `size` is zero.
[[nodiscard]] CyVar make_heap_var(Host& host, CyVarType type, const void* data, u64 size) noexcept;

/// Another owned reference to the same value. An inline value is copied; a borrowed one becomes an
/// owned copy of nothing, which is to say it stays borrowed and releasing it stays a no-op.
[[nodiscard]] CyVar clone_var(const CyVar& var) noexcept;

/// Drop one reference and clear `*var` to nil. Safe on nil, on an inline value, on a borrowed one,
/// and — because it clears — twice.
void release_var(CyVar* var) noexcept;

/// The payload bytes of a heap-backed value, or null. A borrowed view: it is valid for as long as
/// the caller holds a reference.
[[nodiscard]] const void* var_data(const CyVar& var) noexcept;

/// Inline constructors, for the values that never touch the heap. They exist so that engine code
/// filling a `CyVar*` out-parameter does not open-code the tag and the payload union at every site.
[[nodiscard]] CyVar var_nil() noexcept;
[[nodiscard]] CyVar var_bool(bool value) noexcept;
[[nodiscard]] CyVar var_i64(i64 value) noexcept;
[[nodiscard]] CyVar var_f64(f64 value) noexcept;
[[nodiscard]] CyVar var_entity(CyEntity value) noexcept;
/// `count` floats, 1 to 4, tagged as `type`. Used for vec2, vec3, vec4 and quat.
[[nodiscard]] CyVar var_floats(CyVarType type, const f32* values, u32 count) noexcept;

}  // namespace cy::abi
