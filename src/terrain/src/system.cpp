// Terrain's registration as an environment producer, its declared consumption, the cooked soil tile
// loader, and the one call that propagates a deformation's invalidation. M10 tasks 2.1 and 2.2.

#include <cy/terrain/system.h>

#include <cmath>

namespace cy::terrain {
namespace {

/// The material layer at a position, from the cooked tiles alone. Null where nothing is resident.
[[nodiscard]] const TerrainTile* tile_for(const TerrainStore& store, f64 x, f64 z,
                                          u8 from_level) noexcept {
    return store.finest_at(x, z, from_level);
}

[[nodiscard]] u8 layer_at(const TerrainStore& store, f64 x, f64 z) noexcept {
    const TerrainTile* tile = tile_for(store, x, z, 0);
    if (tile == nullptr) {
        return 0;
    }
    const TerrainBounds bounds = tile_bounds(store.layout(), tile->coord);
    const f64 span = bounds.max_x - bounds.min_x;
    const auto i = static_cast<u32>((x - bounds.min_x) / span * kTileTexels);
    const auto j = static_cast<u32>((z - bounds.min_z) / span * kTileTexels);
    return tile
        ->texel((i < kTileTexels) ? i : kTileTexels - 1, (j < kTileTexels) ? j : kTileTexels - 1)
        .dominant();
}

}  // namespace

environment::FieldDeclaration soil_field_declaration(f32 fine_cell_metres) noexcept {
    environment::FieldDeclaration declaration;
    declaration.name = environment::fields::kSoil;
    declaration.unit = "index";
    declaration.semantics =
        "the dominant terrain material layer at a position; terrain is its only producer";
    declaration.type = environment::FieldType::Category;
    declaration.encoding = environment::FieldEncoding::Uint8;
    // A category is never interpolated: averaging soil 2 and soil 6 gives soil 4, which is a
    // different material that happens to sit between them in an arbitrary ordering.
    declaration.interpolation = environment::FieldInterpolation::Nearest;
    declaration.cadence = environment::FieldCadence::SlowlyVarying;
    declaration.production = environment::FieldProduction::Cpu;
    declaration.range_min = 0.0F;
    declaration.range_max = 255.0F;
    declaration.default_value = environment::FieldValue::category(0);
    declaration.levels[0] = environment::FieldLevel{fine_cell_metres, false};
    declaration.levels[1] = environment::FieldLevel{fine_cell_metres * 4.0F, false};
    declaration.levels[2] = environment::FieldLevel{fine_cell_metres * 32.0F, true};
    // Gameplay-visible: navigation cost and footstep audio both read it, so it must be the same
    // value on two machines whatever streamed.
    declaration.classification = determinism::SimulationClass::Persistent;
    declaration.gameplay_level = environment::FieldResidency::Macro;
    declaration.persistent = true;
    return declaration;
}

TerrainSystem::TerrainSystem(Allocator& allocator, const TerrainStore& store,
                             TerrainDeltaStore& deltas) noexcept
    : allocator_(&allocator), store_(&store), deltas_(&deltas) {}

Status TerrainSystem::register_producer(environment::FieldRegistry& registry,
                                        const char* producer_name, f32 fine_cell_metres) noexcept {
    const environment::FieldDeclaration declaration = soil_field_declaration(fine_cell_metres);
    if (Status declared = registry.declare(declaration); !declared) {
        return declared;
    }
    Expected<environment::ProducerToken, Error> claimed =
        registry.claim(declaration.id(), producer_name, environment::ProducerKind::Baked);
    if (!claimed) {
        // The refusal is passed through unchanged. A terrain that retried, or that wrote without a
        // token, would be the first system to route around the rule the substrate exists to keep.
        return Status{make_unexpected(claimed.error())};
    }
    token_ = std::move(claimed.value());
    soil_ = declaration.id();
    return ok();
}

Status TerrainSystem::declare_consumption(environment::FieldRegistry& registry,
                                          const char* consumer_name,
                                          const MaterialRuleSet& materials) noexcept {
    Array<environment::FieldId> read(*allocator_);
    if (Status listed = materials.fields_read(read); !listed) {
        return listed;
    }
    for (const environment::FieldId field : read.span()) {
        // Terrain's own material rules are authoritative content: they decide what the ground IS,
        // which navigation cost and footstep audio then read. Declaring the reader class is what
        // lets `FieldRegistry::validate()` refuse a terrain rule driven by a presentation-only
        // field before a frame runs.
        if (Status declared = registry.declare_consumer(
                consumer_name, determinism::SimulationClass::Persistent, field);
            !declared) {
            return declared;
        }
    }
    return ok();
}

Status TerrainSystem::write_soil_tile(environment::FieldWriter& writer,
                                      const environment::TileAddress& address,
                                      f32 cell) const noexcept {
    if (Status staged = writer.stage(address); !staged) {
        return staged;
    }
    for (u32 j = 0; j < environment::kTileCells; ++j) {
        for (u32 i = 0; i < environment::kTileCells; ++i) {
            const f64 x =
                ((static_cast<f64>(address.x) * environment::kTileCells) + static_cast<f64>(i)) *
                static_cast<f64>(cell);
            const f64 z =
                ((static_cast<f64>(address.z) * environment::kTileCells) + static_cast<f64>(j)) *
                static_cast<f64>(cell);
            const environment::FieldValue value =
                environment::FieldValue::category(layer_at(*store_, x, z));
            if (Status set = writer.set(address, i, 0, j, value); !set) {
                return set;
            }
        }
    }
    return ok();
}

Expected<u32, Error> TerrainSystem::publish_soil(environment::FieldStore& fields,
                                                 const HeightfieldSource& heights,
                                                 Span<const TileCoord> tiles,
                                                 environment::FieldResidency level) noexcept {
    (void)heights;
    if (!token_.valid()) {
        return fail(ErrorCode::PermissionDenied,
                    "terrain: the soil field has not been claimed; call register_producer first");
    }
    const environment::FieldDeclaration* declaration = fields.registry().declaration(soil_);
    if (declaration == nullptr) {
        return fail(ErrorCode::NotFound, "terrain: the soil field is not declared in this store");
    }
    const f32 cell = declaration->levels[static_cast<u32>(level)].cell_metres;
    if (cell <= 0.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "terrain: the soil field does not declare that residency level");
    }

    Expected<environment::FieldWriter, Error> opened = fields.open_writer(token_);
    if (!opened) {
        return make_unexpected(opened.error());
    }
    environment::FieldWriter writer = std::move(opened.value());

    u32 written = 0;
    const f64 field_tile_metres =
        static_cast<f64>(cell) * static_cast<f64>(environment::kTileCells);
    for (const TileCoord& coord : tiles) {
        const TerrainBounds bounds = tile_bounds(store_->layout(), coord);
        const auto first_x = static_cast<i32>(std::floor(bounds.min_x / field_tile_metres));
        const auto first_z = static_cast<i32>(std::floor(bounds.min_z / field_tile_metres));
        const auto last_x = static_cast<i32>(std::ceil(bounds.max_x / field_tile_metres)) - 1;
        const auto last_z = static_cast<i32>(std::ceil(bounds.max_z / field_tile_metres)) - 1;
        for (i32 tz = first_z; tz <= last_z; ++tz) {
            for (i32 tx = first_x; tx <= last_x; ++tx) {
                environment::TileAddress address;
                address.field = soil_;
                address.x = tx;
                address.z = tz;
                address.level = static_cast<u8>(level);
                address.layer = static_cast<u8>(environment::FieldLayer::Base);
                if (Status wrote = write_soil_tile(writer, address, cell); !wrote) {
                    return make_unexpected(wrote.error());
                }
                ++written;
            }
        }
    }
    if (Status published = writer.publish(); !published) {
        return make_unexpected(published.error());
    }
    return written;
}

Expected<TerrainChange, Error> TerrainSystem::deform(const Deformation& deformation,
                                                     const HeightfieldSource& heights,
                                                     MaterialPageCache* pages,
                                                     TerrainCollision* collision,
                                                     TerrainNavigation* navigation) noexcept {
    Expected<InvalidationSet, Error> applied = deltas_->apply(deformation);
    if (!applied) {
        return make_unexpected(applied.error());
    }

    TerrainChange change(*allocator_);
    change.invalidated = std::move(applied.value());

    if (pages != nullptr && change.invalidated.rendering) {
        // "only the pages covering it SHALL be invalidated and re-produced."
        change.pages_invalidated = pages->invalidate(deformation.bounds);
    }
    if (collision != nullptr && change.invalidated.collision) {
        Expected<u32, Error> rebuilt = collision->invalidate(heights, deformation.bounds);
        if (!rebuilt) {
            return make_unexpected(rebuilt.error());
        }
        change.collision_rebuilt = rebuilt.value();
    }
    if (navigation != nullptr && change.invalidated.navigation) {
        if (Status marked = navigation->mark_dirty(change.invalidated.navigation_dirty.span());
            !marked) {
            return make_unexpected(marked.error());
        }
        change.navigation_regions = static_cast<u32>(change.invalidated.navigation_dirty.size());
    }
    return change;
}

Status terrain_soil_loader(void* user, const environment::TileAddress& address,
                           Array<u8>& out) noexcept {
    const auto* context = static_cast<const SoilLoaderContext*>(user);
    if (context == nullptr || context->store == nullptr || context->registry == nullptr) {
        return fail(ErrorCode::InvalidArgument, "terrain: the soil loader needs its context");
    }
    const environment::FieldDeclaration* declaration =
        context->registry->declaration(address.field);
    if (declaration == nullptr) {
        return fail(ErrorCode::NotFound, "terrain: the soil field is not declared");
    }
    const f32 cell = declaration->levels[address.level].cell_metres;
    if (cell <= 0.0F) {
        return fail(ErrorCode::NotFound, "terrain: that residency level is not declared");
    }

    if (Status sized = out.resize(environment::FieldStore::tile_bytes(*declaration)); !sized) {
        return sized;
    }
    bool any = false;
    for (u32 j = 0; j < environment::kTileCells; ++j) {
        for (u32 i = 0; i < environment::kTileCells; ++i) {
            const f64 x =
                ((static_cast<f64>(address.x) * environment::kTileCells) + static_cast<f64>(i)) *
                static_cast<f64>(cell);
            const f64 z =
                ((static_cast<f64>(address.z) * environment::kTileCells) + static_cast<f64>(j)) *
                static_cast<f64>(cell);
            const TerrainTile* tile = context->store->finest_at(x, z, 0);
            any = any || (tile != nullptr);
            const environment::FieldValue value =
                environment::FieldValue::category(layer_at(*context->store, x, z));
            environment::encode_value(
                *declaration, value,
                out.data() + environment::FieldStore::lattice_offset(*declaration, i, 0, j));
        }
    }
    if (!any) {
        // "no cooked data for that tile", which the substrate turns into a cancelled admission
        // rather than a tile of invented values.
        return fail(ErrorCode::NotFound, "terrain: no terrain is resident under that field tile");
    }
    return ok();
}

}  // namespace cy::terrain
