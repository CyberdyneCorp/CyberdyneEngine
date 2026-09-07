#include <cy/rendering/lighting/channels.h>

namespace cy::rendering {

const char* named_channel_name(NamedChannel channel) noexcept {
    switch (channel) {
        case NamedChannel::World:
            return "world";
        case NamedChannel::Characters:
            return "characters";
        case NamedChannel::FirstPerson:
            return "first-person";
        case NamedChannel::Vfx:
            return "vfx";
        case NamedChannel::Interior:
            return "interior";
        case NamedChannel::Count:
            break;
    }
    return "unknown";
}

ChannelFilterStats assign_cluster_lights(ChannelMask receiver_channels,
                                         const ChannelMask* candidate_channels,
                                         const f32* candidate_importance, u32 candidate_count,
                                         u32 max_assigned, u32* out_indices) noexcept {
    ChannelFilterStats stats;
    if (candidate_channels == nullptr || candidate_importance == nullptr ||
        out_indices == nullptr) {
        return stats;
    }
    stats.candidates = candidate_count;

    // Selection sort over the survivors, capped at `max_assigned`. Selection rather than a full
    // sort because the cap is small — a cluster's light bound is tens, not thousands — and because
    // it makes the tie-break explicit rather than a property of whichever sort was reached for.
    for (u32 slot = 0; slot < max_assigned; ++slot) {
        u32 best = candidate_count;
        f32 best_importance = -1.0F;
        for (u32 index = 0; index < candidate_count; ++index) {
            if (!channels_intersect(candidate_channels[index], receiver_channels)) {
                continue;
            }
            bool already = false;
            for (u32 taken = 0; taken < slot; ++taken) {
                already = already || out_indices[taken] == index;
            }
            if (already) {
                continue;
            }
            // Strictly greater, so the LOWEST index wins a tie. That is the whole of "the same
            // lights SHALL be dropped both times": index is a property of the frame's light array,
            // not of iteration order or of a hash.
            if (candidate_importance[index] > best_importance) {
                best_importance = candidate_importance[index];
                best = index;
            }
        }
        if (best == candidate_count) {
            break;
        }
        out_indices[slot] = best;
        ++stats.assigned;
    }

    for (u32 index = 0; index < candidate_count; ++index) {
        if (!channels_intersect(candidate_channels[index], receiver_channels)) {
            ++stats.rejected_by_channel;
        }
    }
    const u32 eligible = candidate_count - stats.rejected_by_channel;
    stats.rejected_by_bound = eligible > stats.assigned ? eligible - stats.assigned : 0U;
    return stats;
}

}  // namespace cy::rendering
