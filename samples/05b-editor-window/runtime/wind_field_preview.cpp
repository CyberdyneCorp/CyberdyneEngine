// SPDX-License-Identifier: MIT
#include "wind_field_preview.h"

#include <cy/environment/field.h>

#include <cmath>
#include <utility>

namespace cy::sample::editor_window {
namespace {

[[nodiscard]] world::PartitionConfig preview_partition() noexcept {
    world::PartitionConfig config;
    config.partition = 1;
    config.base_cell_size = 128.0F;
    config.levels = 3;
    config.level_ratio = 4;
    return config;
}

[[nodiscard]] weather::WeatherConfig preview_config(const world::WorldVec3d& centre) noexcept {
    weather::WeatherConfig config;
    config.grid.origin_x = centre.x - 2048.0;
    config.grid.origin_z = centre.z - 2048.0;
    config.grid.regional_cell_metres = 512.0F;
    config.grid.width = 8;
    config.grid.height = 8;
    config.grid.refine_ratio = 4;
    config.grid.max_local_cells = 64;
    config.grid.macro_step_seconds = 1.0F;
    config.grid.relaxation_per_second = 0.02F;
    config.fields.accumulation = false;
    config.fields.ecosystem = false;
    config.fields.local_cell_metres = 16.0F;
    config.fields.regional_cell_metres = 64.0F;
    config.fields.macro_cell_metres = 256.0F;
    config.fields.wind_vertical_cells = 4;
    config.fields.wind_vertical_metres = 24.0F;
    return config;
}

[[nodiscard]] weather::ClimateSample preview_climate() noexcept {
    weather::ClimateSample climate;
    climate.mean_temperature_celsius = 14.0F;
    climate.temperature_range_celsius = 12.0F;
    climate.humidity = 0.65F;
    climate.prevailing_wind = Vec2{10.0F, 0.0F};
    climate.rainfall_potential_mm = 700.0F;
    climate.solar_exposure = 0.55F;
    climate.ocean_influence = 0.5F;
    return climate;
}

}  // namespace

WindFieldPreview::WindFieldPreview(Allocator& allocator) noexcept
    : partition_(preview_partition()),
      registry_(allocator),
      store_(allocator, registry_, partition_),
      climate_(allocator),
      weather_(allocator),
      wind_(environment::field_id(environment::fields::kWind)) {}

bool WindFieldPreview::covers(const world::WorldVec3d& camera) const noexcept {
    return ready() && std::abs(camera.x - centre_.x) <= 256.0 &&
           std::abs(camera.z - centre_.z) <= 256.0;
}

Status WindFieldPreview::initialize(const world::WorldVec3d& centre) noexcept {
    if (ready()) {
        return fail(ErrorCode::AlreadyExists, "editor wind field is already initialized");
    }
    if (Status set = climate_.set_uniform(preview_climate()); !set) {
        return set;
    }
    if (Status configured = weather_.configure(preview_config(centre), climate_); !configured) {
        return configured;
    }
    if (Status bound = weather_.bind_fields(registry_, store_); !bound) {
        return bound;
    }
    weather::PublishRegion region;
    region.min_x = centre.x - 512.0;
    region.min_z = centre.z - 512.0;
    region.max_x = centre.x + 512.0;
    region.max_z = centre.z + 512.0;
    region.level = environment::FieldResidency::Macro;
    weather_.set_publish_region(region);
    determinism::SimulationPoint at;
    at.tick = 1;
    auto advanced = weather_.advance(at, 1.0 / 60.0);
    if (!advanced) {
        return make_unexpected(advanced.error());
    }
    auto image = environment::build_deterministic_field_image(store_, wind_);
    if (!image) {
        return make_unexpected(image.error());
    }
    if (image->tiles == 0 || !environment::is_field_image(image->words.span())) {
        return fail(ErrorCode::Unavailable, "editor weather published no wind field image");
    }
    image_.emplace(std::move(*image));
    centre_ = centre;
    return ok();
}

}  // namespace cy::sample::editor_window
