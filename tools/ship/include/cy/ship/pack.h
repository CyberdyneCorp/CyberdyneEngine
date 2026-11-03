// SPDX-License-Identifier: MIT
#ifndef CY_SHIP_PACK_H
#define CY_SHIP_PACK_H
// Writing a packaged game's chunks, manifest and signature over a staged directory. Issue #87.
//
// The staging is tools/ship/package.py's: it lays the executable, its libraries and its documents
// out where the target expects them, and gathers the cooked content into a directory of its own.
// `pack` then does the part that has to be exact — cuts the content into chunks under the root,
// lists EVERY file the root holds by size and digest, writes the manifest and signs it — and it is
// C++ so that the code that writes a manifest is the code that reads one at startup.

#include <cy/ship/manifest.h>
#include <cy/ship/signature.h>

#include <filesystem>
#include <string>
#include <vector>

namespace cy::ship {

struct PackOptions {
    /// The staged installation root.
    std::filesystem::path root;
    /// The cooked content, every file of it, by its path relative to this directory.
    std::filesystem::path content;
    /// Where the manifest goes, relative to `root`.
    std::string manifest = kManifestName;
    /// Where the chunks go, relative to `root`: `content`, or a macOS bundle's `Resources/content`,
    /// because codesign seals what is under `Resources/` and refuses loose files elsewhere.
    std::string chunk_directory = "content";
    /// game, title, version, revision, target and profile; the rest is computed.
    Manifest header;
    u64 chunk_bytes = kDefaultChunkBytes;
    /// Files listed as sealed by the platform's code signature rather than by digest.
    std::vector<std::string> sealed;
    SecretKey key;
};

[[nodiscard]] bool pack(const PackOptions& options, Manifest& out, std::string& error);

}  // namespace cy::ship

#endif  // CY_SHIP_PACK_H
