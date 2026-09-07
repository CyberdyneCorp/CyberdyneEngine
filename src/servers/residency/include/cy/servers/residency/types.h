#pragma once
// The residency layer's vocabulary: who competes, what they compete for, and what a page costs to
// bring back. Task 4.1.
//
// `residency` — "Shared policy, separate storage". The layer owns *policy and observability*; it
// owns no page storage, no page format and no page production. Everything in this header is
// therefore a description of a page rather than a page: a key the owning subsystem can resolve, a
// size in bytes it reported, and a cost class saying what regenerating it would take. Nothing here
// can hold a texel.
//
// --- WHY A PAGE IS A u64 AND NOT A TYPE
// -----------------------------------------------------------
//
// A geometry page, a texture tile, a shadow page and an illumination brick have nothing in common
// except that they compete for memory. If this layer named any of their identities it would have to
// include their headers, and the "separate storage" requirement would be a comment rather than a
// build property. `PageKey` is therefore a subsystem tag and an opaque 56-bit number the subsystem
// alone knows how to resolve — the residency layer scores it, orders it and reports on it, and can
// do nothing else with it.
//
// 56 bits is not a shortage: virtual texturing's own address encoding (see
// `cy/servers/render/virtual_texturing/address.h`) fits a virtual texture id, mip, tile coordinate
// and layer inside it, and it is the largest of the four address spaces.

#include <cy/core/base/types.h>

namespace cy::residency {

/// The paged subsystems the policy arbitrates between.
///
/// `residency` names four caches — geometry, texture, shadow, illumination — and its deadline
/// requirement adds two more consumers that are not caches but do prepare against a prediction:
/// audio preload and world cell preparation. They are in the same enumeration because a deadline
/// must reach all six from one announcement, and an enumeration that stopped at the caches would
/// make the other two a second mechanism.
enum class Subsystem : u8 {
    Geometry = 0,  // virtual geometry pages: immutable, content-addressed, cheap to re-fetch
    Texture,       // virtual texture tiles: cooked or produced at runtime
    Shadow,        // virtual shadow pages: *rendered*, and expensive to regenerate
    Illumination,  // surface and radiance caches
    Audio,         // preload; a consumer of deadlines, not a paged cache
    WorldCells,    // world partition cell preparation; likewise
    Count,
};

inline constexpr u32 kSubsystemCount = static_cast<u32>(Subsystem::Count);

/// The subsystem's own spelling, for a report. Never null, including for an out-of-range value.
[[nodiscard]] const char* subsystem_name(Subsystem subsystem) noexcept;

/// A set of subsystems, so that one prediction can name its consumers without an array.
///
/// A plain `u32` rather than an enum class with operators: the set is a parameter and a field, and
/// every operator a flag enum needs here would be a place the bit could be got wrong.
using SubsystemMask = u32;

[[nodiscard]] constexpr SubsystemMask subsystem_bit(Subsystem subsystem) noexcept {
    return static_cast<SubsystemMask>(1U) << static_cast<u32>(subsystem);
}

/// Every subsystem. What a world-cell prediction announces to, because arriving at a region needs
/// all six kinds of preparation.
inline constexpr SubsystemMask kAllSubsystems = (1U << kSubsystemCount) - 1U;

[[nodiscard]] constexpr bool mask_contains(SubsystemMask mask, Subsystem subsystem) noexcept {
    return (mask & subsystem_bit(subsystem)) != 0U;
}

/// What bringing a page back costs, as a class rather than a number.
///
/// `residency` — "A shadow page is not a geometry page": eviction "SHALL account for the cost of
/// regenerating a page, so an expensive rendered shadow page and a cheap streamed geometry page are
/// not treated identically". The class is the declaration; `production_cost_ms` on a request is the
/// measurement. Both exist because a subsystem that has not measured yet still has an opinion, and
/// a policy that had only the measurement would treat an unmeasured shadow page as free.
enum class CostClass : u8 {
    Streamed = 0,  // read from disk or from the network; latency, little compute
    Decoded,       // read and transcoded
    Composed,      // evaluated from other resident data: a terrain material graph
    Rendered,      // rasterised or traced: a shadow page, a reflection probe
    Count,
};

[[nodiscard]] const char* cost_class_name(CostClass cost) noexcept;

/// The weight the eviction score gives a class when no measurement is available. Monotonic in the
/// enumeration's order, which is the order the classes were written in.
[[nodiscard]] f32 cost_class_weight(CostClass cost) noexcept;

/// A page, as this layer can see it: whose it is, and a number only they can resolve.
///
/// Packed into a `u64` by `packed()` so that the maps here key on an integer. That is not an
/// optimisation — it is what keeps `Hash<PageKey>` from existing, and with it the temptation to
/// give this layer a comparison that understands what a page *is*.
struct PageKey {
    Subsystem subsystem = Subsystem::Geometry;
    u64 page = 0;  // 56 bits; the subsystem's own encoding

    [[nodiscard]] constexpr u64 packed() const noexcept {
        return (static_cast<u64>(subsystem) << 56U) | (page & kPageMask);
    }

    [[nodiscard]] static constexpr PageKey unpack(u64 packed) noexcept {
        return PageKey{static_cast<Subsystem>(packed >> 56U), packed & kPageMask};
    }

    [[nodiscard]] constexpr bool operator==(const PageKey& other) const noexcept {
        return packed() == other.packed();
    }

    static constexpr u64 kPageMask = (static_cast<u64>(1) << 56U) - 1U;
};

/// Why a page is at the quality it is. `residency` requires the layer to answer this "for any page
/// or any screen pixel", and lists the causes; this is that list, as a type, so the answer is a
/// value a test can assert on rather than a string a report formats.
enum class QualityReason : u8 {
    Resident = 0,        // it is at the level that was asked for
    NeverRequested,      // nothing asked; not a residency failure
    Outscored,           // requested, and other requests scored higher
    BudgetBlocked,       // admitted by score, refused by a hard budget
    AwaitingProduction,  // scheduled, and the subsystem has not reported it resident yet
    DeadlineMissed,      // its deadline passed before it became resident
    PressureCapped,      // deliberately held coarse by the coordinated reduction
    Evicted,             // it was resident and the policy took it back
    Count,
};

[[nodiscard]] const char* quality_reason_name(QualityReason reason) noexcept;

/// A deadline that never falls due. Requests without a predicted arrival carry it, so that "time
/// until needed" is one number rather than a number and a flag.
inline constexpr f64 kNoDeadline = 1.0e30;

}  // namespace cy::residency
