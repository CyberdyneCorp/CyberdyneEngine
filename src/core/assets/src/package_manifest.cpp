// The package manifest's text form, and its compatibility check. Task 3.3.3.
//
// Text, at the end of the file, so that `just` tooling and a human can read what a package declares
// without a parser for the binary half. The keys are the four things `core-assets-and-io` requires
// a manifest to declare: the build identity, the content compatibility versions, the install bundle
// membership, and the dependencies on other packages.

#include "package_manifest.h"

#include <cstdio>
#include <cstring>

namespace cy::assets {
namespace {

std::string_view trim(std::string_view text) noexcept {
    usize begin = 0;
    usize end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

Expected<std::string_view, Error> unquote(std::string_view value) noexcept {
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
        return fail(ErrorCode::InvalidArgument, "a manifest value must be a quoted string");
    }
    return value.substr(1, value.size() - 2);
}

Expected<u32, Error> parse_u32(std::string_view value) noexcept {
    if (value.empty() || value.size() > 10) {
        return fail(ErrorCode::InvalidArgument, "a manifest number is malformed");
    }
    u64 result = 0;
    for (const char c : value) {
        if (c < '0' || c > '9') {
            return fail(ErrorCode::InvalidArgument, "a manifest number is malformed");
        }
        result = (result * 10) + static_cast<u64>(c - '0');
    }
    if (result > 0xFFFF'FFFFULL) {
        return fail(ErrorCode::OutOfRange, "a manifest number does not fit in 32 bits");
    }
    return static_cast<u32>(result);
}

Status copy_bounded(char* destination, usize capacity, std::string_view text,
                    const char* what) noexcept {
    if (text.size() >= capacity) {
        return fail(ErrorCode::InvalidArgument, what);
    }
    std::memcpy(destination, text.data(), text.size());
    destination[text.size()] = '\0';
    return ok();
}

}  // namespace

Status PackageManifest::set_build_id(std::string_view text) noexcept {
    return copy_bounded(build_id, sizeof(build_id), text, "the build identity is too long");
}

Status PackageManifest::set_bundle(std::string_view text) noexcept {
    return copy_bounded(bundle, sizeof(bundle), text, "the bundle name is too long");
}

Status PackageManifest::add_dependency(std::string_view name) noexcept {
    if (dependency_count >= kMaxDependencies) {
        return fail(ErrorCode::OutOfRange,
                    "a package declares more dependencies than the format "
                    "holds");
    }
    if (Status copied = copy_bounded(dependencies[dependency_count], kNameCapacity + 1, name,
                                     "a package dependency name is too long");
        !copied) {
        return copied;
    }
    ++dependency_count;
    return ok();
}

Expected<usize, Error> write_package_manifest(const PackageManifest& manifest, char* out,
                                              usize capacity) noexcept {
    int written = std::snprintf(
        out, capacity,
        "# CyberdyneEngine package manifest.\n"
        "build_id = \"%s\"\nbundle = \"%s\"\ncontent_version = %u\nminimum_content_version = %u\n",
        manifest.build_id, manifest.bundle, manifest.compatibility.content_version,
        manifest.compatibility.minimum_content_version);
    if (written < 0) {
        return fail(ErrorCode::Internal, "the manifest could not be rendered");
    }
    auto total = static_cast<usize>(written);
    if (total >= capacity) {
        return fail(ErrorCode::BufferTooSmall, "the manifest buffer is too small");
    }
    for (u32 index = 0; index < manifest.dependency_count; ++index) {
        written = std::snprintf(out + total, capacity - total, "depends = \"%s\"\n",
                                manifest.dependencies[index]);
        if (written < 0) {
            return fail(ErrorCode::Internal, "the manifest could not be rendered");
        }
        const auto line = static_cast<usize>(written);
        if (line >= capacity - total) {
            return fail(ErrorCode::BufferTooSmall, "the manifest buffer is too small");
        }
        total += line;
    }
    return total;
}

Expected<PackageManifest, Error> parse_package_manifest(std::string_view text) noexcept {
    PackageManifest manifest;
    usize cursor = 0;
    while (cursor <= text.size()) {
        const usize newline = text.find('\n', cursor);
        const usize end = newline == std::string_view::npos ? text.size() : newline;
        const std::string_view line = trim(text.substr(cursor, end - cursor));
        cursor = end + 1;

        if (!line.empty() && line.front() != '#') {
            const usize equals = line.find('=');
            if (equals == std::string_view::npos) {
                return fail(ErrorCode::InvalidArgument, "a manifest line is not `key = value`");
            }
            const std::string_view key = trim(line.substr(0, equals));
            const std::string_view value = trim(line.substr(equals + 1));

            if (key == "build_id" || key == "bundle" || key == "depends") {
                Expected<std::string_view, Error> quoted = unquote(value);
                if (!quoted) {
                    return make_unexpected(quoted.error());
                }
                Status stored = ok();
                if (key == "build_id") {
                    stored = manifest.set_build_id(quoted.value());
                } else if (key == "bundle") {
                    stored = manifest.set_bundle(quoted.value());
                } else {
                    stored = manifest.add_dependency(quoted.value());
                }
                if (!stored) {
                    return make_unexpected(stored.error());
                }
            } else if (key == "content_version" || key == "minimum_content_version") {
                Expected<u32, Error> number = parse_u32(value);
                if (!number) {
                    return make_unexpected(number.error());
                }
                if (key == "content_version") {
                    manifest.compatibility.content_version = number.value();
                } else {
                    manifest.compatibility.minimum_content_version = number.value();
                }
            } else {
                return fail(ErrorCode::InvalidArgument,
                            "the package manifest carries a key this engine does not know");
            }
        }

        if (newline == std::string_view::npos) {
            break;
        }
    }
    return manifest;
}

Status check_compatibility(const ContentCompatibility& package,
                           const ContentCompatibility& runtime) noexcept {
    if (package.minimum_content_version > runtime.content_version) {
        return fail(ErrorCode::Unsupported,
                    "the package requires a newer content version than this runtime implements");
    }
    if (package.content_version < runtime.minimum_content_version) {
        return fail(ErrorCode::Unsupported,
                    "the package is older than the oldest content version this runtime reads");
    }
    return ok();
}

}  // namespace cy::assets
