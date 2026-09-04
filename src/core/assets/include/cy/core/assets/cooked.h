#pragma once
// The header every cooked asset begins with. M2 task 3.2.13.
//
// `core-assets-and-io` — "Assets are cooked, not parsed at runtime":
//
//   "Each cooked asset SHALL begin with a header containing a magic number, format version, asset
//    kind, content hash, and the platform/feature variant key it was cooked for."
//
// M1 built the package format, the loader and the two serialization forms and left cooking to M2 —
// `PackageWriter::add` takes bytes that are already cooked, and until now nothing said what those
// bytes had to start with. This is that, and it is deliberately the smallest thing that answers the
// requirement: five fields, a fixed 64-byte layout, and no interpretation of the payload at all.
//
// WHY THE HEADER IS SEPARATE FROM THE PAYLOAD'S OWN FORMAT. A cooked scene's payload is a
// `cy::serialize` cooked stream, a cooked texture's is block-compressed pixels, and a cooked
// shader's is whatever the shader toolchain emits. What they have in common is not their contents;
// it is that the loader has to be able to reject the wrong one **before** handing it to a parser
// that would misread it. So the header is the loader's, the payload is the kind's, and the boundary
// between them is 64 bytes wide.
//
// THREE REJECTIONS, EACH WITH ITS OWN DIAGNOSTIC.
//
//   wrong magic       these bytes are not a cooked asset. A source file, a text scene, a truncated
//                     download — anything but what the loader was told to expect.
//   newer version     "loading SHALL fail with a clear diagnostic rather than misparsing". The
//                     version moves when the *header* changes; the payload's own evolution is the
//                     payload format's business.
//   wrong variant     the desktop package handed a mobile texture. Both are addressable by the same
//                     `AssetId` plus a variant key, so the key is the only thing that can tell them
//                     apart once the bytes are in hand.
//
// The content hash is the cooker's record of what it produced, and it is what an incremental build
// compares to decide whether to cook again. It is **not** the asset's identity: an `AssetId` is
// minted and survives every edit, and deriving one from content would change it on every save. The
// two are separate values with separate jobs, which is the same distinction `identity.h` argues for
// asset ids and handles.

#include <cy/core/assets/hash.h>
#include <cy/core/assets/identity.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

namespace cy::assets {

/// "CYCOOK" and two bytes that make a text file fail the check: a CR-LF pair survives a text-mode
/// transfer that would have mangled it, so a file that arrives corrupted fails here rather than
/// somewhere further in.
inline constexpr u8 kCookedMagic[8] = {'C', 'Y', 'C', 'O', 'O', 'K', 0x0D, 0x0A};

/// The header's own version. It moves when this layout changes and at no other time.
inline constexpr u32 kCookedFormatVersion = 1;

/// The header is a fixed 80 bytes, so a payload begins at a known offset and a reader can map the
/// rest without a second seek. Eighty rather than sixty-four because a `VariantKey` is a
/// twenty-four-byte inline string and hashing it to fit would make a collision serve the wrong
/// variant silently — which is the argument `identity.h` already makes for keeping it a string.
inline constexpr usize kCookedHeaderBytes = 80;

/// What a cooked asset declares about itself before any of it is parsed.
struct CookedAssetHeader {
    u32 format_version = kCookedFormatVersion;
    AssetKind kind = AssetKind::Unknown;
    /// The digest of the payload that follows. What an incremental cook compares.
    ContentHash content;
    VariantKey variant;
    /// Payload bytes after the header. Recorded so a reader can bound its read before it maps.
    u64 payload_size = 0;
};

/// Append a header for `payload` to `out`, then the payload itself.
///
/// The content hash is computed here rather than taken from the caller: a header whose hash the
/// cooker filled in by hand is a header that can disagree with its payload, and the disagreement is
/// undetectable by construction.
[[nodiscard]] Status write_cooked_asset(AssetKind kind, VariantKey variant, Span<const u8> payload,
                                        Array<u8>& out) noexcept;

/// Read the header at the front of `data`, leaving the payload where it is.
///
/// Fails with `InvalidArgument` on a wrong magic and with `Unsupported` on a version this build
/// does not know — never by parsing on and hoping.
[[nodiscard]] Expected<CookedAssetHeader, Error> read_cooked_header(const u8* data,
                                                                    usize size) noexcept;

/// The payload of a cooked asset, checked against its declared size and its recorded hash.
///
/// The hash check is what makes a truncated or corrupted payload a diagnostic rather than a parse
/// of rubbish. It costs a pass over the bytes, which is why it is a separate call from
/// `read_cooked_header`: a shipping load that trusts its package skips it, and a development load,
/// a patch verification and a cook-cache lookup do not.
[[nodiscard]] Expected<Span<const u8>, Error> read_cooked_payload(const u8* data, usize size,
                                                                  bool verify_hash) noexcept;

/// Reject a cooked asset that was produced for another platform or feature set.
///
/// Separate from reading the header because the caller is what knows which variant it wants, and
/// because a tool that inspects a package wants to read every variant rather than only its own.
[[nodiscard]] Status check_cooked_variant(const CookedAssetHeader& header,
                                          VariantKey wanted) noexcept;

}  // namespace cy::assets
