#pragma once
// CyberNet's umbrella header, and the ONE `#if defined(CY_NETWORKING)` in the tree. M9 task 4.1.
//
// ================================================================================================
// WHY A PREPROCESSOR GUARD AS WELL AS THE CMAKE ONE
// ================================================================================================
//
// `src/CMakeLists.txt` reads `src/networking/` only when `CY_NETWORKING` is on, which is what makes
// the option remove the module. That is the whole of what the delta requirement asks for, and it is
// enough for anything inside this tree: with the option off there is no include path to this
// header, so nothing can reach it.
//
// It is NOT enough for a consumer outside the tree. A downstream project that adds
// `src/networking/include` to its own include path — a plugin, a cooked tool, a build that was
// stitched together by hand — would reach these declarations in a build where the option is off,
// link nothing, and fail with a page of undefined symbols naming `cy::net::` functions that do not
// obviously say why. So the guard below turns that into one sentence at the point of the include.
//
// `<cy_features.h>` is the generated header `cmake/features.cmake` writes: an enabled feature is
// defined to 1, a disabled one is not defined at all. It is included FIRST and unconditionally,
// because a guard that tested a macro nobody had defined yet would pass in every build — which is
// the shape of check this project's gates have removed eighteen times.

#include <cy_features.h>

#if !defined(CY_NETWORKING)
#    error \
        "<cy/networking/networking.h> was included in a build where CY_NETWORKING is off. The networking system is excluded at build time rather than stubbed at runtime, so there is no library to link against here. Configure with -D CY_NETWORKING=ON, or remove the include: with the option off the engine is fully functional without it, and src/replay/'s record — which a replication layer reads — is deliberately NOT behind this option."
#endif

#include <cy/networking/authority.h>
#include <cy/networking/interest.h>
#include <cy/networking/local_transport.h>
#include <cy/networking/mode.h>
#include <cy/networking/prediction.h>
#include <cy/networking/profiler.h>
#include <cy/networking/reliability.h>
#include <cy/networking/replication.h>
#include <cy/networking/rpc.h>
#include <cy/networking/scheduler.h>
#include <cy/networking/schema.h>
#include <cy/networking/server.h>
#include <cy/networking/transport.h>
#include <cy/networking/udp_transport.h>

namespace cy::net {

/// True in a build that has this module. Not a runtime switch — there is no configuration in which
/// this is false, because in that configuration the header above does not compile. It exists so a
/// consumer can write a `static_assert` that says which build it needs.
inline constexpr bool kNetworkingBuilt = true;

}  // namespace cy::net
