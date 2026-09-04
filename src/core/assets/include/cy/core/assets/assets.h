#ifndef CY_CORE_ASSETS_ASSETS_H
#define CY_CORE_ASSETS_ASSETS_H
// The asset layer, in one include. Section 3.3.
//
// `core-assets-and-io` reaches **Seed** at M1: identity, the virtual filesystem, the package format
// READ PATH, asynchronous loading, file and directory access, the two serialization forms, and
// compression through an engine-owned interface. Cooking is M2 and streaming under a residency
// policy is M6; both seams are named where they sit, in package.h and asset_system.h.
//
// A translation unit that needs one part includes that part. This header exists for the ones that
// need most of the layer — a tool, a test, the runtime's start-up — and for a reader looking for
// the map.

#include <cy/core/assets/asset_system.h>
#include <cy/core/assets/compression.h>
#include <cy/core/assets/diagnostics.h>
#include <cy/core/assets/file.h>
#include <cy/core/assets/hash.h>
#include <cy/core/assets/identity.h>
#include <cy/core/assets/package.h>
#include <cy/core/assets/path.h>
#include <cy/core/assets/serialization.h>
#include <cy/core/assets/vfs.h>

#endif  // CY_CORE_ASSETS_ASSETS_H
