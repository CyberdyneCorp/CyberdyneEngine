#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# tools/workflow/jobs.sh — how many compile jobs one build may run, and how many the machine may.
#
# ONE DEFINITION, READ BY EVERY PLACE THAT CHOOSES A JOB COUNT: `just _jobs` and `just _job-slots`
# (and through them build-engine, build-editor, build-editor-check and quality-lint),
# cmake/jobpool.cmake (which bakes `machine` into the job-slot launcher every compile and link goes
# through, tools/workflow/job_slot.py), and tools/roadmap/matrix.py. Two copies of this arithmetic
# would drift, and a drift here is a machine that locks up under a ledger.
#
#   jobs.sh build     the per-build default: CY_JOBS when set, else max(1, cores - reserved)
#   jobs.sh machine   the machine-wide cap on concurrent compiles and links: max(1, cores - reserved).
#                     CY_JOBS does NOT raise it — CY_JOBS is one build's share, not the machine's.
#   jobs.sh reserved  the cores kept free: CY_RESERVED_CORES when set, else 2 — or 0 when `CI` is
#                     set (every hosted runner sets it): a runner has no interactive user to keep
#                     responsive, and its 2-4 cores would otherwise lose half their throughput.
#
# Sourcing the file defines the functions without running anything.

cy_cores() {
    local cores=""
    if command -v nproc >/dev/null 2>&1; then
        cores="$(nproc 2>/dev/null || true)"
    fi
    if [ -z "${cores}" ] && command -v getconf >/dev/null 2>&1; then
        cores="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
    fi
    if [ -z "${cores}" ] && command -v sysctl >/dev/null 2>&1; then
        cores="$(sysctl -n hw.logicalcpu 2>/dev/null || true)"
    fi
    case "${cores}" in
        '' | *[!0-9]*) cores=1 ;;
    esac
    echo "${cores}"
}

cy_reserved_cores() {
    if [ -n "${CY_RESERVED_CORES:-}" ]; then
        echo "${CY_RESERVED_CORES}"
    elif [ -n "${CI:-}" ]; then
        echo 0
    else
        echo 2
    fi
}

cy_machine_jobs() {
    local jobs=$(( $(cy_cores) - $(cy_reserved_cores) ))
    [ "${jobs}" -ge 1 ] || jobs=1
    echo "${jobs}"
}

cy_build_jobs() {
    if [ -n "${CY_JOBS:-}" ]; then
        echo "${CY_JOBS}"
    else
        cy_machine_jobs
    fi
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    set -euo pipefail
    case "${1:-build}" in
        build)    cy_build_jobs ;;
        machine)  cy_machine_jobs ;;
        reserved) cy_reserved_cores ;;
        *) echo "usage: jobs.sh [build|machine|reserved]" >&2; exit 2 ;;
    esac
fi
