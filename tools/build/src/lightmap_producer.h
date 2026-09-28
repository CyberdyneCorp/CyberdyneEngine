// SPDX-License-Identifier: MIT
#ifndef CY_BUILD_SRC_LIGHTMAP_PRODUCER_H
#define CY_BUILD_SRC_LIGHTMAP_PRODUCER_H
// The `lightmap` producer. Private to tools/build/src/; `add_content_producers` registers it.

#include <cy/build/producer.h>

namespace cy::build {

/// A `cylightmap 1` description and its upstream mesh bundles in, one cooked lightmap
/// (`lightmap_bake::encode_lightmap_asset`) out.
[[nodiscard]] Status produce_lightmap(NodeContext& context);

}  // namespace cy::build

#endif  // CY_BUILD_SRC_LIGHTMAP_PRODUCER_H
