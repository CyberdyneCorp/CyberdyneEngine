# `tests/harness/`

The framework seam and the fixtures. Every test binary links `cy::test-harness`, and this is the
only directory permitted to name doctest.

| File | What it is |
|---|---|
| `include/cy/test/test.h` | `CY_TEST_CASE`, the assertions, and the budget guard. The one include a test needs. |
| `include/cy/test/fixtures.h` | The injectable fixtures: a deterministic clock, a seeded generator, a temporary directory. |
| `src/main.cpp` | doctest's `main`, so no test file carries one. It pages the executable in before the first case: see below. |
| `src/image_warmup.cpp` | That warm-up: one read per page of the test executable's own segments, so the first case is not charged for loading the binary. |
| `src/budget.cpp` | The per-test budget check: the CPU clock, the wall-clock stall ceiling, and the contention clock that separates the two. |
| `src/host_blocking.cpp` | The fourth clock: a sampler that sees the case's thread in an uninterruptible wait. A diagnostic in the stall message, never an excuse. |
| `include/cy/test/quiet_host.h`, `src/quiet_host_marker.cpp` | Whether this run is inside a verified `cy_quiet_host`, and so whether the stall ceiling fails a case or only reports it. |
| `src/fixtures.cpp` | The filesystem half of the fixtures. |

## Why a wrapper

`design.md` §5 chose doctest on a compile-time argument — an argument about today's numbers at
today's scale. The wrapper is what keeps that choice reversible: replacing the framework is a change
to this directory, not to every test in the tree. The seam is enforced twice, at configure time and
at run time, because a seam nobody checks is a seam that has already been crossed.

## The four clocks, and why the budget needs all of them

A case is timed by **CPU time**, which is the budget, and by **wall clock**, which is the stall
ceiling — a hundred times the budget, there to catch a case that is waiting rather than working: a
sleep, a blocking read, a lock, a thread it joined. M9's closing gate added a third, and task 7.5b
says why: wall clock is a property of the machine as much as of the test, and the ledger's own load
was tripping the ceiling. Three wall-clock-bound suites each failed once across three full ledger
runs and each passed three to five times in isolation on an idle machine.

`testing-and-quality` already had the answer — "a case that exceeds its budget only under load SHALL
be reported as a case to reclassify rather than failing the build outright" — and no tolerance could
deliver it, because a case descheduled by forty spinning compilers and a case sleeping on a lock look
identical in wall clock.

They do not look identical in `/proc/thread-self/schedstat`, whose second field counts the
nanoseconds a thread spent **runnable and not running**. Preemption grows it; blocking does not. So
the ceiling is applied to wall clock *minus* contention, a case over the ceiling on a busy machine is
reported on stderr and counted by `contended_cases()`, and a case that slept past it still fails.

M11.c's fourth close found the half that clock cannot see: a machine busy with **I/O** rather than
CPU. `unit.determinism`'s first case held the suite for 655 ms with 0.21 ms of CPU and *zero*
runqueue wait beside a heavy build, and passed three runs in three alone. A thread waiting for the
disk — a read, or a major page fault on code a build pushed out of the page cache — is not runnable,
so it accumulates no runqueue wait. It is, however, in an **uninterruptible** sleep (`D`), which a
sleep, a futex, a join and a pipe read are not (`S`). So a sampler thread reads the case thread's
state from `/proc/self/task/<tid>/stat` every one to five milliseconds while the case runs, and
counts the time it sees in `D`. `host_blocking.cpp` records why the other instruments were rejected
as that clock — delay accounting is off by default since Linux 5.14 and needs root to enable, and
`/proc/pressure/io` is neither per thread nor proportional to one thread's wait.

**That clock is a diagnostic and never an excuse.** M11.c's fifth, sixth and seventh closes each
refuted an allowance that tried to subtract some of it as the host's: every uninterruptible wait
(refuted by a case whose **own** `vfork` child held it 300 ms on an idle host, reported `contended:`
and passed); at most the host's I/O pressure less the calling thread's share (refuted by a case
whose own sixteen threads made the pressure, 212 ms excused on a quiet host); at most the pressure
less the case's whole process tree's share, by a census (refuted by the same readers double-forked
and re-parented to init, 208 ms excused through the real guard). `D` says the kernel made the case
wait and never says what for, and nothing a process can read about itself tells its own orphans
from a build in another terminal. The owner's decision was to stop patching the allowance:

- the guard subtracts **runqueue wait and nothing else**; an uninterruptible wait is reported in the
  `stalled:` message — "%.3f ms in an uninterruptible wait" — so that a stall behind a build's
  writeback is explained rather than mistaken for a sleep, and it changes no verdict;
- the premise that the host is quiet is **stated in the ledger criteria** that run timing-sensitive
  suites and **checked from outside the process**: `just test-quiet-host -- <command>`
  (`tools/quiet-host/`, the same check `m11a:world-budget-on-a-device` has used since M11.c) waits
  for every other process together to use at most two cores and for CPU and I/O pressure to be
  low, for five seconds running (the I/O half since M11.c's eighth close, whose writeback burst
  used no core — `tools/quiet-host/README.md` has the measured limits), runs the command in a
  session of its own, judges the host across the run by its busiest second with the command's
  whole process tree subtracted, and **fails with `host too busy:` and the numbers** — never a
  pass, never a skip;
- on that premise a case over its ceiling is the case's own, whatever it was waiting for.

## Where the stall ceiling is enforced, and where it is only reported

M11.c's eighth and ninth closes found that the premise could not be guaranteed from the ledger's
text: three gates in a row found another route by which a wrapped suite also ran bare — a `&&`
after the wrapper, a matrix row's `test-all`, a sanitizer loop over `just test-sanitize --tests`.
So the owner moved the premise **into the harness** (option B):

- **Inside `cy_quiet_host`, a stall fails the case**, exactly as above. The harness learns it is
  inside from `CY_QUIET_HOST=<pid>:<start time>`, which the wrapper sets for the command it runs
  **only after its pre-run quiet check passed**, and it verifies the marker through `/proc` rather
  than believing it (`src/quiet_host_marker.cpp`): the pid must be a live **ancestor** of the test
  process, started at the marker's tick (field 22 of `/proc/<pid>/stat`, so a reused pid is
  refused), whose executable **is the `cy_quiet_host` this build produced** — the same device and
  inode as the path CMake compiles into the harness, not merely a file of that name (M11.d task
  9.7: a copy of `sh` renamed `cy_quiet_host` was trusted by the name check). A marker exported by
  hand in a shell, left over from an earlier run, copied from another terminal's wrapper, naming
  the shell or ctest, or set by a renamed impostor names no such ancestor and is refused. So is a
  wrapper from another build tree, a copy of the wrapper, or one relinked while it ran — each
  reported rather than failed, the direction this check always errs in. The stall message then says
  `enforced: inside cy_quiet_host: cy_quiet_host is pid N, ...`.
- **Anywhere else a stall is reported and does not fail the case**: the same `stalled:` diagnosis
  on stderr, marked `not enforced: not on a quiet host (<why the marker was refused>)`. That is a
  developer's `just test-unit`, a sanitizer run, a bare matrix row, and every run on Windows and
  macOS, where the wrapper does not exist. A wall-clock verdict nobody checked the host for says
  nothing about the case, and this is the owner's explicit, accepted trade-off: an unwrapped route
  loses the stall check rather than failing on the machine's account.
- **The CPU budget is enforced everywhere**, inside the wrapper and out, because CPU time does not
  grow when a neighbour spins. Its `over budget:` message names the stall ceiling's state as well.

Proven by `smoke.quiet_host_marker` (the wrapper around the stall probe, and the probe under six
forged markers, the sixth set by a copy of `sh` renamed `cy_quiet_host`), by `integration.harness`'s probe cases (both halves when the suite itself runs
inside the wrapper, as `m0:test` runs it; the unenforced half and the forgeries everywhere) and by
`unit.harness`'s marker cases.

| Clock | Reads | Fails a case when |
|---|---|---|
| CPU | `CLOCK_THREAD_CPUTIME_ID` | the case spends more than its kind's budget, scaled to this machine |
| Wall | `steady_clock` | the case's own time over the window exceeds a hundred times that budget, **and** the run is inside a verified `cy_quiet_host`; elsewhere reported, not failed |
| Contention | `/proc/thread-self/schedstat` | never — it is subtracted, and `budget_measures_contention()` says whether it exists |
| Host blocking | `/proc/self/task/<tid>/stat`, sampled | never — it is reported, and `budget_measures_host_blocking()` says whether it exists |

The decision is `stall_verdict()`, a pure function of three numbers — wall clock, runqueue wait and
the ceiling — so the arithmetic is tested without arranging for a machine to be busy. The
measurements it rests on are asserted separately: `tests/unit/harness/test_budget.cpp` shows that a
sleeping case accumulates neither contention nor host blocking, and that the verdict takes no input
that could excuse the three refuted probes. `tests/integration/test_budget_contention.cpp` shows
that a preempted case accumulates contention; that a case blocked by its own `vfork` child, its own
uncached reads, its own major page faults, or its own `vfork` child beside its own sixteen disk
readers is seen as blocked and is a stall; and that a case which sleeps, holds a mutex or burns its
CPU accumulates no blocking and is still a stall. It also runs `tests/integration/stall_probe.cpp` —
the gates' probes, real `CY_TEST_CASE`s — as a child process and reads the verdict the guard
printed: the vfork case, the vfork case beside its own readers, the vfork case beside its own
**orphaned** readers and the held mutex are `stalled:` — failed inside a verified wrapper,
reported and passed outside one — the spin fails as `over budget:` in both, and none is ever
`contended:`.

## The first case does not pay for loading the binary

The budget is the case's own CPU time, and the kernel charges a page fault to the thread that
takes it. Until this change, the first case in a binary was the first to execute most of the
binary's code, so it paid for paging that code in: work that belongs to starting the process,
which a second run of the same body in the same process does not repeat. CTest runs every binary
once, freshly built, so on a CI leg every case that runs first pays it.

On the hosted macOS runner a first touch of a code page is a 16 KiB fault that also checks the
page's code signature. `unit.animation_runtime_only` has one case. It spent 1.115 ms of CPU against
its 1 ms budget on main (CI run 37002996092), and in that runner's 60 earlier CI runs, 5 of the 13
distinct unit cases that went over budget were the first case of their binary. The same macOS
runner, idle, measured this as the first-case CPU of three such binaries (ms):

| Binary | First run, without the warm-up | Runs 2–25, without | First run, with | Runs 2–25, with |
|---|---|---|---|---|
| `unit.animation_runtime_only` | 0.177 | 0.035–0.058 | 0.025 | 0.027–0.035 |
| `unit.gameplay_spawn` | 0.172 | 0.052–0.075 | 0.046 | 0.043–0.060 |
| `unit.ecs` | 0.082 | 0.035–0.048 | 0.037 | 0.030–0.053 |

So `main` calls `warm_process_image()` before doctest runs anything: one read of each page of every
readable segment of the main executable (`dl_iterate_phdr` on Linux, the load commands of image 0
on Apple platforms). Every test binary links the engine statically, so that is all the code a case
runs apart from the system runtimes, which the loader has already paged in. The largest test
binary has about 2.7 MB of text, so the warm-up reads a few hundred pages, once per process.
Windows is not walked: its clock counts cycles, and no first-case overrun has been seen there.

It warms the image and nothing else. Memory a case allocates, the caches and the core's clock are
still the case's, or the calibration's. `unit.harness_image_warmup` is the regression: it is the
only case in its binary, and on Linux it reads `/proc/self/pagemap` to check that every page of the
executable is mapped before it runs. The binary carries a 256 KiB table that nothing reads, so with
the warm-up removed the check fails with 40 to 45 pages unmapped.

The walk (`touch_pages`) is not instrumented by AddressSanitizer. The first byte of a page can be
any byte of the image, including the redzone ASan places after a global, and an instrumented read
there aborts the process before doctest starts: every suite in the `jobs` sanitizer step failed
that way (CI run 37076205289). The case "the image warm-up may read a global's redzone without
tripping ASan" in `unit.harness` reads such a byte on purpose, so the exemption cannot be lost
without a sanitized run noticing, whatever the linker's layout.

## The macros under clang

Every harness macro expands into every test, so a warning one of them raises fails every test
binary. Two did, under compilers no CI leg runs on Linux:

- clang 18 reports `-Wdouble-promotion` when a float reaches `doctest::Approx`, which holds
  doubles. `CY_CHECK_NEAR` now casts its expectation and tolerance explicitly.
- clang 22 reports `__COUNTER__` as a C2y extension under `-Wpedantic`. Every test declaration
  expands one, in `CY_TEST_UNIQUE` and inside doctest's own macros. The diagnostic lands on the
  test's line, so including doctest as a system header does not suppress it.
  `CY_TEST_COUNTER_BEGIN` and `CY_TEST_COUNTER_END` suppress it for the expansion of the macros
  that use it. The suppression applies only on a clang that knows the warning, and the project's
  flags do not change.

Two suites, `render.vfx_gpu` and `render.material_binding`, used doctest's short names
(`TEST_CASE`, `CHECK_EQ`) instead of the wrapper. Their cases had no budget guard, and their
`__COUNTER__` was outside the suppression. They now use the `CY_*` macros, and `cy/test/test.h`
defines `DOCTEST_CONFIG_NO_SHORT_MACRO_NAMES`, so a test that uses a short name does not compile.

The long names reach past the wrapper the same way. Five `DOCTEST_INFO` calls in the graph
backend, debugger and equivalence suites compiled under GCC and failed every clang 22 build; they
are `CY_TEST_INFO` now. `cy_add_test` refuses at configure time a suite whose source calls any
`DOCTEST_*(` macro, on every compiler, so the next one fails the GCC legs too.
`integration.harness`'s seam case applies the same rule to `tests/` at run time. When a test needs
a doctest macro the wrapper lacks, add a `CY_*` spelling to `cy/test/test.h`, bracketed by
`CY_TEST_COUNTER_BEGIN`/`END` if it expands a `__COUNTER__`.

`integration.harness_under_clang` compiles `probe/clang_warnings_probe.cpp` with every clang++ on
PATH, under the project's warning options and `-Werror`. It skips on a host with no clang.

## What the harness does not have yet

`testing-and-quality` names a fuller set than this: scene and world fixtures, a mock platform and
display server, an in-memory filesystem mount, network condition simulation, image comparison, and
state hashing. Each is a mock or a comparison of an interface that does not exist yet, and a mock
written before its interface is a guess that has to be rewritten.

| Fixture | Arrives with |
|---|---|
| Mock `Platform` and `DisplayServer` | M0's headless implementations, once `core-platform-abstraction` has landed |
| World and scene fixtures | M2, with the ECS and the scene graph |
| In-memory filesystem mount | M2, with the asset pipeline's virtual filesystem |
| Image comparison | M3, with the renderer and `tests/render/` |
| State hashing, network conditions | M9, with determinism and replication |

**Governed by**: `testing-and-quality`.
