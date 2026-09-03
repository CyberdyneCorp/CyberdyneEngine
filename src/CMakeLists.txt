# src/ — the engine layer stack. Subdirectories are added in layer order, low to high, so that a
# target's dependencies are always already declared when it is.
#
# Only the directories that contain targets are added. ecs/, servers/, backends/, scene/ and abi/
# arrive at M2 and M4; adding an empty directory now would only hide which layers actually exist.
add_subdirectory(core)
add_subdirectory(runtime)
