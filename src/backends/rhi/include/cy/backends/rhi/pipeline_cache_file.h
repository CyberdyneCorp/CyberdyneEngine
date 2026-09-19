// SPDX-License-Identifier: MIT
#pragma once
// The two file operations `save_pipeline_cache` and `load_pipeline_cache` are made of — Metal
// gap 6. Written once, in the interface module, and called by every backend.
//
// The pipeline cache takes a PATH rather than a blob because `MTLBinaryArchive` serialises to a URL
// and has no byte form to hand over. That decision moves a file read and a file write into every
// backend, and "what does an absent file mean" is a contract question rather than a backend one:
// it means A COLD START, and it is answered here so that no backend can answer it differently.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>

namespace cy::rhi {

/// Write `bytes` to `path`, truncating what was there. An empty span writes an empty file, which is
/// what a backend with nothing to persist produces and what loads back as a cold cache.
[[nodiscard]] Status write_pipeline_cache_file(const char* path, Span<const u8> bytes) noexcept;

/// Read `path` into `out`. Returns **false when the file does not exist** — a cold start, not an
/// error — and true when it was read, `out` empty for an empty file.
[[nodiscard]] Expected<bool, Error> read_pipeline_cache_file(const char* path,
                                                             Array<u8>& out) noexcept;

}  // namespace cy::rhi
