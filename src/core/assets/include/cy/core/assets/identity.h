#ifndef CY_CORE_ASSETS_IDENTITY_H
#define CY_CORE_ASSETS_IDENTITY_H
// Asset identity: the id, its sidecar, the variant key, and the database that keeps them honest.
// Task 3.3.1.
//
// `core-assets-and-io` — "Asset identity": every asset has a stable 128-bit `AssetId`, generated
// once when the source file is first imported and stored in a sidecar `.meta` file next to the
// source; serialized references use the id, never a path, so moving or renaming a source does not
// break them; and an asset additionally has a **content hash** of its cooked payload, used for
// cache validation, incremental builds and patch generation.
//
// THE TYPE ITSELF IS NOT HERE. `AssetId` is cy::AssetId from the values module (task 1.3.2), which
// is where the distinction between an id and a runtime handle is made structural. This file owns
// what the specification gives to `core-assets-and-io`: how an id is minted, what travels with it,
// and what happens when two files claim the same one.
//
// THREE RULES, EACH ENFORCED RATHER THAN DOCUMENTED.
//
//   * An id is minted ONCE. `mint_asset_id()` draws 128 random bits; there is no function that
//     derives an id from a path, a name or content, because every one of those changes under the
//     edits the id exists to survive.
//   * A SECOND CLAIM ON AN ID IS REFUSED. `AssetDatabase::register_asset` fails with AlreadyExists
//     and names both source paths — the specification's "report a collision and refuse to register
//     the second, rather than silently shadowing the first". A duplicated `.meta` is a copy-paste,
//     and shadowing makes it a mystery instead of a message.
//   * A MISSING ASSET RESOLVES TO A TYPED PLACEHOLDER. `placeholder_for(kind)` is a reserved id per
//     asset kind, so a broken reference loads a missing-texture rather than failing the whole load.
//
// COOKING IS M2. This file describes an asset; it does not import one. There is no importer, no
// source-format parser, and no cooked-output cache here, and `AssetMeta` deliberately carries no
// importer settings — those belong with the importer that reads them.

#include <cy/core/assets/hash.h>
#include <cy/core/assets/path.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/asset_id.h>

#include <string_view>

namespace cy::assets {

/// What an asset is, as the runtime sees it. Persistent: the numbers appear in package directories,
/// so an enumerator is added at the end and never renumbered.
enum class AssetKind : u16 {
    Unknown = 0,
    Texture = 1,
    Mesh = 2,
    Material = 3,
    Shader = 4,
    Audio = 5,
    Scene = 6,
    Prefab = 7,
    Font = 8,
    Animation = 9,
    /// Cooked bytes the engine does not interpret. A project's own data, and what a test uses.
    Binary = 10,
};

/// The enumerator's own spelling, for a diagnostic and for the sidecar's text form. Never null.
const char* asset_kind_name(AssetKind kind) noexcept;
[[nodiscard]] Expected<AssetKind, Error> asset_kind_from_name(std::string_view name) noexcept;

/// The platform-and-feature key a cooked variant was produced for.
///
/// `core-assets-and-io` — "Platform variants": both a desktop (BC7) and a mobile (ASTC) texture are
/// addressable by the same `AssetId` plus a variant key. It is a short inline string rather than a
/// hash of one, deliberately: a hash collision here would serve the wrong variant silently, and a
/// 23-character budget covers every key the engine will spell ("desktop-bc7", "mobile-astc").
class VariantKey {
public:
    static constexpr usize kCapacity = 23;

    constexpr VariantKey() noexcept = default;

    /// The key that matches whatever a package holds — an asset cooked once for every platform.
    [[nodiscard]] static constexpr VariantKey any() noexcept { return {}; }

    /// Rejects a key longer than `kCapacity`, and one containing anything but lowercase letters,
    /// digits, `-` and `.`, so that a key is spelled one way and sorts predictably.
    [[nodiscard]] static Expected<VariantKey, Error> parse(std::string_view text) noexcept;

    [[nodiscard]] constexpr bool is_any() const noexcept { return text_[0] == '\0'; }
    [[nodiscard]] std::string_view view() const noexcept;
    [[nodiscard]] constexpr const char* c_str() const noexcept { return text_; }

    friend bool operator==(const VariantKey& a, const VariantKey& b) noexcept;
    friend bool operator!=(const VariantKey& a, const VariantKey& b) noexcept { return !(a == b); }
    friend bool operator<(const VariantKey& a, const VariantKey& b) noexcept;

private:
    char text_[kCapacity + 1] = {};
};

static_assert(sizeof(VariantKey) == 24, "a variant key is inline and fixed");

// --- Minting -------------------------------------------------------------------------------------

/// Draw a fresh 128-bit asset id.
///
/// Random, never derived. The entropy comes from the platform's own source through
/// `std::random_device`, which on the platforms the engine targets is the OS CSPRNG. The
/// specification puts a CSPRNG in `core/crypto`; when that module exists this function moves onto
/// it, and its contract — 128 unpredictable bits, never twice — does not change.
///
/// Never returns the nil id and never returns an id in the reserved placeholder namespace.
[[nodiscard]] cy::AssetId mint_asset_id() noexcept;

/// True for an id in the namespace `placeholder_for` draws from. An importer must never mint one,
/// and `AssetDatabase::register_asset` refuses one, so a placeholder cannot be shadowed by content.
[[nodiscard]] bool is_placeholder(cy::AssetId id) noexcept;

/// The typed placeholder for a kind — missing-texture, missing-mesh, and so on.
///
/// `core-assets-and-io` — "Source deleted": loading a reference whose asset no longer exists yields
/// a typed placeholder and a diagnostic naming the referrer, rather than failing the whole load.
/// The id is reserved and stable, so a placeholder is a thing a package can actually contain.
[[nodiscard]] cy::AssetId placeholder_for(AssetKind kind) noexcept;

/// The kind a placeholder id stands for, or Unknown when the id is not one.
[[nodiscard]] AssetKind placeholder_kind(cy::AssetId id) noexcept;

// --- The sidecar
// ----------------------------------------------------------------------------------

/// The record stored beside a source file as `<source>.meta`.
///
/// It is TEXT, and diff-friendly, because it is committed to source control beside the asset it
/// describes: a binary sidecar makes every import a merge conflict nobody can read.
struct AssetMeta {
    /// The format of this record. Bumped when a field is added; a reader refuses a newer one.
    static constexpr u32 kVersion = 1;

    cy::AssetId id;
    AssetKind kind = AssetKind::Unknown;
    /// The source file, relative to the project root. METADATA, not identity: it changes when the
    /// asset moves, and nothing resolves through it.
    VirtualPath source;
    /// The digest of the source file at import time, so a re-import can tell whether it must.
    ContentHash source_hash;
    /// The digest of the cooked payload. Zero until something cooks it, which is M2.
    ContentHash cooked_hash;
};

/// The conventional sidecar name for a source path: `<source>.meta`.
[[nodiscard]] Expected<VirtualPath, Error> meta_path_for(const VirtualPath& source) noexcept;

/// Render a sidecar. Writes at most `capacity` bytes including the terminator and reports how many
/// it wrote; fails with BufferTooSmall rather than truncating, because a truncated sidecar reads as
/// a valid one with fields missing.
[[nodiscard]] Expected<usize, Error> write_meta(const AssetMeta& meta, char* out,
                                                usize capacity) noexcept;

/// Parse a sidecar. Every field is required except `cooked_hash`, which an uncooked asset has not
/// got. An unknown key is an error rather than a silent skip: a sidecar written by a newer engine
/// carries meaning this one would drop on the next write.
[[nodiscard]] Expected<AssetMeta, Error> parse_meta(std::string_view text) noexcept;

// --- The database
// ---------------------------------------------------------------------------------

/// What a collision was, so a diagnostic can name both sides of it.
struct MetaCollision {
    cy::AssetId id;
    VirtualPath first;   ///< the source that holds the id
    VirtualPath second;  ///< the source that tried to claim it
};

/// The id-to-source index the importer and the editor resolve through.
///
/// The whole point of it is the refusal: registering a second claim on one id fails and says which
/// two files are involved. Everything else here is the lookup that makes the id, rather than the
/// path, the thing references are written as.
class AssetDatabase {
public:
    AssetDatabase() noexcept = default;

    AssetDatabase(const AssetDatabase&) = delete;
    AssetDatabase& operator=(const AssetDatabase&) = delete;
    AssetDatabase(AssetDatabase&&) noexcept = default;
    AssetDatabase& operator=(AssetDatabase&&) noexcept = default;

    /// Register an asset. Fails with:
    ///   * InvalidArgument — a nil id, or one in the reserved placeholder namespace;
    ///   * AlreadyExists — the id is already registered to a different source. `last_collision()`
    ///     then names both, which is what the diagnostic prints.
    /// Registering the SAME id against the SAME source again succeeds and replaces the record: that
    /// is a re-import, not a collision.
    [[nodiscard]] Status register_asset(const AssetMeta& meta) noexcept;

    /// The record for an id, or null. This is how a serialized reference resolves, and it is why a
    /// moved file does not break one.
    [[nodiscard]] const AssetMeta* find(cy::AssetId id) noexcept;

    /// The record whose source is this path, or null. For the importer, which starts from a file.
    [[nodiscard]] const AssetMeta* find_by_source(const VirtualPath& source) noexcept;

    /// Record that an asset moved. The id is unchanged, which is the entire point of the model.
    [[nodiscard]] Status rebind_source(cy::AssetId id, const VirtualPath& source) noexcept;

    /// Forget an asset. The id is not reused — nothing here mints one.
    [[nodiscard]] Status unregister(cy::AssetId id) noexcept;

    [[nodiscard]] usize size() const noexcept;
    void clear() noexcept;

    /// How many registrations have been refused as collisions, and what the most recent one was.
    [[nodiscard]] u64 collisions() const noexcept { return collisions_; }
    [[nodiscard]] const MetaCollision& last_collision() const noexcept { return last_collision_; }

    /// Visit every record, in ascending id order, so a listing is reproducible.
    using Visitor = void (*)(void* user, const AssetMeta& meta) noexcept;
    void for_each(Visitor visitor, void* user) noexcept;

private:
    /// Sorted by id. A sorted vector rather than a hash table: the database is built once and read
    /// many times, iteration must be ordered for a reproducible listing, and a binary search over a
    /// contiguous array beats a hash probe at the sizes a project reaches.
    Array<AssetMeta> records_;
    bool sorted_ = true;

    void ensure_sorted() noexcept;
    [[nodiscard]] usize lower_bound(cy::AssetId id) noexcept;

    u64 collisions_ = 0;
    MetaCollision last_collision_{};
};

}  // namespace cy::assets

#endif  // CY_CORE_ASSETS_IDENTITY_H
