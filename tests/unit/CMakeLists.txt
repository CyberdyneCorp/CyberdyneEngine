# tests/unit/ — one directory per module under test, one binary per directory.
#
# The budget is a millisecond per test case: no I/O, no window, no sleeping, no thread that has to
# be waited on. A test that needs any of those belongs in tests/integration/, and the budget guard
# in the harness says so when one arrives here by accident.

add_subdirectory(harness)
