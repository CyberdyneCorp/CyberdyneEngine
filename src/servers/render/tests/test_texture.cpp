// Cooked texture formats, and the accounting a residency budget is charged. Task 4.2.2.
//
// `rendering-geometry-and-resources`, "Texture formats and compression":
//
//   "Textures SHALL declare their **usage** (colour, normal, data, HDR) so the cooker selects an
//    appropriate format and colour space automatically."
//   "WHEN a texture is marked as a normal map THEN it SHALL be cooked to BC5 (two-channel) on
//    desktop and the appropriate ASTC mode on mobile, and stored linear."
//   "Cooked textures SHALL include a full mip chain."
//
// The engine's format table is the whole of "automatically" — usage in, format out, no device asked
// and no platform branch at the call site — so the cases below are about the table being a function
// rather than about any one row's value. The one row that IS asserted by name is the normal map's,
// because the specification names it.

#include <cy/core/memory/system_allocator.h>
#include <cy/servers/render/server.h>
#include <cy/servers/render/types.h>
#include <cy/test/test.h>

using cy::u32;
using cy::u64;
using namespace cy::render;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

}  // namespace

CY_TEST_CASE("every format has a row, and an out-of-range one is answered rather than indexed") {
    for (u32 index = 0; index < static_cast<u32>(TextureFormat::Count); ++index) {
        const TextureFormatInfo& info = texture_format_info(static_cast<TextureFormat>(index));
        CY_CHECK(info.name[0] != '\0');
        CY_CHECK_GE(info.block_width, 1U);
        CY_CHECK_GE(info.block_height, 1U);
    }
    // `Undefined` is the one row with no bytes, so a caller can test the answer instead of
    // multiplying by it.
    CY_CHECK_EQ(texture_format_info(TextureFormat::Undefined).bytes_per_block, 0U);
    CY_CHECK_EQ(texture_format_byte_size(TextureFormat::Undefined, 64, 64), 0ULL);
}

CY_TEST_CASE("a normal map cooks to BC5 on desktop and to ASTC on mobile, both linear") {
    const TextureFormat desktop = desktop_format_for(TextureUsageClass::Normal);
    const TextureFormat mobile = mobile_format_for(TextureUsageClass::Normal);

    CY_CHECK(desktop == TextureFormat::Bc5Unorm);
    CY_CHECK(texture_format_info(desktop).is_compressed);
    CY_CHECK_FALSE(texture_format_info(desktop).is_srgb);
    CY_CHECK(texture_format_info(mobile).is_compressed);
    CY_CHECK_FALSE(texture_format_info(mobile).is_srgb);
}

CY_TEST_CASE("colour cooks sRGB and data cooks linear, on both platforms") {
    // The requirement behind it: "Colour textures SHALL be stored and sampled in sRGB, decoded to
    // linear by hardware. Data textures (normal, roughness, metallic, masks) SHALL be linear."
    CY_CHECK(texture_format_info(desktop_format_for(TextureUsageClass::Color)).is_srgb);
    CY_CHECK(texture_format_info(mobile_format_for(TextureUsageClass::Color)).is_srgb);
    CY_CHECK_FALSE(texture_format_info(desktop_format_for(TextureUsageClass::Data)).is_srgb);
    CY_CHECK_FALSE(texture_format_info(mobile_format_for(TextureUsageClass::Data)).is_srgb);
    // HDR is never sRGB-encoded: the encoding has no headroom above one, which is the whole point
    // of the class.
    CY_CHECK_FALSE(texture_format_info(desktop_format_for(TextureUsageClass::Hdr)).is_srgb);
    CY_CHECK_FALSE(texture_format_info(mobile_format_for(TextureUsageClass::Hdr)).is_srgb);
}

CY_TEST_CASE("every usage cooks to something on both platforms") {
    const TextureUsageClass kUsages[] = {TextureUsageClass::Color, TextureUsageClass::Normal,
                                         TextureUsageClass::Data, TextureUsageClass::Hdr};
    for (const TextureUsageClass usage : kUsages) {
        CY_CHECK(desktop_format_for(usage) != TextureFormat::Undefined);
        CY_CHECK(mobile_format_for(usage) != TextureFormat::Undefined);
    }
}

CY_TEST_CASE("a compressed mip smaller than a block still costs one block") {
    // The arithmetic every size computation gets wrong once: a 2x2 mip of a BC7 image is one 4x4
    // block, not a quarter of one. Under-counting here under-charges the residency budget by the
    // whole tail of every mip chain.
    CY_CHECK_EQ(texture_format_byte_size(TextureFormat::Bc7Unorm, 4, 4), 16ULL);
    CY_CHECK_EQ(texture_format_byte_size(TextureFormat::Bc7Unorm, 2, 2), 16ULL);
    CY_CHECK_EQ(texture_format_byte_size(TextureFormat::Bc7Unorm, 1, 1), 16ULL);
    CY_CHECK_EQ(texture_format_byte_size(TextureFormat::Bc7Unorm, 8, 8), 64ULL);
    // Uncompressed is the plain product.
    CY_CHECK_EQ(texture_format_byte_size(TextureFormat::Rgba8Unorm, 8, 8), 256ULL);
}

CY_TEST_CASE("a full mip chain reaches 1x1 and a non-square one is driven by its longer side") {
    CY_CHECK_EQ(full_mip_count(1, 1), 1U);
    CY_CHECK_EQ(full_mip_count(256, 256), 9U);
    CY_CHECK_EQ(full_mip_count(256, 4), 9U);
    CY_CHECK_EQ(full_mip_count(4, 256), 9U);
}

CY_TEST_CASE("the chain costs about a third more than its base mip") {
    // The classic 4/3 for an uncompressed square chain, which is the sanity check that says the
    // loop walks the whole chain rather than stopping early.
    const u64 base = texture_format_byte_size(TextureFormat::Rgba8Unorm, 256, 256);
    const u64 chain =
        texture_mip_chain_byte_size(TextureFormat::Rgba8Unorm, 256, 256, full_mip_count(256, 256));
    CY_CHECK_GT(chain, base);
    CY_CHECK_LT(chain, base + (base / 2U));

    // Asking for more levels than exist is answered with the ones that do, rather than reading past
    // the chain: the number arrives from an asset header and a bad one should not overcharge.
    CY_CHECK_EQ(texture_mip_chain_byte_size(TextureFormat::Rgba8Unorm, 256, 256, 64), chain);
    // Asking for fewer is a partially resident texture, which is what mip streaming charges.
    CY_CHECK_EQ(texture_mip_chain_byte_size(TextureFormat::Rgba8Unorm, 256, 256, 1), base);
    CY_CHECK_EQ(texture_mip_chain_byte_size(TextureFormat::Rgba8Unorm, 0, 256, 4), 0ULL);
}

CY_TEST_CASE("compression is what it is for: a BC7 chain is a quarter of the uncompressed one") {
    const u64 raw = texture_mip_chain_byte_size(TextureFormat::Rgba8Unorm, 1024, 1024,
                                                full_mip_count(1024, 1024));
    const u64 compressed = texture_mip_chain_byte_size(TextureFormat::Bc7Unorm, 1024, 1024,
                                                       full_mip_count(1024, 1024));
    CY_CHECK_GT(raw, compressed * 3U);
    CY_CHECK_LT(raw, compressed * 5U);
}

CY_TEST_CASE(
    "a created texture is charged its whole chain, and destroying it gives the bytes back") {
    RenderServer server(allocator());
    RenderServerConfig config;
    config.debug_primitive_capacity = 4;
    config.debug_label_capacity = 1;
    CY_REQUIRE(server.configure(config).has_value());
    CY_REQUIRE(server.initialize().has_value());

    TextureRecord record;
    record.name = cy::Name::intern("albedo");
    record.format = TextureFormat::Bc7Srgb;
    record.usage_class = TextureUsageClass::Color;
    record.width = 256;
    record.height = 256;
    // Left at zero: the server fills in the full chain rather than making every caller compute it.
    record.mip_levels = 0;

    const auto handle = server.create_texture(record);
    CY_REQUIRE(handle.has_value());
    const TextureRecord* stored = server.texture(*handle);
    CY_REQUIRE(stored != nullptr);
    CY_CHECK_EQ(stored->mip_levels, static_cast<cy::u16>(full_mip_count(256, 256)));
    CY_CHECK_EQ(stored->bytes, texture_mip_chain_byte_size(TextureFormat::Bc7Srgb, 256, 256,
                                                           full_mip_count(256, 256)));

    server.refresh_statistics(SceneHandle{});
    const u64 charged =
        server.frame_statistics().memory_bytes[static_cast<u32>(MemoryCategory::Textures)];
    CY_CHECK_EQ(charged, stored->bytes);

    server.destroy_texture(*handle);
    server.refresh_statistics(SceneHandle{});
    CY_CHECK_EQ(server.frame_statistics().memory_bytes[static_cast<u32>(MemoryCategory::Textures)],
                0ULL);
}

CY_TEST_CASE("an array texture is charged once per layer") {
    RenderServer server(allocator());
    RenderServerConfig config;
    config.debug_primitive_capacity = 4;
    config.debug_label_capacity = 1;
    CY_REQUIRE(server.configure(config).has_value());
    CY_REQUIRE(server.initialize().has_value());

    TextureRecord record;
    record.format = TextureFormat::Rgba16Sfloat;
    record.width = 64;
    record.height = 64;
    record.mip_levels = 1;
    record.array_layers = 6;

    const auto handle = server.create_texture(record);
    CY_REQUIRE(handle.has_value());
    CY_CHECK_EQ(server.texture(*handle)->bytes,
                texture_format_byte_size(TextureFormat::Rgba16Sfloat, 64, 64) * 6U);
}
