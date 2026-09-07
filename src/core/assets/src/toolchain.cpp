#include <cy/core/assets/toolchain.h>
#include <cy/core/assets/toolchain_generated.h>

namespace cy::assets {
namespace {

[[nodiscard]] ToolchainFingerprint build_current() noexcept {
    ToolchainFingerprint fingerprint;
    fingerprint.compiler = CY_TOOLCHAIN_COMPILER;
    fingerprint.flags = CY_TOOLCHAIN_FLAGS;
    fingerprint.standard_library = CY_TOOLCHAIN_STDLIB;
    fingerprint.target = CY_TOOLCHAIN_TARGET;
    fingerprint.libraries = CY_TOOLCHAIN_LIBRARIES;
    return fingerprint;
}

}  // namespace

void ToolchainFingerprint::contribute(DerivationKeyBuilder& builder) const noexcept {
    builder.text("toolchain.compiler", compiler);
    builder.text("toolchain.flags", flags);
    builder.text("toolchain.stdlib", standard_library);
    builder.text("toolchain.target", target);
    builder.text("toolchain.libraries", libraries);
}

ContentHash ToolchainFingerprint::digest() const noexcept {
    ContentHasher hasher;
    const auto add = [&hasher](std::string_view field) noexcept {
        const u64 size = field.size();
        hasher.update(&size, sizeof(size));
        hasher.update(field.data(), field.size());
    };
    add(compiler);
    add(flags);
    add(standard_library);
    add(target);
    add(libraries);
    return hasher.finish();
}

const ToolchainFingerprint& current_toolchain() noexcept {
    static const ToolchainFingerprint fingerprint = build_current();
    return fingerprint;
}

bool toolchain_is_complete(const ToolchainFingerprint& fingerprint) noexcept {
    return !fingerprint.compiler.empty() && !fingerprint.target.empty() &&
           !fingerprint.libraries.empty();
}

}  // namespace cy::assets
