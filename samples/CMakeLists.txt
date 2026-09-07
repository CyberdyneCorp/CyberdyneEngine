# samples/ — the milestone artefacts. Each is a small, complete program that exercises the engine
# through its public surface, and each milestone's closing artefact is one of them.
#
# `just run-sample <name>` finds a sample's binary as samples/*/cy_sample_<name>, so a new sample is
# a directory, a cy_add_module(NAME cy_sample_<name> ...) and one line here — there is no manifest
# to keep in step.
#
# harness/ is not an artefact. It is what every artefact below reports THROUGH, and it is first
# because both of the rules it carries were broken at M6 by artefacts that were otherwise careful:
# a run that recorded a gap returned 0, and a run led with an extreme-value statistic that four
# re-runs contradicted. `artefact.Report.exit_code` is derived from the gaps and
# `artefact.Report.headline` refuses a maximum, so neither can happen again without deleting the
# module. M7 tasks 5b.5 and 5b.5b.
add_subdirectory(harness)
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
# M5.5's artefacts are TWO, and the split is the point rather than a convenience: a person operating
# a window and an agent authoring through the same editor cannot be demonstrated by one program, and
# pretending otherwise would repeat the mistake M5.5 exists to correct (design.md §5).
#
# samples/05b-editor-window opens the editor's window on a project and operates it with SYNTHESISED
# X11 INPUT — real key presses and pointer drags through XTEST, delivered to the server rather than
# to the application, so the editor has no idea it is not a hand on a keyboard. It therefore needs a
# display, XTEST, a graphics device and two Python modules, and its directory declares nothing at all
# where any of those is missing. It is the FIRST artefact in this tree that cannot run on a hosted
# runner, which is why there is a second one.
#
# samples/05b-agent-authoring is that second one: an agent on the other end of `cyberdyne-editor
# --mcp`, over the Model Context Protocol on standard input. No window, no device, and therefore the
# half of the milestone that runs in continuous integration. Each directory's CMakeLists.txt argues
# its own shape.
add_subdirectory(05b-editor-window)
add_subdirectory(05b-agent-authoring)
# M6's artefact is a FIFTH shape: one C++ target and one driver, and it is the first sample that
# links a TOOL as well as the engine. samples/06-open-world/cy_sample_open-world reads its world out
# of an installation `cy_build` produced — by logical name, through the manifest in force — so the
# game cannot tell whether the bytes arrived in the original build or in a patch, which is the only
# way to show that a patch ships content rather than that a file was overwritten. openworld.py puts
# the five acts around it in order: cook, package and install; play, quit and save; resume; change
# one line of content and rebuild, watching which nodes hit the cache; ship the difference as a
# patch and play the patched world. Its CMakeLists.txt argues the shape, and declares nothing at all
# where the build tools are switched off.
add_subdirectory(06-open-world)
# M7's artefact is a SIXTH shape: one C++ target and one driver, like M6's, but what the two
# processes divide is not content and game — it is the frame and the report. cy_sample_fidelity
# generates a film-detail interior and exterior, cooks every shell through virtual geometry's real
# builder with the crack-free check the cook is required to run, puts the result on the device
# through the cluster traversal and the visibility buffer, lights it with the illumination system,
# and drives the budget arbiter under a scripted load spike with the frame time it just measured.
# fidelity.py checks every claim the program printed and reports through harness/artefact.py, which
# is what makes a recorded gap a non-zero exit. Its CMakeLists.txt argues the shape and declares
# nothing where python3 is absent.
add_subdirectory(07-fidelity)
