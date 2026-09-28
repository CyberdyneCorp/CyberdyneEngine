# tools/workflow/ — target-platform selection, as a table rather than an `if`

M11.d task 7.7. `developer-workflow-and-just`:

> The workflow SHALL support **selecting a target platform** for build, test, deploy, and package
> recipes, and SHALL report clearly when a target cannot be built on the current host and why.
>
> **Scenario: An impossible target is explained** — WHEN a target cannot be built on the current
> host, THEN the workflow SHALL say so and state what is required.

## What was here before

One `if` in `just/build.just`'s `_resolve-target`, and one sentence:

> This milestone builds for the host only. Cross-compilation needs a CMake toolchain file and a
> platform SDK, and the first of those arrive with the mobile targets at M9.

Three problems, and the stale milestone number is the least of them. It said **the same words**
about `macos` (the engine supports it; this host cannot produce it), about `android` (no port exists
in the tree at all) and about `macoss` (a misspelling) — so a developer could not tell "wrong
machine" from "not written yet" from "you typed it wrong". And only `build-engine` took the flag:
`test-*`, `content-package` and any deploy recipe ignored it or passed it to a tool that would have
failed naming an option.

## What is here now

| Piece | What it decides |
|---|---|
| `just/targets.toml` | the targets, their hosts, what each needs, and **which rung owns** each unwritten one |
| `targets.py` | reads the table, resolves a request, prints the listing, and proves its own refusals |
| `just env-targets` | the table with this host's verdict on every row |
| `just env-targets --selftest` | the refusals' negative cases |

`build-engine`, `_ctest` (so every `test-*` recipe), `content-package` and `deploy-install` all
resolve through the same function, so `just build-engine --platform ios` and `just test-unit
--platform ios` cannot give different answers.

### The three refusals are deliberately different, in words and in exit status

| Request | Says | Exit |
|---|---|---|
| `macoss` | there is no such target, and lists the ones that exist | **3** |
| `macos` from Linux | the engine supports it, this host cannot produce it, what a cross-build would need, and which hosts can | **2** |
| `android` | no port exists in this tree, what one would need, and **the rung that writes it** | **2** |

Different exit statuses because a caller should be able to tell a typo from a machine limit without
parsing prose.

### The guard that stops this becoming `just/release.just`

`just/release.just`'s four recipes refuse naming *"M12 — build-and-packaging"*, **a milestone that
does not exist on a ladder whose `record.MILESTONES` ends at `m11e`** — so a developer who reads
that refusal is told to wait for nothing. This table's `owner` field is checked against
`record.MILESTONES` **at load**, and a target owned by a rung that is not on the ladder fails the
selftest and every recipe that resolves a target. M11.d task 7.8 is the same rule pointed at the
release recipes; this is it enforced mechanically for targets.

## What `deploy` does and does not do

`just deploy-install` is real: `cy_build install` into an installation root followed by
`cy_build verify`, which re-digests every chunk the installed build names. The verification is not a
second recipe on purpose — an install that succeeded while writing a chunk whose digest does not
match is exactly what an installation root exists to catch.

`just deploy-device` **refuses**, naming what is required (a transport; `RemoteFileProvider` is the
seam) and the rung that owns it (M11.e). A recipe that pretended to deploy to a device by copying a
directory would be the ninth check in this repository that cannot fail.

# How many jobs, and the machine-wide pool

M11.c's fifth close. The owner's rule: **at least two of the machine's cores stay free while
compiling, machine-wide** — however many builds run at once, the compile jobs between them stay at or
below cores − 2 (22 on the 24-core workstation the ledger closes on).

## Where a job count is chosen

| Piece | What it decides |
|---|---|
| `jobs.sh` | the one copy of the arithmetic. `jobs.sh build` = `CY_JOBS`, else `max(1, cores − reserved)`; `jobs.sh machine` = `max(1, cores − reserved)`, which `CY_JOBS` does not raise; `reserved` = `CY_RESERVED_CORES`, else 2, else 0 when `CI` is set |
| `just _jobs` | `jobs.sh build`. Every recipe that starts a compiler passes it: `cmake --build --parallel`, `cargo --jobs`, clang-tidy's `xargs -P`, and `RUST_TEST_THREADS` for `cargo test` |
| `job_slot.py` | the machine-wide cap: a pool of `jobs.sh machine` flock(2) slots under `/tmp/cyberdyne-job-slots-<uid>` that every compile and link waits in |
| `cmake/jobpool.cmake` | bakes `jobs.sh machine` into two wrapper scripts in the build tree and hands them to `cmake/launchers.cmake`: compiles through ccache's `prefix_command` (a cache hit takes no slot), links with `--link` |
| `cmake/profiles.cmake` | `CY_LTO_JOBS` (default 4): the fixed `-flto=N` a Shipping link runs its link-time optimisation at, in place of CMake's `-flto=auto` |
| `just _cargo-pool`, `just _job-slots` | the same pool for rustc (Cargo's `RUSTC_WRAPPER`, with `--jobserver`) and for clang-tidy |

ctest is not given a number: it runs one test at a time unless `CY_JOBS` is set, because the suites
carry wall-clock budgets.

## What happens when two builds overlap

Each build still starts its own `-j` worth of processes, but at most `jobs.sh machine` of them —
across every build of this user on this machine — run a compiler or a linker at any moment. The rest
wait **asleep** in `flock(2)`, which costs no CPU and does not count toward the load average, and
only one waiter at a time (the holder of the pool's `gate`) looks for a free slot, so a slot build A
frees goes to whoever has waited longest rather than straight back to A. The kernel drops a slot
when its holder exits however it exits, so a crash or a `kill -9` leaks nothing. Two builds started
together share the budget and each finishes later than it would alone.

## A link can use more than one core, and is never handed a jobserver

GCC's link-time optimisation (the Shipping configuration's IPO) runs its LTRANS stage as `make -jN`
under the link and streams WPA partitions from forked children. CMake's own flag is `-flto=auto`:
N is the machine's core count — measured here, `make -j24` for one link — or a GNU make jobserver
when the link finds one in `MAKEFLAGS`, and **GCC 13.3 takes a jobserver over a fixed `-flto=N` too
when one is there**.

Until M11.c's seventh close the link wrapper handed each link a jobserver holding its own slot plus
whatever was free at that moment. That is the defect the seventh close found: GCC 13.3's
`lto1 -fwpa` acquires one token per partition it streams and returns them only after the last one
is forked, so a link whose partitions outnumber its tokens waits in `read(2)` on the pipe for ever,
its finished children left as zombies. A saturated pool — three matrix rows building at once — hands
a link no spare slot as a matter of course, so `-j1` and an empty pipe; four release links stalled
holding 22 of 22 slots and every other build on the machine slept. Reproduced alone:
`job_slot.py --slots 1 --jobserver` around the link hung until killed at 300 s, and the same link
without the jobserver ran in 1 s. Nine small objects and `-flto-partition=max` reproduce it in the
selftest in under a second.

**Now a link gets no jobserver, and its parallelism is fixed.** `job-slot-link` runs
`job_slot.py --link`, which:

* strips every jobserver word out of `MAKEFLAGS` (one inherited from a make above, for the
  Makefiles generator), so the link never finds one;
* reads the link's parallelism from its own last `-flto=N` — `cmake/profiles.cmake` puts
  `-flto=${CY_LTO_JOBS}` on every Shipping link line, four by default — and **waits for that many
  slots before the link starts**, at most the pool's size; a link without LTO holds one;
* pins `-flto=<slots held>` on the command line when the link asked for `auto`, `jobserver`, a
  bare `-flto`, or more than the pool has, so the link can never run more jobs than it holds.

The link collects its slots one at a time while it holds the pool's gate: nobody else can take one
meanwhile, every holder finishes without waiting on the pool, and so the count is always reached
and two links cannot each hold half of what the other needs. On the 22-slot workstation five
Shipping links run side by side with two slots left for compiles.

Why four: on the release row's largest link (`cy_test_unit_memory`, 8 LTRANS partitions) serial
took 3.68 s, `-flto=2` 1.99 s, `-flto=4` 1.17 s, `-flto=8` 0.80 s and `-flto=auto` on an idle
machine 0.79 s. Four buys most of the speed-up for four slots; eight would hold the pool for two
links at a time to gain 0.37 s per link, and the WPA stage is serial whatever the number. Only the
link options change — the objects are still compiled with CMake's `-flto=auto -fno-fat-lto-objects`,
so ccache's hashes of every Shipping compile are unchanged. `-D CY_LTO_JOBS=<N>` overrides it.

**rustc keeps `--jobserver`** (`just _cargo-pool`): rustc is a cooperative client that always
proceeds on its implicit token, has a helper thread wait for more, and with none compiles on one
thread — measured, 16 codegen units under an empty pipe finish in 1.4 s — and without any jobserver
it would make its own 32-token one and run codegen on every core. clang-tidy takes one slot and no
jobserver. Clang's `-flto=thin` parallelises inside the linker; no profile in this tree links with
it, and the launcher does not bound it.

Compiles and links run at `nice 10`.

**Not in the pool:** Swift (bindings/swift drives `swiftc` through a Python script, and CMake has no
Swift launcher) and anything a developer runs by hand outside the recipes and CMake. `CY_JOB_POOL=OFF`
at configure time, or `CY_JOB_POOL=OFF` in the environment for `_job-slots`, turns the pool off.

## Measured

* **Two builds at `-j8` each over a pool of 4** (a 32-file C project configured with
  `CY_RESERVED_CORES=20`, sampled every 0.1 s): never more than **4** compilers running; the same
  two builds with `CY_JOB_POOL=OFF` ran **16** at once.
* **The whole machine while the engine built in `build/m11c-speed`** beside three other agents'
  builds: every running `cc1`/`cc1plus` held a slot (at most 10 compilers, 10 slots held, in 30
  samples): the trees reconfigured since the pool landed share it.
* **Load average over that build** (1-minute, every 10 s, 167 samples): mean 14.8, maximum 24.3,
  above 22 in 3 samples. The load average counts everything runnable — the other agents' tests and
  the deliberate 24-spinner load generator one of them ran for `m11a:world-budget-on-a-device` —
  so it bounds the pool from above rather than measuring it; the slot count is the measurement.

`tools/ci/test_recipes.py` holds each piece: the per-build default on 1 to 24 cores, `CY_JOBS` and
`CI`; every compile site in `just/*.just` passing `just _jobs`; two overlapping builds never
exceeding the pool and both making progress; a link finding no jobserver and being pinned to the
slots it holds; a real GCC LTO link started inside a saturated pool completing (it hung at
`c7ff54b`); two LTO links overlapping eight compiles never running more than the pool's jobs at
once; a probe project that configures AND builds through the launchers (and installs none when `CI`
is set); and `build-reap --apply` keeping a marked tree and the matrix.
