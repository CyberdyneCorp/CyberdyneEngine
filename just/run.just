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
    # build, and Vulkan on a build configured with -D CY_RENDERER_VULKAN=ON.
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

# Run the runtime host with no window system, as CI does. Usage: just run-headless [<sample>] [args...]
run-headless *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    # M0's runtime host is the empty sample: it is the only program that starts the runtime, and it
    # chooses its display server at run time. When a standalone host binary lands, this recipe runs
    # that instead and `run-sample empty --headless` keeps working unchanged.
    just run-sample {{args}} --headless
