#ifndef CY_CORE_ASSETS_SRC_PACKAGE_MANIFEST_H
#define CY_CORE_ASSETS_SRC_PACKAGE_MANIFEST_H
// The manifest's text form. PRIVATE — a package's manifest is reached through PackageReader, and
// the spelling of it on disk is this module's business.

#include <cy/core/assets/package.h>

#include <string_view>

namespace cy::assets {

[[nodiscard]] Expected<usize, Error> write_package_manifest(const PackageManifest& manifest,
                                                            char* out, usize capacity) noexcept;

[[nodiscard]] Expected<PackageManifest, Error> parse_package_manifest(
    std::string_view text) noexcept;

/// Refuse a package the runtime cannot read, with a message naming which way round it is.
[[nodiscard]] Status check_compatibility(const ContentCompatibility& package,
                                         const ContentCompatibility& runtime) noexcept;

}  // namespace cy::assets

#endif  // CY_CORE_ASSETS_SRC_PACKAGE_MANIFEST_H
