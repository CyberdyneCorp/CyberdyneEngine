# just/test.just — Test, graduated by cost. Each recipe states its expected duration; the fast set
# exceeding its budget is a regression to correct, not a new normal.
#
# The graduation is the taxonomy in `testing-and-quality`, and it is data rather than convention:
# tests/CMakeLists.txt gives every suite its kind as a CTest label and its budget as a compiled-in
# constant, so these recipes select a suite by label and nothing here can drift away from the table.
#
# Every recipe builds first. A test recipe that runs whatever binaries happen to be lying in the
# build tree reports on a revision nobody has, and CMake and Ninja decide what is out of date — a
# recipe that tried to would be reimplementing incremental logic, which the workflow specification
# forbids.

# Unit tests: sub-millisecond, no I/O. Under a minute including the build. Run on every save.
test-unit *args:
    @just _ctest unit {{args}}

# Integration tests: subsystems together, a second each. Seconds to a couple of minutes. Before pushing.
test-integration *args:
    @just _ctest integration {{args}}

# Smoke tests: the samples, headless, asserted to exit cleanly. Thirty seconds each; a minute or two.
test-smoke *args:
    @just _ctest smoke {{args}}

# Every suite: unit, integration, smoke, and anything registered outside them. What CI runs. Minutes.
test-all *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    failed=()
    # `unclassified` is the fourth pass, and it is not a suite: it runs every test that carries none
    # of the three labels. Without it, a test registered with a bare add_test() would be run by
    # nobody — invisible to every recipe here and green in CI by never executing. Running it is the
    # honest half; the recipe also names it, because a test with no label is a test with no budget
    # and the taxonomy is where it belongs.
    for kind in unit integration smoke unclassified; do
        # Not `set -e`: a failing suite must not hide the state of the two after it. CI reads the
        # summary below, and a developer who has broken two suites should learn that in one run.
        if ! just _ctest "${kind}" {{args}}; then
            failed+=("${kind}")
        fi
    done
    echo
    if [ ${#failed[@]} -eq 0 ]; then
        echo "test-all: unit, integration, smoke and the unclassified pass all green"
    else
        echo "test-all: failed — ${failed[*]}" >&2
        exit 1
    fi

# Benchmarks, compared against benchmarks/baseline.json. Pass --record to rewrite it. A minute.
test-bench *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    # --record is this recipe's, not the runner's: it rewrites the committed thresholds, which is a
    # reviewed step, so it is separated out here rather than passed through.
    record=""
    runner_args=()
    for argument in ${rest}; do
        if [ "${argument}" = "--record" ]; then
            record="--record"
        else
            runner_args+=("${argument}")
        fi
    done

    if [ "$(just _profile-column "${profile}" 3)" = "on" ]; then
        echo "note: profile '${profile}' has assertions on. Measurements from it are not shipping" >&2
        echo "      numbers — use --profile profile for a figure worth recording." >&2
    fi

    just build-engine --profile "${profile}"

    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    results="${build_dir}/benchmarks/results"
    mkdir -p "${results}"

    status=0
    found=0
    for runner in "${build_dir}"/benchmarks/cy_bench_*; do
        [ -x "${runner}" ] || continue
        found=1
        name="$(basename "${runner}")"
        echo "==> ${name}"
        "${runner}" --json "${results}/${name}.json" ${runner_args[@]+"${runner_args[@]}"}
        echo "==> thresholds  benchmarks/baseline.json"
        python3 benchmarks/tools/compare.py \
            --results "${results}/${name}.json" \
            --baseline benchmarks/baseline.json ${record} || status=$?
    done

    if [ "${found}" -eq 0 ]; then
        echo "test-bench: no benchmark runners in ${build_dir}/benchmarks/" >&2
        exit 1
    fi
    exit "${status}"

# --- Shared implementation ------------------------------------------------------------------------

# Build, then run one kind of suite. Private: the graduated recipes above are the interface.
#
# CY_BUILD_DIR redirects the build tree, CY_JOBS sets the parallelism, and CY_TEST_JUNIT names a
# file for the machine-readable results `testing-and-quality` requires of a CI run. Everything after
# `--profile <name>` is forwarded to ctest, so `just test-unit -R harness` and `just test-unit
# --rerun-failed` work without this recipe knowing what those options mean.
[private]
_ctest kind *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    just _report-override CY_TEST_JUNIT "${CY_TEST_JUNIT:-}"

    just build-engine --profile "${profile}"

    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    ctest_args=(--test-dir "${build_dir}" --output-on-failure)

    # A suite that matches no test is an error everywhere except `unclassified`, which is empty
    # when every test has found its place in the taxonomy. That one exception is named rather than
    # blanket, so the day a label is misspelled the run says so instead of passing. Smoke carried
    # the same exemption until task 4.1.4 landed smoke.empty_sample; it does not any more, so a
    # smoke suite that stops being registered fails here rather than reporting a vacuous pass.
    if [ "{{kind}}" = "unclassified" ]; then
        ctest_args+=(--label-exclude "^(unit|integration|smoke)\$" --no-tests=ignore)
    else
        ctest_args+=(--label-regex "^{{kind}}\$" --no-tests=error)
    fi
    if [ -n "${CY_JOBS:-}" ]; then
        ctest_args+=(--parallel "${CY_JOBS}")
    fi
    if [ -n "${CY_TEST_JUNIT:-}" ]; then
        ctest_args+=(--output-junit "${CY_TEST_JUNIT}")
    fi

    if [ "{{kind}}" = "unclassified" ]; then
        echo "==> tests carrying none of the taxonomy's labels"
    else
        echo "==> {{kind}} tests"
    fi
    started=${SECONDS}
    status=0
    ctest "${ctest_args[@]}" ${rest} || status=$?
    elapsed=$((SECONDS - started))

    # The taxonomy's fast-feedback requirement is that the unit set completes in under a minute on a
    # development machine. Reported every run, because a budget nobody sees is a budget nobody keeps.
    if [ "{{kind}}" = "unclassified" ] && [ "${status}" -eq 0 ]; then
        # ctest names them on failure; on success they are silent, and silently-passing tests
        # outside the taxonomy are exactly what this pass exists to surface.
        unclassified="$(ctest --test-dir "${build_dir}" -N \
            --label-exclude "^(unit|integration|smoke)\$" 2>/dev/null \
            | sed -n 's/^  Test *#[0-9]*: //p')"
        if [ -n "${unclassified}" ]; then
            echo "note: these tests carry no taxonomy label, so no graduated recipe runs them:" >&2
            printf '        %s\n' ${unclassified} >&2
            echo "      Declare them with cy_add_test(NAME <n> KIND unit|integration|smoke ...)" >&2
            echo "      from tests/, or set LABELS on them; that is what gives them a budget." >&2
        fi
    fi

    if [ "{{kind}}" = "unit" ]; then
        echo "==> unit suite ran in ${elapsed}s of its 60 s budget"
        if [ "${elapsed}" -gt 60 ]; then
            echo "warning: the unit set is over its budget. Move what is slow into" >&2
            echo "         tests/integration/ — the taxonomy in testing-and-quality is the rule." >&2
        fi
    fi
    exit "${status}"

# The job system's throughput, checked against benchmarks/thresholds. Pass --record to rewrite them.
test-bench-jobs *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    # A separate recipe from `test-bench` because this benchmark is not one of the shared harness's:
    # it measures a thread pool against a serial baseline rather than a fixed-iteration loop, and it
    # owns its own thresholds (src/core/jobs/bench/thresholds.json) and its own comparison. The
    # runner exits non-zero on a regression, so this is a gate rather than a report.
    if [ "$(just _profile-column "${profile}" 3)" = "on" ]; then
        echo "note: profile '${profile}' has assertions on. Measurements from it are not shipping" >&2
        echo "      numbers — use --profile profile for a figure worth recording." >&2
    fi

    just build-engine --profile "${profile}"
    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    runner="${build_dir}/benchmarks/cy_bench_jobs_throughput"
    if [ ! -x "${runner}" ]; then
        echo "test-bench-jobs: no runner at ${runner}" >&2
        echo "  It is built with the tests (CY_BUILD_TESTS); check that they are enabled." >&2
        exit 1
    fi
    echo "==> jobs throughput"
    exec "${runner}" ${rest}

# Build and run a suite under a sanitizer. Usage: just test-sanitize [--sanitizer thread|address|...] [--tests <regex>]
test-sanitize *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"

    # `core-jobs-and-concurrency` asks for a data-race detector over the concurrency suites, and
    # `testing-and-quality` asks for the unit and integration suites under ASan, UBSan and TSan at
    # least nightly. This is the one recipe both mean: it configures a sanitized tree of its own —
    # a sanitized build is not interchangeable with an ordinary one, so it never reuses build/<profile>
    # — builds it, and runs the selected tests in it.
    sanitizer="thread"
    tests="."
    leaks="off"
    expecting=""
    ctest_args=()
    for argument in ${rest}; do
        case "${expecting}" in
            sanitizer) sanitizer="${argument}"; expecting=""; continue ;;
            tests)     tests="${argument}";     expecting=""; continue ;;
            leaks)     leaks="${argument}";     expecting=""; continue ;;
        esac
        case "${argument}" in
            --sanitizer) expecting=sanitizer ;;
            --sanitizer=*) sanitizer="${argument#*=}" ;;
            --tests) expecting=tests ;;
            --tests=*) tests="${argument#*=}" ;;
            --leaks) expecting=leaks ;;
            --leaks=*) leaks="${argument#*=}" ;;
            *) ctest_args+=("${argument}") ;;
        esac
    done

    build_dir="${CY_BUILD_DIR:-build/sanitize-${sanitizer}}"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    echo "==> sanitize    ${sanitizer}  profile=${profile}  tree=${build_dir}"
    CY_BUILD_DIR="${build_dir}" just build-engine --profile "${profile}" -D "CY_SANITIZE=${sanitizer}"

    # A sanitized binary is five to twenty times slower, which is the tool working rather than the
    # test regressing. The harness scales its per-test budget under a sanitizer already; this makes
    # the intent explicit and gives CTest room too.
    export CY_TEST_BUDGET_SCALE="${CY_TEST_BUDGET_SCALE:-0}"

    # LeakSanitizer is off by default and that is a recorded state, not an oversight: the trace's
    # per-thread rings are pooled for the life of the process and are not yet declared to the
    # detector (design.md §9, M1 task 2.12). Any suite that opens a trace therefore reports them.
    # `--leaks on` turns detection on, and the change that closes task 2.12 makes that the default.
    if [ "${leaks}" = "on" ]; then
        export ASAN_OPTIONS="${ASAN_OPTIONS:-}${ASAN_OPTIONS:+:}detect_leaks=1"
    else
        export ASAN_OPTIONS="${ASAN_OPTIONS:-}${ASAN_OPTIONS:+:}detect_leaks=0"
    fi
    export UBSAN_OPTIONS="${UBSAN_OPTIONS:-}${UBSAN_OPTIONS:+:}print_stacktrace=1"

    # ThreadSanitizer maps its shadow memory at fixed addresses and refuses to start when the
    # kernel's ASLR entropy puts the executable where it wants to be — "unexpected memory mapping"
    # on a recent Linux with vm.mmap_rnd_bits at 32. `setarch -R` disables randomisation for the
    # run, which is the documented workaround and costs nothing else.
    launcher=()
    if [ "${sanitizer}" = "thread" ] && command -v setarch >/dev/null 2>&1; then
        launcher=(setarch "$(uname -m)" -R)
    fi

    echo "==> tests       ${tests}"
    "${launcher[@]}" ctest --test-dir "${build_dir}" --output-on-failure \
        --tests-regex "${tests}" --no-tests=error ${ctest_args[@]+"${ctest_args[@]}"}
