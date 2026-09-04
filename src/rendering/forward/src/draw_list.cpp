#include <cy/rendering/forward/draw_list.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering {
namespace {

/// The bytes the radix passes over, least significant first: surface (4), stable id (8), key (8).
/// Twenty passes, in the order that makes the result the total order rather than a stable
/// approximation of it. See the header comment.
constexpr u32 kRadixDigits = 20;
constexpr u32 kRadixBuckets = 256;

/// Byte `digit` of a draw's sort triple, in least-to-most-significant order.
[[nodiscard]] u8 radix_byte(const render::DrawItem& draw, u32 digit) noexcept {
    if (digit < 4) {
        return static_cast<u8>((draw.surface >> (digit * 8U)) & 0xFFU);
    }
    if (digit < 12) {
        return static_cast<u8>((draw.stable_id >> ((digit - 4U) * 8U)) & 0xFFU);
    }
    return static_cast<u8>((draw.key >> ((digit - 12U) * 8U)) & 0xFFU);
}

}  // namespace

u32 pack_lod_and_fade(u32 level, f32 fade) noexcept {
    const u32 clamped_level = math::min(level, 0xFFU);
    const auto quantised = static_cast<u32>(std::lround(math::clamp(fade, 0.0F, 1.0F) * 255.0F));
    return clamped_level | (quantised << 8U);
}

u32 unpack_lod(u32 packed) noexcept {
    return packed & 0xFFU;
}

f32 unpack_fade(u32 packed) noexcept {
    return static_cast<f32>((packed >> 8U) & 0xFFU) / 255.0F;
}

DrawList::DrawList(Allocator& allocator) noexcept
    : items(allocator), instances(allocator), batches(allocator) {}

void DrawList::clear() noexcept {
    items.clear();
    instances.clear();
    batches.clear();
}

Status build_draw_list(Span<const VisibleInstance> visible, SurfaceQueryFn surfaces_of, void* user,
                       DrawList& out) noexcept {
    if (surfaces_of == nullptr) {
        return fail(ErrorCode::InvalidArgument, "build_draw_list: a surface query is required");
    }
    out.clear();

    for (const VisibleInstance& instance : visible) {
        const Span<const DrawSurface> surfaces = surfaces_of(instance, user);
        for (const DrawSurface& surface : surfaces) {
            render::DrawKeyInputs inputs;
            // The layer is the blend mode's, never the caller's. `sort_layer_for` is the only
            // place that mapping exists.
            inputs.layer = render::sort_layer_for(surface.blend);
            inputs.pipeline = surface.pipeline;
            inputs.material = surface.material;
            inputs.mesh = surface.mesh;
            inputs.view_depth = instance.view_depth;

            render::DrawItem item;
            item.key = render::make_sort_key(inputs);
            item.stable_id = instance.stable_id;
            item.instance_slot = instance.gpu_slot;
            item.surface = surface.surface;
            if (Status pushed = out.items.push_back(item); !pushed) {
                return pushed;
            }

            GpuDrawInstance record;
            record.instance_slot = instance.gpu_slot;
            record.material = surface.material;
            record.parameter_offset = surface.parameter_offset;
            record.gi_address = surface.gi_address;
            record.lod_and_fade = pack_lod_and_fade(instance.lod_level, instance.lod_fade);
            record.surface = surface.surface;
            if (Status pushed = out.instances.push_back(record); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Status radix_sort_order(Span<const render::DrawItem> draws, Array<u32>& order,
                        Array<u32>& scratch) noexcept {
    const usize count = draws.size();
    if (Status sized = order.resize(count); !sized) {
        return sized;
    }
    if (Status sized = scratch.resize(count); !sized) {
        return sized;
    }
    for (usize index = 0; index < count; ++index) {
        order[index] = static_cast<u32>(index);
    }
    if (count < 2) {
        return ok();
    }

    u32* source = order.data();
    u32* destination = scratch.data();
    for (u32 digit = 0; digit < kRadixDigits; ++digit) {
        u32 histogram[kRadixBuckets] = {};
        for (usize index = 0; index < count; ++index) {
            ++histogram[radix_byte(draws[source[index]], digit)];
        }
        // A pass whose bytes are all equal changes nothing, and skipping it avoids a full copy —
        // which matters here because twelve of the twenty digits are usually constant (a frame's
        // stable ids share their high bytes, and every key shares its layer).
        u32 offset = 0;
        bool uniform = false;
        for (u32& bucket : histogram) {
            if (bucket == count) {
                uniform = true;
                break;
            }
            const u32 size = bucket;
            bucket = offset;
            offset += size;
        }
        if (uniform) {
            continue;
        }
        // Stable scatter: equal bytes keep the order the previous, less significant pass left them
        // in, which is what makes an LSD radix sort produce the composite order.
        for (usize index = 0; index < count; ++index) {
            destination[histogram[radix_byte(draws[source[index]], digit)]++] = source[index];
        }
        u32* swap = source;
        source = destination;
        destination = swap;
    }

    if (source != order.data()) {
        for (usize index = 0; index < count; ++index) {
            order[index] = source[index];
        }
    }
    return ok();
}

Status radix_sort_draws(Span<render::DrawItem> draws, DrawSortScratch& scratch) noexcept {
    if (Status ordered = radix_sort_order(draws, scratch.order, scratch.alternate); !ordered) {
        return ordered;
    }
    if (Status sized = scratch.items.resize(draws.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < draws.size(); ++index) {
        scratch.items[index] = draws[scratch.order[index]];
    }
    for (usize index = 0; index < draws.size(); ++index) {
        draws[index] = scratch.items[index];
    }
    return ok();
}

Status sort_draw_list(DrawList& list, DrawSortScratch& scratch) noexcept {
    if (list.items.size() != list.instances.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "sort_draw_list: items and instances must be parallel");
    }
    if (Status ordered = radix_sort_order(list.items.span(), scratch.order, scratch.alternate);
        !ordered) {
        return ordered;
    }
    const usize count = list.items.size();
    if (Status sized = scratch.items.resize(count); !sized) {
        return sized;
    }
    if (Status sized = scratch.instances.resize(count); !sized) {
        return sized;
    }
    for (usize index = 0; index < count; ++index) {
        scratch.items[index] = list.items[scratch.order[index]];
        scratch.instances[index] = list.instances[scratch.order[index]];
    }
    for (usize index = 0; index < count; ++index) {
        list.items[index] = scratch.items[index];
        list.instances[index] = scratch.instances[index];
    }
    return ok();
}

}  // namespace cy::rendering
