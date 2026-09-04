# src/ — the engine layer stack. Subdirectories are added in layer order, low to high, so that a
# target's dependencies are always already declared when it is.
#
# Only the directories that contain targets are added. servers/ and abi/ are still empty and are
# added when they hold a target; adding an empty directory now would only hide which layers actually
# exist.
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

# M3's own layers. backends/ is layer 3 and opens with the RHI and its null backend; rendering/ is
# layer 4 and holds the render graph that computes barriers over that interface. backends/ is read
# before scene/ and rendering/ because both of those are above it, and a target's dependencies are
# always already declared when it is.
add_subdirectory(backends)

# servers/ is layer 2 and opens with the render server (M3 section 4.1). It is read before
# backends/ would need it — nothing at layer 2 may depend on layer 3 — and before rendering/, which
# consumes it.
add_subdirectory(servers)

add_subdirectory(scene)
add_subdirectory(rendering)

add_subdirectory(runtime)
