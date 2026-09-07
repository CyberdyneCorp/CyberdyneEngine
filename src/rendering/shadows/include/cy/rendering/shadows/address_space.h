#pragma once
// CyberShadow's virtual address space: pages, projections, and the texel footprint every other
// decision in this module is derived from. Task 8.1.
//
// `virtual-shadows` — "Virtual shadow address spaces". A shadowed light owns an address space far
// larger than the memory backing it, subdivided into fixed-size pages resolved through a page
// table. Nothing here allocates a texel: this file is the arithmetic that says which page a world
// position lands in and how big a shadow texel is there, and every consumer in this module —
// marking, clipmap snapping, invalidation, bias, the fallback chain — asks it rather than
// recomputing it.
//
// ================================================================================================
// THE POINT-LIGHT MAPPING IS INTERNAL, AND THAT IS WHAT `ShadowAddress` IS FOR
// ================================================================================================
//
// The specification requires that the point light projection — cube faces, octahedral, or another
// mapping — "SHALL be an internal decision behind the address space abstraction", and that
// "consumers of shadow lookups SHALL be unaffected" when it changes. That is only true if no
// consumer ever spells a face. So `address_of()` answers a `ShadowAddress` — a page and a UV inside
// it — and `PointMapping` is a field of the address space, not a parameter of the lookup.
// `test_address_space.cpp` changes the mapping and asserts the consumer-visible answer keeps its
// shape and stays inside a valid page.
//
// ================================================================================================
// PAGE SIZE IS REPORTED, NOT FIXED
// ================================================================================================
//
// "Page size SHALL be a measured platform decision reported by the implementation rather than fixed
// by this specification." `ShadowPageGeometry::page_texels` is therefore a configured value with a
// documented default and `describe_page_geometry()` is how a diagnostic reports the one in force.
// The engine does not have the measurement yet — there is no device in this module — so the default
// is stated as a default and not as a result.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>

namespace cy::rendering {

/// `virtual-shadows`' six shadow modes, in the order its table lists them. The renderer profile
/// constrains which are available; a light declares one.
enum class ShadowMode : u8 {
    None = 0,
    Baked,
    Conventional,
    Virtual,
    RayTraced,
    Hybrid,
    Count,
};

[[nodiscard]] const char* shadow_mode_name(ShadowMode mode) noexcept;

/// What a light's address space is shaped like. `Point` hides its own mapping — see the header.
enum class ShadowProjection : u8 {
    DirectionalClipmap = 0,
    Spot,
    Point,
    Count,
};

[[nodiscard]] const char* shadow_projection_name(ShadowProjection projection) noexcept;

/// The point light's internal mapping. Selectable "on measured distortion, filtering quality, and
/// culling cost"; no consumer of a lookup may name it.
enum class PointMapping : u8 {
    CubeFaces = 0,
    Octahedral,
    Count,
};

/// The page grid. `page_texels` is the platform decision; `virtual_texels` is the per-light logical
/// resolution, which is why it is here and not a constant.
struct ShadowPageGeometry {
    /// Texels along one edge of a page. A measured decision; 128 is the default this build reports.
    u32 page_texels = 128;
    /// Texels along one edge of the light's whole virtual space. Far larger than memory: a 16384²
    /// space is 128×128 pages, of which a frame typically needs tens.
    u32 virtual_texels = 16384;

    [[nodiscard]] constexpr u32 pages_per_side() const noexcept {
        return page_texels == 0 ? 0 : virtual_texels / page_texels;
    }

    [[nodiscard]] constexpr bool valid() const noexcept {
        return page_texels != 0 && virtual_texels != 0 && virtual_texels % page_texels == 0;
    }
};

/// One page of one light's address space. `level` is the clipmap level for a directional light and
/// the resolution level of the page pyramid for a local one; `face` is the point mapping's own
/// index and is always zero for the other two projections.
///
/// Packed into a u64 for the page table and for a deterministic sort — see `pack()`. The packing is
/// the identity: two marks of the same page from two receivers must collide, or compaction does
/// nothing.
struct VirtualPage {
    u32 light_slot = 0;
    u8 level = 0;
    u8 face = 0;
    u16 x = 0;
    u16 y = 0;

    /// x and y take sixteen bits each, the level four, the face four, and the light the remaining
    /// twenty-four. Masked rather than asserted, because a page id is built per marked receiver
    /// sample and this must stay a handful of instructions.
    [[nodiscard]] constexpr u64 pack() const noexcept {
        return (static_cast<u64>(light_slot & 0xFFFFFFU) << 40U) |
               (static_cast<u64>(face & 0xFU) << 36U) | (static_cast<u64>(level & 0xFU) << 32U) |
               (static_cast<u64>(y) << 16U) | static_cast<u64>(x);
    }

    [[nodiscard]] static constexpr VirtualPage unpack(u64 packed) noexcept {
        VirtualPage page;
        page.light_slot = static_cast<u32>(packed >> 40U);
        page.face = static_cast<u8>((packed >> 36U) & 0xFU);
        page.level = static_cast<u8>((packed >> 32U) & 0xFU);
        page.y = static_cast<u16>((packed >> 16U) & 0xFFFFU);
        page.x = static_cast<u16>(packed & 0xFFFFU);
        return page;
    }

    [[nodiscard]] constexpr bool operator==(const VirtualPage& other) const noexcept {
        return pack() == other.pack();
    }
};

/// What a lookup answers: which page, and where inside it. No consumer sees a face, a cube map or a
/// clipmap level's world extent through this type — which is the whole point of it.
struct ShadowAddress {
    VirtualPage page;
    /// Position inside the page, in [0,1)².
    Vec2 page_uv{0.0F, 0.0F};
    /// False when the position falls outside the light's space entirely — behind a spot cone, or
    /// past the level's extent. The fallback chain turns this into a substitution.
    bool inside = false;
};

/// The light's own orthonormal basis. `forward` is the light direction. Derived from the direction
/// alone and deterministically, because two levels of one light must agree about which way "x"
/// points or a page in one is not the same world square as the page above it.
struct ShadowBasis {
    Vec3 right{1.0F, 0.0F, 0.0F};
    Vec3 up{0.0F, 1.0F, 0.0F};
    Vec3 forward{0.0F, 0.0F, -1.0F};
};

[[nodiscard]] ShadowBasis shadow_basis(Vec3 light_direction) noexcept;

/// ONE LEVEL of one light's address space.
///
/// One level, not a whole light: a clip level's origin is snapped per frame and is therefore not a
/// property of the light, and a spot light's resolution levels each have their own page grid. A
/// frame builds an array of these — one per light per active level — which is exactly the table a
/// GPU page-marking dispatch reads. Value type; owns no storage.
struct ShadowAddressSpace {
    ShadowProjection projection = ShadowProjection::Spot;
    PointMapping point_mapping = PointMapping::CubeFaces;
    ShadowPageGeometry geometry;

    u32 light_slot = 0;
    /// Which resolution level this instance addresses. Clip level for a directional light; page
    /// pyramid level for a local one.
    u8 level = 0;

    ShadowBasis basis;
    /// The light's world position. Unused by a directional light, whose pages are placed by
    /// `origin_light_space` instead.
    Vec3 position{0.0F, 0.0F, 0.0F};
    /// Directional only: the snapped minimum corner of this level, in the light basis. Filled from
    /// a `ClipmapLevel` — see `clipmap.h`, which owns the snapping.
    Vec2 origin_light_space{0.0F, 0.0F};
    /// Directional: the world edge length this level covers. Spot and point: the far distance.
    f32 extent = 50.0F;
    /// Spot only: the cone half angle in radians.
    f32 half_angle = 0.7853982F;
};

/// Where `world_position` lands in this level of this light's space.
[[nodiscard]] ShadowAddress address_of(const ShadowAddressSpace& space,
                                       Vec3 world_position) noexcept;

/// The world-space edge length of one shadow texel at `distance` from the light. A directional
/// level ignores the distance — that is what constant texel density means. The number the bias
/// derivation, the page resolution selection and the caster policy all read, and the reason each of
/// them is measurement-driven rather than authored.
[[nodiscard]] f32 shadow_texel_world_size(const ShadowAddressSpace& space, f32 distance) noexcept;

/// The level whose texel footprint is no finer than `receiver_texel_world_size`, given `space` as
/// level 0 and a ladder that doubles per level. A distant receiver marks a coarser page than a near
/// one, which is the requirement's "page resolution SHALL be selected from the receiver's projected
/// shadow texel density".
[[nodiscard]] u8 select_level(const ShadowAddressSpace& base_level, f32 distance,
                              f32 receiver_texel_world_size, u8 level_count) noexcept;

/// The pages an axis-aligned box projects into at this level, appended to `out` and capped at
/// `out_capacity`. Returns how many it would have written — a caller that sees more than it passed
/// has a box covering more of the light than it budgeted for, which is a diagnostic, not a crash.
///
/// This is the primitive precise invalidation is built from: a caster's previous and current bounds
/// are two calls to it. A box entirely behind a local light projects into nothing and answers zero;
/// a box straddling the light is clamped to the level rather than refused, which is conservative in
/// the safe direction — extra pages are dirtied, never too few.
[[nodiscard]] u32 pages_covering(const ShadowAddressSpace& space, const Aabb& bounds,
                                 VirtualPage* out, u32 out_capacity) noexcept;

/// The page geometry in force, for a diagnostic: "128 texels/page, 16384² virtual, 128×128 pages".
/// Writes at most `capacity` bytes including the terminator and returns the length written.
[[nodiscard]] usize describe_page_geometry(const ShadowPageGeometry& geometry, char* out,
                                           usize capacity) noexcept;

}  // namespace cy::rendering
