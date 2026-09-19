# just/deploy.just — Deploy: install a built product where something can run it. M11.d task 7.7.
#
# `developer-workflow-and-just` names four recipe kinds that must take a target platform — build,
# test, **deploy** and package — and until M11.d three of the four existed. This file is the fourth,
# and it is deliberately narrow.
#
# --- WHAT IS REAL HERE AND WHAT IS NOT --------------------------------------------------------------
#
# The requirement has two halves and this rung can honestly do one of them:
#
#     Deployment to a device or console SHALL be a recipe, and SHALL integrate with the
#     hosted-runtime and remote play modes rather than being a separate manual procedure.
#
#   * INSTALLING A BUILT PACKAGE INTO AN INSTALLATION ROOT IS REAL. `cy_build install` has verified,
#     content-addressed installation with an atomic switch since M6, and `cy_build verify`
#     re-digests every chunk the installed build names. `deploy-install` is those two behind one
#     recipe, with the target resolved through `just/targets.toml` like every other recipe.
#
#   * SENDING IT TO A DEVICE IS NOT. There is no transport: `core-assets-and-io`'s "development file
#     serving" had no transport at all when this rung opened, the mobile targets are M11.e's, and
#     consoles are out of scope by specification. `deploy-device` therefore REFUSES, naming what is
#     required and the rung that owns it — which is the requirement's own scenario applied to itself,
#     and the opposite of `just/release.just`'s four recipes, which refuse naming a milestone that is
#     not on the ladder at all.
#
# A recipe that pretended to deploy to a device by copying a directory would be the ninth check in
# this repository that cannot fail. M11.d task 7.8 is the same rule for the release recipes.

# Install a built package into an installation root, verified. Usage: just deploy-install [--profile <p>] [--platform <t>] --package <manifest> --artefacts <store> --install <dir>
deploy-install *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    build_dir="${CY_BUILD_DIR:-build/${profile}}"

    platform=()
    tool_args=()
    install_root=""
    expecting=""
    for argument in ${rest}; do
        case "${expecting}" in
            platform) platform=(--platform "${argument}"); expecting=""; continue ;;
            install)  install_root="${argument}"; tool_args+=(--install "${argument}"); expecting=""; continue ;;
        esac
        case "${argument}" in
            --platform)   expecting=platform ;;
            --platform=*) platform=(--platform "${argument#*=}") ;;
            --install)    expecting=install ;;
            --install=*)  install_root="${argument#*=}"; tool_args+=(--install "${install_root}") ;;
            *)            tool_args+=("${argument}") ;;
        esac
    done
    if [ -n "${expecting}" ]; then
        echo "deploy-install: --${expecting} needs a value." >&2
        exit 2
    fi
    if [ -z "${install_root}" ]; then
        echo "deploy-install: --install names the installation root and is required." >&2
        echo "  Usage: just deploy-install --package <manifest> --artefacts <store> --install <dir>" >&2
        exit 2
    fi

    # The target is resolved before anything is built, so an impossible one is refused in a second
    # rather than after a configure. The refusal is `just/targets.toml`'s, which is the same one
    # `just build-engine --platform` gives — one table, one answer.
    just build-engine --profile "${profile}" ${platform[@]+"${platform[@]}"} -D CY_BUILD_TOOLS=ON

    echo "==> install     ${install_root}"
    "${build_dir}/tools/build/cy_build" install ${tool_args[@]+"${tool_args[@]}"}
    # VERIFY IS NOT OPTIONAL AND IS NOT A SECOND RECIPE. An install that reported success and wrote a
    # chunk whose digest does not match what the manifest names is exactly the failure an
    # installation root is for catching, and a developer who has to remember a second command is a
    # developer who finds out on the device.
    echo "==> verify      every chunk the installed build names, re-digested"
    exec "${build_dir}/tools/build/cy_build" verify --install "${install_root}"

# Deploy to a device or console. Not implemented, and this says exactly what is missing. Usage: just deploy-device --platform <target>
deploy-device *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"

    requested=""
    expecting=0
    for argument in ${rest}; do
        if [ "${expecting}" -eq 1 ]; then requested="${argument}"; expecting=0; continue; fi
        case "${argument}" in
            --platform)   expecting=1 ;;
            --platform=*) requested="${argument#*=}" ;;
        esac
    done

    # An impossible target is refused by the table FIRST, so "there is no such target" and "there is
    # no transport" are never confused with each other. This is the scenario the requirement writes:
    # the workflow says so, and states what is required.
    if [ -n "${requested}" ]; then
        python3 tools/workflow/targets.py resolve "${requested}" > /dev/null
    fi

    echo "just deploy-device: there is no device transport in this tree." >&2
    echo "  What exists: \`just deploy-install\` installs a built package into an installation root" >&2
    echo "  on this machine, verified chunk by chunk, and \`just run-editor-runtime\` and" >&2
    echo "  \`just run-editor-session\` are the hosted-runtime and remote play modes the requirement" >&2
    echo "  says a deployment integrates with." >&2
    echo "  What is required: a transport. \`core-assets-and-io\`'s RemoteFileProvider is the seam" >&2
    echo "  and the mobile targets are M11.e's; consoles are out of scope by specification." >&2
    echo "  The rung that owns it: M11.E — build-system-and-platforms." >&2
    exit 1
