// The toolchain fingerprint, and the thing M6 shipped that could not be reached. M7 task 1.1.
//
// The defect these cases guard against is not hypothetical and is not a design preference: M6's
// spike built one importer at `-O2` and at `-O0`, ran both over the same glTF, and got the
// identical derivation key `04a6fff1…`; M6's closing gate re-measured it on two importer binaries
// sharing one cache and recorded **1 hit, 0 miss**. The remedy existed at M6 — this fingerprint —
// but it lived at layer 7 inside `tools/build/`, where the importer and the shader cache could not
// link it.
//
// So there are two properties here, and the second is the one that was actually missing:
//
//   1. A fingerprint that differs in any field produces a different key.
//   2. The fingerprint of THIS binary is reachable from layer 0 and is complete, so every producer
//      in the tree can contribute it without linking a tool.
//
// Unit: nothing here opens a file.

#include <cy/core/assets/derivation.h>
#include <cy/core/assets/toolchain.h>
#include <cy/test/test.h>

using namespace cy::assets;

namespace {

ToolchainFingerprint reference_fingerprint() {
    ToolchainFingerprint fingerprint;
    fingerprint.compiler = "GNU 13.3.0";
    fingerprint.flags = "c++20 -O2 -g";
    fingerprint.standard_library = "libstdc++ 20240904";
    fingerprint.target = "Linux x86_64";
    fingerprint.libraries = "blake3=1.8.7;zstd=1.5.7";
    return fingerprint;
}

DerivationKey key_with(const ToolchainFingerprint& fingerprint) {
    DerivationKeyBuilder builder;
    builder.producer(DerivedKind::Import, "gltf", 1);
    fingerprint.contribute(builder);
    builder.source("source", content_hash("mesh", 4));
    auto key = builder.finish();
    CY_REQUIRE(key.has_value());
    return key.value();
}

}  // namespace

CY_TEST_CASE("ToolchainFingerprint: the same toolchain contributes the same key") {
    CY_CHECK(key_with(reference_fingerprint()) == key_with(reference_fingerprint()));
}

CY_TEST_CASE("ToolchainFingerprint: the optimisation level changes the key") {
    // The spike's own measurement, as an assertion. Two builds of one producer at -O2 and at -O0
    // differ only in `flags`, and a key that could not see it served either binary's artefact to
    // the other.
    auto changed = reference_fingerprint();
    changed.flags = "c++20 -O0 -g";
    CY_CHECK(key_with(changed) != key_with(reference_fingerprint()));
}

CY_TEST_CASE("ToolchainFingerprint: every field is load-bearing") {
    const DerivationKey reference = key_with(reference_fingerprint());

    auto changed = reference_fingerprint();
    changed.compiler = "Clang 18.1.3";
    CY_CHECK(key_with(changed) != reference);

    changed = reference_fingerprint();
    changed.standard_library = "libc++ 190100";
    CY_CHECK(key_with(changed) != reference);

    changed = reference_fingerprint();
    changed.target = "Darwin arm64";
    CY_CHECK(key_with(changed) != reference);

    // zstd 1.5.7 (the pin) and zstd 1.5.5 (a system copy) compress the same input to different
    // bytes, so which one was linked has to reach the key.
    changed = reference_fingerprint();
    changed.libraries = "blake3=1.8.7;zstd=1.5.5(system)";
    CY_CHECK(key_with(changed) != reference);
}

CY_TEST_CASE("ToolchainFingerprint: an empty field set is refused rather than keyed against") {
    ToolchainFingerprint empty;
    CY_CHECK(!toolchain_is_complete(empty));

    // A fingerprint that silently degraded to "no libraries" is the state M6's spike found on the
    // real importer arriving through a different door.
    auto without_libraries = reference_fingerprint();
    without_libraries.libraries = "";
    CY_CHECK(!toolchain_is_complete(without_libraries));
}

CY_TEST_CASE("ToolchainFingerprint: this binary's own fingerprint is complete at layer 0") {
    // The regression M7 task 1.1 exists to close. `current_toolchain()` used to live in
    // `tools/build/`, so `cy::import` and `cy::shader` — the two producers that cook real content —
    // could not contribute it. That it is reachable from a layer-0 test IS the fix.
    const ToolchainFingerprint& current = current_toolchain();
    CY_CHECK(toolchain_is_complete(current));
    CY_CHECK(!current.compiler.empty());
    CY_CHECK(!current.flags.empty());
    CY_CHECK(!current.standard_library.empty());
    CY_CHECK(!current.target.empty());
    // Generated from deps/manifest.toml and deps/host-tools.toml, and the generator refuses to emit
    // an empty list, so this can only fail if the generated header was replaced by hand.
    CY_CHECK(current.libraries.find("blake3=") != std::string_view::npos);
    CY_CHECK(current.libraries.find("zstd=") != std::string_view::npos);
}

CY_TEST_CASE("ToolchainFingerprint: the digest moves with the fields") {
    auto changed = reference_fingerprint();
    changed.flags = "c++20 -O0 -g";
    CY_CHECK(reference_fingerprint().digest() != changed.digest());
    CY_CHECK(reference_fingerprint().digest() == reference_fingerprint().digest());
}
