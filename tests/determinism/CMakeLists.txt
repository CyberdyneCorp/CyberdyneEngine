# tests/determinism/ — reproducibility of simulation and replication. M10 task 6.4.
#
# `testing-and-quality`'s taxonomy row: `tests/determinism/`, 10 s per case, "reproducibility of
# simulation and replication". The budget and the timeout are declared once in tests/CMakeLists.txt
# and read by cy_add_test(); nothing here restates them.
#
# WHY THIS DIRECTORY WAS EMPTY UNTIL M10, WRITTEN DOWN RATHER THAN TIDIED AWAY. The kind was
# promised "at M9" by tests/CMakeLists.txt's own error message and by this directory's README. M9
# built the simulation the suite is about and then declared its determinism cases as `unit` and
# `integration` — which is a defensible reading of a taxonomy whose other rows are graded by COST,
# and which left the one row graded by SUBJECT with nothing in it. `m9`'s closing gate said so, and
# section 6 of the M10 tasks is where it is repaired.
#
# WHAT IS HERE, AND WHAT IS DECLARED FROM ELSEWHERE.
#
#   determinism.golden_replay   THE NEW WORK. A recorded session with committed hashes, replayed.
#                               Nothing in the tree did this before: `src/replay/tests/test_bitexact.cpp`
#                               records and replays in one process, so a change in the simulation
#                               moves both sides of its comparison at once.
#   determinism.replay_fuzz     THE OTHER HALF OF THE NEW WORK. `read_log()` over a file that has
#                               been truncated at every length and corrupted at every byte.
#   determinism.save_fuzz       Declared from src/save/tests/, where its source lives with the module
#   determinism.save_kill_nine  it tests. Both were written at M6 as `integration` suites and are
#                               MOVED into this kind rather than copied here: `testing-and-quality`
#                               lists "replay and save fuzzing" and "transactional save tests" in
#                               this row, and a module's suites are declared from the module in this
#                               tree (src/replay/tests/, src/save/tests/, and twenty more).

# THE INCLUDE PATH REACHES INTO src/replay/tests/, AND THAT IS THE POINT RATHER THAN A SHORTCUT.
# A golden replay is worth exactly as much as the simulation it replays, and `sim.h` is the engine's
# own pieces wired as a game wires them — a real world, four participants, one command producer each,
# real state codecs, a real provider. A fixture invented in this directory would be a counter, and a
# committed hash of a counter proves that addition is stable. The suites declared here consume it
# header-only; nothing in src/replay/tests/ is compiled twice.
set(cy_replay_fixture_dir "${CMAKE_SOURCE_DIR}/src/replay/tests")
if(NOT EXISTS "${cy_replay_fixture_dir}/sim.h")
    message(FATAL_ERROR
        "tests/determinism: the replay fixture is not at ${cy_replay_fixture_dir}/sim.h. The golden "
        "suite drives the engine's own session rather than a fixture of its own — if that file "
        "moved, point this at its new home rather than growing a second simulation here.")
endif()

cy_add_test(
    NAME    golden_replay
    KIND    determinism
    SOURCES test_golden_replay.cpp
    DEPENDENCIES cy::replay
    # The committed artefact is addressed by an absolute path compiled in, never discovered relative
    # to the working directory: a test that guesses where its expectation lives passes vacuously when
    # it guesses wrong, which is the rule tests/integration/test_wrapper_seam.cpp states for itself.
    DEFINITIONS CY_DETERMINISM_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

cy_add_test(
    NAME    replay_fuzz
    KIND    determinism
    SOURCES test_replay_fuzz.cpp
    DEPENDENCIES cy::replay
    DEFINITIONS CY_DETERMINISM_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

target_include_directories(cy_test_determinism_golden_replay PRIVATE "${cy_replay_fixture_dir}")
target_include_directories(cy_test_determinism_replay_fuzz PRIVATE "${cy_replay_fixture_dir}")
