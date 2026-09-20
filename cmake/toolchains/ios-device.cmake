# SPDX-License-Identifier: MIT
# Cross-compile CyberdyneEngine libraries and applications for physical iOS devices.
# Signing stays application-owned; engine libraries do not require an identity.

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_SYSROOT iphoneos CACHE STRING "iOS device SDK" FORCE)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "iOS device architectures" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET 17.0 CACHE STRING "Minimum supported iOS version" FORCE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
