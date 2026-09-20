# SPDX-License-Identifier: MIT
# Cross-compile CyberdyneEngine libraries and applications for the iOS simulator.

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_SYSROOT iphonesimulator CACHE STRING "iOS simulator SDK" FORCE)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "iOS simulator architectures" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET 17.0 CACHE STRING "Minimum supported iOS version" FORCE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
