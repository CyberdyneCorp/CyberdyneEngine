// VMA's implementation, compiled exactly once. Task 2.3.1.
//
// The Vulkan Memory Allocator is a single header with its implementation behind a macro. This file
// exists to define that macro in one translation unit; everything else includes the declarations.
//
// STATIC LOADING IS OFF AND DYNAMIC LOADING IS ON, both set on the target so that every translation
// unit in this module agrees. VMA is handed vkGetInstanceProcAddr and vkGetDeviceProcAddr — the two
// volk itself resolved — and fetches the rest for itself. The alternative, letting VMA link the
// loader's symbols directly, would defeat the reason volk is here: with VK_NO_PROTOTYPES the engine
// links no Vulkan library at all and still builds on a machine with no driver.
//
// The engine's warning set is deliberately not applied to somebody else's header. This is one of
// two files in the tree that suppresses warnings, and it does it around the include rather than for
// the target, so the engine's own code in this module is still compiled under -Werror.

#include "vulkan_common.h"

#define VMA_IMPLEMENTATION

#if defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wall"
#    pragma GCC diagnostic ignored "-Wextra"
#    pragma GCC diagnostic ignored "-Wunused-parameter"
#    pragma GCC diagnostic ignored "-Wunused-variable"
#    pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#    pragma GCC diagnostic ignored "-Wparentheses"
#    pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif

#include <vk_mem_alloc.h>

#if defined(__clang__)
#    pragma clang diagnostic pop
#elif defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif
