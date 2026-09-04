# src/core/serialize/ — layer 0: the reflection-driven traversal, and the two serialization modes
# written from it.
#
# Tasks 3.2.2, 3.2.3, 3.2.4, 3.2.9. Governed by `serialization-and-prefabs` for the modes and by
# `core-type-system` for the identity every record addresses.
#
# WHAT BELONGS HERE. One traversal driven by cy::core-reflect, and two writers over it (design.md
# §6): the tagged form — field-tagged, skip-unknown, migratable, the authoring artefact under
# version control — and the cooked form — packed, no per-field tags, matching the runtime's
# in-memory layout. Field classification, schema versioning, value-record migration, unknown-field
# preservation, and the entity-reference encoding a cooked block carries in place of an id.
#
# WHAT DOES NOT BELONG HERE. Anything that knows what a prefab, a scene or a node is: that is
# src/scene/serialization/ at layer 4. This module serialises reflected values and knows nothing
# above layer 0, which is what lets a save, a replay and a cooked cell share one traversal.
#
# It is layer 0 rather than a member of src/core/ proper only in the file tree: it is registered
# from src/CMakeLists.txt because src/core/CMakeLists.txt belongs to another agent's ownership this
# milestone. The line belongs in src/core/CMakeLists.txt beside the other core modules, and moving
# it there is the correct next edit to that file.

# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
#
# THE DEPENDENCY LIST IS THE MODULE'S ARGUMENT IN ONE LINE. cy::core-reflect is where the traversal
# gets its field descriptors and its identifiers; cy::core-memory is where every container and every
# byte of storage comes from, because a serializer that grew its own buffer would be a second
# allocator outside the budget tree that `core-memory-and-containers` requires everything to be
# inside. Nothing above layer 0 appears, and nothing above layer 0 may: a save, a replay and a
# cooked cell all go through this module, and it must not know what any of them is.
cy_add_module(
    NAME cy_core_serialize
    LAYER core
    ALIAS cy::core-serialize
    TYPE STATIC
    SOURCES
        src/cooked.cpp
        src/migration.cpp
        src/tagged.cpp
        src/text.cpp
        src/traversal.cpp
        src/value_record.cpp
        src/wire.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::core-reflect)

# A suite lands with the first real source. The EXISTS guard is what lets this line be written now:
# it is the one line an agent adding tests/ does not then have to remember to add here.
if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
