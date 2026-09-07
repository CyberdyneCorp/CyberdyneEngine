#ifndef CY_BUILD_TOOLCHAIN_H
#define CY_BUILD_TOOLCHAIN_H
// The toolchain fingerprint — what produced an artefact, contributed to every derivation key by
// construction. M6 task 7.1, design.md §1.3 and §1.9 item 2.
//
// --- THE DEFECT THIS FILE CLOSES -----------------------------------------------------------------
//
// M6's spike built `cy_import_cli` twice from one source at `-O2` and at `-O0`, ran each over the
// same glTF, and got the identical derivation key `04a6fff1…`. Pointed at the other build's cache,
// the `-O0` binary reported a hit and was served the `-O2` binary's artefact. The payloads happened
// to agree for that asset; the key could not tell, and would have served it either way.
//
// `import_derivation_key` contributes the producer's name, a hand-maintained version, the source
// hash, the variant, the profile and the options. No compiler, no flags, no library versions.
// `build-and-packaging` — "Derivation keys" — requires "the versions of the tools it invokes", and
// a build that does not treat its compiler as an input is lying about what it built.
//
// --- WHY IT IS GENERATED RATHER THAN WRITTEN -----------------------------------------------------
//
// A hand-maintained list of library versions is a list that goes stale the first time somebody adds
// a dependency in a hurry — which is the same failure mode as the hand-maintained `u32` importer
// version the spike found. So `tools/build/CMakeLists.txt` reads `deps/manifest.toml` and
// `deps/host-tools.toml` at configure time and generates `cy/build/toolchain_generated.h` from
// them. Adding a dependency without adding it to the key is then impossible rather than
// discouraged, and the attribution gate (task 0b) and this key model become one mechanism seen from
// two ends: both fail on a dependency that is not in the manifest.
//
// The generator refuses to emit an empty library list. A fingerprint that silently degraded to
// "no libraries" would be exactly the state the spike found, arriving through a different door.
//
// --- WHAT IS IN IT, AND WHAT IS NOT --------------------------------------------------------------
//
// In: the C++ compiler's identity and version; the flags that can move a float — the optimisation
// level, `-ffp-contract`, `-ffast-math`, the target architecture; the standard library's own
// version, read from the macro the library defines rather than guessed from the compiler; the
// target system and processor; and the name and version of every third-party library the manifests
// declare, plus whether a system copy was substituted for the pin (`CY_SYSTEM_ZSTD` selects between
// zstd 1.5.7 and 1.5.5 on the development machine, and they compress the same input to different
// bytes).
//
// Not in: the build directory, the host name, the user, the wall clock, the worker count. §1.6 of
// the design measures what putting one of them in costs — five identical builds, zero hits, forty
// cache entries for eight nodes' worth of content.

#include <cy/core/assets/derivation.h>
#include <cy/core/assets/hash.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <string>
#include <string_view>

namespace cy::build {

/// Everything about the machinery that produced an artefact which can change the artefact's bytes.
///
/// Copyable and cheap to hold: a build service takes one at configure time and hands the same one
/// to every key it computes, which is what "by construction" means here — no producer assembles it,
/// and no producer can forget it.
struct ToolchainFingerprint {
    /// "GNU 13.3.0", "Clang 18.1.3". Compiler identity and version, from the configure that built
    /// this binary.
    std::string compiler;
    /// The flags this binary was compiled with that can change a producer's arithmetic: the
    /// optimisation level, the floating-point contraction mode, the architecture.
    std::string flags;
    /// The standard library and its version, read from `__GLIBCXX__` or `_LIBCPP_VERSION` at
    /// compile time. A guess derived from the compiler would be wrong the first time clang was
    /// pointed at libstdc++, which is the ordinary configuration on this machine.
    std::string standard_library;
    /// The target system and processor: "Linux x86_64".
    std::string target;
    /// Every third-party library the manifests declare, as `name=version` joined by ';', sorted by
    /// name, with `(system)` appended where a system copy was substituted for the pin. Sorted so
    /// that reordering the manifest does not change a key.
    std::string libraries;

    /// The fingerprint's own digest, for a report and for provenance. Not the key: the key frames
    /// each field separately, so that a diagnostic can say which field differs.
    [[nodiscard]] assets::ContentHash digest() const noexcept;

    /// One line, for `cy_build toolchain` and for a build's provenance record.
    [[nodiscard]] std::string describe() const;

    /// Add the fingerprint to a key under a fixed field order.
    ///
    /// Order is part of the contract — `DerivationKeyBuilder` hashes contributions in sequence — so
    /// this is the only function in the tree that contributes these five fields, and it contributes
    /// them in this order.
    void contribute(assets::DerivationKeyBuilder& builder) const noexcept;
};

/// The fingerprint of the toolchain that compiled this binary.
///
/// Assembled once, from the generated header and from the standard library's own macros. A build
/// service that wanted to key artefacts for a *different* toolchain — a cross-compile, a remote
/// worker with its own compiler — constructs a `ToolchainFingerprint` by hand; nothing here
/// requires the current one, and `build-and-packaging`'s distributed execution is why.
[[nodiscard]] const ToolchainFingerprint& current_toolchain() noexcept;

/// Whether the generated manifest list is non-empty.
///
/// The configure-time generator refuses to emit an empty list, so this can only be false if the
/// generated header was replaced by hand. It is checked at every `BuildService::configure`, because
/// a fingerprint that quietly lost its libraries is the exact shape of the defect this file exists
/// to prevent, and it must fail the build rather than key against nothing.
[[nodiscard]] bool toolchain_is_complete(const ToolchainFingerprint& fingerprint) noexcept;

}  // namespace cy::build

#endif  // CY_BUILD_TOOLCHAIN_H
