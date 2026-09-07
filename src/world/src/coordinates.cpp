#include <cy/world/coordinates.h>

#include <bit>
#include <cmath>

namespace cy::world {
namespace {

/// Mix one 64-bit contribution into an identity hash, at the fixed world seed.
[[nodiscard]] u64 fold(u64 accumulator, u64 contribution) noexcept {
    return hash_combine(accumulator, hash_integer(contribution, kWorldHashSeed));
}

/// A f64's bits, so that a cell size or an origin contributes exactly, with no rounding and no
/// locale-dependent formatting anywhere on the path.
[[nodiscard]] u64 bits_of(f64 value) noexcept {
    // A negative zero and a positive zero are the same origin, and would otherwise be two
    // signatures for one configuration.
    const f64 normalised = (value == 0.0) ? 0.0 : value;
    return std::bit_cast<u64>(normalised);
}

/// floor(a / b) for a f64 quotient, as an i32. Saturates rather than wrapping: a coordinate outside
/// the representable grid is a diagnostic the partitioner produces, not undefined behaviour here.
[[nodiscard]] i32 floor_div(f64 numerator, f64 denominator) noexcept {
    const f64 quotient = std::floor(numerator / denominator);
    constexpr f64 kLow = -2147483648.0;
    constexpr f64 kHigh = 2147483647.0;
    if (!(quotient >= kLow)) {  // also catches NaN
        return -2147483647 - 1;
    }
    if (quotient > kHigh) {
        return 2147483647;
    }
    return static_cast<i32>(quotient);
}

}  // namespace

f64 PartitionConfig::cell_size(u8 level) const noexcept {
    f64 size = static_cast<f64>(base_cell_size);
    const u8 capped =
        (level < kMaxPartitionLevels) ? level : static_cast<u8>(kMaxPartitionLevels - 1);
    for (u8 step = 0; step < capped; ++step) {
        size *= static_cast<f64>(level_ratio);
    }
    return size;
}

u64 PartitionConfig::signature() const noexcept {
    u64 value = hash_integer(partition, kWorldHashSeed);
    value = fold(value, bits_of(static_cast<f64>(base_cell_size)));
    value = fold(value, levels);
    value = fold(value, level_ratio);
    value = fold(value, bits_of(origin.x));
    value = fold(value, bits_of(origin.y));
    value = fold(value, bits_of(origin.z));
    return value;
}

bool PartitionConfig::is_valid() const noexcept {
    return levels != 0 && levels <= kMaxPartitionLevels && base_cell_size > 0.0f &&
           std::isfinite(static_cast<f64>(base_cell_size)) && level_ratio >= 2;
}

CellId cell_id_of(const PartitionConfig& config, CellCoord coord) noexcept {
    u64 value = config.signature();
    value = fold(value, coord.level);
    value = fold(value, static_cast<u64>(static_cast<u32>(coord.x)));
    value = fold(value, static_cast<u64>(static_cast<u32>(coord.y)));
    value = fold(value, static_cast<u64>(static_cast<u32>(coord.z)));
    // Zero is reserved for "no cell", so the one value in 2^64 that would collide with it is moved.
    return CellId{(value == 0) ? 1 : value};
}

WorldVec3d to_absolute(const PartitionConfig& config, const WorldPosition& position) noexcept {
    const f64 size = config.cell_size(position.cell.level);
    return WorldVec3d{config.origin.x + (static_cast<f64>(position.cell.x) * size) +
                          static_cast<f64>(position.local.x),
                      config.origin.y + (static_cast<f64>(position.cell.y) * size) +
                          static_cast<f64>(position.local.y),
                      config.origin.z + (static_cast<f64>(position.cell.z) * size) +
                          static_cast<f64>(position.local.z)};
}

WorldPosition from_absolute(const PartitionConfig& config, const WorldVec3d& absolute,
                            u8 level) noexcept {
    const f64 size = config.cell_size(level);
    const f64 x = absolute.x - config.origin.x;
    const f64 y = absolute.y - config.origin.y;
    const f64 z = absolute.z - config.origin.z;

    WorldPosition position;
    position.cell.level = level;
    position.cell.x = floor_div(x, size);
    position.cell.y = floor_div(y, size);
    position.cell.z = floor_div(z, size);
    position.local.x = static_cast<f32>(x - (static_cast<f64>(position.cell.x) * size));
    position.local.y = static_cast<f32>(y - (static_cast<f64>(position.cell.y) * size));
    position.local.z = static_cast<f32>(z - (static_cast<f64>(position.cell.z) * size));
    return position;
}

WorldPosition normalized(const PartitionConfig& config, const WorldPosition& position) noexcept {
    return from_absolute(config, to_absolute(config, position), position.cell.level);
}

WorldVec3d simulation_origin(const PartitionConfig& config, CellCoord region) noexcept {
    const f64 size = config.cell_size(region.level);
    return WorldVec3d{config.origin.x + (static_cast<f64>(region.x) * size),
                      config.origin.y + (static_cast<f64>(region.y) * size),
                      config.origin.z + (static_cast<f64>(region.z) * size)};
}

Vec3 to_simulation_local(const PartitionConfig& config, const WorldPosition& position,
                         CellCoord region) noexcept {
    const WorldVec3d absolute = to_absolute(config, position);
    const WorldVec3d origin = simulation_origin(config, region);
    return Vec3{static_cast<f32>(absolute.x - origin.x), static_cast<f32>(absolute.y - origin.y),
                static_cast<f32>(absolute.z - origin.z)};
}

Aabb cell_bounds(const PartitionConfig& config, CellCoord coord) noexcept {
    const WorldVec3d low = simulation_origin(config, coord);
    const f64 size = config.cell_size(coord.level);
    const Vec3 min{static_cast<f32>(low.x), static_cast<f32>(low.y), static_cast<f32>(low.z)};
    const Vec3 max{static_cast<f32>(low.x + size), static_cast<f32>(low.y + size),
                   static_cast<f32>(low.z + size)};
    return Aabb::from_min_max(min, max);
}

}  // namespace cy::world
