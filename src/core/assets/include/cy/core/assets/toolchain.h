#ifndef CY_CORE_ASSETS_TOOLCHAIN_H
#define CY_CORE_ASSETS_TOOLCHAIN_H
// The toolchain fingerprint — what produced an artefact, contributed to every derivation key by
// construction. M6 task 7.1; moved here from `tools/build/` by M7 task 1.1.
//
// --- THE DEFECT THIS FILE CLOSES, AND WHY IT SURVIVED M6 -----------------------------------------
//
// M6's spike built `cy_import_cli` twice from one source at `-O2` and at `-O0`, ran each over the
// same glTF, and got the identical derivation key `04a6fff1…`. Pointed at the other build's cache,
// the `-O0` binary reported a hit and was served the `-O2` binary's artefact. The payloads happened
// to agree for that asset; the key could not tell, and would have served it either way.
//
// M6 built the remedy — this fingerprint, and a `derivation_key` that refuses a key without it —
// and then put it in `tools/build/`, at layer 7. `cy::import::import_derivation_key` is at layer 7
// too but in a different module, and `cy::shader::derive_cache_key` is at the backends layer and
// may not link a tool at all. So the one function that actually cooks content could not reach the
// fix, and M6's closing gate re-measured the same defect on two importer binaries sharing one
// cache: **1 hit, 0 miss.**
//
// The fingerprint therefore lives beside the derivation key it contributes to, at layer 0, where
// every producer in the tree can reach it. `asset-import-pipeline` and `build-and-packaging` both
// require ONE cache over all derived data; one cache needs one key, and one key needs its inputs to
// be reachable from everything that computes one.
//
// --- WHY IT IS GENERATED RATHER THAN WRITTEN -----------------------------------------------------
//
// A hand-maintained list of library versions is a list that goes stale the first time somebody adds
// a dependency in a hurry — which is the same failure mode as the hand-maintained `u32` importer
// version the spike found. So `src/core/assets/CMakeLists.txt` reads `deps/manifest.toml` and
// `deps/host-tools.toml` at configure time and generates `cy/core/assets/toolchain_generated.h`
// from them. Adding a dependency without adding it to the key is then impossible rather than
// discouraged, and the attribution gate and this key model become one mechanism seen from two ends:
// both fail on a dependency that is not in the manifest.
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
// Not in: the build directory, the host name, the user, the wall clock, the worker count. M6's
// design §1.6 measures what putting one of them in costs — five identical builds, zero hits, forty
// cache entries for eight nodes' worth of content.
//
// --- WHY THE FIELDS ARE VIEWS --------------------------------------------------------------------
//
// Every one of the five is a compile-time string literal: four come from the generated header and
// the fifth from the standard library's own macro. A layer-0 header does not own a heap string for
// something that is always a literal, and a caller keying artefacts for a DIFFERENT toolchain — a
// cross-compile, a remote worker — supplies views over storage it owns, which is the same shape it
// would have needed anyway.

#include <cy/core/assets/derivation.h>
#include <cy/core/assets/hash.h>

#include <string_view>

namespace cy::assets {

/// Everything about the machinery that produced an artefact which can change the artefact's bytes.
///
/// Copyable and cheap to hold: a build service takes one at configure time and hands the same one
/// to every key it computes, which is what "by construction" means here — no producer assembles it,
/// and no producer can forget it.
struct ToolchainFingerprint {
    /// "GNU 13.3.0", "Clang 18.1.3". Compiler identity and version, from the configure that built
    /// this binary.
    std::string_view compiler;
    /// The flags this binary was compiled with that can change a producer's arithmetic: the
    /// optimisation level, the floating-point contraction mode, the architecture.
    std::string_view flags;
    /// The standard library and its version, read from `__GLIBCXX__` or `_LIBCPP_VERSION` at
    /// compile time. A guess derived from the compiler would be wrong the first time clang was
    /// pointed at libstdc++, which is the ordinary configuration on this machine.
    std::string_view standard_library;
    /// The target system and processor: "Linux x86_64".
    std::string_view target;
    /// Every third-party library the manifests declare, as `name=version` joined by ';', sorted by
    /// name, with `(system)` appended where a system copy was substituted for the pin. Sorted so
    /// that reordering the manifest does not change a key.
    std::string_view libraries;

    /// The fingerprint's own digest, for a report and for provenance. Not the key: the key frames
    /// each field separately, so that a diagnostic can say which field differs.
    [[nodiscard]] ContentHash digest() const noexcept;

    /// Add the fingerprint to a key under a fixed field order.
    ///
    /// Order is part of the contract — `DerivationKeyBuilder` hashes contributions in sequence — so
    /// this is the only function in the tree that contributes these five fields, and it contributes
    /// them in this order. Every producer calls THIS rather than adding fields of its own, which is
    /// what makes "the importer's key and the build graph's key agree about the toolchain" a
    /// property of the code instead of a convention.
    void contribute(DerivationKeyBuilder& builder) const noexcept;
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
/// generated header was replaced by hand. It is checked wherever a key is computed, because a
/// fingerprint that quietly lost its libraries is the exact shape of the defect this file exists to
/// prevent, and it must fail rather than key against nothing.
[[nodiscard]] bool toolchain_is_complete(const ToolchainFingerprint& fingerprint) noexcept;

}  // namespace cy::assets

#endif  // CY_CORE_ASSETS_TOOLCHAIN_H
