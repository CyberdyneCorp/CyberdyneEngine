#ifndef CY_CORE_ASSETS_DERIVATION_H
#define CY_CORE_ASSETS_DERIVATION_H
// The derivation key: what makes a piece of derived data *the same* piece of derived data. M5 task
// 5.1.
//
// `asset-import-pipeline` — "Cook cache": "The derivation key SHALL include the source content, the
// importer and processor versions, the import settings, the target platform, and the cook profile."
// `build-and-packaging` says the same thing of every other kind of derived data — shaders, material
// programs, geometry and texture pages — and both specifications are explicit that there is ONE
// cache rather than one per producer. So this type is in `cy::assets` at layer 0, beneath the
// importer, beneath the shader toolchain and beneath the renderer, rather than inside any of them.
//
// --- WHY A BUILDER RATHER THAN A STRUCT WITH FIELDS ----------------------------------------------
//
// The obvious shape is a struct — source hash, version, platform, profile — hashed field by field.
// It does not survive contact with the second producer: a shader has an include list and no import
// options, a texture page has a page index and no source file at all, and a virtual-geometry
// cluster has a cook policy that did not exist when the struct was written. Every one of those
// would add a field that means nothing to the other producers, and a field nobody sets is a field
// that silently stops distinguishing anything.
//
// So the key is an ordered sequence of NAMED contributions, and the name is hashed with the value.
// A producer adds what it has; a producer that adds a new input gets a new key for every entry it
// produces, which is exactly the invalidation that new input requires.
//
// --- THE AMBIGUITY THIS FILE EXISTS TO PREVENT ---------------------------------------------------
//
// Concatenating "ab" and "c" and concatenating "a" and "bc" produce the same bytes, so a naive
// hash-the-fields-in-order scheme collapses two different inputs onto one key — and a cook cache
// that collapses two inputs serves the wrong artefact with no diagnostic whatsoever. Every
// contribution is therefore framed: a type tag, the name's length, the name, the value's length,
// the value. That makes the encoding prefix-free, so two different field sequences cannot produce
// the same byte stream.
//
// --- WHAT IS DELIBERATELY NOT IN THE KEY ---------------------------------------------------------
//
// The output path, the machine, the wall-clock time and the build tree. A key that carried any of
// them would be unique per developer, which turns the shared cache — the entire reason the cache is
// content-addressed — into an expensive way of never getting a hit.

#include <cy/core/assets/hash.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <string_view>

namespace cy::assets {

/// The address of one piece of derived data. A BLAKE3 digest of the framed contributions below.
///
/// It is a distinct type from `ContentHash` rather than an alias, and the distinction earns its
/// keep: a content hash answers "are these the same bytes" and a derivation key answers "were these
/// produced by the same computation". Passing one where the other is wanted is the mistake that
/// makes a cache serve a stale artefact, and the compiler refuses it here.
struct DerivationKey {
    static constexpr usize kTextLength = ContentHash::kTextLength;

    ContentHash digest;

    [[nodiscard]] bool is_zero() const noexcept { return digest.is_zero(); }

    /// The canonical text form: 64 lowercase hex digits. This is the cache's file name.
    void format(char (&out)[kTextLength + 1]) const noexcept { digest.format(out); }

    [[nodiscard]] static Expected<DerivationKey, Error> parse(std::string_view text) noexcept;

    friend bool operator==(const DerivationKey& a, const DerivationKey& b) noexcept {
        return a.digest == b.digest;
    }
    friend bool operator!=(const DerivationKey& a, const DerivationKey& b) noexcept {
        return !(a == b);
    }
    friend bool operator<(const DerivationKey& a, const DerivationKey& b) noexcept {
        return a.digest < b.digest;
    }
};

/// What kind of computation produced an entry.
///
/// It is a LABEL, not a namespace: two producers of different kinds cannot collide, because the
/// kind is itself a contribution to the key. It exists for the report `asset-import-pipeline`
/// requires — "total cooked size by category" — and for a human reading a cache directory.
///
/// Persistent: the numbers appear in cache records, so an enumerator is appended and never
/// renumbered.
enum class DerivedKind : u16 {
    Unknown = 0,
    /// An imported source asset: a mesh, a texture, an animation, a prefab.
    Import = 1,
    /// A compiled shader or a pipeline's SPIR-V.
    Shader = 2,
    /// A material program produced by the material compiler.
    MaterialProgram = 3,
    /// One page of streamed geometry or of a virtual texture.
    Page = 4,
    /// A report, a manifest, or anything else a cook step produces.
    Metadata = 5,
};

/// The enumerator's own spelling, for a diagnostic and for a record's text form. Never null.
[[nodiscard]] const char* derived_kind_name(DerivedKind kind) noexcept;
[[nodiscard]] Expected<DerivedKind, Error> derived_kind_from_name(std::string_view name) noexcept;

/// Accumulates the framed contributions and produces the key.
///
/// Contributions are hashed IN ORDER. Two producers that add the same fields in different orders
/// produce different keys, which is harmless — they are different producers — but a single producer
/// must add its own fields in a fixed order or it will miss its own cache. That is a real trap, and
/// the remedy is the one every producer in this tree uses: build the key in one function, not
/// scattered across the code that discovers each input.
class DerivationKeyBuilder {
public:
    DerivationKeyBuilder() noexcept = default;

    DerivationKeyBuilder(const DerivationKeyBuilder&) = delete;
    DerivationKeyBuilder& operator=(const DerivationKeyBuilder&) = delete;

    /// What produced the entry, and the version of the code that produced it.
    ///
    /// `asset-import-pipeline` — "WHEN an importer's version increases THEN all assets it handles
    /// SHALL be re-cooked, since the version is part of the derivation key". This is that, and it
    /// is a separate call from `text` so that a producer cannot forget it: `finish()` refuses a key
    /// with no producer.
    DerivationKeyBuilder& producer(DerivedKind kind, std::string_view name, u32 version) noexcept;

    /// The content of an input the computation read.
    DerivationKeyBuilder& source(std::string_view name, const ContentHash& hash) noexcept;

    /// A text setting: the target platform, the cook profile, an option's value.
    DerivationKeyBuilder& text(std::string_view name, std::string_view value) noexcept;

    /// A numeric setting. Written little-endian so the key is the same on both byte orders — which
    /// matters, because a shared cache is populated by continuous integration and read by
    /// developers, and nothing says those are the same architecture.
    DerivationKeyBuilder& number(std::string_view name, u64 value) noexcept;

    /// A boolean setting. Distinct from `number` so that `true` and `1` are different keys, which
    /// keeps a setting that changes type from silently keeping its old artefacts.
    DerivationKeyBuilder& flag(std::string_view name, bool value) noexcept;

    /// Opaque bytes: an options struct already serialised, a compiled fragment, a lookup table.
    DerivationKeyBuilder& bytes(std::string_view name, const void* data, usize size) noexcept;

    /// How many contributions have been added. For a diagnostic that says why two keys differ.
    [[nodiscard]] u32 contributions() const noexcept { return contributions_; }

    /// The key. Fails with `InvalidArgument` when no producer was declared: a key with no producer
    /// is a key that will collide with a different producer's key over the same source, which is
    /// the one collision this scheme is otherwise immune to.
    [[nodiscard]] Expected<DerivationKey, Error> finish() noexcept;

private:
    /// The tag byte that opens a framed contribution. Distinct per kind so that `number("n", 1)`
    /// and `flag("n", true)` cannot encode identically.
    enum class Tag : u8 {
        Producer = 1,
        Source = 2,
        Text = 3,
        Number = 4,
        Flag = 5,
        Bytes = 6,
    };

    void frame(Tag tag, std::string_view name, const void* value, usize value_size) noexcept;

    ContentHasher hasher_;
    u32 contributions_ = 0;
    bool has_producer_ = false;
    bool finished_ = false;
};

}  // namespace cy::assets

#endif  // CY_CORE_ASSETS_DERIVATION_H
