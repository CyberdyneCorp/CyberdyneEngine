#include <cy/import/importer.h>

#include <algorithm>
#include <cstring>

namespace cy::import {
namespace {

/// Copy at most `capacity - 1` characters and terminate. Truncation is deliberate and silent for a
/// diagnostic's text: a message that would not fit is still worth most of, and refusing to report a
/// problem because its sentence was long would be the wrong failure.
void copy_truncated(char* out, usize capacity, std::string_view text) noexcept {
    const usize length = text.size() < capacity - 1 ? text.size() : capacity - 1;
    if (length != 0) {
        std::memcpy(out, text.data(), length);
    }
    out[length] = '\0';
}

[[nodiscard]] char lowered(char character) noexcept {
    return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                : character;
}

[[nodiscard]] bool equal_ignoring_case(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize index = 0; index < a.size(); ++index) {
        if (lowered(a[index]) != lowered(b[index])) {
            return false;
        }
    }
    return true;
}

}  // namespace

const char* cook_profile_name(CookProfile profile) noexcept {
    switch (profile) {
        case CookProfile::Client:
            return "client";
        case CookProfile::DedicatedServer:
            return "dedicated-server";
        case CookProfile::Editor:
            return "editor";
    }
    return "client";
}

Expected<CookProfile, Error> cook_profile_from_name(std::string_view name) noexcept {
    constexpr CookProfile kAll[] = {CookProfile::Client, CookProfile::DedicatedServer,
                                    CookProfile::Editor};
    for (const CookProfile profile : kAll) {
        if (name == cook_profile_name(profile)) {
            return profile;
        }
    }
    return fail(ErrorCode::InvalidArgument, "not a cook profile this build defines");
}

const char* import_severity_name(ImportSeverity severity) noexcept {
    switch (severity) {
        case ImportSeverity::Info:
            return "info";
        case ImportSeverity::Warning:
            return "warning";
        case ImportSeverity::Error:
            return "error";
    }
    return "info";
}

std::string_view SubAsset::view() const noexcept {
    return {name.data(), name.size()};
}

Expected<OptionValue, Error> ImportRequest::option(const OptionsSchema& schema,
                                                   std::string_view name) const noexcept {
    if (options == nullptr) {
        const OptionSpec* declared = schema.find(name);
        if (declared == nullptr) {
            return fail(ErrorCode::NotFound, "this importer declares no option of that name");
        }
        return declared->default_value;
    }
    return options->get(schema, name);
}

// --- ImportResult --------------------------------------------------------------------------------

Status ImportResult::add(assets::AssetKind kind, std::string_view name, Array<u8>&& payload,
                         bool primary) noexcept {
    if (name.empty()) {
        return fail(ErrorCode::InvalidArgument, "a sub-asset must have a stable name");
    }
    SubAsset produced;
    produced.kind = kind;
    produced.primary = primary;
    if (Status appended = produced.name.append(Span<const char>(name.data(), name.size()));
        !appended) {
        return appended;
    }
    produced.payload = std::move(payload);
    return assets_.push_back(std::move(produced));
}

Status ImportResult::report(ImportSeverity severity, const char* code, std::string_view detail,
                            std::string_view subject) noexcept {
    ImportDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.code = code == nullptr ? "" : code;
    copy_truncated(diagnostic.detail, ImportDiagnostic::kDetailCapacity, detail);
    copy_truncated(diagnostic.subject, ImportDiagnostic::kSubjectCapacity, subject);
    return diagnostics_.push_back(diagnostic);
}

Status ImportResult::add_dependency(std::string_view name,
                                    const assets::ContentHash& digest) noexcept {
    // A dependency recorded twice is a dependency read twice, which is ordinary — a glTF reads its
    // buffer once per accessor. Recording it once keeps the cache record small and keeps the
    // invalidation check from doing the same digest several times.
    for (const DependencySlot& slot : dependencies_) {
        const std::string_view recorded(dependency_names_.data() + slot.offset, slot.length);
        if (recorded == name) {
            return ok();
        }
    }
    DependencySlot slot;
    slot.offset = static_cast<u32>(dependency_names_.size());
    slot.length = static_cast<u32>(name.size());
    slot.digest = digest;
    if (Status appended = dependency_names_.append(Span<const char>(name.data(), name.size()));
        !appended) {
        return appended;
    }
    return dependencies_.push_back(slot);
}

Span<const SubAsset> ImportResult::assets() const noexcept {
    return {assets_.data(), assets_.size()};
}

Span<const ImportDiagnostic> ImportResult::diagnostics() const noexcept {
    return {diagnostics_.data(), diagnostics_.size()};
}

assets::DerivedDependency ImportResult::dependency(usize index) const noexcept {
    CY_ASSERT_MSG(index < dependencies_.size(), "a dependency index past the end");
    const DependencySlot& slot = dependencies_[index];
    assets::DerivedDependency dependency;
    dependency.name = {dependency_names_.data() + slot.offset, slot.length};
    dependency.hash = slot.digest;
    return dependency;
}

bool ImportResult::has_errors() const noexcept {
    return std::ranges::any_of(diagnostics_, [](const ImportDiagnostic& diagnostic) noexcept {
        return diagnostic.severity == ImportSeverity::Error;
    });
}

usize ImportResult::warning_count() const noexcept {
    usize count = 0;
    for (const ImportDiagnostic& diagnostic : diagnostics_) {
        count += diagnostic.severity == ImportSeverity::Warning ? 1U : 0U;
    }
    return count;
}

const SubAsset* ImportResult::primary() const noexcept {
    for (const SubAsset& produced : assets_) {
        if (produced.primary) {
            return &produced;
        }
    }
    return nullptr;
}

void ImportResult::clear() noexcept {
    assets_.clear();
    diagnostics_.clear();
    dependencies_.clear();
    dependency_names_.clear();
}

// --- ImporterRegistry ----------------------------------------------------------------------------

Status ImporterRegistry::register_importer(Importer* importer) noexcept {
    if (importer == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a null importer");
    }
    const ImporterInfo info = importer->info();
    if (info.name.empty()) {
        return fail(ErrorCode::InvalidArgument, "an importer with no name");
    }
    if (info.extensions.empty()) {
        return fail(ErrorCode::InvalidArgument, "an importer that handles no extension");
    }
    if (info.description.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "an importer with no description saying what it handles");
    }
    if (Status valid = importer->schema().validate(); !valid) {
        return valid;
    }
    for (const Importer* registered : importers_) {
        const ImporterInfo other = registered->info();
        if (other.name == info.name) {
            return fail(ErrorCode::AlreadyExists, "an importer of that name is already registered");
        }
        for (const std::string_view extension : info.extensions) {
            for (const std::string_view claimed : other.extensions) {
                if (equal_ignoring_case(extension, claimed)) {
                    // Two importers for one extension is an ambiguity nothing downstream can
                    // resolve. Refusing it here makes it a registration error naming both, rather
                    // than a silent race in which whichever registered first wins.
                    return fail(ErrorCode::AlreadyExists,
                                "another importer already claims that extension");
                }
            }
        }
    }
    return importers_.push_back(importer);
}

Importer* ImporterRegistry::find_for_extension(std::string_view extension) const noexcept {
    for (Importer* importer : importers_) {
        for (const std::string_view claimed : importer->info().extensions) {
            if (equal_ignoring_case(extension, claimed)) {
                return importer;
            }
        }
    }
    return nullptr;
}

Importer* ImporterRegistry::find_for_source(const assets::VirtualPath& source) const noexcept {
    return find_for_extension(source.extension());
}

Importer* ImporterRegistry::find_by_name(std::string_view name) const noexcept {
    for (Importer* importer : importers_) {
        if (importer->info().name == name) {
            return importer;
        }
    }
    return nullptr;
}

Importer* ImporterRegistry::at(usize index) const noexcept {
    CY_ASSERT_MSG(index < importers_.size(), "an importer index past the end");
    return importers_[index];
}

// --- The key -------------------------------------------------------------------------------------

Expected<assets::DerivationKey, Error> import_derivation_key(
    const ImporterInfo& info, const OptionsSchema& schema, const ImportRequest& request,
    const assets::ContentHash& source_hash) noexcept {
    assets::DerivationKeyBuilder builder;
    // The order below is the specification's own list, and it is fixed here rather than at each
    // call site: "The derivation key SHALL include the source content, the importer and processor
    // versions, the import settings, the target platform, and the cook profile."
    builder.producer(assets::DerivedKind::Import, info.name, info.version)
        .source("source", source_hash)
        .text("variant", request.variant.view())
        .text("profile", cook_profile_name(request.profile));
    if (request.options != nullptr) {
        request.options->contribute_to(schema, builder);
    } else {
        ImportOptions defaults;
        defaults.contribute_to(schema, builder);
    }
    return builder.finish();
}

}  // namespace cy::import
