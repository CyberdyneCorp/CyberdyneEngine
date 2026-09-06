# samples/ — the milestone artefacts. Each is a small, complete program that exercises the engine
# through its public surface, and each milestone's closing artefact is one of them.
#
# `just run-sample <name>` finds a sample's binary as samples/*/cy_sample_<name>, so a new sample is
# a directory, a cy_add_module(NAME cy_sample_<name> ...) and one line here — there is no manifest
# to keep in step.
add_subdirectory(00-empty)
add_subdirectory(01-headless-host)
add_subdirectory(02-headless-sim)
# M3's artefact declares TWO targets: cy::sample-first-light, the scene and the renderer, and the
# host around it. tests/render/ links the first so that the golden images photograph the sample
# rather than a copy of it — see 03-first-light/CMakeLists.txt for why that is worth the split.
add_subdirectory(03-first-light)
# M4's artefact is a THIRD shape again: one executable, and a game module that is not C++ at all.
# samples/04-character/game/ is five Swift files built into a shared library by SwiftPM and loaded at
# run time over the C ABI; host/ is the engine's side and decides nothing. The directory declares its
# own smoke test and its own "no C++ gameplay" check, because on a machine with no Swift toolchain the
# target does not exist and tests/ has already been added by the time this line runs.
add_subdirectory(04-character)
# M5's artefact is a FOURTH shape, and the only one that declares no C++ target at all: the session
# is three processes in two languages — the importer, the editor, and a hosted runtime on the other
# side of a Unix domain socket — and what is being demonstrated is what happens when one of them is
# killed. samples/05-editor-session/session.py is the artefact and `just run-editor-session` is the
# recipe; this directory contributes the two CTest entries that run it, which is also where task
# 6.3's headless editor tests are declared. Its CMakeLists.txt says why it is shaped that way.
add_subdirectory(05-editor-session)
