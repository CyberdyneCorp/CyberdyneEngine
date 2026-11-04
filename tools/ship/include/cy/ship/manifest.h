// SPDX-License-Identifier: MIT
#ifndef CY_SHIP_MANIFEST_H
#define CY_SHIP_MANIFEST_H
// The shipping manifest: every file of a packaged game, and its cooked content split into chunks.
// Issue #87.
//
// ONE TEXT FILE, `ship.manifest`, signed by `ship.manifest.sig` (signature.h). Text, because a
// person checking a release should be able to read what it promises with `cat`, and because the
// signature covers the exact bytes — there is no canonical-encoding step to get wrong.
//
//     cyship 1
//     game rts
//     title CyberdyneEngine RTS
//     version 0.1.0
//     revision 28edcceb774a9829920764d53d063d23b69e8d79
//     target linux-x86_64
//     profile dev
//     signer 5c2d0a6e9b8f1c34
//     chunk-bytes 4194304
//     content-bytes 7421690
//     file 76309712 <blake3> bin/cyberdyne-rts
//     sealed MacOS/cyberdyne-rts
//     chunk 4194304 <blake3> content/chunk-0000.cychunk
//     entry 0 7207068 <blake3> frontier.cypak
//
// `file` is any file of the artefact, pinned by size and BLAKE3 digest. `chunk` is a slice of the
// CONTENT STREAM — every cooked content file concatenated in name order and cut every `chunk-bytes`
// — pinned the same way; `entry` names one content file by its offset and size in that stream and
// pins its own digest too. `sealed` is a file whose bytes the platform's code signature seals
// instead: the main executable of a macOS bundle is rewritten by `codesign` after the manifest is
// signed, so its digest cannot be in a manifest the bundle's seal also covers. A sealed file must
// be present; its integrity is Gatekeeper's to check.
//
// A PATH IS THE LAST FIELD, so it may hold spaces, and it is relative, `/`-separated, and never
// climbs: `..`, a leading `/`, a drive letter or a backslash is refused when the manifest is read,
// so a manifest cannot name a file outside the artefact it describes.

#include <cy/core/assets/hash.h>
#include <cy/core/base/types.h>

#include <string>
#include <string_view>
#include <vector>

namespace cy::ship {

inline constexpr u32 kManifestFormat = 1;
inline constexpr const char* kManifestName = "ship.manifest";
inline constexpr const char* kSignatureSuffix = ".sig";
/// Four mebibytes: small enough that a modified chunk names a narrow slice of the content, large
/// enough that a game's content is a handful of files rather than thousands.
inline constexpr u64 kDefaultChunkBytes = 4ULL * 1024ULL * 1024ULL;

enum class FileKind : u8 {
    File,
    Chunk,
    Sealed,
};

struct ManifestFile {
    FileKind kind = FileKind::File;
    std::string path;
    u64 bytes = 0;
    assets::ContentHash digest;
};

struct ContentEntry {
    std::string name;
    u64 offset = 0;
    u64 bytes = 0;
    assets::ContentHash digest;
};

struct Manifest {
    u32 format = kManifestFormat;
    std::string game;
    std::string title;
    std::string version;
    std::string revision;
    std::string target;
    std::string profile;
    std::string signer;
    u64 chunk_bytes = kDefaultChunkBytes;
    u64 content_bytes = 0;
    /// Every file, in the order the manifest lists them; chunks in stream order.
    std::vector<ManifestFile> files;
    std::vector<ContentEntry> content;

    [[nodiscard]] std::vector<const ManifestFile*> chunks() const;
    [[nodiscard]] const ContentEntry* find_content(std::string_view name) const noexcept;
};

/// The manifest's text. Deterministic: the same manifest writes the same bytes.
[[nodiscard]] std::string write_manifest(const Manifest& manifest);

/// Read a manifest, or say which line is wrong and why. Checks the structure the runtime relies on:
/// paths stay inside the artefact, the chunks tile the content stream, and every entry lies in it.
[[nodiscard]] bool parse_manifest(std::string_view text, Manifest& out, std::string& error);

/// True when `path` is relative, `/`-separated, non-empty in every component and never climbs.
[[nodiscard]] bool valid_relative_path(std::string_view path) noexcept;

/// `ship.manifest.sig`: `ed25519 <key id> <signature hex>`.
[[nodiscard]] std::string write_signature_file(std::string_view key_id, const u8* signature,
                                               usize size);
[[nodiscard]] bool parse_signature_file(std::string_view text, std::string& key_id, u8* signature,
                                        usize size, std::string& error);

}  // namespace cy::ship

#endif  // CY_SHIP_MANIFEST_H
