// The entry point every test binary links, so that no test file carries a main.
//
// It is doctest's own runner: option parsing, filtering (`--test-case=`, `--test-suite=`), and the
// reporters — including `--reporters=junit --out=<file>`, which is the machine-readable result
// `testing-and-quality` requires of the harness. CTest's `--output-junit` covers the suite level;
// this covers the test-case level inside one binary.
//
// Defining the implementation here rather than in a test file is what keeps doctest's translation
// unit out of the incremental build: a test that changes recompiles itself, not the framework.
//
// The one thing added to doctest's main is `warm_process_image()`, before any case runs: paging in
// the executable is the process's cost, and charged to whichever case ran first it took a case
// over its budget on the macOS runner. See cy/test/test.h. The other is `current_case_state()`,
// which the budget guard asks before it runs an over-budget case again.

#define DOCTEST_CONFIG_IMPLEMENT

#include <cy/test/test.h>

#include "case_state.h"

namespace cy::test::detail {

// doctest's own bookkeeping, which nothing outside its implementation can name. Both fields are
// reset by doctest itself: the failure count when a case starts, `reachedLeaf` before each pass.
CaseState current_case_state() noexcept {
    const doctest::detail::ContextState* state = doctest::detail::g_cs;
    if (state == nullptr) {
        return {};
    }
    return {static_cast<int>(state->numAssertsFailedCurrentTest_atomic), state->reachedLeaf};
}

}  // namespace cy::test::detail

int main(int argc, char** argv) {
    (void)cy::test::warm_process_image();
    doctest::Context context(argc, argv);
    return context.run();
}
