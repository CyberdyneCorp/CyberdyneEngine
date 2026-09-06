#include <cy/core/assets/derived_cache.h>

#include <cy/core/assets/file.h>

#include <cstring>

namespace cy::assets {
namespace {

/// The record's own magic. "CYDERIVE" is eight characters exactly, so the header stays aligned
/// without padding it out.
constexpr u8 kRecordMagic[8] = {'C', 'Y', 'D', 'E', 'R', 'I', 'V', 'E'};

/// The record format's version. It moves when THIS layout changes, and at no other time — a
/// producer's own format is the producer's business and is covered by its version in the key.
///
/// A record from a newer version is treated as a miss rather than as an error: a shared cache is
/// read by several checkouts at once, and a developer on last week's engine must be able to build
/// past an entry a colleague's newer build wrote.
constexpr u32 kRecordVersion = 1;

/// The fixed part of the record, before the producer name.
///
///    0   8  magic
///    8   4  format version
///   12   2  derived kind
///   14   2  reserved, zero
///   16  32  derivation key digest, so a file's content can be checked against its own name
///   48  32  payload content hash
///   80   8  payload size
///   88   4  dependency count
///   92   4  producer name length
constexpr usize kHeaderBytes = 96;
constexpr usize kVersionOffset = 8;
constexpr usize kKindOffset = 12;
constexpr usize kKeyOffset = 16;
constexpr usize kPayloadHashOffset = 48;
constexpr usize kPayloadSizeOffset = 80;
constexpr usize kDependencyCountOffset = 88;
constexpr usize kProducerLengthOffset = 92;

/// A ceiling on what one record may declare, so a corrupted length cannot ask for a terabyte before
/// the size check catches it. The bound is generous — a cooked virtual-geometry page is megabytes,
/// not gigabytes — and its purpose is to fail on the header rather than in the allocator.
constexpr u64 kMaxPayloadBytes = 1ULL << 32;
constexpr u32 kMaxNameBytes = 4096;

void write_u16_le(u8* out, u16 value) noexcept {
    out[0] = static_cast<u8>(value & 0xFFU);
    out[1] = static_cast<u8>((value >> 8U) & 0xFFU);
}

void write_u32_le(u8* out, u32 value) noexcept {
    for (u32 index = 0; index < 4; ++index) {
        out[index] = static_cast<u8>((value >> (index * 8U)) & 0xFFU);
    }
}

void write_u64_le(u8* out, u64 value) noexcept {
    for (u32 index = 0; index < 8; ++index) {
        out[index] = static_cast<u8>((value >> (index * 8U)) & 0xFFU);
    }
}

[[nodiscard]] u16 read_u16_le(const u8* data) noexcept {
    return static_cast<u16>(static_cast<u16>(data[0]) |
                            static_cast<u16>(static_cast<u16>(data[1]) << 8U));
}

[[nodiscard]] u32 read_u32_le(const u8* data) noexcept {
    u32 value = 0;
    for (u32 index = 0; index < 4; ++index) {
        value |= static_cast<u32>(data[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] u64 read_u64_le(const u8* data) noexcept {
    u64 value = 0;
    for (u32 index = 0; index < 8; ++index) {
        value |= static_cast<u64>(data[index]) << (index * 8U);
    }
    return value;
}

/// Copy `text` into `out`, refusing anything that does not fit rather than truncating. A truncated
/// cache root points at a directory that exists and is the wrong one.
[[nodiscard]] Status copy_root(char* out, usize capacity, const char* text) noexcept {
    const usize length = text == nullptr ? 0 : std::strlen(text);
    if (length >= capacity) {
        return fail(ErrorCode::InvalidArgument, "a derived cache root is longer than the maximum");
    }
    if (length != 0) {
        std::memcpy(out, text, length);
    }
    out[length] = '\0';
    return ok();
}

/// The path of one record: `<root>/<first two hex characters>/<key>.cyderive`.
///
/// The two-character shard directory is not decoration. A flat directory of a hundred thousand
/// content-addressed files is slow to enumerate on every filesystem the engine targets and
/// pathological on some; two hex characters give 256 buckets for the cost of one `mkdir` per
/// bucket.
struct RecordPath {
    char text[DerivedCache::kMaxRootLength + 1 + 2 + 1 + DerivationKey::kTextLength + 16] = {};
    /// The shard directory, so a store can create it before writing.
    char directory[DerivedCache::kMaxRootLength + 4] = {};
};

void build_record_path(const char* root, const DerivationKey& key, RecordPath& out) noexcept {
    char hex[DerivationKey::kTextLength + 1] = {};
    key.format(hex);

    const usize root_length = std::strlen(root);
    std::memcpy(out.directory, root, root_length);
    out.directory[root_length] = '/';
    out.directory[root_length + 1] = hex[0];
    out.directory[root_length + 2] = hex[1];
    out.directory[root_length + 3] = '\0';

    const usize directory_length = root_length + 3;
    std::memcpy(out.text, out.directory, directory_length);
    out.text[directory_length] = '/';
    std::memcpy(out.text + directory_length + 1, hex, DerivationKey::kTextLength);
    std::memcpy(out.text + directory_length + 1 + DerivationKey::kTextLength, ".cyderive", 10);
}

}  // namespace

const char* cache_tier_name(CacheTier tier) noexcept {
    switch (tier) {
        case CacheTier::None:
            return "none";
        case CacheTier::Local:
            return "local";
        case CacheTier::Shared:
            return "shared";
        case CacheTier::Remote:
            return "remote";
    }
    return "none";
}

const char* cache_outcome_name(CacheOutcome outcome) noexcept {
    switch (outcome) {
        case CacheOutcome::Hit:
            return "hit";
        case CacheOutcome::Miss:
            return "miss";
        case CacheOutcome::Invalidated:
            return "invalidated";
        case CacheOutcome::Corrupt:
            return "corrupt";
    }
    return "miss";
}

// --- The entry -----------------------------------------------------------------------------------

std::string_view DerivedEntry::producer() const noexcept {
    if (record_.empty()) {
        return {};
    }
    return {reinterpret_cast<const char*>(record_.data()) + producer_offset_, producer_size_};
}

Span<const u8> DerivedEntry::payload() const noexcept {
    if (record_.empty()) {
        return {};
    }
    return {record_.data() + payload_offset_, static_cast<usize>(payload_size_)};
}

DerivedDependency DerivedEntry::dependency(u32 index) const noexcept {
    CY_ASSERT_MSG(index < dependency_count_, "a dependency index past the end of the record");
    // Dependencies are variable-length, so reaching the nth means walking the first n. That is
    // linear, and it is the right trade: a record has a handful of dependencies, and an index built
    // at read time would cost an allocation on every hit to save a walk almost nobody makes twice.
    const u8* cursor = record_.data() + dependencies_offset_;
    for (u32 position = 0; position < index; ++position) {
        const u32 length = read_u32_le(cursor);
        cursor += 4 + length + ContentHash::kByteLength;
    }
    const u32 length = read_u32_le(cursor);
    DerivedDependency dependency;
    dependency.name = {reinterpret_cast<const char*>(cursor) + 4, length};
    std::memcpy(dependency.hash.bytes, cursor + 4 + length, ContentHash::kByteLength);
    return dependency;
}

// --- The cache -----------------------------------------------------------------------------------

Status DerivedCache::configure(const DerivedCacheTiers& tiers) noexcept {
    if (Status copied = copy_root(local_, sizeof(local_), tiers.local); !copied) {
        return copied;
    }
    if (Status copied = copy_root(shared_, sizeof(shared_), tiers.shared); !copied) {
        return copied;
    }
    if (Status copied = copy_root(remote_, sizeof(remote_), tiers.remote); !copied) {
        return copied;
    }
    write_remote_ = tiers.write_remote;
    promote_hits_ = tiers.promote_hits;
    verify_payload_ = tiers.verify_payload;

    if (local_[0] != '\0') {
        if (Status created = fs::create_directories(local_); !created) {
            return created;
        }
    }
    // A shared or remote tier that is absent is NOT an error; see the header. It degrades to a
    // miss, which is what a developer with no access to the team's share needs to happen.
    return ok();
}

CacheResult DerivedCache::lookup(const DerivationKey& key, DependencyDigest resolver,
                                 void* user) noexcept {
    CacheResult result;
    if (!is_enabled()) {
        ++statistics_.misses;
        result.reason = "the derived cache is disabled";
        return result;
    }

    const char* roots[3] = {local_, shared_, remote_};
    const CacheTier tiers[3] = {CacheTier::Local, CacheTier::Shared, CacheTier::Remote};
    for (u32 index = 0; index < 3; ++index) {
        if (roots[index][0] == '\0') {
            continue;
        }
        CacheResult attempt =
            read_tier(tiers[index], roots[index], key, resolver, user, verify_payload_);
        if (attempt.outcome == CacheOutcome::Miss) {
            continue;
        }
        // An invalidated or corrupt record in a nearer tier is the answer: a further tier holding a
        // record for the same key holds the same dependencies, because the key covers the inputs
        // that decide them. Carrying on would re-read the same failure over a network mount.
        if (attempt.outcome == CacheOutcome::Hit) {
            switch (tiers[index]) {
                case CacheTier::Local:
                    ++statistics_.hits_local;
                    break;
                case CacheTier::Shared:
                    ++statistics_.hits_shared;
                    break;
                case CacheTier::Remote:
                    ++statistics_.hits_remote;
                    break;
                case CacheTier::None:
                    break;
            }
            statistics_.bytes_read += attempt.entry.payload().size();
            if (promote_hits_ && tiers[index] != CacheTier::Local) {
                // Promotion is best-effort by design: a full disk must not turn a hit into a miss.
                // It is counted rather than reported, so a promotion that never succeeds shows up
                // in the statistics as hits that stay remote.
                Array<DerivedDependency> dependencies;
                bool copied = true;
                for (u32 slot = 0; slot < attempt.entry.dependency_count(); ++slot) {
                    if (!dependencies.push_back(attempt.entry.dependency(slot))) {
                        copied = false;
                        break;
                    }
                }
                if (copied) {
                    DerivedArtefact artefact;
                    artefact.kind = attempt.entry.kind();
                    artefact.producer = attempt.entry.producer();
                    artefact.payload = attempt.entry.payload();
                    artefact.dependencies = {dependencies.data(), dependencies.size()};
                    RecordPath path;
                    build_record_path(local_, key, path);
                    if (fs::create_directories(path.directory)) {
                        Array<u8> record;
                        if (encode_record(key, artefact, record) &&
                            fs::write_atomic(path.text, record.data(), record.size())) {
                            ++statistics_.promotions;
                        }
                    }
                }
            }
        } else if (attempt.outcome == CacheOutcome::Invalidated) {
            ++statistics_.invalidated;
        } else {
            ++statistics_.corrupt;
        }
        return attempt;
    }

    ++statistics_.misses;
    result.reason = "no tier holds this derivation key";
    return result;
}

CacheResult DerivedCache::read_tier(CacheTier tier, const char* root, const DerivationKey& key,
                                    DependencyDigest resolver, void* user,
                                    bool verify_payload) noexcept {
    CacheResult result;
    result.tier = tier;

    RecordPath path;
    build_record_path(root, key, path);
    if (!fs::exists(path.text)) {
        result.reason = "no record in this tier";
        return result;
    }

    Array<u8> record;
    if (Status read = fs::read_whole(path.text, record); !read) {
        // A record that exists and cannot be read is not a build failure. See the header: the cache
        // is disposable, so the honest response is to forget it and cook.
        result.outcome = CacheOutcome::Corrupt;
        result.reason = "the record could not be read";
        if (tier == CacheTier::Local) {
            (void)fs::remove_file(path.text);
        }
        return result;
    }

    if (Status decoded = decode_record(record, key, result.entry); !decoded) {
        // A newer format is a miss rather than a corruption: it was written by a build that knows
        // more than this one, and deleting it would make two checkouts fight over the shared tier.
        if (decoded.error().code == ErrorCode::Unsupported) {
            result.reason = "the record was written by a newer cache format";
            return result;
        }
        result.outcome = CacheOutcome::Corrupt;
        result.reason = decoded.error().message;
        if (tier == CacheTier::Local) {
            (void)fs::remove_file(path.text);
        }
        return result;
    }

    if (verify_payload) {
        const Span<const u8> payload = result.entry.payload();
        const ContentHash actual = content_hash(payload.data(), payload.size());
        if (actual != result.entry.payload_hash()) {
            result.outcome = CacheOutcome::Corrupt;
            result.reason = "the payload does not match its recorded digest";
            result.entry = {};
            if (tier == CacheTier::Local) {
                (void)fs::remove_file(path.text);
            }
            return result;
        }
    }

    if (result.entry.dependency_count() != 0) {
        if (resolver == nullptr) {
            result.outcome = CacheOutcome::Invalidated;
            result.reason = "the record has dependencies and the lookup supplied no resolver";
            return result;
        }
        for (u32 index = 0; index < result.entry.dependency_count(); ++index) {
            const DerivedDependency recorded = result.entry.dependency(index);
            bool found = true;
            const ContentHash current = resolver(user, recorded.name, &found);
            if (!found) {
                result.outcome = CacheOutcome::Invalidated;
                result.reason = "a recorded dependency no longer exists";
                result.stale = recorded.name;
                return result;
            }
            if (current != recorded.hash) {
                result.outcome = CacheOutcome::Invalidated;
                result.reason = "a recorded dependency changed";
                result.stale = recorded.name;
                return result;
            }
        }
    }

    result.outcome = CacheOutcome::Hit;
    return result;
}

Status DerivedCache::store(const DerivationKey& key, const DerivedArtefact& artefact) noexcept {
    if (!is_enabled()) {
        return ok();
    }
    if (artefact.payload.size() > kMaxPayloadBytes) {
        return fail(ErrorCode::OutOfRange, "a derived payload larger than the cache's ceiling");
    }

    Array<u8> record;
    if (Status encoded = encode_record(key, artefact, record); !encoded) {
        return encoded;
    }

    RecordPath path;
    build_record_path(local_, key, path);
    if (Status created = fs::create_directories(path.directory); !created) {
        return created;
    }
    if (Status written = fs::write_atomic(path.text, record.data(), record.size()); !written) {
        return written;
    }
    ++statistics_.stores;
    statistics_.bytes_written += record.size();

    if (write_remote_ && remote_[0] != '\0') {
        RecordPath remote_path;
        build_record_path(remote_, key, remote_path);
        if (Status created = fs::create_directories(remote_path.directory); !created) {
            return created;
        }
        if (Status written = fs::write_atomic(remote_path.text, record.data(), record.size());
            !written) {
            return written;
        }
        statistics_.bytes_written += record.size();
    }
    return ok();
}

bool DerivedCache::contains(const DerivationKey& key) const noexcept {
    if (!is_enabled()) {
        return false;
    }
    RecordPath path;
    build_record_path(local_, key, path);
    return fs::exists(path.text);
}

Status DerivedCache::clear_local() noexcept {
    if (!is_enabled()) {
        return ok();
    }
    if (Status removed = fs::remove_directory_recursive(local_); !removed) {
        return removed;
    }
    return fs::create_directories(local_);
}

// --- Encoding ------------------------------------------------------------------------------------

Status DerivedCache::encode_record(const DerivationKey& key, const DerivedArtefact& artefact,
                                   Array<u8>& out) noexcept {
    if (artefact.producer.size() > kMaxNameBytes) {
        return fail(ErrorCode::InvalidArgument, "a producer name longer than the record allows");
    }
    for (const DerivedDependency& dependency : artefact.dependencies) {
        if (dependency.name.size() > kMaxNameBytes) {
            return fail(ErrorCode::InvalidArgument,
                        "a dependency name longer than the record allows");
        }
    }

    const ContentHash payload_hash = content_hash(artefact.payload.data(), artefact.payload.size());

    u8 header[kHeaderBytes] = {};
    std::memcpy(header, kRecordMagic, sizeof(kRecordMagic));
    write_u32_le(header + kVersionOffset, kRecordVersion);
    write_u16_le(header + kKindOffset, static_cast<u16>(artefact.kind));
    std::memcpy(header + kKeyOffset, key.digest.bytes, ContentHash::kByteLength);
    std::memcpy(header + kPayloadHashOffset, payload_hash.bytes, ContentHash::kByteLength);
    write_u64_le(header + kPayloadSizeOffset, artefact.payload.size());
    write_u32_le(header + kDependencyCountOffset, static_cast<u32>(artefact.dependencies.size()));
    write_u32_le(header + kProducerLengthOffset, static_cast<u32>(artefact.producer.size()));

    if (Status appended = out.append(Span<const u8>(header, sizeof(header))); !appended) {
        return appended;
    }
    if (Status appended = out.append(Span<const u8>(
            reinterpret_cast<const u8*>(artefact.producer.data()), artefact.producer.size()));
        !appended) {
        return appended;
    }
    for (const DerivedDependency& dependency : artefact.dependencies) {
        u8 length[4] = {};
        write_u32_le(length, static_cast<u32>(dependency.name.size()));
        if (Status appended = out.append(Span<const u8>(length, sizeof(length))); !appended) {
            return appended;
        }
        if (Status appended = out.append(Span<const u8>(
                reinterpret_cast<const u8*>(dependency.name.data()), dependency.name.size()));
            !appended) {
            return appended;
        }
        if (Status appended =
                out.append(Span<const u8>(dependency.hash.bytes, ContentHash::kByteLength));
            !appended) {
            return appended;
        }
    }
    return out.append(artefact.payload);
}

Status DerivedCache::decode_record(Array<u8>& record, const DerivationKey& expected,
                                   DerivedEntry& out) noexcept {
    const usize size = record.size();
    const u8* data = record.data();
    if (size < kHeaderBytes || std::memcmp(data, kRecordMagic, sizeof(kRecordMagic)) != 0) {
        return fail(ErrorCode::InvalidArgument, "not a derived cache record");
    }
    const u32 version = read_u32_le(data + kVersionOffset);
    if (version > kRecordVersion) {
        return fail(ErrorCode::Unsupported, "a derived cache record from a newer format");
    }

    DerivedEntry entry;
    std::memcpy(entry.key_.digest.bytes, data + kKeyOffset, ContentHash::kByteLength);
    if (entry.key_ != expected) {
        // The record's own key disagreeing with the name it was filed under is the one corruption a
        // content-addressed store cannot shrug off: it would serve one computation's artefact for
        // another's, which is precisely the failure the addressing exists to make impossible.
        return fail(ErrorCode::Internal,
                    "the record's key does not match the key it is filed under");
    }
    std::memcpy(entry.payload_hash_.bytes, data + kPayloadHashOffset, ContentHash::kByteLength);
    entry.kind_ = static_cast<DerivedKind>(read_u16_le(data + kKindOffset));
    entry.payload_size_ = read_u64_le(data + kPayloadSizeOffset);
    entry.dependency_count_ = read_u32_le(data + kDependencyCountOffset);
    entry.producer_size_ = read_u32_le(data + kProducerLengthOffset);

    if (entry.payload_size_ > kMaxPayloadBytes || entry.producer_size_ > kMaxNameBytes) {
        return fail(ErrorCode::InvalidArgument, "a derived cache record declares an absurd length");
    }

    usize cursor = kHeaderBytes;
    if (cursor + entry.producer_size_ > size) {
        return fail(ErrorCode::InvalidArgument, "a derived cache record ends inside its producer");
    }
    entry.producer_offset_ = static_cast<u32>(cursor);
    cursor += entry.producer_size_;

    entry.dependencies_offset_ = static_cast<u32>(cursor);
    for (u32 index = 0; index < entry.dependency_count_; ++index) {
        if (cursor + 4 > size) {
            return fail(ErrorCode::InvalidArgument,
                        "a derived cache record ends inside its dependency table");
        }
        const u32 length = read_u32_le(data + cursor);
        if (length > kMaxNameBytes || cursor + 4 + length + ContentHash::kByteLength > size) {
            return fail(ErrorCode::InvalidArgument,
                        "a derived cache record ends inside its dependency table");
        }
        cursor += 4 + length + ContentHash::kByteLength;
    }

    if (cursor + entry.payload_size_ != size) {
        return fail(ErrorCode::InvalidArgument,
                    "a derived cache record's payload does not fill the rest of the file");
    }
    entry.payload_offset_ = cursor;
    entry.record_ = std::move(record);
    out = std::move(entry);
    return ok();
}

}  // namespace cy::assets
