# src/ — the engine layer stack. Subdirectories are added in layer order, low to high, so that a
# target's dependencies are always already declared when it is.
#
# Only the directories that contain targets are added. servers/, backends/ and abi/ arrive at M4;
# adding an empty directory now would only hide which layers actually exist.
add_subdirectory(core)

# core/serialize and core/determinism are layer 0 and belong in src/core/CMakeLists.txt's list
# beside the other core modules; they are registered here because that file was not this
# milestone's to edit. Moving the two lines there is the correct next edit to it, and nothing else
# has to change when they move.
add_subdirectory(core/serialize)
add_subdirectory(core/determinism)

# M2's own layers. ecs/ is layer 1 and scene/ is layer 4; scene/ reads its own serialization/
# subdirectory, so the four M2 modules are declared by these three lines.
add_subdirectory(ecs)
add_subdirectory(scene)

add_subdirectory(runtime)
