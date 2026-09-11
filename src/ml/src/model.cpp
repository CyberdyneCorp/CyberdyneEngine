// The model asset and its container. M8.c tasks 4.1 and 4.2.

#include <cy/core/memory/hash.h>
#include <cy/ml/model.h>

#include <algorithm>
#include <cstring>

namespace cy::ml {
namespace {

/// A FIXED SEED, and this is the whole reason it is spelled out rather than defaulted.
/// `cy::hash_bytes(data, size)` uses `hash_seed()`, which is randomised per process in development
/// builds so that iteration-order dependencies are caught. A content hash and a configuration
/// digest are the opposite kind of value: they are compared BETWEEN processes — a cook writes one
/// and a runtime reads it, and two machines cooking the same content must agree — so they take a
/// constant.
inline constexpr u64 kAssetSeed = 0x63796d6c5f763100ull;  // "cyml_v1"

constexpr const char* kBackendNames[static_cast<usize>(BackendKind::Count)] = {
    "unknown", "onnxruntime", "coreml", "directml", "tensorrt",
};

constexpr const char* kPrecisionNames[static_cast<usize>(Precision::Count)] = {
    "unknown", "f32", "f16", "i8", "mixed",
};

void append_u32(Array<u8>& out, u32 value, Status& status) noexcept {
    for (u32 shift = 0; shift < 32 && status; shift += 8) {
        status = out.push_back(static_cast<u8>((value >> shift) & 0xFFu));
    }
}

void append_u64(Array<u8>& out, u64 value, Status& status) noexcept {
    for (u32 shift = 0; shift < 64 && status; shift += 8) {
        status = out.push_back(static_cast<u8>((value >> shift) & 0xFFu));
    }
}

void append_u8(Array<u8>& out, u8 value, Status& status) noexcept {
    if (status) {
        status = out.push_back(value);
    }
}

void append_text(Array<u8>& out, std::string_view text, Status& status) noexcept {
    append_u32(out, static_cast<u32>(text.size()), status);
    for (usize index = 0; index < text.size() && status; ++index) {
        status = out.push_back(static_cast<u8>(text[index]));
    }
}

void append_spec(Array<u8>& out, const TensorSpec& spec, Status& status) noexcept {
    append_text(out, spec.name.text(), status);
    append_u8(out, static_cast<u8>(spec.type), status);
    append_u32(out, spec.shape.rank, status);
    for (u32 index = 0; index < spec.shape.rank && status; ++index) {
        append_u64(out, static_cast<u64>(spec.shape.dimensions[index]), status);
    }
}

/// A bounds-checked cursor. Every read reports whether it had the bytes, so a truncated container
/// is a diagnostic rather than a read past the end.
class Cursor {
public:
    explicit Cursor(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] usize offset() const noexcept { return offset_; }

    [[nodiscard]] u8 read_u8() noexcept {
        if (!have(1)) {
            return 0;
        }
        return bytes_[offset_++];
    }

    [[nodiscard]] u32 read_u32() noexcept {
        u32 value = 0;
        for (u32 shift = 0; shift < 32; shift += 8) {
            value |= static_cast<u32>(read_u8()) << shift;
        }
        return value;
    }

    [[nodiscard]] u64 read_u64() noexcept {
        u64 value = 0;
        for (u32 shift = 0; shift < 64; shift += 8) {
            value |= static_cast<u64>(read_u8()) << shift;
        }
        return value;
    }

    /// A view into the container's own bytes, valid as long as the caller's span is.
    [[nodiscard]] Span<const u8> read_bytes(usize count) noexcept {
        if (!have(count)) {
            return {};
        }
        const Span<const u8> view = bytes_.subspan(offset_, count);
        offset_ += count;
        return view;
    }

private:
    [[nodiscard]] bool have(usize count) noexcept {
        if (!ok_ || offset_ + count > bytes_.size()) {
            ok_ = false;
            return false;
        }
        return true;
    }

    Span<const u8> bytes_;
    usize offset_ = 0;
    bool ok_ = true;
};

[[nodiscard]] Name read_name(Cursor& cursor) noexcept {
    const u32 length = cursor.read_u32();
    const Span<const u8> text = cursor.read_bytes(length);
    if (!cursor.ok()) {
        return {};
    }
    return Name::intern(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()));
}

[[nodiscard]] TensorSpec read_spec(Cursor& cursor) noexcept {
    TensorSpec spec;
    spec.name = read_name(cursor);
    spec.type = static_cast<ElementType>(cursor.read_u8());
    spec.shape.rank = cursor.read_u32();
    if (spec.shape.rank > kMaxTensorRank) {
        spec.shape.rank = 0;
        return spec;
    }
    for (u32 index = 0; index < spec.shape.rank; ++index) {
        spec.shape.dimensions[index] = static_cast<i64>(cursor.read_u64());
    }
    return spec;
}

[[nodiscard]] u64 hash_spec(u64 accumulator, const TensorSpec& spec) noexcept {
    const std::string_view text = spec.name.text();
    accumulator = hash_combine(accumulator, hash_bytes(text.data(), text.size(), kAssetSeed));
    accumulator = hash_combine(accumulator, hash_integer(static_cast<u64>(spec.type), kAssetSeed));
    accumulator = hash_combine(accumulator, hash_integer(spec.shape.rank, kAssetSeed));
    for (u32 index = 0; index < spec.shape.rank; ++index) {
        accumulator = hash_combine(
            accumulator, hash_integer(static_cast<u64>(spec.shape.dimensions[index]), kAssetSeed));
    }
    return accumulator;
}

}  // namespace

const char* backend_kind_name(BackendKind kind) noexcept {
    const auto index = static_cast<usize>(kind);
    return (index < static_cast<usize>(BackendKind::Count)) ? kBackendNames[index] : "unknown";
}

BackendKind backend_kind_from_name(std::string_view text) noexcept {
    for (usize index = 0; index < static_cast<usize>(BackendKind::Count); ++index) {
        if (text == kBackendNames[index]) {
            return static_cast<BackendKind>(index);
        }
    }
    return BackendKind::Unknown;
}

const char* precision_name(Precision precision) noexcept {
    const auto index = static_cast<usize>(precision);
    return (index < static_cast<usize>(Precision::Count)) ? kPrecisionNames[index] : "unknown";
}

Precision precision_from_name(std::string_view text) noexcept {
    for (usize index = 0; index < static_cast<usize>(Precision::Count); ++index) {
        if (text == kPrecisionNames[index]) {
            return static_cast<Precision>(index);
        }
    }
    return Precision::Unknown;
}

u64 configuration_digest(BackendKind backend, Precision precision,
                         std::string_view device) noexcept {
    u64 digest = hash_integer(static_cast<u64>(backend), kAssetSeed);
    digest = hash_combine(digest, hash_integer(static_cast<u64>(precision), kAssetSeed));
    digest = hash_combine(digest, hash_bytes(device.data(), device.size(), kAssetSeed));
    // A digest of zero would read as "declares none" — see ModelDeterminism. One bit forced on
    // costs nothing and makes that impossible, rather than leaving a one-in-2^64 hole in a rule.
    return digest | 1ull;
}

// --- ModelAsset ---------------------------------------------------------------------------------

ModelAsset::ModelAsset(Allocator& allocator) noexcept
    : payload_(allocator), inputs_(allocator), outputs_(allocator), validated_(allocator) {}

Expected<ModelAsset, Error> ModelAsset::create(Allocator& allocator, Name name,
                                               Span<const u8> payload) noexcept {
    if (payload.empty()) {
        return fail(ErrorCode::InvalidArgument, "a model asset carries a payload");
    }
    ModelAsset asset(allocator);
    asset.name_ = name;
    if (Status appended = asset.payload_.append(payload); !appended) {
        return make_unexpected(appended.error());
    }
    return asset;
}

Status ModelAsset::declare_input(const TensorSpec& spec) noexcept {
    if (inputs_.size() >= kMaxModelTensors) {
        return fail(ErrorCode::OutOfRange,
                    "a model asset declares at most kMaxModelTensors inputs");
    }
    return inputs_.push_back(spec);
}

Status ModelAsset::declare_output(const TensorSpec& spec) noexcept {
    if (outputs_.size() >= kMaxModelTensors) {
        return fail(ErrorCode::OutOfRange,
                    "a model asset declares at most kMaxModelTensors outputs");
    }
    return outputs_.push_back(spec);
}

Status ModelAsset::declare_validated_backend(BackendKind kind) noexcept {
    if (kind == BackendKind::Unknown) {
        return fail(ErrorCode::InvalidArgument, "an unknown backend is not a validation");
    }
    for (BackendKind declared : validated_.span()) {
        if (declared == kind) {
            return ok();
        }
    }
    return validated_.push_back(kind);
}

u64 ModelAsset::content_hash() const noexcept {
    u64 digest = hash_bytes(payload_.data(), payload_.size(), kAssetSeed);
    const std::string_view text = name_.text();
    digest = hash_combine(digest, hash_bytes(text.data(), text.size(), kAssetSeed));
    digest = hash_combine(digest, hash_integer(static_cast<u64>(precision_), kAssetSeed));
    digest = hash_combine(digest, hash_integer(static_cast<u64>(determinism_.backend), kAssetSeed));
    digest =
        hash_combine(digest, hash_integer(static_cast<u64>(determinism_.precision), kAssetSeed));
    digest = hash_combine(digest, hash_integer(determinism_.verified_configuration, kAssetSeed));
    for (const TensorSpec& spec : inputs_.span()) {
        digest = hash_spec(digest, spec);
    }
    for (const TensorSpec& spec : outputs_.span()) {
        digest = hash_spec(digest, spec);
    }
    for (BackendKind kind : validated_.span()) {
        digest = hash_combine(digest, hash_integer(static_cast<u64>(kind), kAssetSeed));
    }
    return digest;
}

bool ModelAsset::validated_for(BackendKind kind) const noexcept {
    const Span<const BackendKind> declared = validated_.span();
    return std::ranges::any_of(declared, [kind](BackendKind entry) { return entry == kind; });
}

const TensorSpec* ModelAsset::input(Name name) const noexcept {
    for (const TensorSpec& spec : inputs_.span()) {
        if (spec.name == name) {
            return &spec;
        }
    }
    return nullptr;
}

const TensorSpec* ModelAsset::output(Name name) const noexcept {
    for (const TensorSpec& spec : outputs_.span()) {
        if (spec.name == name) {
            return &spec;
        }
    }
    return nullptr;
}

// --- The container ------------------------------------------------------------------------------

Status write_model_asset(const ModelAsset& asset, Array<u8>& out) noexcept {
    Status status = ok();
    append_u32(out, kModelAssetMagic, status);
    append_u32(out, kModelAssetVersion, status);
    append_text(out, asset.name().text(), status);
    append_u8(out, static_cast<u8>(asset.precision()), status);
    append_u8(out, static_cast<u8>(asset.determinism().backend), status);
    append_u8(out, static_cast<u8>(asset.determinism().precision), status);
    append_u8(out, 0, status);  // reserved, so the payload offset stays 8-byte aligned in spirit
    append_u64(out, asset.determinism().verified_configuration, status);

    append_u32(out, static_cast<u32>(asset.inputs().size()), status);
    for (const TensorSpec& spec : asset.inputs()) {
        append_spec(out, spec, status);
    }
    append_u32(out, static_cast<u32>(asset.outputs().size()), status);
    for (const TensorSpec& spec : asset.outputs()) {
        append_spec(out, spec, status);
    }
    append_u32(out, static_cast<u32>(asset.validated_backends().size()), status);
    for (BackendKind kind : asset.validated_backends()) {
        append_u8(out, static_cast<u8>(kind), status);
    }

    append_u64(out, static_cast<u64>(asset.payload().size()), status);
    if (status) {
        status = out.append(asset.payload());
    }
    return status;
}

Expected<ModelAsset, Error> read_model_asset(Allocator& allocator, Span<const u8> bytes) noexcept {
    Cursor cursor(bytes);
    const u32 magic = cursor.read_u32();
    const u32 version = cursor.read_u32();
    if (!cursor.ok() || magic != kModelAssetMagic) {
        return fail(ErrorCode::InvalidArgument,
                    "this is not a CyberML model asset: the magic does not match");
    }
    if (version != kModelAssetVersion) {
        return fail(ErrorCode::Unsupported, "this model asset was written by another version",
                    static_cast<i64>(version));
    }

    const Name name = read_name(cursor);
    const auto precision = static_cast<Precision>(cursor.read_u8());
    ModelDeterminism determinism;
    determinism.backend = static_cast<BackendKind>(cursor.read_u8());
    determinism.precision = static_cast<Precision>(cursor.read_u8());
    (void)cursor.read_u8();  // reserved
    determinism.verified_configuration = cursor.read_u64();

    ModelAsset asset(allocator);
    asset.set_precision(precision);
    asset.set_determinism(determinism);

    const u32 input_count = cursor.read_u32();
    if (!cursor.ok() || input_count > kMaxModelTensors) {
        return fail(ErrorCode::InvalidArgument,
                    "the model asset declares an impossible input count",
                    static_cast<i64>(input_count));
    }
    for (u32 index = 0; index < input_count; ++index) {
        const TensorSpec spec = read_spec(cursor);
        if (Status declared = asset.declare_input(spec); !declared) {
            return make_unexpected(declared.error());
        }
    }
    const u32 output_count = cursor.read_u32();
    if (!cursor.ok() || output_count > kMaxModelTensors) {
        return fail(ErrorCode::InvalidArgument,
                    "the model asset declares an impossible output count",
                    static_cast<i64>(output_count));
    }
    for (u32 index = 0; index < output_count; ++index) {
        const TensorSpec spec = read_spec(cursor);
        if (Status declared = asset.declare_output(spec); !declared) {
            return make_unexpected(declared.error());
        }
    }
    const u32 validated_count = cursor.read_u32();
    if (!cursor.ok() || validated_count > static_cast<u32>(BackendKind::Count)) {
        return fail(ErrorCode::InvalidArgument,
                    "the model asset declares an impossible validated-backend count",
                    static_cast<i64>(validated_count));
    }
    for (u32 index = 0; index < validated_count; ++index) {
        const auto kind = static_cast<BackendKind>(cursor.read_u8());
        if (Status declared = asset.declare_validated_backend(kind); !declared) {
            return make_unexpected(declared.error());
        }
    }

    const u64 payload_size = cursor.read_u64();
    const Span<const u8> payload = cursor.read_bytes(static_cast<usize>(payload_size));
    if (!cursor.ok()) {
        return fail(ErrorCode::InvalidArgument,
                    "the model asset is truncated: its declared payload runs past the end",
                    static_cast<i64>(payload_size));
    }

    asset.name_ = name;
    if (Status appended = asset.payload_.append(payload); !appended) {
        return make_unexpected(appended.error());
    }
    return asset;
}

}  // namespace cy::ml
