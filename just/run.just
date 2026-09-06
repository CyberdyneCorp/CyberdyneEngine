# just/run.just — Run: samples, the runtime host, the headless runtime, the editor.
#
# These recipes build first, for the same reason the test recipes do: running whatever binary
# happens to be lying in the build tree reports on a revision nobody has. CMake and Ninja decide
# what is out of date.

# Run a sample. Usage: just run-sample <name> [--profile <p>] [--headless] [--frames <n>] [--trace <path>]
run-sample *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    # The first argument is the sample; everything after it belongs to the sample, not to this
    # recipe, and is forwarded untouched. An omitted name means `empty`, which is the sample that
    # takes `--headless`, so that `just run-sample --headless` and `just run-headless` say what they
    # mean. M1's artefact is `just run-sample headless-host`, and it is named because it has no
    # display server to choose between and takes a different command line entirely; M2's is
    # `just run-sample headless-sim` and M3's is `just run-sample first-light`, which renders the
    # milestone's frame through whichever RHI backend the build has — the null one on a default
    # build, and Vulkan on a build configured with -D CY_RENDERER_VULKAN=ON. M4's is
    # `just run-sample character`, whose gameplay is a Swift module the host loads over the C ABI:
    # it is the one sample that is NOT BUILT AT ALL without a Swift toolchain, and the failure below
    # then names it as an unbuilt sample rather than as a missing binary. `just env-doctor` says why.
    argv=(${rest})
    if [ "${#argv[@]}" -gt 0 ] && [[ "${argv[0]}" != --* ]]; then
        name="${argv[0]}"
        forwarded=("${argv[@]:1}")
    else
        name="empty"
        forwarded=("${argv[@]}")
    fi

    just build-engine --profile "${profile}"

    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    # samples/<nn>-<name>/cy_sample_<name>: the sample's number orders the directory listing and is
    # not part of its name, so the binary is found by pattern rather than by a table kept in step.
    mapfile -t binaries < <(compgen -G "${build_dir}/samples/*/cy_sample_${name}" || true)
    if [ "${#binaries[@]}" -eq 0 ]; then
        echo "run-sample: no sample '${name}' in ${build_dir}/samples/." >&2
        echo "  built samples:" >&2
        compgen -G "${build_dir}/samples/*/cy_sample_*" | sed 's|.*/cy_sample_|    |' >&2 || \
            echo "    none" >&2
        exit 2
    fi

    echo "==> run         ${name}  profile=${profile}"
    "${binaries[0]}" ${forwarded[@]+"${forwarded[@]}"}

# WHAT `run-editor` RUNS, AND WHAT IT DOES NOT.
#
# The editor is headless at M5 and that is a decision rather than an omission:
# `editor-rust-application` requires the interface toolkit to be "an implementation choice behind
# editor abstractions, selected on measurement", and the layers that must be finished before that
# measurement is worth making are what this milestone delivers. So this recipe runs a binary that
# opens documents, invokes commands from a script, and prints where the session ended up — which is
# what a test, an agent and a future window all do through the same registry.
#
# `--host <socket>` attaches a hosted runtime. That is the production default per
# `editor-rust-application`, and it is reported and NOT fatal when nothing is listening: an editor
# with no runtime is a mode, not a broken state, and refusing to start would make the
# out-of-process decision cost more than it saves.

# Run the editor. Usage: just run-editor [--profile <p>] [--open <asset>] [--script <path>] [--host <socket>]
run-editor *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    just build-editor --profile "${profile}"

    cargo_profile="$(just _cargo-profile "${profile}")"
    target_dir="$(just _editor-target-dir)"
    # Cargo's own `dev` profile writes to `debug/`; every other profile writes to a directory of its
    # own name. Spelled out here rather than assumed, because getting it wrong runs a binary from a
    # different profile than the one that was just built — which is the class of mistake that makes
    # a measurement meaningless and takes an afternoon to notice.
    if [ "${cargo_profile}" = "dev" ]; then
        binary="${target_dir}/debug/cyberdyne-editor"
    else
        binary="${target_dir}/${cargo_profile}/cyberdyne-editor"
    fi

    if [ ! -x "${binary}" ]; then
        echo "run-editor: no editor binary at ${binary}." >&2
        echo "  Build it with: just build-editor --profile ${profile}" >&2
        exit 2
    fi

    echo "==> run         cyberdyne-editor  profile=${profile}"
    "${binary}" ${rest}

# It is what `--host` connects to before the engine's own hosted runtime lands with `live-editing`
# at task 5.2. It speaks the same message set over the same committed encoding, so the editor's side
# of a hosted session does not change when the real one arrives.

# Run the hosted-runtime stub the editor connects to. Usage: just run-editor-runtime [<socket>]
run-editor-runtime *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just build-editor --profile "${profile}"

    cargo_profile="$(just _cargo-profile "${profile}")"
    target_dir="$(just _editor-target-dir)"
    if [ "${cargo_profile}" = "dev" ]; then
        binary="${target_dir}/debug/cy-runtime-stub"
    else
        binary="${target_dir}/${cargo_profile}/cy-runtime-stub"
    fi

    socket="${rest:-/tmp/cyberdyne-runtime.sock}"
    echo "==> run         cy-runtime-stub  socket=${socket}"
    echo "    This is a STUB. It speaks the live bridge's message set and holds no world; the"
    echo "    engine's own hosted runtime arrives with live-editing at task 5.2." >&2
    "${binary}" "${socket}"

# M5's ARTEFACT, AND WHY IT IS NOT `just run-sample editor-session`.
#
# `run-sample` finds a sample's binary as samples/*/cy_sample_<name>, and samples/05-editor-session
# declares no such target: the artefact is a session across three processes in two languages — the
# importer, the editor, and a hosted runtime on the other side of a Unix domain socket — and the
# claim it demonstrates is what happens when one of them is killed mid-session. A C++ driver would
# link no engine code and prove nothing about the engine. The sample's CMakeLists.txt argues it at
# length; this recipe is the entry point, and it builds the editor and the engine's tools first for
# the same reason every other run recipe builds first.
#
# `--only survive` runs just the act that kills the runtime, which is the one worth watching.

# Run the M5 artefact: a scripted editor session that outlives its runtime. Usage: just run-editor-session [--profile <p>] [--only <act>] [--keep]
run-editor-session *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    just build-editor --profile "${profile}"
    just build-tools --profile "${profile}"

    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    echo "==> run         editor-session  profile=${profile}"
    python3 samples/05-editor-session/session.py --profile "${profile}" \
        --import-cli "${build_dir}/tools/import/cy_import_cli" ${rest}

# Run the runtime host with no window system, as CI does. Usage: just run-headless [<sample>] [args...]
run-headless *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    # M0's runtime host is the empty sample: it is the only program that starts the runtime, and it
    # chooses its display server at run time. When a standalone host binary lands, this recipe runs
    # that instead and `run-sample empty --headless` keeps working unchanged.
    just run-sample {{args}} --headless
