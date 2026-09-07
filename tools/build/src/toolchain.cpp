#include <cy/build/toolchain.h>
#include <cy/build/toolchain_generated.h>

#include <cstdio>

namespace cy::build {
namespace {

/// The standard library's own version, from the macro the library defines.
///
/// Read here rather than derived from the compiler: clang against libstdc++ is the ordinary
/// configuration on a Linux developer machine, and a fingerprint that inferred libc++ from "Clang"
/// would key two different standard libraries identically.
[[nodiscard]] std::string standard_library_version() {
    char buffer[64] = {};
#if defined(_LIBCPP_VERSION)
    std::snprintf(buffer, sizeof(buffer), "libc++ %ld", static_cast<long>(_LIBCPP_VERSION));
#elif defined(__GLIBCXX__)
    // `long` rather than `int`: __GLIBCXX__ is a release date, and 20240904 fits an int only
    // because int is 32 bits here. A fingerprint that overflowed would key two libraries alike.
    std::snprintf(buffer, sizeof(buffer), "libstdc++ %ld", static_cast<long>(__GLIBCXX__));
#else
    std::snprintf(buffer, sizeof(buffer), "unknown");
#endif
    return {static_cast<const char*>(buffer)};
}

[[nodiscard]] ToolchainFingerprint build_current() {
    ToolchainFingerprint fingerprint;
    fingerprint.compiler = CY_BUILD_TOOLCHAIN_COMPILER;
    fingerprint.flags = CY_BUILD_TOOLCHAIN_FLAGS;
    fingerprint.standard_library = standard_library_version();
    fingerprint.target = CY_BUILD_TOOLCHAIN_TARGET;
    fingerprint.libraries = CY_BUILD_TOOLCHAIN_LIBRARIES;
    return fingerprint;
}

}  // namespace

void ToolchainFingerprint::contribute(assets::DerivationKeyBuilder& builder) const noexcept {
    builder.text("toolchain.compiler", compiler);
    builder.text("toolchain.flags", flags);
    builder.text("toolchain.stdlib", standard_library);
    builder.text("toolchain.target", target);
    builder.text("toolchain.libraries", libraries);
}

assets::ContentHash ToolchainFingerprint::digest() const noexcept {
    assets::ContentHasher hasher;
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

std::string ToolchainFingerprint::describe() const {
    std::string text;
    text += "compiler  ";
    text += compiler;
    text += "\nflags     ";
    text += flags;
    text += "\nstdlib    ";
    text += standard_library;
    text += "\ntarget    ";
    text += target;
    text += "\nlibraries ";
    text += libraries;
    text += "\n";
    return text;
}

const ToolchainFingerprint& current_toolchain() noexcept {
    static const ToolchainFingerprint fingerprint = build_current();
    return fingerprint;
}

bool toolchain_is_complete(const ToolchainFingerprint& fingerprint) noexcept {
    return !fingerprint.compiler.empty() && !fingerprint.target.empty() &&
           !fingerprint.libraries.empty();
}

}  // namespace cy::build
