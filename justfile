# CyberdyneEngine — the single entry point for developer tasks.
#
# `just` orchestrates; it does not build. CMake and Ninja own the engine, Cargo owns the editor, the
# Slang toolchain owns shaders, and the engine's own tools own content. A recipe that reimplements
# incremental logic is a defect, and so is a documented procedure that has no recipe.
#
# Recipes are named `<category>-<verb>` and live one category per file under just/, imported below.
# The names are flat rather than `just` modules so that bare `just` lists every recipe with its
# description — see design.md §10.

# bash on every host, including Windows: a recipe that behaves differently depending on the shell
# that happened to run it is a recipe that cannot gate anything.
set shell := ["bash", "-euo", "pipefail", "-c"]
set windows-shell := ["bash", "-euo", "pipefail", "-c"]

# Local configuration without editing a shared file. `.just.local` is git-ignored and holds
# `CY_*=value` lines; every override below reads through env_var_or_default, so the file and a real
# environment variable are the same mechanism and an exported variable wins over the file.
# `just env-overrides` reports what is in effect, and every recipe that acts on one says so.
set dotenv-filename := '.just.local'
set dotenv-load := true

# List every recipe with its description. This is the workflow's documentation.
[private]
default:
    @just --list --list-heading $'CyberdyneEngine — developer workflow\n\nRecipes:\n'

import 'just/env.just'
import 'just/build.just'
import 'just/run.just'
import 'just/test.just'
import 'just/quality.just'
import 'just/generate.just'
import 'just/content.just'
import 'just/diagnose.just'
import 'just/roadmap.just'
import 'just/maintenance.just'
import 'just/release.just'
import 'just/ci.just'

# --- The profile table ----------------------------------------------------------------------------
#
# Task 0.1, design.md §7. Four names that mean the same thing in every toolchain the engine will use.
# Cargo arrives at M5 and Slang at M3; their columns are filled in and unused, so that wiring them is
# a lookup rather than a decision made under the pressure of a milestone.
#
#   profile | CMake configuration | assertions | editor | Cargo profile | slangc flags
#
# The CMake column is checked, not merely documented: `build-engine` passes both the profile and the
# configuration this table gives, and cmake/profiles.cmake fails if they disagree. The Cargo and
# Slang columns become checkable the same way when those toolchains land.

profile_table := '
debug   | Debug       | on  | on  | dev         | -O0 -g
dev     | Development | on  | on  | development | -O1 -g
profile | Profile     | off | off | profiling   | -O2 -g
release | Shipping    | off | off | shipping    | -O3
'

# The repository root. `just` 1.21 runs a recipe in the directory of the file that *defines* it, so
# a recipe in just/ starts in just/ rather than at the root. Every recipe that touches the tree
# begins `cd "{{root}}"`; a recipe that forgets will fail loudly on the first path it uses.
root := justfile_directory()

# The working profile when none is named. Override per invocation with `--profile <name>`, or for a
# whole shell with CY_PROFILE.
default_profile := env_var_or_default('CY_PROFILE', 'dev')

# --- Shared helpers -------------------------------------------------------------------------------
#
# Private: they are implementation, not workflow, and they do not belong in the recipe listing.

# Split recipe arguments into the selected profile and everything else, tab separated.
[private]
_split *args:
    #!/usr/bin/env bash
    set -euo pipefail
    profile='{{default_profile}}'
    rest=()
    argv=({{args}})
    i=0
    while (( i < ${#argv[@]} )); do
        case "${argv[i]}" in
            --profile)   i=$((i + 1)); profile="${argv[i]:-}" ;;
            --profile=*) profile="${argv[i]#*=}" ;;
            *)           rest+=("${argv[i]}") ;;
        esac
        i=$((i + 1))
    done
    if ! just _profile-column "$profile" 1 >/dev/null 2>&1; then
        echo "unknown profile '$profile'. Profiles: $(just _profiles | tr '\n' ' ')" >&2
        exit 2
    fi
    printf '%s\t%s\n' "$profile" "${rest[*]:-}"

# Print column N of a profile's row in the table, 1-based.
[private]
_profile-column profile column:
    #!/usr/bin/env bash
    set -euo pipefail
    row="$(printf '%s\n' '{{profile_table}}' | awk -F'|' -v p='{{profile}}' \
        '{ key = $1; gsub(/^[ \t]+|[ \t]+$/, "", key) } key == p { print; found = 1 } END { exit !found }')"
    printf '%s\n' "$row" | awk -F'|' -v n='{{column}}' \
        '{ v = $n; gsub(/^[ \t]+|[ \t]+$/, "", v); print v }'

# Print the profile names, one per line.
[private]
_profiles:
    @printf '%s\n' '{{profile_table}}' | awk -F'|' 'NF > 1 { gsub(/[ \t]/, "", $1); print $1 }'

# Report an active local override, so that a divergence from the default is visible when diagnosing.
[private]
_report-override name value:
    @[ -n '{{value}}' ] && echo "override: {{name}}={{value}}" >&2 || true

# The one shape every unimplemented recipe takes: name what is missing and fail.
[private]
_not-implemented recipe task:
    @echo "just {{recipe}}: not implemented (task {{task}})" >&2
    @exit 1

# Ask before an irreversible action, and refuse rather than guess when nobody can answer.
#
# `developer-workflow-and-just` requires a destructive recipe to name what it will do and require
# confirmation "unless explicitly invoked in a non-interactive mode". So a pipe or a CI log is not
# consent: with no terminal and no --yes the answer is no, and the recipe says which flag would have
# made it yes. Callers pass the flag they parsed as `assumed_yes`.
[private]
_confirm action assumed_yes='':
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -n '{{assumed_yes}}' ]; then
        echo "--yes: {{action}}" >&2
        exit 0
    fi
    if [ ! -t 0 ]; then
        echo "refusing to {{action}} without a terminal to confirm at." >&2
        echo "  Pass --yes to say so explicitly; that is what a script or CI does." >&2
        exit 2
    fi
    read -r -p "{{action}}? [y/N] " reply
    case "${reply}" in
        [yY] | [yY][eE][sS]) exit 0 ;;
        *) echo "cancelled; nothing was removed" >&2; exit 1 ;;
    esac

# The local overrides in effect, one `NAME=value` per line, empty when the defaults are in use.
#
# One table, so that a new override is added in one place and is reported, documented and diagnosed
# by that single edit rather than by three that drift.
[private]
_overrides:
    #!/usr/bin/env bash
    set -euo pipefail
    for name in CY_PROFILE CY_BUILD_DIR CY_JOBS CY_TARGET_PLATFORM CY_TEST_JUNIT \
                CY_CLANG_FORMAT CY_CLANG_TIDY; do
        value="${!name:-}"
        if [ -n "${value}" ]; then
            printf '%s=%s\n' "${name}" "${value}"
        fi
    done
    if [ -f '{{root}}/.just.local' ]; then
        printf '%s\n' '.just.local=loaded'
    fi

# Report every active override on stderr, so a divergence from the default is visible in the log of
# whatever went wrong. Silent when there is nothing to report.
[private]
_report-overrides:
    #!/usr/bin/env bash
    set -euo pipefail
    active="$(just _overrides)"
    if [ -n "${active}" ]; then
        while IFS= read -r line; do
            echo "override: ${line}" >&2
        done <<< "${active}"
    fi
