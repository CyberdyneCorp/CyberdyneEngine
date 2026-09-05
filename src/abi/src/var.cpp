// CyVar: the heap payload, its reference count, and the inline constructors. Task 2.5.

#include <cy/abi/var.h>

#include <cy/abi/errors.h>
#include <cy/core/base/assert.h>
#include <cy/core/memory/allocator.h>

#include <atomic>
#include <cstring>
#include <new>

namespace cy::abi {
namespace {

// The bookkeeping in front of a heap payload.
//
// EVERYTHING `var_release` NEEDS IS HERE, WHICH IS THE POINT. The ABI's `var_release` takes only
// the value, so the allocator that produced the block and the host whose live count must be
// decremented cannot come from a parameter — they have to travel with the payload. A Swift `deinit`
// releasing a value it captured is exactly the case this pays for.
struct VarBlob {
    Allocator* allocator;
    Host* host;
    u64 size;
    std::atomic<u32> references;
    u32 padding;  // explicit, so the 32 bytes are stated rather than inferred from the alignment
};

static_assert(sizeof(VarBlob) == kVarBlobHeaderSize, "the var blob header is 32 bytes");
static_assert(alignof(VarBlob) == 8, "the var blob header is 8-byte aligned");

// The payload sits immediately after the header, so the header is at a fixed negative offset from
// the pointer a `CyVar` carries. Crossing between the two is the one place in the ABI that does
// pointer arithmetic between unrelated types, and `reinterpret_cast` says so — a pair of
// `static_cast`s through `void*` would do exactly the same thing while looking milder.
u8* payload_of(VarBlob* blob) noexcept {
    return reinterpret_cast<u8*>(blob) + kVarBlobHeaderSize;  // NOLINT(*-reinterpret-cast)
}

VarBlob* blob_of(const CyVar& var) noexcept {
    if ((var.flags & CY_VAR_FLAG_OWNED) == 0U || var.payload.as_bytes == nullptr) {
        return nullptr;
    }
    // NOLINTNEXTLINE(*-const-cast) — the reference count is mutable state of the block, and both
    // `clone_var` and `release_var` take the value by const pointer because neither changes its
    // tag.
    auto* bytes = static_cast<u8*>(const_cast<void*>(var.payload.as_bytes));
    return reinterpret_cast<VarBlob*>(bytes - kVarBlobHeaderSize);  // NOLINT(*-reinterpret-cast)
}

bool is_heap_type(CyVarType type) noexcept {
    return type == CY_VAR_STRING || type == CY_VAR_BYTES;
}

}  // namespace

CyVar make_heap_var(Host& host, CyVarType type, const void* data, u64 size) noexcept {
    CyVar var = var_nil();
    if (!is_heap_type(type)) {
        (void)report(CY_RESULT_INVALID_ARGUMENT, "only a string or a byte block is heap-backed");
        return var;
    }
    if (data == nullptr && size != 0) {
        (void)report(CY_RESULT_INVALID_ARGUMENT, "a sized value needs a source to copy from");
        return var;
    }

    void* block = host.allocator.allocate(kVarBlobHeaderSize + static_cast<usize>(size), 8);
    if (block == nullptr) {
        (void)report(CY_RESULT_OUT_OF_MEMORY, "a CyVar payload was refused by its allocator");
        return var;
    }

    auto* blob = new (block) VarBlob{&host.allocator, &host, size, {1}, 0};
    if (size != 0) {
        std::memcpy(static_cast<void*>(payload_of(blob)), data, static_cast<usize>(size));
    }
    host.live_vars.fetch_add(1, std::memory_order_relaxed);

    var.type = static_cast<u32>(type);
    var.flags = CY_VAR_FLAG_OWNED;
    var.length = size;
    var.payload.as_bytes = payload_of(blob);
    clear_last_error();
    return var;
}

CyVar clone_var(const CyVar& var) noexcept {
    VarBlob* blob = blob_of(var);
    if (blob != nullptr) {
        // Relaxed is enough to add a reference: the caller already holds one, so the object cannot
        // be destroyed underneath this increment. The release side is what needs the ordering.
        blob->references.fetch_add(1, std::memory_order_relaxed);
    }
    return var;
}

void release_var(CyVar* var) noexcept {
    if (var == nullptr) {
        return;
    }
    VarBlob* blob = blob_of(*var);
    *var = var_nil();
    if (blob == nullptr) {
        return;
    }
    if (blob->references.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
    }

    Allocator* allocator = blob->allocator;
    Host* host = blob->host;
    const usize bytes = kVarBlobHeaderSize + static_cast<usize>(blob->size);
    blob->~VarBlob();
    allocator->deallocate(static_cast<void*>(blob), bytes, 8);
    host->live_vars.fetch_sub(1, std::memory_order_relaxed);
}

const void* var_data(const CyVar& var) noexcept {
    return (blob_of(var) != nullptr) ? var.payload.as_bytes : nullptr;
}

CyVar var_nil() noexcept {
    CyVar var{};
    var.type = CY_VAR_NIL;
    return var;
}

CyVar var_bool(bool value) noexcept {
    CyVar var = var_nil();
    var.type = CY_VAR_BOOL;
    var.payload.as_bool = value;
    return var;
}

CyVar var_i64(i64 value) noexcept {
    CyVar var = var_nil();
    var.type = CY_VAR_I64;
    var.payload.as_i64 = value;
    return var;
}

CyVar var_f64(f64 value) noexcept {
    CyVar var = var_nil();
    var.type = CY_VAR_F64;
    var.payload.as_f64 = value;
    return var;
}

CyVar var_entity(CyEntity value) noexcept {
    CyVar var = var_nil();
    var.type = CY_VAR_ENTITY;
    var.payload.as_entity = value;
    return var;
}

CyVar var_floats(CyVarType type, const f32* values, u32 count) noexcept {
    CY_ASSERT_MSG(count >= 1 && count <= 4, "a float payload holds one to four components");
    CyVar var = var_nil();
    var.type = static_cast<u32>(type);
    for (u32 index = 0; index < count; ++index) {
        var.payload.as_f32x4[index] = values[index];
    }
    return var;
}

}  // namespace cy::abi
