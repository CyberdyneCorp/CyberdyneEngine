// The shared operator vocabulary's spellings. See include/cy/pcg/ops.h.

#include <cy/pcg/ops.h>

namespace cy::pcg {

const char* node_kind_name(NodeKind kind) noexcept {
    switch (kind) {
        case NodeKind::Constant:
            return "constant";
        case NodeKind::Noise:
            return "noise";
        case NodeKind::FieldRead:
            return "field-read";
        case NodeKind::Stamp:
            return "stamp";
        case NodeKind::Smooth:
            return "smooth";
        case NodeKind::Propagate:
            return "propagate";
        case NodeKind::Compute:
            return "compute";
        case NodeKind::Scatter:
            return "scatter";
        case NodeKind::Filter:
            return "filter";
        case NodeKind::Spacing:
            return "spacing";
        case NodeKind::Script:
            return "script";
        case NodeKind::Output:
            return "output";
        case NodeKind::kCount:
            break;
    }
    return "unknown";
}

bool produces_raster(NodeKind kind) noexcept {
    switch (kind) {
        case NodeKind::Constant:
        case NodeKind::Noise:
        case NodeKind::FieldRead:
        case NodeKind::Stamp:
        case NodeKind::Smooth:
        case NodeKind::Propagate:
        case NodeKind::Compute:
            return true;
        case NodeKind::Scatter:
        case NodeKind::Filter:
        case NodeKind::Spacing:
        case NodeKind::Script:
        case NodeKind::Output:
        case NodeKind::kCount:
            return false;
    }
    return false;
}

const char* neighbour_access_name(NeighbourAccess access) noexcept {
    switch (access) {
        case NeighbourAccess::None:
            return "none";
        case NeighbourAccess::Raster:
            return "raster";
        case NeighbourAccess::Candidates:
            return "candidates";
        case NeighbourAccess::AcceptedOutput:
            return "accepted-output";
    }
    return "unknown";
}

const char* iteration_policy_name(IterationPolicy policy) noexcept {
    switch (policy) {
        case IterationPolicy::None:
            return "none";
        case IterationPolicy::Convergence:
            return "convergence";
        case IterationPolicy::Budget:
            return "budget";
    }
    return "unknown";
}

const char* determinism_level_name(DeterminismLevel level) noexcept {
    switch (level) {
        case DeterminismLevel::Presentation:
            return "presentation";
        case DeterminismLevel::Gameplay:
            return "gameplay";
    }
    return "unknown";
}

const char* execution_domain_name(ExecutionDomain domain) noexcept {
    switch (domain) {
        case ExecutionDomain::Editor:
            return "editor";
        case ExecutionDomain::Cook:
            return "cook";
        case ExecutionDomain::Runtime:
            return "runtime";
        case ExecutionDomain::Streaming:
            return "streaming";
        case ExecutionDomain::Dynamic:
            return "dynamic";
        case ExecutionDomain::kCount:
            break;
    }
    return "unknown";
}

}  // namespace cy::pcg
