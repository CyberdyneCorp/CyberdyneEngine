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
# M8.a's artefact is a SEVENTH shape, and the split is the milestone's own claim: the editor has to
# do it, and somebody has to measure what happened. `authoring.py` drives a real editor and a real
# engine runtime over the editor's control socket — create a box and a sphere, place one above the
# other, add a body to each, press play, stop, undo back to empty — and `cy_sample_authoring` plays
# the SAME authored world headlessly and says how far the sphere fell, whether it came to rest where
# the two colliders say it should, and whether stopping put the file back byte for byte. The runtime
# counts sessions and ticks; it does not count metres, which is why there are two programs and not
# one. Its CMakeLists.txt argues the shape and declares nothing where python3, cargo or just is
# absent.
add_subdirectory(08a-authoring)
# M8.b's artefact is an EIGHTH shape, and it is the shape of the milestone: one program that is a
# GAME. samples/08-vertical-slice/cy_sample_vertical-slice builds a level, spawns characters that
# animate and think, gives them abilities with effects, puts a heads-up interface and a 2D menu over
# them, scores the sound, and assembles the frame out of the renderer's own parts — every one of
# those the engine's real module, in one tick, in one order. slice.py runs it in four acts, checks
# every claim it printed, and draws the picture from the frame's own draw list.
#
# It is also the line that answers M7's closing gate. Eight rendering modules were "linked by
# nothing but their own test binaries"; M8.b's section 11 gave them one assembly, and this directory
# is the first thing outside a test binary that links it. The same is true of cy::animation,
# cy::ai, cy::navigation, cy::ui, cy::audio-acoustics, cy::camera and cy::rendering-2d.
add_subdirectory(08-vertical-slice)
# M9's artefact is a NINTH shape, and it is the shape of the milestone: one program that is FIVE
# machines. samples/09-multiplayer/cy_sample_multiplayer runs four clients and a host on one seeded,
# lossy datagram substrate — the clients predict, the host substitutes for inputs that never arrived,
# and the clients roll back when they learn they were wrong — then reads the host's one command log
# three more ways: replayed bit-exactly into a fresh world, diverged on purpose and narrowed to a
# field, and flushed out of the crash replay buffer into an artefact that re-simulates to its own
# hash. multiplayer.py judges every claim and paints the two pictures from the program's own trace.
#
# It declares nothing at all with `-D CY_NETWORKING=OFF`, for the reason samples/08-vertical-slice
# declares nothing without CY_ANIMATION: an option that removes a subsystem must not turn into a
# configure error naming the artefact. Its CMakeLists.txt argues the shape.
add_subdirectory(09-multiplayer)
# M8.d's artefact is a TENTH shape, and it is the first thing in this repository ever to draw an
# IMPORTED mesh. samples/09b-animated-character/cy_sample_animated-character reads four Mixamo FBX
# exports through the importer's steps 7 and 8, retargets three of the four clips onto the rig that
# came with the mesh, compiles a four-state locomotion machine over them, evaluates a pose per fixed
# timestep, publishes it into the GPU pose world, and skins the character with the compute dispatch
# that reads it — then writes one PNG per frame. `just capture-animated-character` turns the
# sequence into the video and the still this milestone commits.
#
# It is also the line that answers M8.d's own gate. tools/import's steps 7 and 8, the retarget
# correspondence, the locomotion compiler and the skinning pass each landed reachable only from
# their own suites; this is the first target that names all four.
add_subdirectory(09b-animated-character)
