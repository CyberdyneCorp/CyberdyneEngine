// SPDX-License-Identifier: MIT
// The fourth clock's private seam: how budget.cpp starts and stops sampling the case's thread, and
// asks what of its waiting the host may be charged with. The public half — `blocked_on_host_ns()`,
// `host_pressure_now()`, `host_stall_allowance()` and the rest — is declared in cy/test/test.h.
// See host_blocking.cpp.

#pragma once

namespace cy::test::host_blocking {

/// What `begin` did.
enum class Session {
    /// Nothing is sampled: not Linux, no /proc, the sampler thread could not be started, or another
    /// thread already owns the sampler. The caller must treat its blocked time as zero, never as
    /// the difference of two readings of a counter that some other thread was feeding.
    None,
    /// The calling thread opened the session. Only this guard is measured against the host's I/O
    /// pressure, because the pressure baseline is taken once per session.
    Owner,
    /// The calling thread joined a session a guard around it opened. Its blocked time is sampled,
    /// but it is excused nothing: the session's pressure baseline predates it.
    Nested,
};

/// Starts sampling the calling thread's scheduler state every `interval_ns`, or joins the session
/// already sampling it (a guard nested inside a case). Once the session has run `baseline_delay_ns`
/// the sampler takes the host-pressure baseline that `allowance` measures from; a case that ends
/// sooner never reads the pressure files at all.
[[nodiscard]] Session begin(unsigned long long interval_ns,
                            unsigned long long baseline_delay_ns) noexcept;

/// What of the session's uninterruptible waiting the HOST accounts for, from the pressure baseline
/// to now: `host_stall_allowance` over that window. Zero when the calling thread does not own the
/// session, when no baseline was taken, or when the host's pressure cannot be read. Called before
/// `end`, and only for a case already over its ceiling.
[[nodiscard]] unsigned long long allowance() noexcept;

/// Leaves the session `begin` joined. Called only after a `begin` that did not return None.
void end() noexcept;

}  // namespace cy::test::host_blocking
