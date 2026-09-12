// CPU and GPU access to the SAME field, measured rather than asserted. Task 1.1.
//
// `environment-fields` — "WHEN CPU foliage placement and a GPU terrain material sample the same
// field at the same point, THEN they SHALL obtain the same value within the field's declared
// precision."
//
// WHAT IS ACTUALLY BEING COMPARED. `FieldStore::sample_at()` walks the store's tiles in f64 world
// coordinates using `decode_value()`; `sample_field_image()` walks a flat buffer of 32-bit words in
// f32 image-local coordinates using its own decode, its own blend and its own layer rule, and
// touches no part of this module's CPU side. Two implementations, no shared code below the
// declaration — which is what makes an agreement evidence.
//
// THE VALUES VARY PER LATTICE POINT ON PURPOSE. A uniformly filled tile agrees under a sampler
// whose lattice arithmetic is completely wrong, which is the shape of test this project has shipped
// before and does not intend to ship again. Every case here writes a pattern that differs at every
// lattice point and samples across tile boundaries, where the two implementations' tile lookups
// have to agree as well as their arithmetic.
//
// TWO CLAIMS, NOT ONE, AND THE SECOND IS WHY.
//
//   * ACROSS THE SWEEP, agreement within the field's declared precision. That is the
//     specification's own sentence, and it is the right bound for a sample between lattice points:
//     the two sides compute the same blend with f64 and f32 weights, and the last bits of a
//     mantissa are not a disagreement about the field.
//   * AT LATTICE CENTRES, agreement EXACTLY. At a cell centre the interpolation weight is zero on
//     both sides, so the sample IS one stored value decoded — no arithmetic to differ in — and any
//     difference at all is a decode that disagrees.
//
// The second claim exists because the first is not strong enough to be evidence on its own, and
// this was MEASURED rather than assumed: `decode_point()`'s `UNorm8` branch was changed to divide
// by 256 instead of 255 — a plausible transcription slip — and the sweep DID NOT NOTICE, because
// the error it introduces (1.5e-5 at most) is far inside a `UNorm8` field's 3.9e-3 quantum. The
// lattice-centre claim was added, the same mutation was run again, and "a quantised scalar agrees
// across the tile table" went red — 0.0823529 against 0.0820312 at the first centre, and 1019 of
// 1024 lattice centres disagreeing — as did the layered case at 254. It was then restored. That
// sequence is this milestone's `verified_failing` for the GPU half of section 1.1, and the first
// version of this file is exactly the test-that-cannot-fail the brief warns about.

#include <cy/test/test.h>

#include <cy/environment/gpu.h>
#include <cy/environment/store.h>

#include "fixtures.h"

using cy::environment::build_field_image;
using cy::environment::FieldDeclaration;
using cy::environment::FieldGpuImage;
using cy::environment::FieldImageLocal;
using cy::environment::FieldLayer;
using cy::environment::FieldRegistry;
using cy::environment::FieldResidency;
using cy::environment::FieldStore;
using cy::environment::FieldValue;
using cy::environment::image_local;
using cy::environment::ProducerKind;
using cy::environment::ProducerToken;
using cy::environment::sample_field_image;
using cy::environment::TileAddress;
namespace test = cy::environment::test;

namespace {

/// A value that differs at every lattice point of every tile, and is a pure function of the point's
/// place in the world rather than of the tile it happens to live in — so a sampler that confused
/// two tiles produces a value that is wrong rather than one that is merely from next door.
[[nodiscard]] cy::f32 pattern(cy::i32 tile_x, cy::i32 tile_z, cy::u32 x, cy::u32 z, cy::u32 y,
                              cy::f32 scale) noexcept {
    const cy::i64 global_x = (static_cast<cy::i64>(tile_x) * cy::environment::kTileCells) + x;
    const cy::i64 global_z = (static_cast<cy::i64>(tile_z) * cy::environment::kTileCells) + z;
    const cy::i64 mixed = (global_x * 7) + (global_z * 13) + (static_cast<cy::i64>(y) * 31);
    const auto folded = static_cast<cy::f32>(((mixed % 241) + 241) % 241);
    return (folded / 241.0F) * scale;
}

/// Fill one tile with the pattern, through the producer's own writer.
[[nodiscard]] cy::Status write_pattern(FieldStore& store, const ProducerToken& token,
                                       const FieldDeclaration& declaration,
                                       const TileAddress& address, cy::f32 scale) noexcept {
    cy::Expected<cy::environment::FieldWriter, cy::Error> writer = store.open_writer(token);
    if (!writer) {
        return cy::make_unexpected(writer.error());
    }
    if (cy::Status staged = writer->stage(address); !staged) {
        return staged;
    }
    for (cy::u32 y = 0; y < declaration.vertical_cells; ++y) {
        for (cy::u32 z = 0; z < cy::environment::kTileCells; ++z) {
            for (cy::u32 x = 0; x < cy::environment::kTileCells; ++x) {
                const cy::f32 value = pattern(address.x, address.z, x, z, y, scale);
                FieldValue sample = FieldValue::scalar(value);
                if (declaration.components() == 3) {
                    sample = FieldValue::vec3(value, -value, value * 0.5F);
                }
                if (declaration.type == cy::environment::FieldType::Category) {
                    sample = FieldValue::category(static_cast<cy::u32>(value));
                }
                if (cy::Status written = writer->set(address, x, y, z, sample); !written) {
                    return written;
                }
            }
        }
    }
    return writer->publish();
}

/// The agreement, measured over a grid of positions that crosses tile boundaries. Returns the worst
/// absolute deviation seen on any component.
[[nodiscard]] cy::f32 worst_deviation(const FieldStore& store, const FieldGpuImage& image,
                                      const FieldDeclaration& declaration, FieldResidency level,
                                      cy::f64 from, cy::f64 to, cy::u32 steps) noexcept {
    cy::f32 worst = 0.0F;
    const cy::f64 span = (to - from) / static_cast<cy::f64>(steps);
    for (cy::u32 iz = 0; iz < steps; ++iz) {
        for (cy::u32 ix = 0; ix < steps; ++ix) {
            const cy::world::WorldVec3d at =
                test::at(from + (static_cast<cy::f64>(ix) * span),
                         from + (static_cast<cy::f64>(iz) * span), 30.0);
            const cy::environment::FieldSample cpu = store.sample_at(declaration.id(), at, level);
            const FieldImageLocal local = image_local(image, at);
            const FieldValue gpu =
                sample_field_image(image.words.span(), local.x, local.y, local.z);
            for (cy::u32 component = 0; component < declaration.components(); ++component) {
                const cy::f32 difference =
                    cpu.value.components[component] - gpu.components[component];
                const cy::f32 magnitude = (difference < 0.0F) ? -difference : difference;
                worst = (magnitude > worst) ? magnitude : worst;
            }
        }
    }
    return worst;
}

/// The agreement at lattice centres, where the interpolation weight is zero on both sides and a
/// sample is one stored value decoded. Returns the number of positions at which the two disagree by
/// any amount at all.
[[nodiscard]] cy::u32 exact_disagreements(const FieldStore& store, const FieldGpuImage& image,
                                          const FieldDeclaration& declaration, FieldResidency level,
                                          cy::i32 from_cell, cy::i32 to_cell, cy::f64 y) noexcept {
    cy::u32 disagreements = 0;
    const auto metres =
        static_cast<cy::f64>(declaration.levels[static_cast<cy::u32>(level)].cell_metres);
    for (cy::i32 iz = from_cell; iz <= to_cell; ++iz) {
        for (cy::i32 ix = from_cell; ix <= to_cell; ++ix) {
            const cy::world::WorldVec3d at = test::at((static_cast<cy::f64>(ix) + 0.5) * metres,
                                                      (static_cast<cy::f64>(iz) + 0.5) * metres, y);
            const cy::environment::FieldSample cpu = store.sample_at(declaration.id(), at, level);
            const FieldImageLocal local = image_local(image, at);
            const FieldValue gpu =
                sample_field_image(image.words.span(), local.x, local.y, local.z);
            for (cy::u32 component = 0; component < declaration.components(); ++component) {
                if (cpu.value.components[component] != gpu.components[component]) {
                    ++disagreements;
                }
            }
        }
    }
    return disagreements;
}

}  // namespace

CY_TEST_CASE("a quantised scalar agrees across the tile table") {
    FieldRegistry registry(test::allocator());
    const cy::world::PartitionConfig partition = test::partition();
    FieldStore store(test::allocator(), registry, partition);
    const FieldDeclaration moisture = test::moisture_like();
    CY_REQUIRE(registry.declare(moisture).has_value());
    cy::Expected<ProducerToken, cy::Error> token =
        registry.claim(moisture.id(), "terrain.hydrology", ProducerKind::System);
    CY_REQUIRE(token.has_value());

    // Nine tiles, including negative coordinates — the half of the arithmetic a world east and
    // north of its origin never exercises.
    for (cy::i32 z = -1; z <= 1; ++z) {
        for (cy::i32 x = -1; x <= 1; ++x) {
            CY_REQUIRE(
                write_pattern(store, *token, moisture, test::tile_at(moisture.id(), 0, x, z), 1.0F)
                    .has_value());
        }
    }

    cy::Expected<FieldGpuImage, cy::Error> image =
        build_field_image(store, moisture.id(), FieldResidency::Local);
    CY_REQUIRE(image.has_value());
    CY_CHECK(cy::environment::is_field_image(image->words.span()));
    CY_CHECK_EQ(image->tiles, 9u);

    // Level 0 cells are 2 m, so a tile is 32 m: this sweeps from the middle of tile (-1,-1) to the
    // middle of tile (1,1), crossing both boundaries in both directions.
    const cy::f32 worst =
        worst_deviation(store, *image, moisture, FieldResidency::Local, -20.0, 44.0, 48);
    CY_TEST_MESSAGE("worst CPU/GPU deviation: ", worst, " against declared precision ",
                    moisture.resolved_precision());
    CY_CHECK_LE(worst, moisture.resolved_precision());

    // At a cell centre there is no arithmetic to differ in: the sample is one stored byte decoded,
    // and a single disagreement is a decode that does not match. This is the claim the sweep above
    // is too generous to make.
    const cy::environment::FieldSample centre =
        store.sample_at(moisture.id(), test::at(3.0, 3.0), FieldResidency::Local);
    const FieldImageLocal centre_local = image_local(*image, test::at(3.0, 3.0));
    CY_CHECK_EQ(centre.value.x(), sample_field_image(image->words.span(), centre_local.x,
                                                     centre_local.y, centre_local.z)
                                      .x());
    CY_CHECK_EQ(exact_disagreements(store, *image, moisture, FieldResidency::Local, -16, 15, 0.0),
                0u);

    // AND THE TWO ARE NOT AGREEING BECAUSE BOTH RETURN THE DEFAULT EVERYWHERE. Two positions three
    // cells apart must differ, on both sides, or this case would pass for a store with no tiles in
    // it and a sampler that reads nothing.
    const cy::environment::FieldSample left =
        store.sample_at(moisture.id(), test::at(3.0, 5.0), FieldResidency::Local);
    const cy::environment::FieldSample right =
        store.sample_at(moisture.id(), test::at(9.0, 5.0), FieldResidency::Local);
    CY_CHECK(left.resolved);
    CY_CHECK(right.resolved);
    CY_CHECK_NE(left.value.x(), right.value.x());
    const FieldImageLocal left_local = image_local(*image, test::at(3.0, 5.0));
    const FieldImageLocal right_local = image_local(*image, test::at(9.0, 5.0));
    CY_CHECK_NE(
        sample_field_image(image->words.span(), left_local.x, left_local.y, left_local.z).x(),
        sample_field_image(image->words.span(), right_local.x, right_local.y, right_local.z).x());
}

CY_TEST_CASE("a volumetric vector field agrees, column by column") {
    FieldRegistry registry(test::allocator());
    const cy::world::PartitionConfig partition = test::partition();
    FieldStore store(test::allocator(), registry, partition);
    const FieldDeclaration wind = test::wind_like();
    CY_REQUIRE(registry.declare(wind).has_value());
    cy::Expected<ProducerToken, cy::Error> token =
        registry.claim(wind.id(), "weather.wind", ProducerKind::System);
    CY_REQUIRE(token.has_value());

    for (cy::i32 z = 0; z <= 1; ++z) {
        for (cy::i32 x = 0; x <= 1; ++x) {
            CY_REQUIRE(write_pattern(store, *token, wind, test::tile_at(wind.id(), 0, x, z), 30.0F)
                           .has_value());
        }
    }

    cy::Expected<FieldGpuImage, cy::Error> image =
        build_field_image(store, wind.id(), FieldResidency::Local);
    CY_REQUIRE(image.has_value());

    // Four columns of 25 m; the sweep above samples at y = 30, between the first and second.
    const cy::f32 worst =
        worst_deviation(store, *image, wind, FieldResidency::Local, 4.0, 120.0, 40);
    CY_TEST_MESSAGE("worst CPU/GPU deviation (vec3, volumetric): ", worst);
    CY_CHECK_LE(worst, wind.resolved_precision());
    // Column centres sit at 12.5 m, so this is exact on all three components at every lattice point
    // of the two tiles.
    CY_CHECK_EQ(exact_disagreements(store, *image, wind, FieldResidency::Local, 0, 31, 12.5), 0u);
}

CY_TEST_CASE("a category agrees exactly, because neither side does arithmetic on it") {
    FieldRegistry registry(test::allocator());
    const cy::world::PartitionConfig partition = test::partition();
    FieldStore store(test::allocator(), registry, partition);
    const FieldDeclaration biome = test::biome_like();
    CY_REQUIRE(registry.declare(biome).has_value());
    cy::Expected<ProducerToken, cy::Error> token =
        registry.claim(biome.id(), "pcg.biome", ProducerKind::System);
    CY_REQUIRE(token.has_value());

    for (cy::i32 z = -1; z <= 0; ++z) {
        for (cy::i32 x = -1; x <= 0; ++x) {
            CY_REQUIRE(
                write_pattern(store, *token, biome, test::tile_at(biome.id(), 0, x, z), 200.0F)
                    .has_value());
        }
    }
    cy::Expected<FieldGpuImage, cy::Error> image =
        build_field_image(store, biome.id(), FieldResidency::Local);
    CY_REQUIRE(image.has_value());

    // Exact, not within a precision: a nearest sample of a raw integer is one stored byte on both
    // sides, and a difference of one would be a lattice index that disagrees.
    const cy::f32 worst =
        worst_deviation(store, *image, biome, FieldResidency::Local, -120.0, 120.0, 48);
    CY_CHECK_EQ(worst, 0.0F);
}

CY_TEST_CASE("both layers reach the image, and the image applies the declared rule") {
    FieldDeclaration declaration = test::moisture_like();
    declaration.name = "test.snow-depth";
    declaration.layer_rule = cy::environment::FieldLayerRule::Add;
    declaration.range_max = 2.0F;

    FieldRegistry registry(test::allocator());
    const cy::world::PartitionConfig partition = test::partition();
    FieldStore store(test::allocator(), registry, partition);
    CY_REQUIRE(registry.declare(declaration).has_value());
    cy::Expected<ProducerToken, cy::Error> token =
        registry.claim(declaration.id(), "weather.snow", ProducerKind::System);
    CY_REQUIRE(token.has_value());

    CY_REQUIRE(write_pattern(store, *token, declaration,
                             test::tile_at(declaration.id(), 0, 0, 0, FieldLayer::Base), 1.0F)
                   .has_value());
    CY_REQUIRE(write_pattern(store, *token, declaration,
                             test::tile_at(declaration.id(), 0, 0, 0, FieldLayer::Delta), 0.5F)
                   .has_value());

    cy::Expected<FieldGpuImage, cy::Error> image =
        build_field_image(store, declaration.id(), FieldResidency::Local);
    CY_REQUIRE(image.has_value());
    CY_CHECK_EQ(image->tiles, 2u);

    const cy::f32 worst =
        worst_deviation(store, *image, declaration, FieldResidency::Local, 1.0, 30.0, 32);
    // Two quanta, because a combined sample is two decodes added.
    CY_CHECK_LE(worst, declaration.resolved_precision() * 2.0F);
    CY_CHECK_EQ(exact_disagreements(store, *image, declaration, FieldResidency::Local, 0, 15, 0.0),
                0u);
}

CY_TEST_CASE("the image is a function of the tiles, not of the order they arrived in") {
    // Two stores fed the same tiles in opposite orders produce byte-identical buffers. Streaming
    // order is not content, and a GPU buffer that differed between two runs would make every hash
    // taken over it useless.
    const FieldDeclaration moisture = test::moisture_like();
    const cy::world::PartitionConfig partition = test::partition();

    FieldRegistry forward_registry(test::allocator());
    FieldStore forward(test::allocator(), forward_registry, partition);
    CY_REQUIRE(forward_registry.declare(moisture).has_value());
    cy::Expected<ProducerToken, cy::Error> forward_token =
        forward_registry.claim(moisture.id(), "terrain.hydrology", ProducerKind::System);
    CY_REQUIRE(forward_token.has_value());

    FieldRegistry reverse_registry(test::allocator());
    FieldStore reverse(test::allocator(), reverse_registry, partition);
    CY_REQUIRE(reverse_registry.declare(moisture).has_value());
    cy::Expected<ProducerToken, cy::Error> reverse_token =
        reverse_registry.claim(moisture.id(), "terrain.hydrology", ProducerKind::System);
    CY_REQUIRE(reverse_token.has_value());

    for (cy::i32 index = -2; index <= 2; ++index) {
        CY_REQUIRE(write_pattern(forward, *forward_token, moisture,
                                 test::tile_at(moisture.id(), 0, index, 0), 1.0F)
                       .has_value());
    }
    for (cy::i32 index = 2; index >= -2; --index) {
        CY_REQUIRE(write_pattern(reverse, *reverse_token, moisture,
                                 test::tile_at(moisture.id(), 0, index, 0), 1.0F)
                       .has_value());
    }

    cy::Expected<FieldGpuImage, cy::Error> first =
        build_field_image(forward, moisture.id(), FieldResidency::Local);
    cy::Expected<FieldGpuImage, cy::Error> second =
        build_field_image(reverse, moisture.id(), FieldResidency::Local);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_REQUIRE_EQ(first->words.size(), second->words.size());
    for (cy::usize index = 0; index < first->words.size(); ++index) {
        CY_CHECK_EQ(first->words[index], second->words[index]);
    }
    CY_CHECK_EQ(first->origin_x, second->origin_x);
}

CY_TEST_CASE("a buffer that is not a field image is refused rather than decoded") {
    // A shader handed the wrong buffer must be able to say so. Two word reads, and the answer is
    // the declared default rather than a shadow page read as moisture.
    cy::Array<cy::u32> nonsense(test::allocator());
    for (cy::u32 index = 0; index < 64; ++index) {
        CY_REQUIRE(nonsense.push_back(index * 0x9E37'79B9U).has_value());
    }
    CY_CHECK_FALSE(cy::environment::is_field_image(nonsense.span()));
    const FieldValue value = sample_field_image(nonsense.span(), 1.0F, 2.0F, 3.0F);
    CY_CHECK_EQ(value.x(), 0.0F);

    cy::Array<cy::u32> empty(test::allocator());
    CY_CHECK_FALSE(cy::environment::is_field_image(empty.span()));
}
