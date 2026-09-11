// The derived attribute layout and its precision selection. M8.c task 2.2. See layout.h for the
// rules and why "provably sufficient" is a computation.

#include <cy/vfx/layout.h>

#include <cy/graph/expr.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cy::vfx {
namespace {

using graph::hash_text;
using graph::hash_u64;
using graph::kHashSeed;

/// The relative step of a floating-point encoding: one unit in the last place of its mantissa.
constexpr f32 kFloat32Ulp = 1.0F / 8388608.0F;  // 2^-23
constexpr f32 kFloat16Ulp = 1.0F / 1024.0F;     // 2^-10

[[nodiscard]] f32 larger_magnitude(f32 minimum, f32 maximum) noexcept {
    const f32 low = minimum < 0.0F ? -minimum : minimum;
    const f32 high = maximum < 0.0F ? -maximum : maximum;
    return low > high ? low : high;
}

}  // namespace

f32 precision_step(Precision precision, f32 minimum, f32 maximum) noexcept {
    if (maximum < minimum) {
        return 0.0F;
    }
    const f32 span = maximum - minimum;
    switch (precision) {
        case Precision::Unorm8:
            // The encoding is [0, 1]; a range outside it cannot be represented at all, and saying
            // so with a zero is how `select_precision` rejects it rather than quantising a
            // position into an opacity.
            if (minimum < 0.0F || maximum > 1.0F) {
                return 0.0F;
            }
            return span / 255.0F;
        case Precision::Snorm16:
            if (minimum < -1.0F || maximum > 1.0F) {
                return 0.0F;
            }
            return span / 32767.0F;
        case Precision::Float16: {
            const f32 bound = larger_magnitude(minimum, maximum);
            // A half float's smallest normal is about 6.1e-5; below that the step stops shrinking
            // with the value, so a range entirely inside the subnormals is not better resolved by
            // this encoding than the constant below states.
            const f32 step = bound * kFloat16Ulp;
            return step > 6.0e-8F ? step : 6.0e-8F;
        }
        case Precision::Float32: {
            const f32 bound = larger_magnitude(minimum, maximum);
            const f32 step = bound * kFloat32Ulp;
            return step > 1.4e-45F ? step : 1.4e-45F;
        }
        case Precision::Auto:
            break;
    }
    return 0.0F;
}

Precision select_precision(const AttributeDecl& decl, u32 /*components*/) noexcept {
    if (decl.override_precision != Precision::Auto) {
        return decl.override_precision;
    }
    // A TOLERANCE OF ZERO IS THE DEFAULT AND SELECTS Float32. Nothing is quantised because a
    // compiler guessed; an author who accepts an error writes the error down.
    if (decl.tolerance <= 0.0F) {
        return Precision::Float32;
    }
    static constexpr Precision kOrder[] = {Precision::Unorm8, Precision::Snorm16,
                                           Precision::Float16, Precision::Float32};
    for (const Precision candidate : kOrder) {
        const f32 step = precision_step(candidate, decl.minimum, decl.maximum);
        if (step > 0.0F && step <= decl.tolerance) {
            return candidate;
        }
    }
    return Precision::Float32;
}

Status AttributeLayout::touch(Name attribute, TypeId type, bool read, bool written) noexcept {
    for (AttributeSlot& slot : slots_) {
        if (slot.name == attribute) {
            slot.read = slot.read || read;
            slot.written = slot.written || written;
            // A wider type wins: an attribute written as a float3 and read as a float is a float3.
            if (vfx_type_components(type) > slot.components) {
                slot.type = type;
                slot.components = vfx_type_components(type);
            }
            resolved_ = false;
            return ok();
        }
    }
    if (slots_.size() >= kMaxKernelWrites) {
        return fail(ErrorCode::OutOfRange,
                    "vfx: an emitter references more attributes than a kernel has write roots — "
                    "`kMaxKernelWrites` in ir.h is the bound, and it is the domain's root table");
    }
    AttributeSlot slot;
    slot.name = attribute;
    slot.type = type;
    slot.components = vfx_type_components(type);
    slot.read = read;
    slot.written = written;
    resolved_ = false;
    return slots_.push_back(slot);
}

Status AttributeLayout::declare(const AttributeDecl& decl) noexcept {
    for (AttributeSlot& slot : slots_) {
        if (slot.name == decl.name) {
            slot.precision = select_precision(decl, slot.components);
            resolved_ = false;
            return ok();
        }
    }
    // NOT AN ERROR. A declaration for an attribute nothing reads is a declaration about something
    // that is not allocated, which is exactly what the requirement wants to happen.
    return ok();
}

void AttributeLayout::eliminate_dead(Span<const Name> live_outputs) noexcept {
    for (AttributeSlot& slot : slots_) {
        if (slot.read) {
            continue;
        }
        bool live = false;
        for (const Name output : live_outputs) {
            if (output == slot.name) {
                live = true;
                break;
            }
        }
        // WRITTEN AND NEVER READ, and not one of the renderer's inputs: nothing can observe it, so
        // it is not allocated and the write to it is dropped. This is the attribute liveness
        // analysis `vfx-system` names, and it is a statement about the ordered write list rather
        // than about the expression DAG — which is why the shared core cannot make it.
        slot.elided = !live;
    }
    resolved_ = false;
}

/// The element sizes the offset assignment walks, widest first. A named table rather than a nested
/// conditional, because the three passes and the three sizes are one fact.
constexpr u32 kPassElementBytes[3] = {4, 2, 1};

Status AttributeLayout::resolve(u32 capacity) noexcept {
    bytes_per_particle_ = 0;
    block_bytes_ = 0;
    u64 digest = hash_u64(kHashSeed, capacity);

    // Sorted by descending stride so that every array starts at a multiple of its own element size
    // without padding between them. A structure of arrays has no per-particle alignment problem,
    // but an array of 2-byte elements followed by one of 4-byte elements does.
    for (const u32 wanted : kPassElementBytes) {
        for (AttributeSlot& slot : slots_) {
            if (slot.elided) {
                continue;
            }
            const u32 element = precision_bytes(slot.precision);
            if (element == 0) {
                return fail(ErrorCode::Internal, "vfx: an attribute slot has no precision");
            }
            if (element != wanted) {
                continue;
            }
            slot.stride = element * slot.components;
            slot.array_offset = static_cast<u32>(block_bytes_);
            block_bytes_ += static_cast<u64>(slot.stride) * static_cast<u64>(capacity);
            bytes_per_particle_ += slot.stride;
            digest = hash_text(digest, slot.name.text());
            digest = hash_u64(digest, slot.type);
            digest = hash_u64(digest, static_cast<u64>(slot.precision));
            digest = hash_u64(digest, slot.components);
        }
    }
    digest_ = digest;
    resolved_ = true;
    return ok();
}

const AttributeSlot* AttributeLayout::find(Name attribute) const noexcept {
    for (const AttributeSlot& slot : slots_) {
        if (slot.name == attribute) {
            return &slot;
        }
    }
    return nullptr;
}

u32 AttributeLayout::allocated_attributes() const noexcept {
    u32 count = 0;
    for (const AttributeSlot& slot : slots_) {
        count += slot.elided ? 0U : 1U;
    }
    return count;
}

u32 AttributeLayout::elided_attributes() const noexcept {
    u32 count = 0;
    for (const AttributeSlot& slot : slots_) {
        count += slot.elided ? 1U : 0U;
    }
    return count;
}

u32 AttributeLayout::max_population(u64 memory_budget_bytes) const noexcept {
    if (bytes_per_particle_ == 0) {
        return 0;
    }
    const u64 population = memory_budget_bytes / bytes_per_particle_;
    return population > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<u32>(population);
}

// --- Reading and writing at the derived precision
// -------------------------------------------------
//
// The CPU-side spelling of the four conversions `emit_slang.cpp` generates into the shader. Two
// spellings of one encoding is how a readback and a simulation come to disagree, so the two are
// checked against each other rather than assumed equal.

namespace {

[[nodiscard]] u16 encode_half(f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const u32 sign = (bits >> 16U) & 0x8000U;
    i32 exponent = static_cast<i32>((bits >> 23U) & 0xFFU) - 127 + 15;
    u32 mantissa = bits & 0x7FFFFFU;
    if (exponent <= 0) {
        return static_cast<u16>(sign);
    }
    if (exponent >= 31) {
        return static_cast<u16>(sign | 0x7C00U);
    }
    return static_cast<u16>(sign | (static_cast<u32>(exponent) << 10U) | (mantissa >> 13U));
}

[[nodiscard]] f32 decode_half(u16 half) noexcept {
    const u32 sign = static_cast<u32>(half & 0x8000U) << 16U;
    const u32 exponent = (half >> 10U) & 0x1FU;
    const u32 mantissa = half & 0x3FFU;
    u32 bits = 0;
    if (exponent == 0) {
        bits = sign;
    } else if (exponent == 31) {
        bits = sign | 0x7F800000U | (mantissa << 13U);
    } else {
        bits = sign | ((exponent + 127U - 15U) << 23U) | (mantissa << 13U);
    }
    f32 value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

[[nodiscard]] usize byte_index(const AttributeSlot& slot, u32 particle, u32 component) noexcept {
    const u32 element = precision_bytes(slot.precision);
    return static_cast<usize>(slot.array_offset) +
           (((static_cast<usize>(particle) * slot.components) + component) * element);
}

[[nodiscard]] bool in_range(usize size, const AttributeSlot& slot, usize index) noexcept {
    return index + precision_bytes(slot.precision) <= size;
}

}  // namespace

f32 load_component(Span<const u8> block, const AttributeSlot& slot, u32 particle,
                   u32 component) noexcept {
    const usize index = byte_index(slot, particle, component);
    if (slot.elided || component >= slot.components || !in_range(block.size(), slot, index)) {
        return 0.0F;
    }
    switch (slot.precision) {
        case Precision::Float32: {
            f32 value = 0.0F;
            std::memcpy(&value, block.data() + index, sizeof(value));
            return value;
        }
        case Precision::Float16: {
            u16 raw = 0;
            std::memcpy(&raw, block.data() + index, sizeof(raw));
            return decode_half(raw);
        }
        case Precision::Unorm8:
            return static_cast<f32>(block[index]) * (1.0F / 255.0F);
        case Precision::Snorm16: {
            i16 raw = 0;
            std::memcpy(&raw, block.data() + index, sizeof(raw));
            const f32 value = static_cast<f32>(raw) * (1.0F / 32767.0F);
            return value < -1.0F ? -1.0F : value;
        }
        case Precision::Auto:
            break;
    }
    return 0.0F;
}

void store_component(Span<u8> block, const AttributeSlot& slot, u32 particle, u32 component,
                     f32 value) noexcept {
    const usize index = byte_index(slot, particle, component);
    if (slot.elided || component >= slot.components || !in_range(block.size(), slot, index)) {
        return;
    }
    switch (slot.precision) {
        case Precision::Float32:
            std::memcpy(block.data() + index, &value, sizeof(value));
            return;
        case Precision::Float16: {
            const u16 raw = encode_half(value);
            std::memcpy(block.data() + index, &raw, sizeof(raw));
            return;
        }
        case Precision::Unorm8: {
            const f32 clamped = std::clamp(value, 0.0F, 1.0F);
            block[index] = static_cast<u8>(std::lround(clamped * 255.0F));
            return;
        }
        case Precision::Snorm16: {
            const f32 clamped = std::clamp(value, -1.0F, 1.0F);
            const auto raw = static_cast<i16>(clamped * 32767.0F);
            std::memcpy(block.data() + index, &raw, sizeof(raw));
            return;
        }
        case Precision::Auto:
            break;
    }
}

}  // namespace cy::vfx
