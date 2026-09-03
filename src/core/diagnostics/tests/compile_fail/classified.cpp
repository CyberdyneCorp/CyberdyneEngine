// The declaration that must compile: three arguments, the third a Privacy constant.
#include <cy/core/diagnostics/field.h>

namespace {
CY_TRACE_FIELD(frame_index, u64, cy::Privacy::Public)
CY_TRACE_FIELD(user_path, string, cy::Privacy::Sensitive)
}  // namespace

int main() {
    return static_cast<int>(frame_index() + user_path());
}
