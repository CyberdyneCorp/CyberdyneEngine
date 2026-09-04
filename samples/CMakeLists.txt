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
