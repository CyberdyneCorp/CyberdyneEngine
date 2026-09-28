#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# tools/workflow/build_lock.sh — one build at a time per build tree.
#
# Sourced by `just build-engine`. `cy_build_lock <dir>` returns once this shell owns <dir>; the lock
# is released when the shell exits. A second build WAITS rather than failing, and says so, because a
# build that silently blocks looks like a build that hung.
#
# Two CMake configures in one directory do not merely race, they corrupt: a half-populated
# FetchContent checkout looks exactly like a broken dependency, and the error it produces names SDL
# rather than the concurrency that caused it.
#
# Sourcing the file defines the function without running anything.

cy_build_lock() {
    local lock_dir="$1"
    mkdir -p "${lock_dir}"
    # On CI, `actions/cache` restores the build tree verbatim — including any lock a prior job left
    # behind on that job's runner, whose owner PID means nothing here and may even match a live
    # local process. Every hosted runner sets GITHUB_ACTIONS and runs one build per tree, so a lock
    # found there is always stale.
    if [ "${GITHUB_ACTIONS:-}" = "true" ]; then
        rm -rf "${lock_dir}/.cy-build.lock.pid" "${lock_dir}/.cy-build.lock"
    fi
    exec 9>"${lock_dir}/.cy-build.lock"
    if command -v flock >/dev/null 2>&1; then
        if ! flock -n 9; then
            echo "==> waiting     another build holds ${lock_dir}; waiting for it to finish" >&2
            flock 9
        fi
    elif command -v lockf >/dev/null 2>&1; then
        if ! lockf -s -t 0 9; then
            echo "==> waiting     another build holds ${lock_dir}; waiting for it to finish" >&2
            lockf -s 9
        fi
    else
        _cy_build_lock_fallback "${lock_dir}/.cy-build.lock.pid"
    fi
}

# GitHub's macOS image has no `flock` or `lockf` executable, and Git Bash on Windows has neither.
# `mkdir` is atomic on every filesystem both use, and a pid file inside names the owner so a crashed
# build leaves a lock the next process can identify and reap instead of wedging the tree forever.
#
# NOT `ln -s <pid> <lock>`, which this was: Git Bash cannot create a symlink to a target that does
# not exist, so `ln -s` failed every time, the owner read back empty, and every Windows CI build
# waited on nobody until the six-hour job limit cancelled it.
_cy_build_lock_fallback() {
    _cy_build_lock_path="$1"
    local announced=0 owner
    while ! mkdir "${_cy_build_lock_path}" 2>/dev/null; do
        owner="$(cat "${_cy_build_lock_path}/owner" 2>/dev/null || true)"
        if [[ "${owner}" =~ ^[0-9]+$ ]] && ! kill -0 "${owner}" 2>/dev/null; then
            rm -rf "${_cy_build_lock_path}"
            continue
        fi
        if [ "${announced}" -eq 0 ]; then
            echo "==> waiting     another build holds ${_cy_build_lock_path%/*}; waiting for it to finish" >&2
            announced=1
        fi
        sleep 0.1
    done
    echo "$$" >"${_cy_build_lock_path}/owner"
    # The ownership check keeps one waiter from deleting a lock another has since acquired.
    _cy_release_build_lock() {
        [ "$(cat "${_cy_build_lock_path}/owner" 2>/dev/null || true)" != "$$" ] || \
            rm -rf "${_cy_build_lock_path}"
    }
    trap _cy_release_build_lock EXIT
}
