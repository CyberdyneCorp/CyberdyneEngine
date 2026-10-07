// What navigation guarantees, declared from its worlds. See include/cy/navigation/determinism.h.

#include <cy/navigation/determinism.h>

namespace cy::navigation {

const char* nav_arithmetic_name(NavArithmetic arithmetic) noexcept {
    switch (arithmetic) {
        case NavArithmetic::Float:
            return "Float";
        case NavArithmetic::Fixed:
            return "Fixed";
    }
    return "unknown";
}

const NavWorldDeclaration* first_float_world(Span<const NavWorldDeclaration> worlds) noexcept {
    for (const NavWorldDeclaration& world : worlds) {
        const bool deterministic_everywhere =
            world.arithmetic == NavArithmetic::Fixed && !world.runtime_rebuilds;
        if (world.authoritative && !deterministic_everywhere) {
            return &world;
        }
    }
    return nullptr;
}

determinism::SubsystemDeterminism navigation_determinism(
    Span<const NavWorldDeclaration> worlds) noexcept {
    bool authoritative = false;
    for (const NavWorldDeclaration& world : worlds) {
        authoritative = authoritative || world.authoritative;
    }
    return determinism::SubsystemDeterminism{kNavigationSubsystem,
                                             first_float_world(worlds) == nullptr
                                                 ? determinism::DeterminismProfile::Lockstep
                                                 : determinism::DeterminismProfile::SamePlatform,
                                             authoritative};
}

}  // namespace cy::navigation
