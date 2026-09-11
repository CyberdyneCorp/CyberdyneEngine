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

# M5.5's ARTEFACTS, AND WHY THERE ARE TWO OF THEM.
#
# M5 closed on a scripted session that could not show docking, workspaces, the palette or
# keyboard-first operation, because at M5 the editor had no window. M5.5 is the correction, and it
# has two audiences that one program cannot both demonstrate (`design.md` §5): a person operating a
# window, and an agent authoring through the same editor over the Model Context Protocol.
#
# Neither is `just run-sample <name>`, for the reason `run-editor-session` above records: that recipe
# finds `samples/*/cy_sample_<name>` and neither directory declares a C++ target. What they declare
# is orchestration, in the language this tree already orchestrates multi-toolchain artefacts in.

# Run the M5.5 window artefact: the editor opened, operated and photographed. Usage: just run-editor-window [--profile <p>] [--hold] [--shot <path>]
run-editor-window *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    just build-editor --profile "${profile}"
    # AND THE ENGINE ON THE FAR END OF THE TRANSPORT. From M7 the thing the editor's viewport shows
    # is `cy_editor_window_runtime` — M3's scene and renderer on a real device, publishing over the
    # viewport transport — rather than the fixture in the editor's own Cargo workspace. Building the
    # editor alone would leave the driver reporting that the viewport is neutral, which reads as a
    # defect in the transport rather than as a missing binary.
    just build-engine --profile "${profile}"

    # THIS ONE NEEDS A DISPLAY AND SAYS SO RATHER THAN PASSING. The driver exits 3 where it cannot
    # run — no display, no XTEST, no python-xlib — and this recipe passes that exit code through
    # unchanged, so a machine that cannot judge the claim reports that it could not rather than that
    # it did. `--hold` leaves the window open at the end, which is what a person watching wants.
    echo "==> run         editor-window  profile=${profile}"
    python3 samples/05b-editor-window/window.py --profile "${profile}" ${rest}

# Run the M5.5 agent artefact: an agent composes, authors and observes over MCP. Usage: just run-agent-authoring [--profile <p>] [--shot <path>]
run-agent-authoring *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    just build-editor --profile "${profile}"

    # No display, no graphics device, no window: the editor is started with `--mcp` and this driver
    # is the client. That is why this half of the milestone is the half continuous integration runs.
    echo "==> run         agent-authoring  profile=${profile}"
    python3 samples/05b-agent-authoring/authoring.py --profile "${profile}" ${rest}

# M6's ARTEFACT, AND WHY IT IS NOT `just run-sample open-world`.
#
# `run-sample` finds a sample's binary and runs it, and samples/06-open-world DOES declare one —
# `just run-sample open-world --install <dir>` plays a world that has already been shipped into an
# installation. What that cannot do is SHIP it: the milestone's claim is a world cooked through a
# derivation graph, packaged, installed, played, saved, resumed, and then patched, which is two
# programs and five acts. `openworld.py` is the driver that puts them in order, in the language this
# tree already orchestrates multi-process artefacts in, and this recipe is the entry point.
#
# It builds the engine first, for the same reason every other run recipe does: running whatever
# binary happens to be lying in the build tree reports on a revision nobody has.
#
# `--shot <path>` draws the run — the world's cells, the route across them, the landmarks it passes
# and the run's own figures — which is what refreshes docs/design/images/open-world-m6.png. `--only
# <act>` runs one of ship, play, resume, teardown or patch; `--ticks <n>` shortens the route, which
# is what the CTest entry does and what a person watching usually wants.

# Run the M6 artefact: a world shipped, streamed, saved, resumed and patched. Usage: just run-open-world [--profile <p>] [--ticks <n>] [--only <act>] [--shot <path>]
run-open-world *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    just build-engine --profile "${profile}" -D CY_BUILD_TOOLS=ON

    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    echo "==> run         open-world  profile=${profile}"
    python3 samples/06-open-world/openworld.py --profile "${profile}" \
        --build-dir "${build_dir}" --work "${build_dir}/open-world" ${rest}

# Run the M7 artefact: a film-detail set cooked, put on the device, lit, and held to a frame budget
# while the arbiter reallocates under a scripted load spike.
# Usage: just run-fidelity [--profile <p>] [--detail <f>] [--frames <n>] [--repeat <n>] [--shot <path>]
#
# It builds the engine first, like every recipe here. `--repeat` defaults to four inside the driver
# and the figure the run leads with is the median over those runs — M7 task 5b.5b, and the reason
# `samples/harness/artefact.py` exists: a single sample of a frame time is a draw.
run-fidelity *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    just build-engine --profile "${profile}"

    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    echo "==> run         fidelity  profile=${profile}"
    python3 samples/07-fidelity/fidelity.py --profile "${profile}" \
        --build-dir "${build_dir}" --work "${build_dir}/fidelity" ${rest}

# M8.a's ARTEFACT, AND WHY IT IS NOT `just run-sample authoring`.
#
# `run-sample` finds a sample's binary and runs it, and samples/08a-authoring DOES declare one —
# `just run-sample authoring --world <path>` plays a `.cyworld` headlessly and says how far the
# sphere fell. What that cannot do is AUTHOR it: the milestone's claim is a scene a person builds by
# hand, which is a real editor, a real engine runtime and six steps in order, and the two halves
# together are what `authoring.py` runs. Its CMakeLists.txt argues the split.
#
# It builds the editor AND the engine first, for the reason `run-editor-window` records: the thing
# on the far end of the editor's control socket is `cy_editor_window_runtime`, and building only the
# editor would leave the driver reporting an absent runtime, which reads as a defect rather than as
# an unbuilt target.
#
# `--no-window` leaves out the photograph, which is the one act that needs a display; the CTest entry
# passes it. `--shot <path>` is what refreshes docs/design/images/authoring-m8a.png.

# Run the M8.a artefact: a scene authored in the editor, played, stopped and undone. Usage: just run-authoring [--profile <p>] [--no-window] [--shot <path>] [--runs <n>]
run-authoring *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    just build-editor --profile "${profile}"
    just build-engine --profile "${profile}"

    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    echo "==> run         authoring  profile=${profile}"
    python3 samples/08a-authoring/authoring.py --profile "${profile}" --binaries \
        --build-dir "${build_dir}" --work "${build_dir}/authoring" ${rest}

# M8.b's ARTEFACT, AND WHY IT IS NOT `just run-sample vertical-slice`.
#
# `run-sample` finds a sample's binary and runs it, and samples/08-vertical-slice DOES declare one —
# `just run-sample vertical-slice --agents 512` plays the game and prints what it did. What that
# cannot do is JUDGE it: the milestone's claims are that eight thousand agents and a hundred
# concurrent effects hold declared budgets, that the same options produce the same simulation in two
# processes, and that a per-entity virtual tick is FOUND when one is planted. Each of those is
# several runs of the program compared against each other, which is what `slice.py` does in four
# acts. Its CMakeLists.txt argues the split.
#
# It builds only the engine: there is no editor and no second process in this artefact, and no
# display and no graphics device either — the frame is assembled and no pass is recorded, which is
# what makes the whole of it judgeable on a hosted runner.
#
# `--shot <path>` is what refreshes docs/design/images/vertical-slice-m8b.png. `--only <act>` runs
# one act, and `--scale-agents <n>` moves the population the scale act measures; the exit criterion's
# figure is 8000 and that is the default.

# Run the M8.b artefact: a playable game, measured. Usage: just run-vertical-slice [--profile <p>] [--agents <n>] [--ticks <n>] [--scale-agents <n>] [--only <act>] [--shot <path>]
run-vertical-slice *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    just build-engine --profile "${profile}"

    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    echo "==> run         vertical-slice  profile=${profile}"
    python3 samples/08-vertical-slice/slice.py --profile "${profile}" \
        --sample "${build_dir}/samples/08-vertical-slice/cy_sample_vertical-slice" \
        --build-dir "${build_dir}" --work "${build_dir}/vertical-slice" ${rest}

# M9's ARTEFACT, AND WHY IT IS NOT `just run-sample multiplayer`.
#
# The same split M8.b's recipe above argues. `just run-sample multiplayer --ticks 120` plays the
# session and prints what happened; what it cannot do is JUDGE it. The milestone's claims are that
# four clients converge on the host's state hash after rolling back under real packet loss, that the
# recorded log replays to the identical digest, that an injected divergence is narrowed to one field
# on one entity, and that a crash artefact re-simulates to the hash it carries — each of those is a
# comparison between several things the program printed, which is what `multiplayer.py` does in five
# acts. Its CMakeLists.txt argues the shape.
#
# It builds only the engine: no editor, no second process, no display and no graphics device. There
# is no renderer anywhere in this artefact's link closure, which is what makes the whole of it
# judgeable on a hosted runner.
#
# `--shots <dir>` is what refreshes docs/design/images/m9-multiplayer-session.png and
# m9-divergence-narrowed.png. `--only <act>` runs one act, and `--ticks <n>` moves the length of the
# session — 180 is the figure the committed pictures and the ledger are drawn from.
#
# With `-D CY_NETWORKING=OFF` the sample is not declared and this recipe reports an absent binary
# rather than a defect, exactly as `just run-vertical-slice` does without CY_ANIMATION.

# Run the M9 artefact: four players, packet loss, and a replay that is bit-exact. Usage: just run-multiplayer [--profile <p>] [--ticks <n>] [--seed <n>] [--only <act>] [--shots <dir>]
run-multiplayer *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    just build-engine --profile "${profile}"

    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    echo "==> run         multiplayer  profile=${profile}"
    python3 samples/09-multiplayer/multiplayer.py --profile "${profile}" \
        --sample "${build_dir}/samples/09-multiplayer/cy_sample_multiplayer" \
        --build-dir "${build_dir}" --work "${build_dir}/multiplayer" ${rest}
