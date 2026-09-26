# `tools/quiet-host/`

The quiet-host premise: a library that says whether the host is quiet, and a wrapper that runs one
command only on a quiet host and fails, saying why, on a busy one.

| File | What it is |
|---|---|
| `host_load.h`, `host_load.cpp` | `cy::host-load`: what "quiet" means, read from `/proc`, with the measured program's own load subtracted. `samples/10-world --quiet-host` and the wrapper share it. |
| `main.cpp` | `cy_quiet_host [--wait-s <n>] -- <command...>`: the wrapper. `just test-quiet-host` builds the tree and runs it. |
| `quiet_host_test.py` | `smoke.quiet_host_before`, `smoke.quiet_host_across`, `smoke.quiet_host_own`, `smoke.quiet_host_nested`, `smoke.quiet_host_io`: the wrapper under four niced spinners it starts on purpose, around a command that spins on its own, around one whose spinners start sessions of their own, and under a bounded fsync writer that loads the disk. |

## Why a premise checked from outside, and not an allowance inside the harness

A wall-clock measurement on a loaded machine measures the machine. `m11a:world-budget-on-a-device`
learned it first — 10.8 ms worst alone, 58 to 286 ms beside 24 spinners, same binary — and since
M11.c has stated the host it assumes and checked it with this library.

The test harness's stall ceiling is the same kind of measurement, and M11.c's fourth close found it
tripped by a build beside the ledger: `unit.determinism` held for 655 ms with 0.21 ms of CPU, taking
major page faults behind a linker's writeback. Three closes then tried to excuse that from inside
the case — its uninterruptible time, then that bounded by the host's I/O pressure, then that with
the case's own process tree taken out by a census — and each was refuted by a case that made the
wait itself: its own `vfork`, its own sixteen threads reading the disk, its helpers double-forked
past the census. Nothing a process can read about itself says what the rest of the machine was
doing to it. The owner stopped it there: the harness excuses **no** uninterruptible wait
(`tests/harness/README.md`), and the ledger criteria that run timing-sensitive suites state the
premise and run through this wrapper instead.

## What the wrapper does

1. **Before.** Waits up to `--wait-s` (600 s by default), a second at a time, for every other
   process together to use at most **two cores**, for CPU pressure (`/proc/pressure/cpu`) to
   stay at or under **10%**, and for I/O pressure (`/proc/pressure/io`) to stay at or under
   **10% some** and **5% full** (below), all of it for **five seconds running**. Still
   busy at the deadline: the command is not run, `host too busy: <numbers>` on stderr, exit 1 —
   `host too busy: io pressure ...` when I/O is what decided it. Five, not one, because a burst
   comes in pulses: the kernel's flusher wakes every five seconds here, and a configure on this
   host read 11%, 39%, 10%, 7%, 7% and 59% I/O pressure in six successive seconds, so the first
   quiet second of a burst would start the suite into the rest of it.
2. **Runs the command in a session of its own** (`setsid`), so that everything it starts — a
   build, ctest, the test binaries and their children — is told from the rest of the machine by
   its session id, and its CPU time (with the children each process has reaped) is subtracted.
   The command's environment carries **`CY_QUIET_HOST=<the wrapper's pid>:<its start time>`**,
   set only here, after step 1 passed (below).
3. **Across the run.** Judges the host a second at a time and requires the **busiest** second to
   be quiet: one busy second is enough to make the one case that trips a ceiling. A run shorter
   than a second is held open to a full second, because a tick's resolution decides nothing
   shorter. Not quiet: `host too busy: <numbers>` on stderr and exit 1, whatever the command said.
4. Otherwise the command's own exit status is the wrapper's. Usage errors exit 2.

A descendant that starts a session of its own is still ours: `just test-all` runs
`smoke.quiet_host_own`, which is this wrapper again around a four-core command, and the census
follows the parent chain rather than the session id alone, or the inner run's spinners would count
against the outer run's host (they did, once: `m0:test`'s first proof failed on an idle machine).
`smoke.quiet_host_nested` is that case, kept red-able.

It errs one way. An orphan that also calls `setsid` (a daemon) or a member reaped by init is no
longer counted as ours, so the host looks busier, never quieter. Where `/proc` cannot be read the
verdict is "cannot tell", and that fails too.

## The marker the harness verifies (M11.c's ninth close, option B)

The test harness enforces its wall-clock stall ceiling **only inside this wrapper**; anywhere else
it prints the same `stalled:` diagnosis marked `not enforced: not on a quiet host` and passes the
case unless its CPU budget failed (`tests/harness/README.md`). It learns where it is from
`CY_QUIET_HOST`, and it does not take the variable's word for it. The value is the wrapper's pid
and its start time — field 22 of `/proc/<pid>/stat`, in clock ticks since boot, the kernel's own
nonce for one process instance — and the harness trusts it only when, read from `/proc`, that pid
is a live **ancestor** of the test process, started at that tick, whose executable **is this
build's `cy_quiet_host`**: `stat` on `/proc/<pid>/exe` (which the kernel resolves to the running
image, even once unlinked) must give the same device and inode as the wrapper whose path
tests/harness/CMakeLists.txt compiles in. So:

- the marker exists only in the command's environment, and only once the pre-run check passed:
  a wrapper that refuses the host never runs the command, so nothing ever sees a marker for a
  host that was not quiet;
- a marker **exported by hand** in a shell, left over from an earlier run or copied from another
  terminal's wrapper names a process that is not an ancestor (or is dead) and is refused;
- a marker naming a **reused pid** has the wrong start time and is refused;
- a marker naming the shell, ctest or any other real ancestor names something that is not
  `cy_quiet_host` and is refused;
- a marker set by **another binary renamed `cy_quiet_host`** — a copy of `sh` that names its own
  pid and start time and runs the suite as its child — is refused: until M11.d task 9.7 the check
  compared the executable's basename and trusted it;
- a wrapper from **another build tree**, a copy of this one, or this one relinked while it ran is
  not the same file and is refused too — reported, not failed;
- a nested wrapper overwrites the marker with its own, and a descendant that daemonises out of the
  tree loses its ancestry — both err towards reporting, never towards failing a case on a host
  nobody checked.

The host check across the run (step 3) is unchanged and still fails the whole run when it is
busy: the marker says the host was quiet before the command started, and the wrapper's exit
status says whether it stayed so. `smoke.quiet_host_marker` is the regression: the stall probe
fails as `stalled:` inside the wrapper, passes with the "not enforced" line outside it and under
six forged markers (the sixth a renamed impostor), and fails its CPU budget in both.

## I/O pressure, and why it is judged before the run only

M11.c's eighth close saw the ledger's load reach 59 with no compiler running: 1.9 GB that
`m8b:feature-options-off` had just built was being written back onto the one SATA disk, kernel I/O
pressure read `full avg10=70%`, and 37 processes sat in uninterruptible wait. Nobody was using a
core, so a CPU-only check called that host quiet, and a wrapped suite started into it would have
taken the burst as a `stalled:` case. So a second is also busy when some task waited on I/O for
more than the `some` limit of it, or every non-idle task did at once for more than the `full` limit.

THE LIMITS, FROM MEASUREMENT ON THIS HOST (24 cores, one SATA SSD holding the repository, the
ccache store and `/tmp`), each a second at a time from the `total=` counters exactly as the
wrapper reads them:

| Host state | Seconds | I/O `some` | I/O `full` |
|---|---|---|---|
| idle (two samples, one while a Python prover ran) | 360 | at most 3.0% | at most 2.7% |
| a cold `just build-engine` of `build/m11c-qh2`, ccache mostly missing | 854 | median 63%, max 99.9%; above 10% in 74% of seconds | median 57%, max 99.2%; above 5% in 74% |
| one bounded fsync writer (`smoke.quiet_host_io`'s, steady) | 12 | 16% in its first second, rising to 79% | 16% to 78% |
| the eighth close's writeback burst | — | — | `avg10=70%` |

So the limits are **10% some** and **5% full**: three and two times the idle host's worst second,
and below the writer's first. `full` is the tighter because it is the one that stops a suite
outright — nothing on the host ran while it accrued. A cold build is well above both, which is
the point: the recipe builds FIRST, and the wrapper then waits for that build's writeback to
drain before it starts the suite rather than starting the suite into it.

PSI is machine-wide: it cannot subtract our own I/O the way the core count subtracts our own CPU.
So, exactly like CPU pressure, it is judged **before** the run, while the wrapper is idle, and not
across it — a suite's own fsyncs, or `smoke.quiet_host_io`'s writer inside `m0:test`'s run, would
otherwise be counted against the host they are measuring. A burst that starts after the check is
not seen; that errs the way a CPU-pressure burst mid-run always has, and the wrapper's ten-minute
wait is what lets a burst already in progress drain before the suite starts rather than into it.

## The criteria that carry the premise

`m0:test` (`just test-all`, which runs every suite the harness budgets, `unit.harness` and
`integration.harness` included), `m9:determinism-core` and `m2:determinism` (`unit.determinism`,
the fourth close's case), `m6:culling` (`unit.render_gpu_culling` and
`integration.render_gpu_culling_teardown`) and `m5b:window-artefact` / `m7:viewport`
(`smoke.editor_window`) — the last two are the suites M8.c's gate saw fail once each under the
ledger's own load and pass every time alone — and, since M11.c's eighth close, `four-profiles`
(each matrix row's `just test-all`, in all nineteen byte-identical declarations) and
`m11d5:renderer-options-off-is-clean` (its tree's `just test-all`). Each runs
`just test-quiet-host -- ...`, declares `needs = ["exclusive"]` so the scheduler runs it with
nothing beside it, and says in its own text that a busy host is a failure with a reason, never a
pass and never a skip.

**What the ledger rule still checks, and what it no longer claims.** `m6:culling` was
`just test-quiet-host -- just test-unit ... && just test-integration ...` until the eighth close:
the ledger runs a body with `bash -c`, so the `&&` was that shell's and the teardown suite ran
bare after the wrapper exited. Several suites under one premise are ONE command,
`just test-quiet-host -- just test-suites <kind>:<regex>...`. `tools/roadmap/quiet_host.py`, run
by `just roadmap-test` (`test_quiet_host_bodies`) over every ledger, checks the two things that
are still true: nothing follows the wrapped command on its line but `|| exit <n>`, and a wrapped
criterion declares `exclusive`. It USED to also name six suites and fail any body that ran one
outside the wrapper; the ninth close's gate found `m3:sanitizers-render` and `m2:asan-world`
running two of them through a `for t in ...; do just test-sanitize --tests "$t"` loop it could not
see, and no reading of a body's text sees every route. Since option B that clause is gone rather
than taught another route: a suite run outside the wrapper — a sanitizer loop, a developer's
`just test-unit` — is a run whose stalls the harness **reports and does not enforce**, honestly,
so it is no longer a hole in the premise. `where = "ci"` criteria are not judged:
`three-platforms` runs on Windows and macOS too, where the wrapper does not exist and the stall
ceiling is therefore reported only.

`just test-quiet-host` builds the tree **before** the pre-run check, so that the build's own load is over
before the host is judged and the wrapped `just test-*` finds nothing to rebuild.

**Governed by**: `testing-and-quality`, `delivery-roadmap`.
