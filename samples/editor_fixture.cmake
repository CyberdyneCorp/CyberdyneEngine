# samples/editor_fixture.cmake — the editor, built ONCE and BEFORE the tests that drive it.
#
# Five CTest entries drive the Rust editor: smoke.editor_session, integration.editor_session_selftest,
# smoke.agent_authoring, smoke.authoring and (where a display exists) the window pair. Until this
# file, each of the first three built it from INSIDE its own timed run (`--build`), and the fourth
# built nothing and looked for it in the wrong tree. A warm Cargo build is a second, so that went
# unnoticed on a developer's machine; on a hosted runner the cache holds no editor, the cold build of
# the workspace takes most of five minutes, and the smoke budget (300 s) was spent compiling crates
# before the session had started. `agent` timed out on every run and `authorable` on some; the
# `profiles` and `test` legs failed `smoke.authoring` outright because nothing there built the editor
# at all.
#
# THE BUILD IS A FIXTURE, NOT A STEP OF THE TEST. `smoke.editor_build` runs `just build-editor` for
# this tree's profile and is the setup of the `cy_editor` fixture; every entry that drives the editor
# REQUIRES it and passes no `--build`. CTest runs a required setup first and adds it to any selection
# that needs it, whatever `-R` or `-L` chose — so `just test-smoke -R agent_authoring` still builds
# the editor, and does it outside the session's own budget. When the build fails, CTest does not run
# the entries that required it and says so, rather than letting them report a missing binary.
#
# THE BUILD'S TIMEOUT IS ITS OWN, and it is not a test budget. It bounds a cold Cargo build of the
# whole editor workspace on the slowest runner we use, with room for the machine-wide job pool
# (`_cargo-pool`) to have other work in it; the smoke budget still bounds every session exactly as
# before. tools/ci/editor_fixture.py holds this shape against the configured tree.
#
# ONE PATH FOR BOTH SIDES. `just build-editor` writes into `just _editor-target-dir`, and every driver
# finds the editor through that same recipe in the same environment — so the fixture and the test
# cannot disagree about where the binary is, whatever CY_BUILD_DIR says.

set(CY_EDITOR_BUILD_TIMEOUT 1800 CACHE STRING
    "Seconds a cold `just build-editor` may take as the cy_editor CTest fixture")

# cy_sample_requires_editor(<profile> <test>...)
#
# Declares smoke.editor_build the first time it is called and makes every named test require it.
# Every caller has already declined to register its tests where cargo or just is absent — the tree's
# rule for an artefact that cannot run here — so their absence here is a caller's mistake.
function(cy_sample_requires_editor profile)
    find_program(CY_EDITOR_FIXTURE_CARGO cargo)
    find_program(CY_EDITOR_FIXTURE_JUST just)
    if(NOT CY_EDITOR_FIXTURE_CARGO OR NOT CY_EDITOR_FIXTURE_JUST)
        message(FATAL_ERROR
            "cy_sample_requires_editor: cargo and just are required to build the editor. A caller "
            "checks for them and declines to register its tests before calling this.")
    endif()

    get_property(declared GLOBAL PROPERTY CY_EDITOR_FIXTURE_PROFILE)
    if(NOT declared)
        add_test(NAME smoke.editor_build
                 COMMAND "${CY_EDITOR_FIXTURE_JUST}" build-editor --profile "${profile}"
                 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        set_tests_properties(smoke.editor_build PROPERTIES
                             LABELS "smoke"
                             TIMEOUT "${CY_EDITOR_BUILD_TIMEOUT}"
                             FIXTURES_SETUP cy_editor)
        set_property(GLOBAL PROPERTY CY_EDITOR_FIXTURE_PROFILE "${profile}")
    elseif(NOT declared STREQUAL profile)
        message(FATAL_ERROR
            "cy_sample_requires_editor: the editor fixture builds profile '${declared}' and a test "
            "asked for '${profile}'. One build tree is one profile; the lookups disagree.")
    endif()

    foreach(test IN LISTS ARGN)
        set_property(TEST "${test}" APPEND PROPERTY FIXTURES_REQUIRED cy_editor)
    endforeach()
endfunction()
