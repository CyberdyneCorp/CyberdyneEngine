// SPDX-License-Identifier: MIT
#ifndef CY_SHIP_INSTALLATION_H
#define CY_SHIP_INSTALLATION_H
// A packaged game on disk: verified against its signed manifest, and its content made readable.
// Issue #87.
//
// THE ORDER IS THE POINT. The signature is checked first and alone: a manifest whose signature does
// not hold is not read for anything, because its file list is exactly what an attacker would have
// rewritten. Then every file it lists is checked — present, the size it says, the BLAKE3 digest it
// says — and every file on disk it does NOT list is reported too, so a library dropped beside the
// executable is refused rather than loaded. Every problem is collected and named; one refusal that
// said only "corrupt" would send a player to reinstall a 7 GB game over one missing file.
//
// THEN THE CONTENT IS MATERIALISED. The games read their cooked packages by path, so the verified
// chunks are cut back into files under the user's cache directory, keyed by the manifest's own
// digest: the first launch writes them, and every later launch re-verifies what is there and
// rewrites only what does not match. A cached file is never trusted because it exists.

#include <cy/ship/manifest.h>
#include <cy/ship/signature.h>

#include <filesystem>
#include <string>
#include <vector>

namespace cy::ship {

enum class ProblemKind : u8 {
    /// The manifest or its signature could not be read, or is malformed.
    Unreadable,
    /// The signature is not a valid one by the trusted key over these manifest bytes.
    BadSignature,
    /// The manifest is signed by a key this build does not trust.
    UntrustedKey,
    /// A listed file is absent.
    Missing,
    /// A listed file's size or digest is not the manifest's.
    Modified,
    /// A file is present that the manifest does not list.
    Unlisted,
};

struct Problem {
    ProblemKind kind = ProblemKind::Unreadable;
    /// The file, relative to the installation root, or the manifest's own name.
    std::string path;
    std::string detail;

    /// One sentence naming the file and what is wrong with it.
    [[nodiscard]] std::string describe() const;
};

struct Verification {
    Manifest manifest;
    std::vector<Problem> problems;
    std::string manifest_digest;
    u64 files = 0;
    u64 chunks = 0;
    u64 bytes_hashed = 0;
    f64 milliseconds = 0.0;

    [[nodiscard]] bool ok() const noexcept { return problems.empty(); }
};

/// Where a packaged executable's installation is: the directory above the executable's own, with
/// `ship.manifest` in it (Linux, Windows) or in its `Resources/` (a macOS bundle's `Contents/`).
struct Location {
    std::filesystem::path root;
    /// The manifest's path relative to `root`, `/`-separated.
    std::string manifest;
};

/// False when no manifest is beside `executable`: the program is running out of a build tree.
[[nodiscard]] bool locate_installation(const std::filesystem::path& executable, Location& out);

/// Check an installation against its signed manifest. Never stops at the first problem.
[[nodiscard]] Verification verify_installation(const Location& location, const PublicKey& trusted);

/// Cut the verified content stream back into files under `cache`, re-verifying any already there.
/// `out` is the directory the content entries are files in.
[[nodiscard]] bool materialise_content(const Location& location, const Manifest& manifest,
                                       const std::filesystem::path& cache,
                                       std::filesystem::path& out, std::string& error);

/// Read one content entry straight out of the chunks, verified against its own digest.
[[nodiscard]] bool read_content(const Location& location, const Manifest& manifest,
                                std::string_view name, std::string& bytes, std::string& error);

/// A whole file's bytes, or false when it cannot be read.
[[nodiscard]] bool read_file(const std::filesystem::path& path, std::string& out);

/// The BLAKE3 digest and size of a file, or false when it cannot be read.
[[nodiscard]] bool digest_file(const std::filesystem::path& path, assets::ContentHash& digest,
                               u64& bytes);

/// The files the unlisted-file check ignores: the manifest and its signature, a macOS bundle's
/// `_CodeSignature/` (written by codesign after the manifest is signed) and Finder's `.DS_Store`.
[[nodiscard]] bool exempt_from_listing(std::string_view relative,
                                       std::string_view manifest) noexcept;

}  // namespace cy::ship

#endif  // CY_SHIP_INSTALLATION_H
