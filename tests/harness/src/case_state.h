// SPDX-License-Identifier: MIT
// What doctest knows about the case that is running, read from its implementation's state. Defined
// in main.cpp, the one translation unit that compiles doctest's implementation and so the only one
// that can see it. budget.cpp asks before it runs a case a second time.

#pragma once

namespace cy::test::detail {

struct CaseState {
    /// Checks the running case has failed so far, across every run of it.
    int failed_checks = 0;
    /// Whether the pass doctest is running entered a subcase.
    bool entered_subcase = false;
};

[[nodiscard]] CaseState current_case_state() noexcept;

}  // namespace cy::test::detail
