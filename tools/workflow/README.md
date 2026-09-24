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
| `cmake/jobpool.cmake` | bakes `jobs.sh machine` into two wrapper scripts in the build tree and hands them to `cmake/launchers.cmake`: compiles through ccache's `prefix_command` (a cache hit takes no slot), links with `--jobserver` |
| `just _cargo-pool`, `just _job-slots` | the same pool for rustc (Cargo's `RUSTC_WRAPPER`) and for clang-tidy |

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

A link can use more than one core: GCC's `-flto=auto` (the Shipping configuration's IPO) runs `make
-j<cores>` under every link unless it finds a GNU make jobserver — measured on this workstation,
`make -j24` for one link. The link wrapper hands it a jobserver holding the link's own slot plus up
to seven slots free at that moment, so a Shipping link can never exceed the slots it holds.

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
exceeding the pool and both making progress; a link's jobserver never holding a busy slot; a probe
project that configures AND builds through the launchers (and installs none when `CI` is set); and
`build-reap --apply` keeping a marked tree and the matrix.
