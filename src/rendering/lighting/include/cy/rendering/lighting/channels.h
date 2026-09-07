#pragma once
// Light channels: a bitfield test during assignment, and nothing at all at shading time. Task 10.3.
//
// `rendering-lighting-and-shadows` — "Light channels".
//
// ================================================================================================
// THE REQUIREMENT IS ABOUT WHERE THE COST IS, NOT ABOUT WHAT THE FEATURE DOES
// ================================================================================================
//
// "Channel resolution SHALL be a compact bitfield test during light assignment, not a per-object
// light loop, so channels cost nothing at shading time" and "Channels SHALL be a filter on
// assignment, and SHALL NOT create separate lighting passes."
//
// Both sentences are about the SHAPE of the implementation rather than the behaviour, so the way to
// honour them is to make the alternative unwriteable. Everything in this file takes a mask and
// returns a bool or writes an assignment; nothing here takes a shading point, a material, or a
// pass. A per-pixel channel test would have to be written somewhere that could see a pixel, and no
// function here can.
//
// `GpuLight::layer_mask` is where the light's half already lives — it is one of the two words
// `cy/light.slang` calls padding precisely because the shading loop does not read it.

#include <cy/core/base/types.h>

namespace cy::rendering {

/// A channel set. 32 channels, which is what a `u32` on the light record and on the instance
/// record buys; a project needing more is a project needing a second word on both, and that is a
/// format change rather than a policy one.
using ChannelMask = u32;

inline constexpr ChannelMask kAllChannels = 0xFFFFFFFFU;

/// The channels the engine names. A project uses the rest by number; these five exist so that the
/// common cases have a spelling and a diagnostic has something to print.
enum class NamedChannel : u8 {
    /// Everything that is not otherwise classified. Index 0, so a zeroed mask that has had
    /// `channel_bit(NamedChannel::World)` set is the ordinary case.
    World = 0,
    Characters,
    /// Weapons, held items, first-person geometry — the things lit by a rig rather than by the sun.
    FirstPerson,
    Vfx,
    /// Interiors that must not receive the exterior key light.
    Interior,
    Count,
};

[[nodiscard]] const char* named_channel_name(NamedChannel channel) noexcept;

[[nodiscard]] constexpr ChannelMask channel_bit(NamedChannel channel) noexcept {
    return static_cast<ChannelMask>(1U) << static_cast<u32>(channel);
}

[[nodiscard]] constexpr ChannelMask channel_bit(u32 index) noexcept {
    return index < 32U ? (static_cast<ChannelMask>(1U) << index) : 0U;
}

/// Whether this light may be assigned to this receiver. One instruction, at assignment.
[[nodiscard]] constexpr bool channels_intersect(ChannelMask light, ChannelMask receiver) noexcept {
    return (light & receiver) != 0U;
}

/// A light restricted to nothing illuminates nothing, and a light restricted to everything is the
/// ordinary case. Both are legitimate; an EMPTY mask is the one that is almost always a mistake,
/// because it is what a zeroed structure gives, so it is worth being able to ask.
[[nodiscard]] constexpr bool channel_mask_is_empty(ChannelMask mask) noexcept {
    return mask == 0U;
}

/// What a channel filter did over one cluster's candidate list. Reported because
/// `rendering-lighting-and-shadows` requires the ACTIVE limit to be reported alongside light
/// statistics — a filter that quietly removed a light is indistinguishable from a light that was
/// never there.
struct ChannelFilterStats {
    u32 candidates = 0;
    u32 assigned = 0;
    /// Removed because the channel masks did not intersect.
    u32 rejected_by_channel = 0;
    /// Removed because the per-cluster bound was reached. Distinct from the line above: one is an
    /// artistic decision and the other is a budget, and confusing them is how "my light does not
    /// work" becomes an afternoon.
    u32 rejected_by_bound = 0;
};

/// Filter and bound one cluster's candidate lights.
///
/// `candidate_channels` and `candidate_importance` are parallel arrays over the cluster's
/// candidates; `out_indices` receives the surviving indices, most important first, at most
/// `max_assigned` of them. Returns what it did.
///
/// DETERMINISTIC BY CONSTRUCTION. "WHEN the same scene is rendered twice with too many lights
/// under the clustered path THEN the same lights SHALL be dropped both times." The selection is by
/// importance with the candidate INDEX as the tie-break, and index is a property of the frame's
/// light array rather than of iteration order, so two runs of one frame drop the same lights.
[[nodiscard]] ChannelFilterStats assign_cluster_lights(ChannelMask receiver_channels,
                                                       const ChannelMask* candidate_channels,
                                                       const f32* candidate_importance,
                                                       u32 candidate_count, u32 max_assigned,
                                                       u32* out_indices) noexcept;

}  // namespace cy::rendering
