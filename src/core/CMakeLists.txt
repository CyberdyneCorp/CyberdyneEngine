# src/core/ — layer 0.
#
# The order is a dependency order, low to high, so that a module's dependencies are always already
# declared when it is. base is the floor; memory, math, reflect and values are vocabulary above it;
# platform and diagnostics sit on those; jobs, assets and config sit on top.
#
# Seven of these arrived at M1 as stubs — one placeholder source, one placeholder header, and the
# right target name. A stub is preferable to an absent directory here: the module boundary is the
# thing being decided, and deciding it once, up front, is what stops seven agents from each
# inventing a different one.
add_subdirectory(base)
add_subdirectory(memory)
add_subdirectory(math)
add_subdirectory(reflect)
add_subdirectory(values)
add_subdirectory(platform)
add_subdirectory(diagnostics)
add_subdirectory(jobs)
add_subdirectory(assets)
add_subdirectory(config)
