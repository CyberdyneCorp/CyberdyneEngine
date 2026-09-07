#include <cy/servers/residency/types.h>

namespace cy::residency {

const char* subsystem_name(Subsystem subsystem) noexcept {
    switch (subsystem) {
        case Subsystem::Geometry:
            return "geometry";
        case Subsystem::Texture:
            return "texture";
        case Subsystem::Shadow:
            return "shadow";
        case Subsystem::Illumination:
            return "illumination";
        case Subsystem::Audio:
            return "audio";
        case Subsystem::WorldCells:
            return "world-cells";
        case Subsystem::Count:
            break;
    }
    return "unknown";
}

const char* cost_class_name(CostClass cost) noexcept {
    switch (cost) {
        case CostClass::Streamed:
            return "streamed";
        case CostClass::Decoded:
            return "decoded";
        case CostClass::Composed:
            return "composed";
        case CostClass::Rendered:
            return "rendered";
        case CostClass::Count:
            break;
    }
    return "unknown";
}

f32 cost_class_weight(CostClass cost) noexcept {
    // The numbers are ratios rather than milliseconds: a rendered shadow page is worth roughly an
    // order of magnitude more to keep than a streamed geometry page, and a composed page sits
    // between them because it is GPU work over data that is already resident. A subsystem that has
    // measured its production cost passes the measurement instead, and these are then unused.
    switch (cost) {
        case CostClass::Streamed:
            return 1.0F;
        case CostClass::Decoded:
            return 2.0F;
        case CostClass::Composed:
            return 5.0F;
        case CostClass::Rendered:
            return 10.0F;
        case CostClass::Count:
            break;
    }
    return 1.0F;
}

const char* quality_reason_name(QualityReason reason) noexcept {
    switch (reason) {
        case QualityReason::Resident:
            return "resident";
        case QualityReason::NeverRequested:
            return "never-requested";
        case QualityReason::Outscored:
            return "outscored";
        case QualityReason::BudgetBlocked:
            return "budget-blocked";
        case QualityReason::AwaitingProduction:
            return "awaiting-production";
        case QualityReason::DeadlineMissed:
            return "deadline-missed";
        case QualityReason::PressureCapped:
            return "pressure-capped";
        case QualityReason::Evicted:
            return "evicted";
        case QualityReason::Count:
            break;
    }
    return "unknown";
}

}  // namespace cy::residency
