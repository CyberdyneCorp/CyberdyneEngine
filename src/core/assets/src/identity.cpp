// Asset identity: minting, the sidecar's text form, and the collision-refusing database.
// Task 3.3.1.

#include <cy/core/assets/identity.h>

#include <cy/core/assets/diagnostics.h>
#include <cy/core/base/assert.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>

namespace cy::assets {
namespace {

/// The high half of every reserved placeholder id. Chosen to be a value no random draw will produce
/// in the life of the universe and one a human recognises in a log: "cyberdyne placeholder".
constexpr u64 kPlaceholderHigh = 0xC0DE'C0DE'0000'0000ULL;

/// The sidecar's keys. Spelled once, so the writer and the reader cannot disagree.
constexpr const char* kKeyVersion = "meta_version";
constexpr const char* kKeyId = "id";
constexpr const char* kKeyKind = "kind";
constexpr const char* kKeySource = "source";
constexpr const char* kKeySourceHash = "source_hash";
constexpr const char* kKeyCookedHash = "cooked_hash";

/// Trim ASCII spaces and tabs from both ends. The sidecar is written by the engine and edited by
/// people, and an editor that adds trailing whitespace should not invalidate an asset.
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

/// Strip the surrounding quotes of a value, or report that they are missing. Every value in the
/// sidecar is quoted, so that a path containing a `#` or a trailing space survives the round trip.
Expected<std::string_view, Error> unquote(std::string_view value) noexcept {
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
        return fail(ErrorCode::InvalidArgument, "a sidecar value must be a quoted string");
    }
    return value.substr(1, value.size() - 2);
}

}  // namespace

const char* asset_kind_name(AssetKind kind) noexcept {
    switch (kind) {
        case AssetKind::Unknown:
            return "unknown";
        case AssetKind::Texture:
            return "texture";
        case AssetKind::Mesh:
            return "mesh";
        case AssetKind::Material:
            return "material";
        case AssetKind::Shader:
            return "shader";
        case AssetKind::Audio:
            return "audio";
        case AssetKind::Scene:
            return "scene";
        case AssetKind::Prefab:
            return "prefab";
        case AssetKind::Font:
            return "font";
        case AssetKind::Animation:
            return "animation";
        case AssetKind::Binary:
            return "binary";
    }
    return "unknown";
}

Expected<AssetKind, Error> asset_kind_from_name(std::string_view name) noexcept {
    constexpr AssetKind kKinds[] = {AssetKind::Unknown,   AssetKind::Texture, AssetKind::Mesh,
                                    AssetKind::Material,  AssetKind::Shader,  AssetKind::Audio,
                                    AssetKind::Scene,     AssetKind::Prefab,  AssetKind::Font,
                                    AssetKind::Animation, AssetKind::Binary};
    for (const AssetKind kind : kKinds) {
        if (name == asset_kind_name(kind)) {
            return kind;
        }
    }
    return fail(ErrorCode::InvalidArgument, "no such asset kind");
}

// --- VariantKey
// -----------------------------------------------------------------------------------

Expected<VariantKey, Error> VariantKey::parse(std::string_view text) noexcept {
    if (text.size() > kCapacity) {
        return fail(ErrorCode::InvalidArgument, "a variant key is at most 23 characters");
    }
    for (const char c : text) {
        const bool acceptable =
            (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
        if (!acceptable) {
            return fail(ErrorCode::InvalidArgument,
                        "a variant key holds lowercase letters, digits, '-' and '.' only");
        }
    }
    VariantKey key;
    // Guarded on emptiness rather than called unconditionally: a default-constructed
    // `std::string_view` has a null `data()` with a zero `size()`, and `memcpy`'s source parameter
    // is declared never-null, so `memcpy(dst, nullptr, 0)` is undefined behaviour even though it
    // copies nothing. UndefinedBehaviorSanitizer reports it; every optimiser is entitled to act on
    // it. `VariantKey::any()` is exactly that empty key, so this is the common path, not a corner.
    if (!text.empty()) {
        std::memcpy(key.text_, text.data(), text.size());
    }
    return key;
}

std::string_view VariantKey::view() const noexcept {
    return {text_, std::strlen(text_)};
}

bool operator==(const VariantKey& a, const VariantKey& b) noexcept {
    return std::memcmp(a.text_, b.text_, VariantKey::kCapacity + 1) == 0;
}

bool operator<(const VariantKey& a, const VariantKey& b) noexcept {
    return std::memcmp(a.text_, b.text_, VariantKey::kCapacity + 1) < 0;
}

// --- Minting
// ---------------------------------------------------------------------------------------

cy::AssetId mint_asset_id() noexcept {
    // One generator per thread. std::random_device is the platform's entropy source and may be slow
    // per call; minting is an import-time operation, so correctness of the draw matters and its
    // cost does not. It is NOT seeded into a pseudo-random generator: a pseudo-random sequence from
    // a recorded seed is exactly what an id must not be.
    static thread_local std::random_device device;
    std::uniform_int_distribution<u64> distribution(0, ~0ULL);

    for (;;) {
        const u64 high = distribution(device);
        const u64 low = distribution(device);
        const cy::AssetId candidate(high, low);
        // Both exclusions are astronomically unlikely and both are checked, because "cannot happen"
        // is how a reserved namespace stops being reserved.
        if (!candidate.is_nil() && !is_placeholder(candidate)) {
            counters::record_id_minted();
            return candidate;
        }
    }
}

bool is_placeholder(cy::AssetId id) noexcept {
    return id.high() == kPlaceholderHigh;
}

cy::AssetId placeholder_for(AssetKind kind) noexcept {
    return {kPlaceholderHigh, static_cast<u64>(kind)};
}

AssetKind placeholder_kind(cy::AssetId id) noexcept {
    if (!is_placeholder(id) || id.low() > static_cast<u64>(AssetKind::Binary)) {
        return AssetKind::Unknown;
    }
    return static_cast<AssetKind>(id.low());
}

// --- The sidecar
// ------------------------------------------------------------------------------------

Expected<VirtualPath, Error> meta_path_for(const VirtualPath& source) noexcept {
    if (source.empty()) {
        return fail(ErrorCode::InvalidArgument, "a sidecar needs a source path");
    }
    char buffer[kMaxPathLength + 8] = {};
    const std::string_view text = source.view();
    if (text.size() + 5 > kMaxPathLength) {
        return fail(ErrorCode::InvalidArgument, "the sidecar path exceeds the maximum path length");
    }
    const int written = std::snprintf(buffer, sizeof(buffer), "%s.meta", source.c_str());
    if (written < 0) {
        return fail(ErrorCode::Internal, "the sidecar path could not be rendered");
    }
    return VirtualPath::normalise(std::string_view(buffer, static_cast<usize>(written)));
}

Expected<usize, Error> write_meta(const AssetMeta& meta, char* out, usize capacity) noexcept {
    char id_text[cy::AssetId::kTextLength + 1] = {};
    (void)meta.id.format(id_text);
    char source_hash[ContentHash::kTextLength + 1] = {};
    meta.source_hash.format(source_hash);
    char cooked_hash[ContentHash::kTextLength + 1] = {};
    meta.cooked_hash.format(cooked_hash);

    // snprintf reports the length it *would* have written, which is how BufferTooSmall is detected
    // without a second pass. The header comment says what the file is, because a sidecar is read by
    // people who did not write it.
    const int written = std::snprintf(
        out, capacity,
        "# CyberdyneEngine asset sidecar. Committed beside its source; the id is assigned once "
        "and\n"
        "# never changes, so moving or renaming the source does not break a reference to it.\n"
        "%s = %u\n%s = \"%s\"\n%s = \"%s\"\n%s = \"%s\"\n%s = \"%s\"\n%s = \"%s\"\n",
        kKeyVersion, AssetMeta::kVersion, kKeyId, id_text, kKeyKind, asset_kind_name(meta.kind),
        kKeySource, meta.source.c_str(), kKeySourceHash, source_hash, kKeyCookedHash, cooked_hash);

    if (written < 0) {
        return fail(ErrorCode::Internal, "the sidecar could not be rendered");
    }
    const auto length = static_cast<usize>(written);
    if (length >= capacity) {
        return fail(ErrorCode::BufferTooSmall, "the sidecar buffer is too small");
    }
    return length;
}

Expected<AssetMeta, Error> parse_meta(std::string_view text) noexcept {
    AssetMeta meta;
    bool have_version = false;
    bool have_id = false;
    bool have_kind = false;
    bool have_source = false;
    bool have_source_hash = false;

    usize cursor = 0;
    while (cursor <= text.size()) {
        const usize newline = text.find('\n', cursor);
        const usize end = newline == std::string_view::npos ? text.size() : newline;
        const std::string_view line = trim(text.substr(cursor, end - cursor));
        cursor = end + 1;

        if (line.empty() || line.front() == '#') {
            if (newline == std::string_view::npos) {
                break;
            }
            continue;
        }

        const usize equals = line.find('=');
        if (equals == std::string_view::npos) {
            return fail(ErrorCode::InvalidArgument, "a sidecar line is not `key = value`");
        }
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));

        if (key == kKeyVersion) {
            if (value != "1") {
                return fail(ErrorCode::Unsupported,
                            "the sidecar's meta_version is newer than this engine reads");
            }
            have_version = true;
        } else if (key == kKeyId) {
            Expected<std::string_view, Error> quoted = unquote(value);
            if (!quoted) {
                return make_unexpected(quoted.error());
            }
            Expected<cy::AssetId, Error> id = cy::AssetId::parse(quoted.value());
            if (!id) {
                return make_unexpected(id.error());
            }
            meta.id = id.value();
            have_id = true;
        } else if (key == kKeyKind) {
            Expected<std::string_view, Error> quoted = unquote(value);
            if (!quoted) {
                return make_unexpected(quoted.error());
            }
            Expected<AssetKind, Error> kind = asset_kind_from_name(quoted.value());
            if (!kind) {
                return make_unexpected(kind.error());
            }
            meta.kind = kind.value();
            have_kind = true;
        } else if (key == kKeySource) {
            Expected<std::string_view, Error> quoted = unquote(value);
            if (!quoted) {
                return make_unexpected(quoted.error());
            }
            Expected<VirtualPath, Error> path = VirtualPath::normalise(quoted.value());
            if (!path) {
                return make_unexpected(path.error());
            }
            meta.source = path.value();
            have_source = true;
        } else if (key == kKeySourceHash || key == kKeyCookedHash) {
            Expected<std::string_view, Error> quoted = unquote(value);
            if (!quoted) {
                return make_unexpected(quoted.error());
            }
            Expected<ContentHash, Error> hash = ContentHash::parse(quoted.value());
            if (!hash) {
                return make_unexpected(hash.error());
            }
            if (key == kKeySourceHash) {
                meta.source_hash = hash.value();
                have_source_hash = true;
            } else {
                meta.cooked_hash = hash.value();
            }
        } else {
            // Not skipped: see the declaration. A key this engine does not know is meaning it would
            // silently drop the next time it wrote the file.
            return fail(ErrorCode::InvalidArgument,
                        "the sidecar carries a key this engine does "
                        "not know");
        }

        if (newline == std::string_view::npos) {
            break;
        }
    }

    if (!have_version || !have_id || !have_kind || !have_source || !have_source_hash) {
        return fail(ErrorCode::InvalidArgument, "the sidecar is missing a required key");
    }
    return meta;
}

// --- The database -------------------------------------------------------------------------------

void AssetDatabase::ensure_sorted() noexcept {
    if (sorted_) {
        return;
    }
    std::ranges::sort(records_,
                      [](const AssetMeta& a, const AssetMeta& b) noexcept { return a.id < b.id; });
    sorted_ = true;
}

usize AssetDatabase::lower_bound(cy::AssetId id) noexcept {
    ensure_sorted();
    usize low = 0;
    usize high = records_.size();
    while (low < high) {
        const usize middle = low + ((high - low) / 2);
        if (records_[middle].id < id) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

Status AssetDatabase::register_asset(const AssetMeta& meta) noexcept {
    if (meta.id.is_nil()) {
        return fail(ErrorCode::InvalidArgument, "an asset may not be registered under the nil id");
    }
    if (is_placeholder(meta.id)) {
        return fail(ErrorCode::InvalidArgument,
                    "that id is in the reserved placeholder namespace and names no content");
    }

    const usize index = lower_bound(meta.id);
    if (index < records_.size() && records_[index].id == meta.id) {
        if (records_[index].source == meta.source) {
            records_[index] = meta;  // a re-import of the same file
            return ok();
        }
        ++collisions_;
        counters::record_meta_collision();
        last_collision_ = MetaCollision{meta.id, records_[index].source, meta.source};
        return fail(ErrorCode::AlreadyExists,
                    "two sources claim the same asset id; the second is refused rather than "
                    "shadowing the first — see AssetDatabase::last_collision()");
    }

    if (Status added = records_.push_back(meta); !added) {
        return added;
    }
    // Appending breaks the order unless the new record sorts last, which the common case — ids in
    // no particular order — does not. One flag beats re-sorting on every insertion.
    sorted_ = records_.size() < 2 || records_[records_.size() - 2].id < meta.id;
    return ok();
}

const AssetMeta* AssetDatabase::find(cy::AssetId id) noexcept {
    const usize index = lower_bound(id);
    if (index < records_.size() && records_[index].id == id) {
        return &records_[index];
    }
    return nullptr;
}

const AssetMeta* AssetDatabase::find_by_source(const VirtualPath& source) noexcept {
    // Linear: this is the importer's direction, walked once per file rather than per reference, and
    // a second index would have to be kept correct across every rebind for no measured gain.
    for (const AssetMeta& record : records_) {
        if (record.source == source) {
            return &record;
        }
    }
    return nullptr;
}

Status AssetDatabase::rebind_source(cy::AssetId id, const VirtualPath& source) noexcept {
    const usize index = lower_bound(id);
    if (index >= records_.size() || !(records_[index].id == id)) {
        return fail(ErrorCode::NotFound, "no asset is registered under that id");
    }
    records_[index].source = source;
    return ok();
}

Status AssetDatabase::unregister(cy::AssetId id) noexcept {
    const usize index = lower_bound(id);
    if (index >= records_.size() || !(records_[index].id == id)) {
        return fail(ErrorCode::NotFound, "no asset is registered under that id");
    }
    for (usize i = index + 1; i < records_.size(); ++i) {
        records_[i - 1] = records_[i];
    }
    records_.pop_back();
    return ok();
}

usize AssetDatabase::size() const noexcept {
    return records_.size();
}

void AssetDatabase::clear() noexcept {
    records_.clear();
    sorted_ = true;
}

void AssetDatabase::for_each(Visitor visitor, void* user) noexcept {
    CY_ASSERT(visitor != nullptr);
    ensure_sorted();
    for (const AssetMeta& record : records_) {
        visitor(user, record);
    }
}

}  // namespace cy::assets
