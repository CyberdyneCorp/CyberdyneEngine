// `plugin.h` — versions, constraints, trust tiers, and the manifest reader.

#include <cy/core/plugins/plugin.h>
#include <cy/core/serialize/text.h>

#include <algorithm>
#include <cstring>

namespace cy::plugins {
namespace {

using cy::serialize::TextLine;
using cy::serialize::TextScanner;

[[nodiscard]] Expected<u32, Error> parse_number(std::string_view text) noexcept {
    if (text.empty()) {
        return fail(ErrorCode::InvalidArgument, "a version component is empty");
    }
    u64 value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return fail(ErrorCode::InvalidArgument, "a version component is not a number");
        }
        value = (value * 10U) + static_cast<u64>(character - '0');
        if (value > 0xFFFF'FFFFULL) {
            return fail(ErrorCode::OutOfRange, "a version component does not fit in 32 bits");
        }
    }
    return static_cast<u32>(value);
}

/// Trim ASCII whitespace from both ends. A constraint written `[1.2, 2.0)` has a space in it.
[[nodiscard]] std::string_view trimmed(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace

Expected<Version, Error> parse_version(std::string_view text) noexcept {
    text = trimmed(text);
    if (text.empty()) {
        return fail(ErrorCode::InvalidArgument, "a version is empty");
    }
    Version version;
    u32* const fields[3] = {&version.major, &version.minor, &version.patch};
    usize field = 0;
    usize start = 0;
    for (usize position = 0; position <= text.size(); ++position) {
        if (position != text.size() && text[position] != '.') {
            continue;
        }
        if (field >= 3) {
            return fail(ErrorCode::InvalidArgument, "a version has more than three components");
        }
        const Expected<u32, Error> parsed = parse_number(text.substr(start, position - start));
        if (!parsed) {
            return make_unexpected(parsed.error());
        }
        *fields[field] = *parsed;
        field += 1;
        start = position + 1;
    }
    return version;
}

bool VersionConstraint::satisfied_by(const Version& candidate) const noexcept {
    switch (kind) {
        case ConstraintKind::Exact:
            return candidate == low;
        case ConstraintKind::MinimumCompatible:
            // The same major number, and not older. `^1.4.0` accepts 1.9.2 and refuses 2.0.0,
            // which is what "minimum compatible" means under semantic versioning and is the whole
            // reason the constraint exists rather than a bare minimum.
            return candidate.major == low.major && low <= candidate;
        case ConstraintKind::Range:
            return low <= candidate && candidate < high;
    }
    return false;
}

Expected<VersionConstraint, Error> parse_constraint(std::string_view text) noexcept {
    text = trimmed(text);
    if (text.empty()) {
        return fail(ErrorCode::InvalidArgument, "a version constraint is empty");
    }
    VersionConstraint constraint;
    if (text.front() == '=') {
        constraint.kind = ConstraintKind::Exact;
        const Expected<Version, Error> version = parse_version(text.substr(1));
        if (!version) {
            return make_unexpected(version.error());
        }
        constraint.low = *version;
        return constraint;
    }
    if (text.front() == '^') {
        constraint.kind = ConstraintKind::MinimumCompatible;
        const Expected<Version, Error> version = parse_version(text.substr(1));
        if (!version) {
            return make_unexpected(version.error());
        }
        constraint.low = *version;
        return constraint;
    }
    if (text.front() == '[') {
        if (text.back() != ')') {
            return fail(ErrorCode::InvalidArgument,
                        "a range constraint is written [low, high) with an exclusive upper bound");
        }
        const std::string_view body = text.substr(1, text.size() - 2);
        const usize comma = body.find(',');
        if (comma == std::string_view::npos) {
            return fail(ErrorCode::InvalidArgument, "a range constraint needs two versions");
        }
        const Expected<Version, Error> low = parse_version(body.substr(0, comma));
        if (!low) {
            return make_unexpected(low.error());
        }
        const Expected<Version, Error> high = parse_version(body.substr(comma + 1));
        if (!high) {
            return make_unexpected(high.error());
        }
        if (!(*low < *high)) {
            return fail(ErrorCode::InvalidArgument, "a range constraint's bounds are inverted");
        }
        constraint.kind = ConstraintKind::Range;
        constraint.low = *low;
        constraint.high = *high;
        return constraint;
    }
    // A bare version is an exact one. Spelled out rather than defaulted to `^`, because a manifest
    // that wrote `1.2.0` and meant "or later" is a manifest whose author should have written `^`.
    constraint.kind = ConstraintKind::Exact;
    const Expected<Version, Error> version = parse_version(text);
    if (!version) {
        return make_unexpected(version.error());
    }
    constraint.low = *version;
    return constraint;
}

const char* trust_tier_name(TrustTier tier) noexcept {
    switch (tier) {
        case TrustTier::DataOnly:
            return "DataOnly";
        case TrustTier::Scripted:
            return "Scripted";
        case TrustTier::TrustedNative:
            return "TrustedNative";
    }
    return "DataOnly";
}

Expected<TrustTier, Error> trust_tier_of(std::string_view word) noexcept {
    if (word == "DataOnly") {
        return TrustTier::DataOnly;
    }
    if (word == "Scripted") {
        return TrustTier::Scripted;
    }
    if (word == "TrustedNative") {
        return TrustTier::TrustedNative;
    }
    return fail(ErrorCode::InvalidArgument, "a trust tier is DataOnly, Scripted or TrustedNative");
}

TrustTier required_tier_for(PluginContents contents) noexcept {
    // NATIVE CODE IS THE LINE. Modules and platform binaries are native code in the process, which
    // has the process's privileges; the engine does not claim to sandbox it and therefore requires
    // the decision to be explicit.
    if (contains(contents, PluginContents::Modules) ||
        contains(contents, PluginContents::PlatformBinaries)) {
        return TrustTier::TrustedNative;
    }
    if (contains(contents, PluginContents::Scripts)) {
        return TrustTier::Scripted;
    }
    return TrustTier::DataOnly;
}

Expected<PluginContents, Error> plugin_contents_of(std::string_view word) noexcept {
    if (word == "modules") {
        return PluginContents::Modules;
    }
    if (word == "content") {
        return PluginContents::Content;
    }
    if (word == "editor-extensions") {
        return PluginContents::EditorExtensions;
    }
    if (word == "schemas") {
        return PluginContents::Schemas;
    }
    if (word == "importers") {
        return PluginContents::Importers;
    }
    if (word == "build-steps") {
        return PluginContents::BuildSteps;
    }
    if (word == "platform-binaries") {
        return PluginContents::PlatformBinaries;
    }
    if (word == "scripts") {
        return PluginContents::Scripts;
    }
    return fail(ErrorCode::InvalidArgument, "a plugin content kind this build does not know");
}

bool PluginManifest::supports_platform(std::string_view platform) const noexcept {
    if (platforms_.empty()) {
        return true;
    }
    return std::ranges::any_of(platforms_.span(), [platform](const Name& declared) {
        return declared.text() == platform;
    });
}

bool PluginManifest::supports_engine(const Version& engine) const noexcept {
    return engine_api_min <= engine && engine < engine_api_max;
}

// --- the manifest reader ----------------------------------------------------------------------

namespace {

[[nodiscard]] Status read_dependency(const TextLine& line, PluginManifest& out) noexcept {
    if (line.count() < 3) {
        return fail(ErrorCode::InvalidArgument, "a plugin dependency names an id and a constraint");
    }
    const Expected<VersionConstraint, Error> constraint = parse_constraint(line.word(2));
    if (!constraint) {
        return make_unexpected(constraint.error());
    }
    return out.dependencies().push_back(PluginDependency{Name::intern(line.word(1)), *constraint});
}

[[nodiscard]] Status read_extension(const TextLine& line, PluginManifest& out) noexcept {
    if (line.count() < 4) {
        return fail(ErrorCode::InvalidArgument,
                    "an extension names a point, an interface version and a contribution");
    }
    const Expected<u64, Error> version = line.word_u64(2);
    if (!version) {
        return make_unexpected(version.error());
    }
    ExtensionRegistration registration;
    registration.point = Name::intern(line.word(1));
    registration.interface_version = static_cast<u32>(*version);
    registration.contribution = Name::intern(line.word(3));
    return out.extensions().push_back(registration);
}

[[nodiscard]] Status read_engine_api(const TextLine& line, PluginManifest& out) noexcept {
    if (line.count() < 3) {
        return fail(ErrorCode::InvalidArgument,
                    "engine-api names an inclusive minimum and an exclusive maximum");
    }
    const Expected<Version, Error> low = parse_version(line.word(1));
    if (!low) {
        return make_unexpected(low.error());
    }
    const Expected<Version, Error> high = parse_version(line.word(2));
    if (!high) {
        return make_unexpected(high.error());
    }
    out.engine_api_min = *low;
    out.engine_api_max = *high;
    return ok();
}

/// One line of the manifest body. Split out so `read_plugin_manifest` stays a loop and a switch
/// rather than a function long enough to hide a missing case.
[[nodiscard]] Status read_manifest_line(const TextLine& line, PluginManifest& out) noexcept {
    const std::string_view keyword = line.word(0);
    if (keyword == "id") {
        out.id = Name::intern(line.word(1));
        return ok();
    }
    if (keyword == "name") {
        Array<char> unescaped(current_allocator());
        const Expected<std::string_view, Error> text = line.word_unquoted(1, unescaped);
        if (!text) {
            return make_unexpected(text.error());
        }
        out.display_name = Name::intern(*text);
        return ok();
    }
    if (keyword == "version") {
        const Expected<Version, Error> version = parse_version(line.word(1));
        if (!version) {
            return make_unexpected(version.error());
        }
        out.version = *version;
        return ok();
    }
    if (keyword == "engine-api") {
        return read_engine_api(line, out);
    }
    if (keyword == "tier") {
        const Expected<TrustTier, Error> tier = trust_tier_of(line.word(1));
        if (!tier) {
            return make_unexpected(tier.error());
        }
        out.tier = *tier;
        return ok();
    }
    if (keyword == "hot-reload") {
        out.hot_reload = line.word(1) == "true";
        return ok();
    }
    if (keyword == "contains") {
        for (usize index = 1; index < line.count(); ++index) {
            const Expected<PluginContents, Error> kind = plugin_contents_of(line.word(index));
            if (!kind) {
                return make_unexpected(kind.error());
            }
            out.contents = out.contents | *kind;
        }
        return ok();
    }
    if (keyword == "module") {
        return out.modules().push_back(Name::intern(line.word(1)));
    }
    if (keyword == "type") {
        return out.types().push_back(Name::intern(line.word(1)));
    }
    if (keyword == "platform") {
        return out.platforms().push_back(Name::intern(line.word(1)));
    }
    if (keyword == "depends") {
        return read_dependency(line, out);
    }
    if (keyword == "extends") {
        return read_extension(line, out);
    }
    return fail(ErrorCode::InvalidArgument,
                "a plugin manifest holds a line this build does not know");
}

}  // namespace

Status read_plugin_manifest(std::string_view text, PluginManifest& out) noexcept {
    TextScanner scanner(text);
    const Expected<TextLine, Error> head = scanner.next();
    if (!head) {
        return fail(ErrorCode::InvalidArgument, "a plugin manifest is empty");
    }
    if (head->word(0) != "cyplugin") {
        return fail(ErrorCode::InvalidArgument, "a plugin manifest does not begin with 'cyplugin'");
    }
    for (;;) {
        const Expected<TextLine, Error> line = scanner.next();
        if (!line) {
            if (line.error().code == ErrorCode::NotFound) {
                break;
            }
            return make_unexpected(line.error());
        }
        if (Status read = read_manifest_line(*line, out); !read) {
            return read;
        }
    }
    return validate_plugin_manifest(out);
}

Status validate_plugin_manifest(const PluginManifest& manifest) noexcept {
    if (manifest.id.is_empty()) {
        return fail(ErrorCode::InvalidArgument, "a plugin manifest declares no id");
    }
    if (!(manifest.engine_api_min < manifest.engine_api_max)) {
        return fail(ErrorCode::InvalidArgument,
                    "a plugin's engine API range is empty: the maximum is exclusive");
    }
    // THE TRUST CHECK IS AT THE MANIFEST AND NOT AT THE LOAD. A `DataOnly` plugin shipping a
    // platform binary has to be a refusal somebody sees while packaging it, because at load time
    // the only honest answer left is to refuse it and nobody knows why it was built that way.
    const TrustTier required = required_tier_for(manifest.contents);
    if (manifest.tier < required) {
        return fail(ErrorCode::PermissionDenied,
                    "a plugin declares a trust tier lower than its contents require");
    }
    return ok();
}

}  // namespace cy::plugins
