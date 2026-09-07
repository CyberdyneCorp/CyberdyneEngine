#ifndef CY_BUILD_TOOLCHAIN_H
#define CY_BUILD_TOOLCHAIN_H
// The tools-layer face of the toolchain fingerprint. M6 task 7.1; relocated by M7 task 1.1.
//
// --- WHY THIS FILE IS NOW FOUR LINES OF `using` --------------------------------------------------
//
// M6 built the fingerprint here, at layer 7, and `tools/build/`'s `derivation_key` has refused a
// key without one ever since. But `cy::import::import_derivation_key` lives in a DIFFERENT layer-7
// module and `cy::shader::derive_cache_key` lives at the backends layer, and neither may link a
// tool — so the one function that actually cooks content could not reach the remedy, and M6's
// closing gate re-measured the original defect on two importer binaries sharing one cache: 1 hit,
// 0 miss.
//
// The type therefore moved to `cy/core/assets/toolchain.h`, at layer 0, beside the derivation key
// it contributes to. `asset-import-pipeline` and `build-and-packaging` both require ONE cache over
// all derived data; one cache needs one key, and one key needs its inputs reachable from every
// producer that computes one. This header keeps the `cy::build` spellings so that nothing in the
// build graph had to learn a new name for a type that did not change.

#include <cy/core/assets/toolchain.h>

#include <string>

namespace cy::build {

using ToolchainFingerprint = assets::ToolchainFingerprint;

using assets::current_toolchain;
using assets::toolchain_is_complete;

/// One line per field, for `cy_build toolchain` and for a build's provenance record.
///
/// A free function at the tools layer rather than a method on the type: the fingerprint is five
/// string views over compile-time literals, and a layer-0 struct that returned a heap string to
/// print itself would be the only thing in `cy::assets` that did.
[[nodiscard]] inline std::string describe_toolchain(const ToolchainFingerprint& fingerprint) {
    const auto line = [](const char* label, std::string_view value) {
        std::string text = label;
        text.append(value);
        text.push_back('\n');
        return text;
    };
    std::string text;
    text += line("compiler  ", fingerprint.compiler);
    text += line("flags     ", fingerprint.flags);
    text += line("stdlib    ", fingerprint.standard_library);
    text += line("target    ", fingerprint.target);
    text += line("libraries ", fingerprint.libraries);
    return text;
}

}  // namespace cy::build

#endif  // CY_BUILD_TOOLCHAIN_H
