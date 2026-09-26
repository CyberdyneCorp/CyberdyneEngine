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
    binaries=()
    while IFS= read -r binary; do
        binaries[${#binaries[@]}]="${binary}"
    done < <(compgen -G "${build_dir}/samples/*/cy_sample_${name}" || true)
    # A SAMPLE MAY BE NAMED WITH ITS DIRECTORY'S NUMBER. `samples/00-empty/` builds
    # `cy_sample_empty`, and both spellings are things a reader of the tree would type — M11.d's
    # ledger says `just run-sample 00-empty` because that is what the directory is called. The
    # number orders the listing and is not part of the name, so it is stripped and retried rather
    # than kept in a table that would have to stay in step.
    if [ "${#binaries[@]}" -eq 0 ] && [[ "${name}" =~ ^[0-9]+[a-z]?- ]]; then
        name="${name#*-}"
        binaries=()
        while IFS= read -r binary; do
            binaries[${#binaries[@]}]="${binary}"
        done < <(compgen -G "${build_dir}/samples/*/cy_sample_${name}" || true)
    fi
    if [ "${#binaries[@]}" -eq 0 ]; then
        echo "run-sample: no sample '${name}' in ${build_dir}/samples/." >&2
        echo "  built samples:" >&2
        compgen -G "${build_dir}/samples/*/cy_sample_*" | sed 's|.*/cy_sample_|    |' >&2 || \
            echo "    none" >&2
        exit 2
    fi

    echo "==> run         ${name}  profile=${profile}"
    "${binaries[0]}" ${forwarded[@]+"${forwarded[@]}"}

# Build, install, launch, and screenshot the Metal sample on a booted iOS simulator.
# Usage: just run-ios-simulator [device-name-or-UDID]
run-ios-simulator device="booted":
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    export CY_IOS_BUNDLE_IDENTIFIER="${CY_IOS_BUNDLE_IDENTIFIER:-com.cyberdyne.engine.ship.simulator}"
    just build-ios simulator
    python3 samples/11-ship/ios/run_simulator.py \
        --device "{{device}}" \
        --bundle-id "${CY_IOS_BUNDLE_IDENTIFIER}" \
        --app build/ios-simulator/samples/11-ship/ios/Development-iphonesimulator/cy_sample_ship_ios.app \
        --log build/ios-simulator/ios-open-world.log \
        --screenshot build/ios-simulator/ios-open-world.png

# Build, install, measure, and capture the Metal sample on a physical iPhone.
# Usage: CY_IOS_DEVELOPMENT_TEAM=... CY_IOS_BUNDLE_IDENTIFIER=... just run-ios-device <name-or-UDID>
run-ios-device device:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    : "${CY_IOS_DEVELOPMENT_TEAM:?set CY_IOS_DEVELOPMENT_TEAM to the Apple development team ID}"
    : "${CY_IOS_BUNDLE_IDENTIFIER:?set CY_IOS_BUNDLE_IDENTIFIER to the provisioned bundle ID}"
    just build-ios device
    python3 samples/11-ship/ios/run_device.py \
        --device "{{device}}" \
        --bundle-id "${CY_IOS_BUNDLE_IDENTIFIER}" \
        --app build/ios-device/samples/11-ship/ios/Development-iphoneos/cy_sample_ship_ios.app \
        --seconds 12 \
        --minimum-samples 8 \
        --log build/ios-device/ios-compute-scene.log \
        --screenshot docs/design/images/ios-compute-scene-iphone.png \
        --evidence openspec/changes/integrate-ios-compute-scene/evidence/physical-device.md

# Build and measure the 500-skin, 100-emitter RTS stress scene on a physical iPhone.
# Usage: CY_IOS_DEVELOPMENT_TEAM=... CY_IOS_BUNDLE_IDENTIFIER=... just run-ios-rts-device <UDID>
run-ios-rts-device device:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    : "${CY_IOS_DEVELOPMENT_TEAM:?set CY_IOS_DEVELOPMENT_TEAM to the Apple development team ID}"
    : "${CY_IOS_BUNDLE_IDENTIFIER:?set CY_IOS_BUNDLE_IDENTIFIER to the provisioned bundle ID}"
    cmake --preset ios-device \
        -D "CY_IOS_DEVELOPMENT_TEAM=${CY_IOS_DEVELOPMENT_TEAM}" \
        -D "CY_IOS_BUNDLE_IDENTIFIER=${CY_IOS_BUNDLE_IDENTIFIER}" \
        -D CY_IOS_RTS_STRESS=ON
    cmake --build --preset ios-device --target cy_sample_ship_ios --parallel "$(just _jobs)"
    python3 samples/11-ship/ios/run_device.py \
        --device "{{device}}" \
        --bundle-id "${CY_IOS_BUNDLE_IDENTIFIER}" \
        --app build/ios-device/samples/11-ship/ios/Development-iphoneos/cy_sample_ship_ios.app \
        --seconds 20 \
        --minimum-samples 8 \
        --minimum-median-fps 0 \
        --expected-skin-models 500 \
        --expected-vfx-emitters 100 \
        --log build/ios-device/ios-rts-load.log \
        --screenshot docs/design/images/ios-rts-load-iphone.png \
        --evidence openspec/changes/benchmark-ios-rts-load/evidence/physical-device.md

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
    echo "    This is a STUB. It speaks the live bridge's message set and holds no world. To" >&2
    echo "    author against the engine, use: just run-engine --project <dir>" >&2
    "${binary}" "${socket}"

# AUTHORING A PROJECT AGAINST THE ENGINE: `run-engine` and `run-editor-live`.
#
# The engine process the editor attaches to is `cy_editor_window_runtime`: it loads the project's
# world, renders it on the platform's native device (Metal on macOS, Vulkan on Linux), publishes the
# frames over the viewport transport and applies the editor's transactions over the control socket.
# Until these recipes it was started by hand with eight flags copied from the README, and its
# `--seconds` default of 180 shut it down three minutes into a session.
#
# Both recipes resolve the session the same way, in `_engine-session`: the project is required and
# relative to where `just` was invoked; the world defaults to the first `worlds/*.cyworld`; and the
# two socket paths are derived from the project's absolute path, so two projects can be open at
# once and the editor and engine always name the same pair. A project to author in comes from
# `just content-new-project <dir>`.

# Run the engine for an editor to attach to, until interrupted. Usage: just run-engine --project <dir> [--world <path>] [--profile <p>] [--validation] [engine args...]
run-engine *args:
    #!/usr/bin/env bash
    set -euo pipefail
    export CY_CALLER_DIR="${CY_CALLER_DIR:-{{invocation_directory()}}}"
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    session="$(just _engine-session ${rest})"
    IFS=$'\t' read -r project world viewport control forwarded <<< "${session}"

    just build-engine --profile "${profile}"
    binary="$(just _engine-binary "${profile}")"

    # Metal and Vulkan validation cost frame time an interactive session feels, so it is off unless
    # asked for. The engine reads the FIRST occurrence of a flag, so forwarded arguments go first and
    # can override any default below except the session's own project, world and sockets.
    defaults=(--seconds 0)
    case " ${forwarded} " in
        *" --validation "*) ;;
        *) defaults+=(--no-validation) ;;
    esac

    echo "==> run         engine  profile=${profile}  world=${world}"
    echo "    project   ${project}"
    echo "    attach    CY_VIEWPORT_SOCKET=${viewport} just run-editor --project ${project} --host ${control} --open ${world}"
    exec "${binary}" ${forwarded} "${defaults[@]}" \
        --project "${project}" --world "${world}" --socket "${viewport}" --host "${control}"

# Run the engine and an editor attached to it on one project; closing the editor stops the engine. Usage: just run-editor-live --project <dir> [--world <path>] [--profile <p>] [editor args...]
run-editor-live *args:
    #!/usr/bin/env bash
    set -euo pipefail
    export CY_CALLER_DIR="${CY_CALLER_DIR:-{{invocation_directory()}}}"
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    session="$(just _engine-session ${rest})"
    IFS=$'\t' read -r project world viewport control forwarded <<< "${session}"

    just build-engine --profile "${profile}"
    just build-editor --profile "${profile}"
    binary="$(just _engine-binary "${profile}")"

    # The engine's output goes to a log rather than interleaving with the editor's, and is shown
    # when the engine fails to start — which is the only time a person needs it.
    log="${CY_BUILD_DIR:-build/${profile}}/engine-live.log"
    # Removed first so that the wait below sees THIS engine's sockets, never a crashed one's.
    rm -f "${viewport}" "${control}"
    echo "==> run         engine  profile=${profile}  world=${world}  log=${log}"
    echo "    project   ${project}"
    "${binary}" --seconds 0 --no-validation --project "${project}" --world "${world}" \
        --socket "${viewport}" --host "${control}" > "${log}" 2>&1 &
    engine=$!
    stop_engine() {
        kill "${engine}" 2>/dev/null || true
        wait "${engine}" 2>/dev/null || true
    }
    trap stop_engine EXIT

    # A deadline, not a sleep: the engine is ready when both sockets exist, and a device that takes
    # longer than this to open is a failure worth reporting rather than waiting out.
    deadline=$((SECONDS + 60))
    until [ -S "${viewport}" ] && [ -S "${control}" ]; do
        if ! kill -0 "${engine}" 2>/dev/null; then
            echo "run-editor-live: the engine exited before it was ready. Its log:" >&2
            tail -n 20 "${log}" >&2
            exit 1
        fi
        if [ "${SECONDS}" -ge "${deadline}" ]; then
            echo "run-editor-live: the engine did not open its sockets within 60 s; see ${log}" >&2
            exit 1
        fi
        sleep 0.2
    done

    CY_VIEWPORT_SOCKET="${viewport}" just run-editor --profile "${profile}" \
        --project "${project}" --host "${control}" --open "${world}" ${forwarded}

# Resolve an authoring session from recipe arguments. Prints, tab separated: the project's absolute
# path, the world relative to it, the viewport socket, the control socket, and every argument it did
# not consume. Relative projects resolve against CY_CALLER_DIR, which the public recipes set to the
# directory `just` was invoked from.
[private]
_engine-session *args:
    #!/usr/bin/env bash
    set -euo pipefail
    project=""
    world=""
    expect=""
    rest=()
    for argument in {{args}}; do
        case "${expect}" in
            project) project="${argument}"; expect=""; continue ;;
            world) world="${argument}"; expect=""; continue ;;
        esac
        case "${argument}" in
            --project) expect=project ;;
            --project=*) project="${argument#*=}" ;;
            --world) expect=world ;;
            --world=*) world="${argument#*=}" ;;
            *) rest+=("${argument}") ;;
        esac
    done
    if [ -n "${expect}" ]; then
        echo "--${expect} takes a value and none followed it" >&2
        exit 2
    fi
    if [ -z "${project}" ]; then
        echo "--project <directory> is required. Create one with: just content-new-project <directory>" >&2
        exit 2
    fi
    case "${project}" in
        /*) ;;
        *) project="${CY_CALLER_DIR:-{{invocation_directory()}}}/${project}" ;;
    esac
    if [ ! -f "${project}/project.json" ]; then
        echo "${project} is not a project: it has no project.json." >&2
        echo "  Create one with: just content-new-project ${project}" >&2
        exit 2
    fi
    project="$(cd "${project}" && pwd -P)"
    if [ -z "${world}" ]; then
        world="$(cd "${project}" && compgen -G 'worlds/*.cyworld' | LC_ALL=C sort | head -n 1 || true)"
    fi
    if [ -z "${world}" ]; then
        echo "${project} has no worlds/*.cyworld; name one with --world <path>" >&2
        exit 2
    fi
    if [ ! -f "${project}/${world}" ]; then
        echo "${project} has no world ${world}" >&2
        exit 2
    fi
    # /tmp rather than $TMPDIR: a Unix socket path is limited to about a hundred bytes, and macOS's
    # per-user temporary directory spends half of that before the file name.
    key="$(printf '%s' "${project}" | cksum | cut -d ' ' -f 1)"
    printf '%s\t%s\t%s\t%s\t%s\n' "${project}" "${world}" \
        "/tmp/cy-${key}-viewport.sock" "/tmp/cy-${key}-control.sock" "${rest[*]:-}"

# The engine host `run-engine` and `run-editor-live` start, or a failure naming why it is absent.
[private]
_engine-binary profile:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    binary="${CY_BUILD_DIR:-build/{{profile}}}/samples/05b-editor-window/runtime/cy_editor_window_runtime"
    if [ ! -x "${binary}" ]; then
        echo "no engine host at ${binary}." >&2
        echo "  It is built only with a native shared-texture renderer: Metal on macOS, or Vulkan on" >&2
        echo "  Linux (-D CY_RENDERER_VULKAN=ON). \`just env-doctor\` names what is missing." >&2
        exit 2
    fi
    echo "${binary}"

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

# Regenerate docs/design/images/virtual-geometry-*.png: the M7 scene rendered, and the cluster
# hierarchy choosing at three geometric-error thresholds.
# Usage: just capture-virtual-geometry [--profile <p>]
#
# WHY THREE THRESHOLDS. The threshold is the screen-space geometric error the hierarchy is allowed
# to commit, so one still frame cannot show the mechanism — only the same view at 1, 4 and 16 pixels
# can, and the visible-cluster count falls 5247 -> 3332 -> 1878 across them while coverage holds.
# That progression IS the "no manual level-of-detail authoring" claim, seen rather than read.
#
# It needs a graphics device. On a machine without one the tool says so and writes nothing, which is
# why this is a recipe a person runs and not a test.
capture-virtual-geometry *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just build-engine --profile "${profile}"

    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    work="$(mktemp -d)"
    trap 'rm -rf "${work}"' EXIT

    for threshold in 1 4 16; do
        echo "==> capture     virtual-geometry  threshold=${threshold}px"
        "${build_dir}/samples/07-fidelity/cy_fidelity_capture" "${work}/t${threshold}" "${threshold}"
    done

    python3 tools/docs/collect_virtual_geometry.py "${work}"

# M8.d's ARTEFACT: docs/design/images/animated-character.png and
# docs/design/videos/animated-character.mp4, regenerated from the four Mixamo FBX exports.
#
# WHY A VIDEO AND NOT A STILL. A single frame of a skinned character is indistinguishable from a
# single frame of a POSED one — the claim M8.d has to be able to show is that the geometry deforms
# between frames, and no still can carry it. The still this recipe also writes is for the readers and
# the viewers that cannot play video, and it is the run's most legible frame rather than a second
# claim.
#
# WHY THE FRAMES GO IN THE BUILD TREE AND NOT IN mktemp -d. The engine's PNG writer stores rather
# than deflates, so three hundred frames at 960x540 are about half a gigabyte — more than a tmpfs
# /tmp on many machines will hold. They are written under the build directory and deleted when the
# recipe finishes, which is the same lifetime `mktemp -d` would have given them and not the same
# filesystem.
#
# IT NEEDS TWO THINGS THE REPOSITORY DOES NOT HAVE. A graphics device, and the four Mixamo exports —
# `Breathing Idle.fbx`, `Walking.fbx`, `Running.fbx` and `Dying Backwards.fbx` — which are 1 to 17 MB
# each, are not redistributable, and live wherever `--sources` says. Without either, the program says
# which one is missing and writes nothing. That is why this is a recipe a person runs and not a test,
# exactly as `just capture-virtual-geometry` above is.

# Import four FBX files, play idle->walk->run->die, and encode the video. Usage: just capture-animated-character [--profile <p>] [--sources <dir>] [--seconds <s>] [--width <n>] [--height <n>] [--fps <n>]
capture-animated-character *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    just build-engine --profile "${profile}"

    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    sample="${build_dir}/samples/09b-animated-character/cy_sample_animated-character"
    if [ ! -x "${sample}" ]; then
        echo "capture-animated-character: ${sample} was not built. It is not declared with" >&2
        echo "  -D CY_ANIMATION=OFF, which removes the animation runtime this artefact imports into." >&2
        exit 1
    fi

    fps="${CY_CAPTURE_FPS:-30}"
    work="${build_dir}/animated-character"
    rm -rf "${work}"
    mkdir -p "${work}/frames"
    trap 'rm -rf "${work}/frames"' EXIT

    echo "==> capture     animated-character  fps=${fps}"
    "${sample}" --frames "${work}/frames" --still "${work}/still.png" --fps "${fps}" ${rest}

    echo "==> encode      animated-character"
    python3 tools/docs/collect_animated_character.py "${work}/frames" \
        --still "${work}/still.png" --fps "${fps}"

# M10's ARTEFACT: docs/design/images/m10-world.png, docs/design/images/m10-world-budget.png and
# docs/design/videos/m10-world.mp4, regenerated from nothing but a seed.
#
# WHY A VIDEO AND NOT A STILL. A single frame of a world is a single frame of a world. What M10's
# artefact claims is that the sun crosses it, a weather front moves through it, snow settles on the
# windward slope and thaws on the lee, the sea's spectrum follows the wind that weather published and
# the trees lean with the same field — and no still carries any of that. The still this recipe also
# writes is for the readers and the viewers that cannot play video, and it is the run's most legible
# frame rather than a second claim.
#
# WHY THERE IS A THIRD ARTEFACT. M10 tasks.md 7.3 asks for the environment demo's frame budget
# "measured as a CURVE across the cycle rather than asserted at one time of day". The program writes
# every frame's per-producer cost to a CSV and the collector plots it; a maximum printed in a log
# would have been exactly the assertion that task is written to refuse.
#
# WHY THE FRAMES GO IN THE BUILD TREE AND NOT IN mktemp -d. The engine's PNG writer stores rather
# than deflates, so six hundred frames at 1280x720 are about two gigabytes — more than a tmpfs /tmp
# on many machines will hold. They are written under the build directory and deleted when the recipe
# finishes, which is the same lifetime `mktemp -d` would have given them and not the same filesystem.
#
# IT NEEDS ONE THING THE REPOSITORY DOES NOT HAVE: a graphics device. It needs NO content at all —
# there is no asset, no mesh and no texture anywhere in this artefact; the world is generated from
# the seed on the command line. Without a device the program still generates, cooks, claims, places
# and simulates the whole world, prints its report, says which device it did not find, and writes no
# frames. That is why this is a recipe a person runs and not a test, exactly as
# `just capture-animated-character` and `just capture-virtual-geometry` above are.

# Generate a world, film a day of it, and encode the video, the still and the budget curve. Usage: just capture-world [--profile <p>] [--seconds <s>] [--width <n>] [--height <n>] [--fps <n>] [--seed <n>] [--regions <n>]
capture-world *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    just build-engine --profile "${profile}"

    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    sample="${build_dir}/samples/10-world/cy_sample_world"
    if [ ! -x "${sample}" ]; then
        echo "capture-world: ${sample} was not built." >&2
        exit 1
    fi

    fps="${CY_CAPTURE_FPS:-30}"
    work="${build_dir}/world"
    rm -rf "${work}"
    mkdir -p "${work}/frames"
    trap 'rm -rf "${work}/frames"' EXIT

    echo "==> capture     world  fps=${fps}"
    "${sample}" --frames "${work}/frames" --still "${work}/still.png" \
        --budget "${work}/budget.csv" --fps "${fps}" ${rest}

    echo "==> encode      world"
    python3 tools/docs/collect_world.py "${work}/frames" \
        --still "${work}/still.png" --budget "${work}/budget.csv" --fps "${fps}"

# ==================================================================================================
# THE BEAUTY SHOT. M11.c's closing artefact.
# ==================================================================================================
#
# WHAT IT REGENERATES, AND FROM WHAT. Everything below the line is produced; everything above it is
# committed, and nothing that is produced is committed except the two pictures, the manifest and the
# video.
#
#   content/beauty/materials/*.cymatcanvas   the editor's canvas, written by `cy-author-material`
#   content/beauty/materials/*.cygraph       the canonical authored graph, written by the ENGINE
#   content/beauty/textures/*.png            the project's own textures, from tools/content/
#   content/beauty/meshes/*.cyprim           six primitive sources
#   content/beauty/shot.cyshot              the scene, the camera, the sun and the grade
#   ------------------------------------------------------------------------------------------
#   docs/design/images/m11c-beauty-shot.png          the picture
#   docs/design/images/m11c-beauty-shot-linear.png   the same frame before the post chain
#   docs/design/images/m11c-beauty-shot.manifest     what the frame says it did
#   docs/design/videos/m11c-beauty-shot.mp4          the turntable, when --video is passed
#
# THE FIVE STEPS ARE THE FIVE JUNCTIONS M11.c's SPIKE MEASURED, in order:
#
#   1 AUTHOR    cy-author-material  — the editor's own canvas, `SpecialisedEditors::open(Materials)`
#   2 COMPILE   cy_material author  — lower, compile, and assemble the translation unit
#   3 SHADER    slangc              — the engine's own shader toolchain, over the generated module
#   4 COOK+BIND cy_sample_beauty    — the importer's BC7/BC5 blocks, uploaded and bound
#   5 CAPTURE   cy_sample_beauty    — the assembled frame, and a manifest built from its own report
#
# IT NEEDS A GRAPHICS DEVICE for step 5 and nothing else. Without one the program still authors,
# compiles, cooks and reports, says which device it did not find, and writes no picture — the same
# arrangement `capture-world` and `capture-virtual-geometry` use, and the reason neither is a test.

# Author, compile, cook and photograph M11.c's beauty shot. Usage: just capture-beauty-shot [--profile <p>] [--width <n>] [--height <n>] [--supersample <n>] [--video] [--regenerate-textures]
capture-beauty-shot *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    want_video=0
    regenerate=0
    forwarded=()
    for argument in ${rest}; do
        case "${argument}" in
            --video) want_video=1 ;;
            --regenerate-textures) regenerate=1 ;;
            *) forwarded+=("${argument}") ;;
        esac
    done

    just build-engine --profile "${profile}"
    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    sample="${build_dir}/samples/12-beauty/cy_sample_beauty"
    cooker="${build_dir}/tools/material/cy_material"
    slangc="${build_dir}/Development/bin/slangc"
    for tool in "${sample}" "${cooker}" "${slangc}"; do
        if [ ! -x "${tool}" ]; then
            echo "capture-beauty-shot: ${tool} was not built." >&2
            exit 1
        fi
    done

    # The editor's half. `cargo build` rather than `just build-editor`, because this needs one
    # binary out of one crate and not the whole application.
    echo "==> author      the material graphs, on the editor's own canvas"
    ( cd editor && cargo build --quiet --jobs "$(just _jobs)" -p cy-editor-interface --bin author_material )
    ./editor/target/debug/author_material content/beauty/materials

    if [ "${regenerate}" -eq 1 ]; then
        echo "==> generate    the source textures"
        python3 tools/content/make_beauty_textures.py
    fi

    work="${build_dir}/beauty"
    rm -rf "${work}"
    mkdir -p "${work}"
    just _beauty-programs "${build_dir}" "${work}" content/beauty/materials

    mkdir -p docs/design/images docs/design/videos
    frames=()
    if [ "${want_video}" -eq 1 ]; then
        rm -rf "${work}/frames"
        mkdir -p "${work}/frames"
        frames=(--frames "${work}/frames" --frames-count "${CY_BEAUTY_FRAMES:-240}")
    fi

    echo "==> capture     the frame"
    "${sample}" --shot content/beauty/shot.cyshot --materials "${work}" \
        --still docs/design/images/m11c-beauty-shot.png \
        --no-post-still docs/design/images/m11c-beauty-shot-linear.png \
        --manifest docs/design/images/m11c-beauty-shot.manifest \
        "${frames[@]+"${frames[@]}"}" ${forwarded[@]+"${forwarded[@]}"}

    if [ -f docs/design/images/m11c-beauty-shot.png ]; then
        echo "==> recompress  the stills"
        python3 tools/docs/collect_beauty_shot.py
    fi

    if [ "${want_video}" -eq 1 ] && [ -n "$(ls -A "${work}/frames" 2>/dev/null)" ]; then
        echo "==> encode      the turntable"
        ffmpeg -y -loglevel error -framerate "${CY_BEAUTY_FPS:-30}" \
            -i "${work}/frames/frame_%04d.png" \
            -c:v libx264 -pix_fmt yuv420p -crf 18 \
            docs/design/videos/m11c-beauty-shot.mp4
        rm -rf "${work}/frames"
        echo "    docs/design/videos/m11c-beauty-shot.mp4"
    fi

# STEPS 2 AND 3, SHARED BY THE CAPTURE AND BY ITS MEASUREMENT. `graphs` is where the canonical
# `.cygraph` lands: the capture writes the committed one under content/beauty/materials, and the
# measurement below writes into its own work directory so that it never touches the tree.
#
# THE PIXEL STAGE IS DECLARED IN `beauty.slang` AND NOT HERE, and that is a decision. The generated
# prelude's `cy_material_sample` takes the implicit level of detail only under
# `CY_MATERIAL_PIXEL_STAGE`; without it every albedo and data map in the shot read level 0 and the
# cooked chain beneath it was never sampled (M11.c task 3.8, refuted at the gate by disassembling
# these very modules). The file is the fragment-stage lowering — its header says so — so it is the
# one place that knows it is one, and every caller of slangc on it gets the define. A `-D` on this
# command line would have fixed this recipe and left the next caller to rediscover the defect.
[private]
_beauty-programs build_dir work graphs:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    cooker="{{build_dir}}/tools/material/cy_material"
    slangc="{{build_dir}}/Development/bin/slangc"
    echo "==> compile     the authored graphs, through the engine's own lowering"
    for canvas in content/beauty/materials/*.cymatcanvas; do
        name="$(basename "${canvas}" .cymatcanvas)"
        "${cooker}" author "${canvas}" \
            --graph "{{graphs}}/${name}.cygraph" \
            --module "{{work}}/${name}.slang" \
            --info "{{work}}/${name}.cymatinfo"
    done

    echo "==> shader      the generated programs, through slangc"
    for info in "{{work}}"/*.cymatinfo; do
        name="$(basename "${info}" .cymatinfo)"
        entry="$(awk '/^entry /{print $2}' "${info}")"
        "${slangc}" samples/12-beauty/shaders/beauty.slang \
            -target spirv -profile spirv_1_5 \
            -entry sceneVertex -stage vertex -entry sceneFragment -stage fragment \
            -entry sceneFragmentOccluded -stage fragment \
            -entry scenePrepassFragment -stage fragment \
            -entry shadowVertex -stage vertex \
            -entry skyVertex -stage vertex -entry skyFragment -stage fragment \
            -fvk-use-entrypoint-name \
            -I src/rendering/shaders -I "{{work}}" \
            -DCY_MATERIAL_MODULE="\"${name}.slang\"" -DCY_MATERIAL_ENTRY="${entry}" \
            -o "{{work}}/${name}.spv"
    done

# THE BEAUTY SHOT WITH AND WITHOUT BLOOM: the same scene, the same compiled programs and the same
# settled embers, photographed once with bloom absent from the post chain and once with the shot's
# bloom grade in it (`--bloom`). The published M11.c still is not touched — this writes its own
# pair and the manifest of the bloomed frame, whose post-stage list names step 9. Needs a graphics
# device, and says so and writes nothing without one.
# Photograph the beauty shot with and without bloom. Usage: just capture-beauty-bloom [--profile <p>] [--width <n>] [--height <n>] [--supersample <n>]
capture-beauty-bloom *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    just build-engine --profile "${profile}"
    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    sample="${build_dir}/samples/12-beauty/cy_sample_beauty"
    for tool in "${sample}" "${build_dir}/tools/material/cy_material" \
                "${build_dir}/Development/bin/slangc"; do
        if [ ! -x "${tool}" ]; then
            echo "capture-beauty-bloom: ${tool} was not built." >&2
            exit 1
        fi
    done

    # The committed canvases, compiled into the build directory as `measure-beauty-mip-chain`
    # does: nothing is re-authored into the tree, so the pair is of the published scene.
    work="${build_dir}/beauty-bloom"
    rm -rf "${work}"
    mkdir -p "${work}" docs/design/images
    just _beauty-programs "${build_dir}" "${work}" "${work}"

    echo "==> capture     without bloom"
    "${sample}" --shot content/beauty/shot.cyshot --materials "${work}" \
        --still docs/design/images/bloom-beauty-off.png ${rest}
    echo "==> capture     with bloom"
    "${sample}" --shot content/beauty/shot.cyshot --materials "${work}" --bloom \
        --still docs/design/images/bloom-beauty-on.png \
        --manifest docs/design/images/bloom-beauty-on.manifest ${rest}

    echo "==> recompress  the stills"
    python3 tools/docs/collect_beauty_shot.py docs/design/images/bloom-beauty-off.png \
        docs/design/images/bloom-beauty-on.png

# DOES THE SHOT READ ITS COOKED MIP CHAIN? M11.c task 3.8, measured on the artefact.
#
# The shot photographed three times from the same compiled programs: twice with every cooked chain
# uploaded and once with level 0 alone of every ALBEDO map (`--albedo-levels 1`) — the one texture
# only the material samples; the file below says why. samples/12-beauty/measure_mip_chain.py
# requires the repeat to be byte-identical, the level-0 picture to DIFFER, and the level-0 picture to
# be the more aliased. Nothing is written into the tree; the pictures stay in the build directory.
# It needs a graphics device, and says so and fails without one — a skip is not a measurement.

# Measure whether the beauty shot reads its cooked mip chain. Usage: just measure-beauty-mip-chain [--profile <p>] [--width <n>] [--height <n>] [--supersample <n>]
measure-beauty-mip-chain *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    just build-engine --profile "${profile}"
    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    sample="${build_dir}/samples/12-beauty/cy_sample_beauty"
    for tool in "${sample}" "${build_dir}/tools/material/cy_material" \
                "${build_dir}/Development/bin/slangc"; do
        if [ ! -x "${tool}" ]; then
            echo "measure-beauty-mip-chain: ${tool} was not built." >&2
            exit 1
        fi
    done

    work="${build_dir}/beauty-mip-chain"
    rm -rf "${work}"
    mkdir -p "${work}"
    just _beauty-programs "${build_dir}" "${work}" "${work}"

    for run in chain repeat level-zero; do
        levels=0
        if [ "${run}" = level-zero ]; then
            levels=1
        fi
        echo "==> capture     ${run}"
        "${sample}" --shot content/beauty/shot.cyshot --materials "${work}" \
            --still "${work}/${run}.png" --albedo-levels "${levels}" \
            ${rest} > "${work}/${run}.log" 2>&1 || {
            cat "${work}/${run}.log"
            exit 1
        }
        grep -E "^(device|textures?|albedo|data|normal) " "${work}/${run}.log" || true
        if [ ! -f "${work}/${run}.png" ]; then
            cat "${work}/${run}.log"
            echo "measure-beauty-mip-chain: no picture was written — no graphics device, and a skip is not a measurement." >&2
            exit 1
        fi
    done
    python3 samples/12-beauty/measure_mip_chain.py \
        "${work}/chain.png" "${work}/repeat.png" "${work}/level-zero.png"

# AMBIENT OCCLUSION, OFF AND ON, IN THE BEAUTY SHOT. `rendering-post-processing` — "Ambient
# occlusion".
#
# The same compiled programs photographed twice by two runs of the same program, which differ in
# `--ambient-occlusion` and in nothing else. Off is M11.c's published frame, and
# tools/docs/compare_ambient_occlusion.py REQUIRES its pixels to be the ones
# docs/design/images/m11c-beauty-shot.png holds: the setting off must leave the frame byte-identical
# to the frame before the stage existed. On adds the prepass and the stage and darkens the sky term
# where the columns meet the ground. The graphs are written into the work directory, so nothing in
# content/ is touched.
#
#   docs/design/images/ambient-occlusion-off.png     the frame, the setting off
#   docs/design/images/ambient-occlusion-on.png      the frame, the setting on
#   docs/design/images/ambient-occlusion-detail.png  one contact, off beside on, at 2x
#   docs/design/images/ambient-occlusion-on.manifest what the frame with the stage says it did

# Photograph the beauty shot with ambient occlusion off and on. Usage: just capture-ambient-occlusion [--profile <p>] [--width <n>] [--height <n>] [--supersample <n>]
capture-ambient-occlusion *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    just build-engine --profile "${profile}"
    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    sample="${build_dir}/samples/12-beauty/cy_sample_beauty"
    for tool in "${sample}" "${build_dir}/tools/material/cy_material" \
                "${build_dir}/Development/bin/slangc"; do
        if [ ! -x "${tool}" ]; then
            echo "capture-ambient-occlusion: ${tool} was not built." >&2
            exit 1
        fi
    done

    work="${build_dir}/beauty-occlusion"
    rm -rf "${work}"
    mkdir -p "${work}" docs/design/images
    just _beauty-programs "${build_dir}" "${work}" "${work}"

    images=docs/design/images
    for setting in off on; do
        echo "==> capture     ambient occlusion ${setting}"
        manifest=()
        if [ "${setting}" = on ]; then
            manifest=(--manifest "${images}/ambient-occlusion-on.manifest")
        fi
        "${sample}" --shot content/beauty/shot.cyshot --materials "${work}" \
            --ambient-occlusion "${setting}" \
            --still "${images}/ambient-occlusion-${setting}.png" \
            ${manifest[@]+"${manifest[@]}"} ${rest} | tee "${work}/${setting}.log"
        if [ ! -f "${images}/ambient-occlusion-${setting}.png" ]; then
            echo "capture-ambient-occlusion: no picture was written — no graphics device." >&2
            exit 1
        fi
    done
    python3 tools/docs/compare_ambient_occlusion.py \
        --off "${images}/ambient-occlusion-off.png" --on "${images}/ambient-occlusion-on.png" \
        --reference "${images}/m11c-beauty-shot.png" \
        --detail "${images}/ambient-occlusion-detail.png"
    python3 tools/docs/collect_beauty_shot.py \
        "${images}/ambient-occlusion-off.png" "${images}/ambient-occlusion-on.png" \
        "${images}/ambient-occlusion-detail.png"

# M11.d's ARTEFACT, AND WHY IT IS ONE RECIPE AND NOT FOUR.
#
# Task 8.1 is "one project — built, cooked, packaged and LAUNCHED FROM A SINGLE RECIPE", and the
# word doing the work is *single*. `just content-package` and `just content-patch` already exist and
# already orchestrate `cy_build`; what nothing in this tree had was one entry point that takes a
# project from source to a running window. This is it, and it builds the engine first for the same
# reason every other run recipe does: running whatever binary happens to be lying in the build tree
# reports on a revision nobody has.
#
# `--platform headless` is the leg continuous integration runs — `smoke.ship` — and it presents
# nothing and says so. `--platform sdl3` and `--platform native` are the two that DRAW, and
# `--platform both` runs the same binary through each in turn, which is the only form in which "it
# draws through the native backend and through SDL3" is one run's evidence rather than two
# recollections. `--shot <path>` photographs what the device presented rather than what the host
# composed. `--require-draw` turns "nothing was presented" into a non-zero exit, which is what makes
# the drawing claim falsifiable.
#
# IT IS NOT A GAME. `m11b:the-game-exists` is red and this recipe does not close it; see
# samples/11-ship/README.md.

# Run the M11.d artefact: one project cooked, packaged, installed and launched. Usage: just run-ship [--profile <p>] [--platform sdl3|native|headless|both] [--frames <n>] [--shot <path>] [--coverage <path>] [--require-draw]
run-ship *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    just build-engine --profile "${profile}" -D CY_BUILD_TOOLS=ON

    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    mkdir -p docs/design/images
    echo "==> run         ship  profile=${profile}"
    python3 samples/11-ship/ship.py --build-dir "${build_dir}" \
        --work "${build_dir}/ship" ${rest}

    # A shot written into docs/design/images is a file that lives in the tree, and the golden-image
    # suite's PNG writer STORES rather than compresses — the right trade for a byte comparison and
    # the wrong one here. `collect_ship.py` recompresses and REFUSES to write a file whose pixels
    # changed, which is the same two lines every other capture recipe in this file runs.
    shots=()
    for shot in docs/design/images/m11d-ship-sdl3.png docs/design/images/m11d-ship-native.png; do
        [ -f "${shot}" ] && shots+=("${shot}")
    done
    if [ "${#shots[@]}" -gt 0 ]; then
        echo "==> recompress  the stills"
        python3 tools/docs/collect_ship.py "${shots[@]}"
    fi
