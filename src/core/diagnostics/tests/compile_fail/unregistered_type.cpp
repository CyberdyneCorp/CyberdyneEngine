// The declaration that must NOT compile: a field type that does not exist.
#include <cy/core/diagnostics/field.h>

namespace {
CY_TRACE_FIELD(frame_index, uint64, cy::Privacy::Public)
}  // namespace

int main() {
    return 0;
}
