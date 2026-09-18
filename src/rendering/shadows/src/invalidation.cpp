#include <cy/rendering/shadows/invalidation.h>

#include <cy/core/math/scalar.h>

namespace cy::rendering {
namespace {

[[nodiscard]] Aabb expanded(const Aabb& bounds, f32 margin) noexcept {
    if (margin <= 0.0F || bounds.is_empty()) {
        return bounds;
    }
    const Vec3 grow{margin, margin, margin};
    return Aabb{bounds.min - grow, bounds.max + grow};
}

[[nodiscard]] bool same_box(const Aabb& a, const Aabb& b) noexcept {
    return a.min == b.min && a.max == b.max;
}

/// Dirty every page a box projects into. Shared by the caster and the streaming paths, which differ
/// only in what they blame.
void dirty_box(ShadowPageCache& cache, const ShadowAddressSpace& space, const Aabb& bounds,
               InvalidationSource source, u64 source_id, InvalidationScratch scratch,
               InvalidationReport& report) noexcept {
    const u32 wanted = pages_covering(space, bounds, scratch.pages, scratch.capacity);
    const u32 written = math::min(wanted, scratch.capacity);
    report.overflow += wanted - written;
    for (u32 index = 0; index < written; ++index) {
        if (cache.invalidate(scratch.pages[index], source, source_id)) {
            ++report.pages_dirtied;
        }
    }
}

}  // namespace

const char* shadow_deformation_mode_name(ShadowDeformationMode mode) noexcept {
    switch (mode) {
        case ShadowDeformationMode::Static:
            return "Static";
        case ShadowDeformationMode::Bounded:
            return "Bounded";
        case ShadowDeformationMode::Dynamic:
            return "Dynamic";
        case ShadowDeformationMode::AlwaysDirty:
            return "AlwaysDirty";
        case ShadowDeformationMode::Count:
            break;
    }
    return "Unknown";
}

InvalidationReport invalidate_caster_motion(ShadowPageCache& cache, const ShadowAddressSpace& space,
                                            const CasterMotion& motion,
                                            InvalidationScratch scratch) noexcept {
    InvalidationReport report;
    if (scratch.pages == nullptr || scratch.capacity == 0) {
        return report;
    }
    if (motion.mode == ShadowDeformationMode::AlwaysDirty) {
        ++report.always_dirty_casters;
    }

    // `Bounded` is the forest: the envelope covers the sway, so a tree that stayed inside the
    // envelope it declared dirties nothing. `Static` is the same test without the margin.
    const f32 margin = motion.mode == ShadowDeformationMode::Bounded ? motion.envelope : 0.0F;
    const Aabb previous = expanded(motion.previous, margin);
    const Aabb current = expanded(motion.current, margin);

    const bool deforms_every_frame = motion.mode == ShadowDeformationMode::Dynamic ||
                                     motion.mode == ShadowDeformationMode::AlwaysDirty;
    if (!deforms_every_frame && same_box(previous, current)) {
        ++report.casters_unchanged;
        return report;
    }

    // BOTH boxes. Dirtying only where the caster now is leaves its old shadow painted on the world.
    dirty_box(cache, space, previous, InvalidationSource::Instance, motion.instance_id, scratch,
              report);
    if (!same_box(previous, current)) {
        dirty_box(cache, space, current, InvalidationSource::Instance, motion.instance_id, scratch,
                  report);
    }
    return report;
}

InvalidationReport invalidate_light(ShadowPageCache& cache, u32 light_slot, u64 light_id) noexcept {
    InvalidationReport report;
    // Attributed to the light, which is the whole of "a moving shadowed light is expensive and the
    // cost should be attributable rather than mysterious". Nothing else in this file dirties a
    // light's whole space, and that asymmetry is the requirement.
    for (const PageEntry& entry : cache.entries()) {
        if (entry.page.light_slot != light_slot) {
            continue;
        }
        if (cache.invalidate(entry.page, InvalidationSource::LightMoved, light_id)) {
            ++report.pages_dirtied;
        }
    }
    return report;
}

InvalidationReport invalidate_streaming(ShadowPageCache& cache, const ShadowAddressSpace& space,
                                        const Aabb& cell_bounds, u64 cell_id,
                                        InvalidationScratch scratch) noexcept {
    InvalidationReport report;
    if (scratch.pages == nullptr || scratch.capacity == 0) {
        return report;
    }
    dirty_box(cache, space, cell_bounds, InvalidationSource::Streaming, cell_id, scratch, report);
    return report;
}

}  // namespace cy::rendering
