# `tools/quiet-host/`

The quiet-host premise: a library that says whether the host is quiet, and a wrapper that runs one
command only on a quiet host and fails, saying why, on a busy one.

| File | What it is |
|---|---|
| `host_load.h`, `host_load.cpp` | `cy::host-load`: what "quiet" means, read from `/proc`, with the measured program's own load subtracted. `samples/10-world --quiet-host` and the wrapper share it. |
| `main.cpp` | `cy_quiet_host [--wait-s <n>] -- <command...>`: the wrapper. `just test-quiet-host` builds the tree and runs it. |
| `quiet_host_test.py` | `smoke.quiet_host_before`, `smoke.quiet_host_across`, `smoke.quiet_host_own`, `smoke.quiet_host_nested`: the wrapper under four niced spinners it starts on purpose, around a command that spins on its own, and around one whose spinners start sessions of their own. |

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
   process together to use at most **two cores** and for CPU pressure (`/proc/pressure/cpu`) to
   stay at or under **10%**. Still busy at the deadline: the command is not run, `host too busy:
   <numbers>` on stderr, exit 1.
2. **Runs the command in a session of its own** (`setsid`), so that everything it starts — a
   build, ctest, the test binaries and their children — is told from the rest of the machine by
   its session id, and its CPU time (with the children each process has reaped) is subtracted.
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

## The criteria that carry the premise

`m0:test` (`just test-all`, which runs every suite the harness budgets, `unit.harness` and
`integration.harness` included), `m9:determinism-core` (`unit.determinism`, the fourth close's
case), `m6:culling` (`unit.render_gpu_culling`) and `m5b:window-artefact` / `m7:viewport`
(`smoke.editor_window`) — the last two are the suites M8.c's gate saw fail once each under the
ledger's own load and pass every time alone. Each runs `just test-quiet-host -- ...`, declares
`needs = ["exclusive"]` so the scheduler runs it with nothing beside it, and says in its own text
that a busy host is a failure with a reason, never a pass and never a skip.

`just test-quiet-host` builds the tree **before** the pre-run check, so that the build's own load is over
before the host is judged and the wrapped `just test-*` finds nothing to rebuild.

**Governed by**: `testing-and-quality`, `delivery-roadmap`.
