// The query's small vocabulary: how an answer was arrived at, and what a character is doing in it.
// M10 task 2.3. The query itself is `WaterSystem::query()` in system.cpp, because answering it
// needs the registry, the models and the streamer, and those are the system's.

#include <cy/water/query.h>

namespace cy::water {

const char* water_resolution_name(WaterResolution resolution) noexcept {
    switch (resolution) {
        case WaterResolution::None:
            return "none";
        case WaterResolution::MeanLevel:
            return "mean-level";
        case WaterResolution::Simulated:
            return "simulated";
    }
    return "unknown";
}

const char* character_water_state_name(CharacterWaterState state) noexcept {
    switch (state) {
        case CharacterWaterState::Dry:
            return "dry";
        case CharacterWaterState::Wading:
            return "wading";
        case CharacterWaterState::Swimming:
            return "swimming";
    }
    return "unknown";
}

CharacterWaterState character_state(f32 water_depth, const SwimThresholds& thresholds) noexcept {
    // Ordered deepest first, so a configuration whose two thresholds are equal produces swimming at
    // that depth rather than wading — a character in water deep enough for both is swimming.
    if (water_depth >= thresholds.swim_metres) {
        return CharacterWaterState::Swimming;
    }
    if (water_depth >= thresholds.wade_metres) {
        return CharacterWaterState::Wading;
    }
    return CharacterWaterState::Dry;
}

}  // namespace cy::water
