// SPDX-License-Identifier: MIT
// The fourth clock's private seam: how budget.cpp starts and stops sampling the case's thread. The
// public half — `blocked_on_host_ns()` and `budget_measures_host_blocking()` — is declared in
// cy/test/test.h. See host_blocking.cpp.

#pragma once

namespace cy::test::host_blocking {

/// Starts sampling the calling thread's scheduler state every `interval_ns`, or joins the session
/// already sampling it (a guard nested inside a case). False when nothing is sampled: not Linux, no
/// /proc, the sampler thread could not be started, or another thread owns the sampler. A caller
/// that was refused must treat its blocked time as zero, never as the difference of two readings
/// of a counter that some other thread was feeding.
[[nodiscard]] bool begin(unsigned long long interval_ns) noexcept;

/// Ends the calling thread's session, or leaves a session it joined.
void end() noexcept;

}  // namespace cy::test::host_blocking
