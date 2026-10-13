// SPDX-License-Identifier: MIT
#include <cy/rendering/gi/diagnostics.h>

namespace cy::rendering::gi {

const char* darkness_cause_name(DarknessCause cause) noexcept {
    switch (cause) {
        case DarknessCause::Lit:
            return "Lit";
        case DarknessCause::Occluded:
            return "Occluded";
        case DarknessCause::Unconverged:
            return "Unconverged";
        case DarknessCause::MissingProbes:
            return "MissingProbes";
        case DarknessCause::LowConfidence:
            return "LowConfidence";
        case DarknessCause::Invalidated:
            return "Invalidated";
        case DarknessCause::Count:
            break;
    }
    return "Unknown";
}

const char* gi_stage_name(GiStage stage) noexcept {
    switch (stage) {
        case GiStage::ScreenTrace:
            return "ScreenTrace";
        case GiStage::WorldTrace:
            return "WorldTrace";
        case GiStage::ProbeUpdate:
            return "ProbeUpdate";
        case GiStage::SurfaceCache:
            return "SurfaceCache";
        case GiStage::Denoise:
            return "Denoise";
        case GiStage::ReflectionResolve:
            return "ReflectionResolve";
        case GiStage::Count:
            break;
    }
    return "Unknown";
}

}  // namespace cy::rendering::gi
