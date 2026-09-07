#include <cy/import/sidecar.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cy::import {
namespace {

constexpr std::string_view kKeyVersion = "import_version";
constexpr std::string_view kKeyImporter = "importer";
constexpr std::string_view kKeyImporterVersion = "importer_version";
constexpr std::string_view kKeySourceHash = "source_hash";
constexpr std::string_view kKeyOptionPrefix = "option.";
constexpr std::string_view kKeySubAsset = "sub_asset.";

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    usize begin = 0;
    usize end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) {
        ++begin;
    }
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

[[nodiscard]] Expected<std::string_view, Error> unquote(std::string_view text) noexcept {
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
        return fail(ErrorCode::InvalidArgument, "an import record value is not a quoted string");
    }
    return text.substr(1, text.size() - 2);
}

/// Parse an option value from its written form, using the schema to decide what it should be.
///
/// The schema decides rather than the syntax, deliberately: `1` is a valid float and `true` is not
/// a valid piece of text, and a record whose meaning depended on how a writer happened to render a
/// value would change type when somebody edited it by hand.
[[nodiscard]] Expected<OptionValue, Error> parse_option(const OptionSpec& declared,
                                                        std::string_view text) noexcept {
    switch (declared.type) {
        case OptionType::Bool:
            if (text == "true") {
                return OptionValue::of_bool(true);
            }
            if (text == "false") {
                return OptionValue::of_bool(false);
            }
            return fail(ErrorCode::InvalidArgument, "a bool option is written true or false");
        case OptionType::Int: {
            char buffer[32] = {};
            if (text.empty() || text.size() >= sizeof(buffer)) {
                return fail(ErrorCode::InvalidArgument, "an int option's value is not a number");
            }
            std::memcpy(buffer, text.data(), text.size());
            char* end = nullptr;
            const long long value = std::strtoll(buffer, &end, 10);
            if (end == buffer || *end != '\0') {
                return fail(ErrorCode::InvalidArgument, "an int option's value is not a number");
            }
            return OptionValue::of_int(static_cast<i64>(value));
        }
        case OptionType::Float: {
            char buffer[64] = {};
            if (text.empty() || text.size() >= sizeof(buffer)) {
                return fail(ErrorCode::InvalidArgument, "a float option's value is not a number");
            }
            std::memcpy(buffer, text.data(), text.size());
            char* end = nullptr;
            const double value = std::strtod(buffer, &end);
            if (end == buffer || *end != '\0') {
                return fail(ErrorCode::InvalidArgument, "a float option's value is not a number");
            }
            return OptionValue::of_float(static_cast<f64>(value));
        }
        case OptionType::Text:
        case OptionType::Enumeration: {
            Expected<std::string_view, Error> quoted = unquote(text);
            if (!quoted) {
                return make_unexpected(quoted.error());
            }
            return declared.type == OptionType::Text ? OptionValue::of_text(quoted.value())
                                                     : OptionValue::of_enumeration(quoted.value());
        }
    }
    return fail(ErrorCode::Internal, "an option type this build does not write");
}

}  // namespace

Expected<assets::VirtualPath, Error> import_record_path_for(
    const assets::VirtualPath& source) noexcept {
    char buffer[assets::kMaxPathLength + 8] = {};
    const std::string_view text = source.view();
    if (text.empty() || text.size() + 7 >= sizeof(buffer)) {
        return fail(ErrorCode::OutOfRange, "the import record's path does not fit");
    }
    std::memcpy(buffer, text.data(), text.size());
    std::memcpy(buffer + text.size(), ".import", 8);
    return assets::VirtualPath::normalise(std::string_view(buffer, text.size() + 7));
}

// --- Construction --------------------------------------------------------------------------------

Expected<ImportRecord, Error> ImportRecord::create(std::string_view importer,
                                                   u32 importer_version) noexcept {
    if (importer.empty()) {
        return fail(ErrorCode::InvalidArgument, "an import record must name its importer");
    }
    ImportRecord record;
    if (Status appended =
            record.storage_.append(Span<const char>(importer.data(), importer.size()));
        !appended) {
        return make_unexpected(appended.error());
    }
    record.importer_offset_ = 0;
    record.importer_length_ = static_cast<u32>(importer.size());
    record.importer_version_ = importer_version;
    return record;
}

std::string_view ImportRecord::importer() const noexcept {
    if (storage_.empty()) {
        return {};
    }
    return {storage_.data() + importer_offset_, importer_length_};
}

std::string_view ImportRecord::name_of(const Binding& binding) const noexcept {
    return {names_.data() + binding.offset, binding.length};
}

Expected<ImportRecord::Binding, Error> ImportRecord::intern(std::string_view name,
                                                            cy::AssetId id) noexcept {
    Binding binding;
    binding.offset = static_cast<u32>(names_.size());
    binding.length = static_cast<u32>(name.size());
    binding.id = id;
    if (Status appended = names_.append(Span<const char>(name.data(), name.size())); !appended) {
        return make_unexpected(appended.error());
    }
    return binding;
}

cy::AssetId ImportRecord::sub_asset(std::string_view name) const noexcept {
    for (const Binding& binding : bindings_) {
        if (name_of(binding) == name) {
            return binding.id;
        }
    }
    return {};
}

Status ImportRecord::bind(std::string_view name, cy::AssetId id) noexcept {
    if (name.empty()) {
        return fail(ErrorCode::InvalidArgument, "a sub-asset binding must have a name");
    }
    if (id.is_nil()) {
        return fail(ErrorCode::InvalidArgument, "a sub-asset binding must have an id");
    }
    for (Binding& binding : bindings_) {
        if (name_of(binding) == name) {
            binding.id = id;
            return ok();
        }
    }
    Expected<Binding, Error> binding = intern(name, id);
    if (!binding) {
        return make_unexpected(binding.error());
    }
    return bindings_.push_back(binding.value());
}

usize ImportRecord::retain_only(Span<const SubAsset> keep) noexcept {
    usize dropped = 0;
    usize write = 0;
    for (const Binding& binding : bindings_) {
        const std::string_view name = name_of(binding);
        const bool wanted = std::ranges::any_of(
            keep, [name](const SubAsset& produced) noexcept { return produced.view() == name; });
        if (wanted) {
            bindings_[write] = binding;
            ++write;
        } else {
            ++dropped;
        }
    }
    while (bindings_.size() > write) {
        bindings_.pop_back();
    }
    // `names_` keeps the dropped names' bytes. That is deliberate: compacting it would move every
    // surviving binding's offset, and the blob is rebuilt on the next parse anyway. A record that
    // has churned through many names is a few hundred bytes larger in memory and identical on disk.
    return dropped;
}

std::string_view ImportRecord::binding_name(usize index) const noexcept {
    CY_ASSERT_MSG(index < bindings_.size(), "a binding index past the end");
    return name_of(bindings_[index]);
}

cy::AssetId ImportRecord::binding_id(usize index) const noexcept {
    CY_ASSERT_MSG(index < bindings_.size(), "a binding index past the end");
    return bindings_[index].id;
}

// --- Parsing -------------------------------------------------------------------------------------

Expected<ImportRecord, Error> ImportRecord::parse(std::string_view text,
                                                  const OptionsSchema* schema) noexcept {
    ImportRecord record;
    // The whole text is copied first, so every view the record hands out survives the caller's
    // buffer. It is one allocation for a file that is rarely more than a kilobyte.
    if (Status appended = record.storage_.append(Span<const char>(text.data(), text.size()));
        !appended) {
        return make_unexpected(appended.error());
    }
    const std::string_view owned(record.storage_.data(), record.storage_.size());

    bool have_version = false;
    bool have_importer = false;
    usize cursor = 0;
    while (cursor <= owned.size()) {
        const usize newline = owned.find('\n', cursor);
        const usize end = newline == std::string_view::npos ? owned.size() : newline;
        const std::string_view line = trim(owned.substr(cursor, end - cursor));
        cursor = end + 1;
        if (line.empty() || line.front() == '#') {
            if (newline == std::string_view::npos) {
                break;
            }
            continue;
        }

        const usize equals = line.find('=');
        if (equals == std::string_view::npos) {
            return fail(ErrorCode::InvalidArgument, "an import record line is not `key = value`");
        }
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));

        if (key == kKeyVersion) {
            if (value != "1") {
                return fail(ErrorCode::Unsupported,
                            "the import record's version is newer than this build reads");
            }
            have_version = true;
        } else if (key == kKeyImporter) {
            Expected<std::string_view, Error> quoted = unquote(value);
            if (!quoted) {
                return make_unexpected(quoted.error());
            }
            record.importer_offset_ =
                static_cast<u32>(static_cast<usize>(quoted.value().data() - owned.data()));
            record.importer_length_ = static_cast<u32>(quoted.value().size());
            have_importer = true;
        } else if (key == kKeyImporterVersion) {
            char buffer[16] = {};
            if (value.empty() || value.size() >= sizeof(buffer)) {
                return fail(ErrorCode::InvalidArgument, "an importer version is not a number");
            }
            std::memcpy(buffer, value.data(), value.size());
            char* stop = nullptr;
            const unsigned long parsed = std::strtoul(buffer, &stop, 10);
            if (stop == buffer || *stop != '\0') {
                return fail(ErrorCode::InvalidArgument, "an importer version is not a number");
            }
            record.importer_version_ = static_cast<u32>(parsed);
        } else if (key == kKeySourceHash) {
            Expected<std::string_view, Error> quoted = unquote(value);
            if (!quoted) {
                return make_unexpected(quoted.error());
            }
            Expected<assets::ContentHash, Error> hash = assets::ContentHash::parse(quoted.value());
            if (!hash) {
                return make_unexpected(hash.error());
            }
            record.source_hash_ = hash.value();
        } else if (key.size() > kKeyOptionPrefix.size() && key.starts_with(kKeyOptionPrefix)) {
            const std::string_view name = key.substr(kKeyOptionPrefix.size());
            if (schema == nullptr) {
                // A listing tool reads identity and sub-assets without knowing the importer's
                // schema. Skipping the option rather than failing is what makes that possible, and
                // it is safe because nothing writes a record it parsed without a schema.
                continue;
            }
            const OptionSpec* declared = schema->find(name);
            if (declared == nullptr) {
                return fail(ErrorCode::NotFound,
                            "the record sets an option the importer no longer declares");
            }
            Expected<OptionValue, Error> parsed = parse_option(*declared, value);
            if (!parsed) {
                return make_unexpected(parsed.error());
            }
            if (Status set = record.options_.set(*schema, name, parsed.value()); !set) {
                return make_unexpected(set.error());
            }
        } else if (key.size() > kKeySubAsset.size() && key.starts_with(kKeySubAsset)) {
            Expected<std::string_view, Error> quoted = unquote(value);
            if (!quoted) {
                return make_unexpected(quoted.error());
            }
            Expected<cy::AssetId, Error> id = cy::AssetId::parse(quoted.value());
            if (!id) {
                return make_unexpected(id.error());
            }
            Expected<std::string_view, Error> name = unquote(key.substr(kKeySubAsset.size()));
            if (!name) {
                return make_unexpected(name.error());
            }
            if (Status bound = record.bind(name.value(), id.value()); !bound) {
                return make_unexpected(bound.error());
            }
        } else {
            return fail(ErrorCode::InvalidArgument,
                        "an import record key this build does not read");
        }

        if (newline == std::string_view::npos) {
            break;
        }
    }

    if (!have_version || !have_importer) {
        return fail(ErrorCode::InvalidArgument,
                    "an import record must declare its version and its importer");
    }
    return record;
}

// --- Writing -------------------------------------------------------------------------------------

usize ImportRecord::written_size(const OptionsSchema& schema) const noexcept {
    // The header, the four scalar lines, and a generous fixed cost per option and per binding. It
    // is an upper bound rather than an exact count: `write` still refuses to truncate, so an
    // underestimate would be caught, and an exact count would mean rendering everything twice.
    usize size = 512 + importer_length_;
    for (usize index = 0; index < options_.size(); ++index) {
        size += options_.name_at(index).size() + 96;
    }
    (void)schema;
    for (const Binding& binding : bindings_) {
        size += binding.length + 64;
    }
    return size;
}

Expected<usize, Error> ImportRecord::write(const OptionsSchema& schema, char* out,
                                           usize capacity) const noexcept {
    // A bounded writer rather than a chain of snprintf calls. Two reasons, and the second is the
    // one that decided it: every append is the same three lines of arithmetic and writing them out
    // fifteen times is how one of them ends up missing its bounds check; and a snprintf whose
    // format is a variable trips -Wformat-nonliteral, which this tree compiles as an error. The
    // only formatted conversion left is the float, whose format is a literal.
    struct Writer {
        char* out = nullptr;
        usize capacity = 0;
        usize written = 0;
        bool overflowed = false;

        void text(std::string_view value) noexcept {
            if (overflowed || written + value.size() >= capacity) {
                overflowed = true;
                return;
            }
            if (!value.empty()) {
                std::memcpy(out + written, value.data(), value.size());
            }
            written += value.size();
        }

        void unsigned_number(u64 value) noexcept {
            char digits[24] = {};
            usize length = 0;
            do {
                digits[length++] = static_cast<char>('0' + static_cast<char>(value % 10U));
                value /= 10U;
            } while (value != 0);
            for (usize index = 0; index < length; ++index) {
                text(std::string_view(&digits[length - 1 - index], 1));
            }
        }

        void signed_number(i64 value) noexcept {
            if (value < 0) {
                text("-");
                // Negated as an unsigned value, so the most negative i64 does not overflow on the
                // way to being printed. That case cannot reach here from a dialog and can reach
                // here from a hand-edited record.
                unsigned_number(~static_cast<u64>(value) + 1U);
                return;
            }
            unsigned_number(static_cast<u64>(value));
        }

        void real(f64 value) noexcept {
            // %.17g round-trips a double exactly, which is what makes an unchanged option produce
            // no diff and an unchanged record produce the same derivation key.
            char buffer[40] = {};
            const int produced = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
            if (produced <= 0 || static_cast<usize>(produced) >= sizeof(buffer)) {
                overflowed = true;
                return;
            }
            text(std::string_view(buffer, static_cast<usize>(produced)));
        }

        void quoted(std::string_view value) noexcept {
            text("\"");
            text(value);
            text("\"");
        }
    };

    Writer writer{out, capacity};
    writer.text("# CyberdyneEngine import record. Committed beside its source.\n");
    writer.text(
        "# The ids below survive re-import: a sub-asset keeps the id its name already had.\n");
    writer.text("import_version = ");
    writer.unsigned_number(kImportRecordVersion);
    writer.text("\nimporter = ");
    writer.quoted(importer());
    writer.text("\nimporter_version = ");
    writer.unsigned_number(importer_version_);
    writer.text("\nsource_hash = ");
    char hash_text[assets::ContentHash::kTextLength + 1] = {};
    source_hash_.format(hash_text);
    writer.quoted(hash_text);
    writer.text("\n");

    // Options, in the SCHEMA's order rather than the order they were set, so that two records with
    // the same values are the same bytes and a re-import produces no diff.
    for (const OptionSpec& declared : schema.options()) {
        if (!options_.is_set(declared.name)) {
            continue;
        }
        Expected<OptionValue, Error> value = options_.get(schema, declared.name);
        if (!value) {
            return make_unexpected(value.error());
        }
        writer.text("option.");
        writer.text(declared.name);
        writer.text(" = ");
        switch (value.value().type()) {
            case OptionType::Bool:
                writer.text(value.value().as_bool() ? "true" : "false");
                break;
            case OptionType::Int:
                writer.signed_number(value.value().as_int());
                break;
            case OptionType::Float:
                writer.real(value.value().as_float());
                break;
            case OptionType::Text:
            case OptionType::Enumeration:
                writer.quoted(value.value().as_text());
                break;
        }
        writer.text("\n");
    }

    // Sub-assets in binding order, which is the order they were first produced. Stable across
    // re-imports of an unchanged source, so this file's diff is empty when nothing changed.
    for (const Binding& binding : bindings_) {
        char id_text[cy::AssetId::kTextLength + 1] = {};
        (void)binding.id.format(id_text);
        writer.text("sub_asset.");
        writer.quoted(name_of(binding));
        writer.text(" = ");
        writer.quoted(id_text);
        writer.text("\n");
    }

    if (writer.overflowed || writer.written >= capacity) {
        return fail(ErrorCode::BufferTooSmall, "the import record does not fit the buffer");
    }
    out[writer.written] = '\0';
    return writer.written;
}

// --- Stable identity -----------------------------------------------------------------------------

Status bind_sub_assets(ImportRecord& record, const ImportResult& result, Span<cy::AssetId> out_ids,
                       usize& out_minted, MintPolicy policy) noexcept {
    const Span<const SubAsset> produced = result.assets();
    if (out_ids.size() < produced.size()) {
        return fail(ErrorCode::BufferTooSmall, "not enough room for the produced sub-asset ids");
    }
    out_minted = 0;
    for (usize index = 0; index < produced.size(); ++index) {
        const std::string_view name = produced[index].view();
        cy::AssetId id = record.sub_asset(name);
        if (id.is_nil()) {
            if (policy == MintPolicy::Refuse) {
                // The whole point of the refusal: a reproducible cook does not invent identity. The
                // remedy is to import the asset once interactively and commit the `.import` record
                // beside it, which is what `identity.h` says should have happened anyway.
                return fail(ErrorCode::PermissionDenied,
                            "this cook refuses to mint an asset id, and the import record does not "
                            "bind one of the sub-assets this source produces; import it once and "
                            "commit the .import file beside the source");
            }
            id = assets::mint_asset_id();
            ++out_minted;
        }
        if (Status bound = record.bind(name, id); !bound) {
            return bound;
        }
        out_ids[index] = id;
    }
    (void)record.retain_only(produced);
    return ok();
}

}  // namespace cy::import
