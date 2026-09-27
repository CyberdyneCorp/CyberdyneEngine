// SPDX-License-Identifier: MIT
#pragma once

#include <cy/core/base/expected.h>
#include <cy/environment/gpu.h>
#include <cy/weather/system.h>

#include <optional>

namespace cy::sample::editor_window {

/// Weather-owned wind for a scene preview. The editor has no authored weather component yet, so
/// this provider runs one deterministic clear-weather tick around the viewed scene and exposes the
/// same field image that a world renderer consumes. It never invents wind values in the material.
class WindFieldPreview {
public:
    explicit WindFieldPreview(Allocator& allocator) noexcept;

    WindFieldPreview(const WindFieldPreview&) = delete;
    WindFieldPreview& operator=(const WindFieldPreview&) = delete;

    [[nodiscard]] Status initialize(const world::WorldVec3d& centre) noexcept;
    [[nodiscard]] bool ready() const noexcept { return image_.has_value(); }
    [[nodiscard]] bool covers(const world::WorldVec3d& camera) const noexcept;
    [[nodiscard]] const environment::FieldGpuImage& image() const noexcept { return *image_; }
    [[nodiscard]] const environment::FieldStore& store() const noexcept { return store_; }
    [[nodiscard]] environment::FieldId field() const noexcept { return wind_; }

private:
    world::PartitionConfig partition_;
    environment::FieldRegistry registry_;
    environment::FieldStore store_;
    weather::ClimateMap climate_;
    weather::WeatherSystem weather_;
    environment::FieldId wind_;
    world::WorldVec3d centre_;
    std::optional<environment::FieldGpuImage> image_;
};

}  // namespace cy::sample::editor_window
