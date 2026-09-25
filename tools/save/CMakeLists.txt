# tools/save/ — layer 7: the save inspector's front end, and the forbidden-pattern checker.
#
# `save-and-persistence` — "Save diagnostics and inspection" and "Forbidden save patterns". The
# inspector itself is engine code, `cy/save/inspect.h`, because a game's own debug overlay and a
# crash report want the same answers this tool prints; what is here is its command line over a save
# directory on disk. `check_forbidden.py` and `selftest.py` beside it are scripts, run by
# `just diagnose-save-patterns` and by M11.a's `save-forbidden-patterns-checked` criterion.
#
# LAYER tools, which may link any layer: a tool consumes the engine rather than sitting in it.
cy_add_module(
    NAME cy_save_inspect
    LAYER tools
    TYPE EXECUTABLE
    SOURCES inspect/main.cpp
    PRIVATE_DEPENDENCIES cy::save cy::core-reflect)
