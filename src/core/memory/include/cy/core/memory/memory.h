#pragma once
// The memory module, in one include. Section 2, governed by `core-memory-and-containers`.
//
// Prefer the individual headers in code that only needs one of them: this one pulls in the whole
// module, and a component header that includes it acquires the containers, the budget tree and the
// trace's vocabulary along with whatever it wanted. It is here for a subsystem's own translation
// unit, and for a reader who wants the map.
//
// | What                              | Header                  | Task |
// |-----------------------------------|-------------------------|------|
// | The allocator interface           | `allocator.h`           | 2.1  |
// | The platform heap                 | `system_allocator.h`    | 2.1  |
// | Arena and LIFO stack              | `arena.h`               | 2.1  |
// | Fixed-size object pool            | `pool.h`                | 2.1  |
// | Per-worker slabs                  | `slab.h`                | 2.1  |
// | Fixed-size, address-stable chunks | `chunk_allocator.h`     | 2.1  |
// | The debug wrapper                 | `tracking_allocator.h`  | 2.1  |
// | The allocator scope               | `scope.h`               | 2.1  |
// | Memory domains and accounting     | `domain.h`              | 2.2  |
// | The budget tree                   | `budget.h`              | 2.2  |
// | Pressure levels and responses     | `pressure.h`            | 2.3  |
// | Sequence containers               | `array.h`, `ring_buffer.h`, `sparse_set.h`,
// `intrusive_list.h` | 2.4 | | Associative containers            | `hash_map.h`, `flat_map.h`,
// `hash.h` | 2.4 | | Handle pools                      | `handle_pool.h`         | 2.5  | | Chunked
// storage                   | `chunk_storage.h`       | 2.6  | | Frame and scratch memory |
// `frame_memory.h`        | 2.7  | | Retirement and frame epochs       | `epoch.h` | 2.8  | |
// Virtual address reservation       | `virtual_memory.h`      | 2.9  | | Ownership: UniquePtr, Ref
// | `ownership.h`           | 2.9  | | Process-lifetime declarations     | `lifetime.h` | 2.12 | |
// The diagnostics report            | `diagnostics.h`         | 2.11 |

#include <cy/core/memory/allocator.h>
#include <cy/core/memory/arena.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/budget.h>
#include <cy/core/memory/chunk_allocator.h>
#include <cy/core/memory/chunk_storage.h>
#include <cy/core/memory/diagnostics.h>
#include <cy/core/memory/domain.h>
#include <cy/core/memory/epoch.h>
#include <cy/core/memory/flat_map.h>
#include <cy/core/memory/frame_memory.h>
#include <cy/core/memory/handle_pool.h>
#include <cy/core/memory/hash.h>
#include <cy/core/memory/hash_map.h>
#include <cy/core/memory/intrusive_list.h>
#include <cy/core/memory/lifetime.h>
#include <cy/core/memory/ownership.h>
#include <cy/core/memory/pool.h>
#include <cy/core/memory/pressure.h>
#include <cy/core/memory/relocatable.h>
#include <cy/core/memory/ring_buffer.h>
#include <cy/core/memory/scope.h>
#include <cy/core/memory/slab.h>
#include <cy/core/memory/sparse_set.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/memory/tracking_allocator.h>
#include <cy/core/memory/virtual_memory.h>
