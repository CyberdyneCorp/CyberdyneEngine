// The declaration that must NOT compile: a classification that is not a constant.
//
// require_classification() is consteval, so a field cannot be classified by something decided at
// run time — which would be a classification nobody can audit.
#include <cy/core/diagnostics/field.h>

cy::Privacy chosen_at_runtime();

namespace {
CY_TRACE_FIELD(frame_index, u64, chosen_at_runtime())
}  // namespace

int main() {
    return 0;
}
