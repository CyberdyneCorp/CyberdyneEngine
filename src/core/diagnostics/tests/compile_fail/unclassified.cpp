// The declaration that must NOT compile: the classification is omitted.
//
// design.md section 2: "Classification is a required argument of the field macro, not a decoration.
// There is no overload that omits it." The preprocessor reports
// `macro "CY_TRACE_FIELD" requires 3 arguments, but only 2 given`.
#include <cy/core/diagnostics/field.h>

namespace {
CY_TRACE_FIELD(frame_index, u64)
}  // namespace

int main() {
    return 0;
}
