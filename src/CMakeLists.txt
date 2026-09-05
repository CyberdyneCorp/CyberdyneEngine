# src/ — the engine layer stack. Subdirectories are added in layer order, low to high, so that a
# target's dependencies are always already declared when it is.
#
# Only the directories that contain targets are added; adding an empty directory would only hide
# which layers actually exist. servers/ gained its first target at M3 and abi/ at M4, and both are
# added below.
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

# gameplay/ is layer 4 and arrives at M4 (section 4.4). It is read after scene/ and rendering/ and
# before runtime/, which is where it sits in the stack: above the ECS and the servers, below the
# loop that drives them. Its dependency list is short on purpose and the omission is the point —
# see src/gameplay/CMakeLists.txt, and design.md §3.
add_subdirectory(gameplay)

add_subdirectory(runtime)

# abi/ is layer 6 and arrives at M4 (section 2). It is read last because it is the top of the
# engine's stack: the C ABI may name the ECS and the core, and nothing in the engine may name it
# back. The comment at the top of this file said this directory would be added when it held a
# target — it does now.
add_subdirectory(abi)
