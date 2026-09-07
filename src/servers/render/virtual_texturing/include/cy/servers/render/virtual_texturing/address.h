#pragma once
// CyberTexture's virtual address space: what an address is, and what an asset declares. Task 5.1.
//
// `virtual-texturing` — "Virtual address space": a virtual texture defines an address space far
// larger than the physical memory backing it, "addressed by virtual texture identity, mip level,
// tile coordinates, and layer". The asset declares virtual dimensions, tile size, border size, mip
// count, format, page index, a fallback representation and the semantic of its data.
//
// --- TWO CLAUSES THAT ARE ENFORCED HERE RATHER THAN REMEMBERED
// ------------------------------------
//
// "Physical cache coordinates SHALL NOT appear in the asset or in any authored content."
// `VirtualTextureDesc` has no field that could hold one. A physical tile index exists only in
// `PhysicalTileCache` and in a `PageTableEntry`, both of which are runtime state; nothing that is
// cooked or authored can name one, because there is no type here that could carry it.
//
// "The address encoding SHALL be versioned so that a future volumetric address space can be added
// without invalidating existing content." The top four bits of an encoded address are an
// `AddressKind`, and `Planar` is zero. A volumetric space takes kind 1 with its own field layout,
// and every planar address ever cooked still decodes.
//
// --- WHY THE ENCODING IS EXACTLY 56 BITS
// ------------------------------------------------------------
//
// `cy::residency::PageKey` carries a subsystem tag and 56 opaque bits, and the residency layer must
// be able to score a texture tile without knowing what one is. The whole address therefore fits
// inside those 56 bits by construction rather than by luck:
//
//   [55:52] kind      4 bits   Planar today; the reserved room for a volumetric space
//   [51:32] texture  20 bits   about a million virtual textures in one project
//   [31:27] mip       5 bits   up to 32 levels, which is a 4-gigatexel edge
//   [26:24] layer     3 bits   up to 8 layers sampled together; see "UDIM and multi-layer assets"
//   [23:12] tile y   12 bits   4096 tiles per axis
//   [11: 0] tile x   12 bits
//
// At a 128-texel tile that is a 524,288-texel edge per virtual texture, which is two orders of
// magnitude beyond anything a project will author, and the mip and layer fields are the two most
// likely to need room later — which is why the kind field exists.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

namespace cy::render::vt {

/// `virtual-texturing`'s four residency models. "the choice SHALL NOT be visible in the public
/// handle" — nothing here is a handle; this is the cook decision the material compiler reads.
enum class ResidencyModel : u8 {
    Resident = 0,     // icons, lookup tables, small masks
    StreamedMip,      // conventional mip streaming; ordinary assets and the compatibility path
    VirtualStreamed,  // pages cooked offline: terrain, large environments, UDIM sets, lightmaps
    VirtualRuntime,   // pages produced at runtime: composition, decals, world state, procedural
    Count,
};

[[nodiscard]] const char* residency_model_name(ResidencyModel model) noexcept;

/// Whether a model is paged at all. `Resident` and `StreamedMip` are not, and `StreamedMip` is
/// first-class rather than legacy: "virtualising every texture would be a regression for most of a
/// project's content".
[[nodiscard]] constexpr bool is_virtual(ResidencyModel model) noexcept {
    return model == ResidencyModel::VirtualStreamed || model == ResidencyModel::VirtualRuntime;
}

/// `virtual-texturing` — "Texture semantics and mip generation": the semantic drives compression
/// format, colour space, filtering defaults, cache class and mip generation.
enum class TextureSemantic : u8 {
    Colour = 0,
    Normal,
    Mask,
    Height,
    Data,
    HighDynamicRange,
    UserInterface,
    Count,
};

[[nodiscard]] const char* texture_semantic_name(TextureSemantic semantic) noexcept;

/// The physical caches are grouped by these — "block-compressed colour, two-channel normal,
/// single-channel mask, high dynamic range" — "rather than one physical cache per asset".
enum class FormatClass : u8 {
    BlockColour = 0,
    TwoChannelNormal,
    SingleChannelMask,
    HighDynamicRange,
    Count,
};

inline constexpr u32 kFormatClassCount = static_cast<u32>(FormatClass::Count);

[[nodiscard]] const char* format_class_name(FormatClass klass) noexcept;

/// The semantic decides the cache class. One function, so that a texture cannot end up in a cache
/// its filtering does not suit by somebody choosing separately.
[[nodiscard]] FormatClass format_class_for(TextureSemantic semantic) noexcept;

/// How a mip is reduced. `virtual-texturing`: mip generation "SHALL be semantic-aware", and a
/// semantically incorrect mip is a defect rather than a tuning issue "because it is magnified by
/// virtualisation: a wrong coarse mip is what a distant surface actually shows".
enum class MipReduction : u8 {
    ColourSpaceAware = 0,  // filtered in the correct space
    NormalRenormalised,    // filtered as vectors and renormalised
    CoveragePreserving,    // masks and alpha-tested textures: alpha rescaled to preserve coverage
    Arithmetic,            // height and data: the declared numeric reduction
};

[[nodiscard]] MipReduction mip_reduction_for(TextureSemantic semantic) noexcept;

/// Which address layout an encoded address uses. Four bits; `Planar` is zero so that a zeroed
/// address is a planar one.
enum class AddressKind : u8 {
    Planar = 0,
    // 1 is reserved for the volumetric address space the specification says must be addable
    // without invalidating existing content. Adding it changes nothing below this line.
};

/// "No physical tile" and "no resident level". They live beside the address rather than beside the
/// cache because three files need them — the page table, the producer interface and the sampler —
/// and only one of those three has any business including the cache's header.
inline constexpr u32 kNoPhysicalTile = 0xFFFFFFFFU;
inline constexpr u8 kNoResidentMip = 0xFFU;

inline constexpr u32 kAddressBits = 56;
inline constexpr u32 kMaxVirtualTextures = 1U << 20U;
inline constexpr u32 kMaxMipLevels = 32;
inline constexpr u32 kMaxLayers = 8;
inline constexpr u32 kMaxTilesPerAxis = 1U << 12U;

/// One page of one virtual texture.
struct VirtualAddress {
    u32 texture = 0;
    u16 tile_x = 0;
    u16 tile_y = 0;
    u8 mip = 0;
    u8 layer = 0;
    AddressKind kind = AddressKind::Planar;

    /// The 56-bit encoding. Fields wider than their bit budget are masked rather than rejected,
    /// because this runs per sample; `validate_address` is the checked form.
    [[nodiscard]] constexpr u64 encode() const noexcept {
        return (static_cast<u64>(kind) << 52U) | (static_cast<u64>(texture & 0xFFFFFU) << 32U) |
               (static_cast<u64>(mip & 0x1FU) << 27U) | (static_cast<u64>(layer & 0x7U) << 24U) |
               (static_cast<u64>(tile_y & 0xFFFU) << 12U) | static_cast<u64>(tile_x & 0xFFFU);
    }

    [[nodiscard]] static constexpr VirtualAddress decode(u64 encoded) noexcept {
        VirtualAddress address;
        address.kind = static_cast<AddressKind>((encoded >> 52U) & 0xFU);
        address.texture = static_cast<u32>((encoded >> 32U) & 0xFFFFFU);
        address.mip = static_cast<u8>((encoded >> 27U) & 0x1FU);
        address.layer = static_cast<u8>((encoded >> 24U) & 0x7U);
        address.tile_y = static_cast<u16>((encoded >> 12U) & 0xFFFU);
        address.tile_x = static_cast<u16>(encoded & 0xFFFU);
        return address;
    }

    [[nodiscard]] constexpr bool operator==(const VirtualAddress& other) const noexcept {
        return encode() == other.encode();
    }
};

/// Reject an address no encoding could represent. Separate from `encode()` so that the sampling
/// path is a shift and a mask and the asset path is checked.
[[nodiscard]] Status validate_address(const VirtualAddress& address) noexcept;

/// What the asset declares. Note what is absent: any physical coordinate, any cache identity, any
/// runtime state. See the header note.
struct VirtualTextureDesc {
    u32 id = 0;
    /// Virtual dimensions of mip 0, in texels.
    u32 width = 0;
    u32 height = 0;
    /// Texels per tile edge, excluding the border.
    u16 tile_size = 128;
    /// Border texels replicated from neighbouring pages, on every edge.
    u8 border = 4;
    u8 mip_count = 1;
    u8 layers = 1;
    /// How many of the coarsest levels are permanently resident. At least one, always: the mip
    /// tail is what makes "a surface is never missing, only blurry" true.
    u8 mip_tail_levels = 1;
    TextureSemantic semantic = TextureSemantic::Colour;
    ResidencyModel model = ResidencyModel::VirtualStreamed;
    /// Bytes one tile occupies in its cooked, block-compressed form, border included.
    u32 bytes_per_tile = 0;

    [[nodiscard]] FormatClass format_class() const noexcept { return format_class_for(semantic); }

    /// Tiles along an axis at `mip`. Rounded up, so a texture whose edge is not a whole number of
    /// tiles has one partial tile rather than a missing one.
    [[nodiscard]] u32 tiles_x(u8 mip) const noexcept;
    [[nodiscard]] u32 tiles_y(u8 mip) const noexcept;
    [[nodiscard]] u32 tile_count(u8 mip) const noexcept;

    /// The coarsest level, and whether `mip` is inside the always-resident tail.
    [[nodiscard]] u8 coarsest_mip() const noexcept {
        return static_cast<u8>((mip_count == 0) ? 0 : mip_count - 1);
    }
    [[nodiscard]] bool in_mip_tail(u8 mip) const noexcept {
        return mip_count != 0 && mip + mip_tail_levels >= mip_count;
    }
    /// The finest level of the tail — the coarsest level a sample can ever be forced down to.
    [[nodiscard]] u8 mip_tail_base() const noexcept {
        return static_cast<u8>((mip_count <= mip_tail_levels) ? 0 : mip_count - mip_tail_levels);
    }

    /// Bytes the tail occupies, over every layer. What the cache must reserve before anything else,
    /// and what a budget report should show as unavailable rather than free.
    [[nodiscard]] u64 mip_tail_bytes() const noexcept;
};

/// Reject a description that cannot be paged: no mips, no tail, a tile size of zero, a border too
/// small for the filtering it will see, dimensions beyond the address encoding.
[[nodiscard]] Status validate_description(const VirtualTextureDesc& desc) noexcept;

/// `virtual-texturing` — "Tiles and borders": the border must be "sized so that bilinear,
/// trilinear, and anisotropic filtering never sample across a tile edge into unrelated data", and
/// must "account for the maximum filtering width the sampling path may use".
///
/// Bilinear reaches half a texel; anisotropy of N reaches N/2 texels along the major axis; a
/// trilinear step samples the next mip, where the same reach is twice as wide in this level's
/// texels. The border is therefore `ceil(aniso / 2) + 1`, rounded up to an even number so that a
/// block-compressed tile's border is a whole number of 4x4 blocks on both edges.
[[nodiscard]] u8 required_border(u32 max_anisotropy) noexcept;

}  // namespace cy::render::vt
