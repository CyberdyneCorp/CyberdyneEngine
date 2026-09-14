# src/gameplay/live/ — layer 4: the live edit policy and the live edit compiler. M11.b task 3.2.
#
# ================================================================================================
# WHY A TARGET OF ITS OWN, BESIDE cy::gameplay-play RATHER THAN INSIDE IT
# ================================================================================================
#
# `cy_gameplay_play` is what a dedicated server links to spawn and run a session — it is one of the
# three targets `cy_require_headless` is applied to, and its dependency list is deliberately short.
# Live editing is the opposite kind of thing: it exists so that an EDITOR can reach a running world,
# and a shipped game has no use for it at all. Putting the compiler inside the play target would put
# an editor-facing feature into the link closure of every dedicated server.
#
# So the dependency goes one way and only one way: this target depends on `cy::gameplay-play`, and
# `play/CMakeLists.txt` is unchanged. A game that never live-edits never links this.
#
# WHAT DEPENDS ON WHAT:
#
#   cy::gameplay-play        layer 4 — the session a change is applied to, and the two operations
#                                      (`reinitialize_physics`, `recreate_entity`) only it can do
#   cy::scene                layer 4 — `LocalTransform`, whose offsets the transform bindings name
#   cy::scene-serialization  layer 4 — the `.cyworld` an authoring change is recorded into
#   cy::physics-bridge       layer 4 — the `RigidBody` and `Collider` columns the bridge writes
#   cy::core-reflect         below    — `PersistenceKind`, which every default policy derives from
#
# There is no renderer and no asset system here. A `ReloadAsset` policy is handed to a
# `LiveEditHost` the caller supplies, which is what keeps this module headless and what makes the
# absence of an asset rebinder a REFUSAL rather than a link error.
cy_add_module(
    NAME cy_gameplay_live
    LAYER scene
    ALIAS cy::gameplay-live
    TYPE STATIC
    SOURCES
        src/compiler.cpp
        src/policy.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::core-reflect cy::ecs cy::gameplay-play
                        cy::physics-bridge cy::scene cy::scene-serialization
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

# NO tests/ DIRECTORY HERE, deliberately. Both suites that exercise this module live in
# `src/gameplay/play/tests/` — `integration.editor_live_edit` drives a live edit against a play
# session, and a suite for it here would need the session, the physics server and the same fixture
# world. `play/tests/CMakeLists.txt` says why they are declared from there rather than from
# `tests/integration/`.
