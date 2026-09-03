// Enumerator spellings for attributes.h. Task 1.1.3.
//
// A switch rather than a table indexed by the enumerator, so that adding a unit without adding its
// name is a compiler warning about an unhandled case rather than a silent read past the end of an
// array.

#include <cy/core/reflect/attributes.h>

namespace cy::reflect {

const char* unit_name(UnitKind unit) noexcept {
    switch (unit) {
        case UnitKind::None:
            return "none";
        case UnitKind::Metres:
            return "metres";
        case UnitKind::Radians:
            return "radians";
        case UnitKind::Degrees:
            return "degrees";
        case UnitKind::Seconds:
            return "seconds";
        case UnitKind::Milliseconds:
            return "milliseconds";
        case UnitKind::Kilograms:
            return "kilograms";
        case UnitKind::Newtons:
            return "newtons";
        case UnitKind::Percent:
            return "percent";
    }
    return "unknown";
}

const char* persistence_name(PersistenceKind persistence) noexcept {
    switch (persistence) {
        case PersistenceKind::Authoring:
            return "Authoring";
        case PersistenceKind::RuntimeState:
            return "RuntimeState";
        case PersistenceKind::PersistentState:
            return "PersistentState";
        case PersistenceKind::Derived:
            return "Derived";
    }
    return "unknown";
}

}  // namespace cy::reflect
