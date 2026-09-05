// cy/abi/module.h — module manifests, images, generations, and hot reload. Tasks 2.4, 2.6, 2.7.
//
// --- THE RELOAD MODEL, AND WHY IT DOES NOT CONTAIN A dlclose -----------------------------------
//
// M4's spike (openspec/changes/implement-m4-playable/tasks.md §0) measured this rather than
// reasoning about it, over 40 consecutive edit/rebuild/reload cycles including a type-layout
// change. Three findings decide the shape of this file, and every one of them was a *negative*
// result:
//
//   1. STATE DOES NOT SURVIVE IN PLACE, AND FAILS SILENTLY WHEN YOU ASSUME IT DOES. Keeping the
//      instance pointers across a module swap looks perfect while the layout is unchanged, and
//      after a layout change there is no trap and no diagnostic — the new code read the old
//      object's `ammo` as `health`, and the raw bits of a Swift `String` as a `Double`. State
//      survives only by serialize -> migrate-by-name -> recreate.
//
//   2. dlclose OF A SWIFT IMAGE IS UNSAFE WHENEVER THE SWIFT RUNTIME OUTLIVES THE MODULE — which is
//      the real engine configuration. Two process-global runtime structures keep pointers into the
//      unloaded image: the foreign (imported C) type metadata cache interns by name and holds the
//      first image's rodata string, and the protocol-conformance section list keeps the unloaded
//      image's section. Worse, dlclose really does unmap, and the *next* image is then mapped over
//      the same addresses — so a stale call is a jump into unrelated live code rather than a
//      reliable crash.
//
//   3. A UNIQUE FILENAME IS NOT ENOUGH; THE SWIFT MODULE NAME MUST DIFFER TOO. Name-based type
//      lookup (`Codable`, `_typeByName`, reflection) is process-global and first-registration-wins:
//      with two resident images both named `CyGame`, the *new* image asking for its own type got
//      the *old* image's metadata. Since images are never unloaded, same-named generations would
//      accumulate.
//
// So a reload is: quiesce, serialize through the vtable of the generation that created each
// instance, destroy through that same vtable, call the old image's shutdown, drop the host's copy
// of its vtables, `dlopen` a NEW FILE (a new path and a unique Swift `-module-name` per
// generation), open the next generation, call the entry point at each init level, recreate and
// restore by name with migration. The old image stays mapped for the process lifetime.
//
// THE SPECIFICATION SAYS "UNLOAD THE LIBRARY" AND THE SPECIFICATION IS WRONG. The amendment is
// carried in openspec/changes/implement-m4-playable/specs/native-abi/spec.md: development builds
// retire an image rather than unloading it, and only Shipping — which has no reload at all — ever
// unloads. Everything else in that requirement is confirmed by measurement.
//
// WHAT IT COSTS, SO THAT "LEAK EVERY GENERATION" HAS A NUMBER: 58-85 kB of virtual address space
// per reload, never reclaimed, and roughly 0.1-0.6 ms per reload, flat across 40 generations. A
// session doing a thousand reloads spends under 90 MB of address space. If that ever matters the
// mitigation is a process restart, not a `dlclose`.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::abi {

/// The platforms a manifest may name a library for. The same set, spelled the same way, as
/// `CY_MODULE_PLATFORMS` in cmake/modules.cmake — a manifest is read by the build and by the
/// loader, and two spellings of "linux" would be one bug.
enum class ModulePlatform : u8 { Linux, Windows, MacOS, Ios, Android, VisionOs, Web, Count };

[[nodiscard]] const char* module_platform_name(ModulePlatform platform) noexcept;

/// The host's own platform, which is the entry the loader reads.
[[nodiscard]] ModulePlatform host_module_platform() noexcept;

/// What `module.toml` declares. `native-abi`: "module name, entry symbol, minimum ABI version,
/// per-platform library paths, and whether the module is hot-reloadable".
///
/// Every string points into the text the manifest was parsed from, which the caller therefore keeps
/// alive. That is what lets this parse with no allocation for the strings themselves, and it is
/// checked by `ModuleManifest::parse` taking the text by reference.
struct ModuleManifest {
    const char* name = "";
    const char* entry_symbol = "cy_module_entry";
    u32 min_abi_major = CY_ABI_MAJOR;
    u32 min_abi_minor = 0;
    bool hot_reload = false;
    /// Indexed by `ModulePlatform`. Empty for a platform the module does not ship for.
    const char* library[static_cast<usize>(ModulePlatform::Count)] = {};

    [[nodiscard]] const char* library_for(ModulePlatform platform) const noexcept;
};

/// Parse a `module.toml`. `text` is modified in place — keys and values are NUL-terminated where
/// they sit — and must outlive the manifest.
///
/// THE SUBSET IS DELIBERATE AND SMALL: top-level `key = value` pairs, `[platform.<name>]` tables
/// with a `library` key, strings in double quotes, integers, `true`/`false`, `#` comments. A module
/// manifest is a handful of declarations, and the alternative — a TOML library at layer 6 loaded
/// before the engine is up — buys nothing this needs. An unknown key is an error naming it rather
/// than being ignored, which is `project-and-plugins`' rule for the build-time manifests and has
/// the same reason here: a typo that is skipped is a setting that silently did not apply.
[[nodiscard]] Expected<ModuleManifest, Error> parse_module_manifest(char* text) noexcept;

/// One loaded shared-library image, and the generation it belongs to.
///
/// An image is never unloaded while the process lives. `close()` exists for the ONE case in which
/// unloading is safe: a library that did not export its declared entry symbol, so none of its code
/// ever ran, nothing of it was registered, and no runtime cache anywhere holds a pointer into it.
///
/// An image whose entry point RAN AND RETURNED FALSE is not that case, and is not closed. Its code
/// executed, which on a Swift image is enough to intern foreign type metadata by name and to leave
/// its conformance section on a process-global list — the two structures the spike measured
/// dangling after a `dlclose`.
class ModuleImage {
public:
    ModuleImage() noexcept = default;
    ~ModuleImage();

    ModuleImage(const ModuleImage&) = delete;
    ModuleImage& operator=(const ModuleImage&) = delete;
    ModuleImage(ModuleImage&& other) noexcept;
    ModuleImage& operator=(ModuleImage&& other) noexcept;

    /// `dlopen(path, RTLD_NOW | RTLD_LOCAL)` and resolve `entry_symbol`. Fails with
    /// CY_RESULT_MODULE_LOAD_FAILED and the loader's own message on either step.
    [[nodiscard]] static Expected<ModuleImage, Error> open(const char* path,
                                                           const char* entry_symbol) noexcept;

    [[nodiscard]] bool is_open() const noexcept { return handle_ != nullptr; }
    [[nodiscard]] CyModuleEntryFn entry() const noexcept { return entry_; }
    [[nodiscard]] CyModuleShutdownFn shutdown() const noexcept { return shutdown_; }
    [[nodiscard]] u32 generation() const noexcept { return generation_; }
    void set_generation(u32 generation) noexcept { generation_ = generation; }

    /// Unload. Legal ONLY for an image that registered nothing — see the class comment. Named
    /// `close` rather than `unload` so that a reader looking for the unload step in the reload
    /// sequence does not find one.
    void close() noexcept;

private:
    void* handle_ = nullptr;
    CyModuleEntryFn entry_ = nullptr;
    CyModuleShutdownFn shutdown_ = nullptr;
    u32 generation_ = 0;
};

/// A live behaviour instance, and the generation whose code created it.
///
/// THE GENERATION IS NOT BOOKKEEPING. The spike ran v2 code against a v1 object and got
/// health = 17, shield = 1 and mana = 3.5e18 with no diagnostic: every call on an instance must be
/// resolved through the vtable of the generation that created it, never through "the current one".
/// That is why the record pointer is stored per instance rather than looked up by name at use.
struct BehaviourInstance {
    CyInstance instance = nullptr;
    BehaviourRecord* record = nullptr;
    CyEntity entity = CY_ENTITY_NULL;
    u32 generation = 0;
};

/// Why a reload was refused. Returned rather than logged, because the loader's caller is what keeps
/// the previous generation live and it needs to know which failure it is looking at.
enum class ReloadFailure : u8 {
    None,
    ImageDidNotOpen,    ///< dlopen failed, or the entry symbol is missing
    EntryRefused,       ///< `cy_module_entry` returned false — an ABI or dependency mismatch
    TypeNotRegistered,  ///< a live instance's behaviour type is not in the new image
    SchemaTooNew,       ///< the new module's schema predates the blob — "Incompatible reload"
    RestoreFailed,      ///< `deserialize` reported a failure that is not a schema ordering problem
};

[[nodiscard]] const char* reload_failure_name(ReloadFailure failure) noexcept;

/// What a reload did, whether or not it succeeded.
struct ReloadReport {
    ReloadFailure failure = ReloadFailure::None;
    u32 generation = 0;        ///< the generation now live
    u32 instances = 0;         ///< instances carried across
    u32 bytes_serialized = 0;  ///< the total blob size, for the cost this milestone has to state
    const char* detail = "";   ///< the offending type's name where there is one
};

/// The thing that owns live behaviour instances and performs the reload sequence.
///
/// It is deliberately not the ECS and not the Swift overlay: it is the smallest object that can
/// hold "every live instance and the generation that made it", which is exactly what the reload
/// algorithm needs and what nothing else in the engine has a reason to hold.
///
/// IT MUST NOT OUTLIVE ITS HOST. Its destructor destroys every live instance through the record its
/// generation registered, and those records are owned by the `Host`; a runtime destroyed after its
/// host would be calling through freed vtables. Declare the host first and the runtime after it, so
/// that ordinary reverse destruction order is the correct one.
///
/// IT IS NOT THREAD-SAFE, and neither is `Host`. Loading, reloading and creating instances happen
/// at the frame boundary the reload quiesces at — `native-abi` requires the quiesce, and having it
/// makes a lock here a lock that would only ever be uncontended.
class BehaviourRuntime {
public:
    BehaviourRuntime(Allocator& allocator, Host& host) noexcept;
    ~BehaviourRuntime();

    BehaviourRuntime(const BehaviourRuntime&) = delete;
    BehaviourRuntime& operator=(const BehaviourRuntime&) = delete;
    BehaviourRuntime(BehaviourRuntime&&) = delete;
    BehaviourRuntime& operator=(BehaviourRuntime&&) = delete;

    /// Load the first image and bring it up through every init level. The path is the library the
    /// manifest names for this platform; the manifest itself is borrowed and must outlive the call.
    [[nodiscard]] Expected<ReloadReport, Error> load(const ModuleManifest& manifest,
                                                     const char* library_path) noexcept;

    /// Create an instance of a registered behaviour type. The instance remembers this generation.
    [[nodiscard]] Expected<u32, Error> create(const char* type_name, CyEntity entity) noexcept;

    /// Destroy one instance through the vtable of the generation that created it.
    [[nodiscard]] Status destroy(u32 slot) noexcept;

    /// One fixed tick over every live instance, each through its own generation's vtable.
    void fixed_update(f32 dt) noexcept;

    /// THE RELOAD. `library_path` MUST be a different file from the current one — see the header
    /// comment, item 3. Returns a report; on failure the previous generation is still live and
    /// every instance is still valid, which is `native-abi`'s "Incompatible reload" scenario.
    [[nodiscard]] Expected<ReloadReport, Error> reload(const char* library_path) noexcept;

    [[nodiscard]] u32 generation() const noexcept { return host_.generation; }
    [[nodiscard]] u32 live_instances() const noexcept;
    [[nodiscard]] const BehaviourInstance* instance(u32 slot) const noexcept;
    /// Images retired so far, including the live one. The address-space cost is this times the
    /// image size, and the number a long editor session would watch.
    [[nodiscard]] u32 images() const noexcept { return static_cast<u32>(images_.size()); }

private:
    struct SavedInstance {
        u32 slot = 0;
        u32 schema = 0;
        u32 offset = 0;
        u32 size = 0;
        const char* type_name = "";
        CyEntity entity = CY_ENTITY_NULL;
    };

    [[nodiscard]] Status bring_up(ModuleImage& image) noexcept;
    [[nodiscard]] Expected<u32, Error> quiesce_and_save(Array<SavedInstance>& saved,
                                                        Array<u8>& blobs) noexcept;
    [[nodiscard]] ReloadFailure restore(const Array<SavedInstance>& saved, const Array<u8>& blobs,
                                        const char** detail) noexcept;

    Allocator& allocator_;
    Host& host_;
    Array<ModuleImage> images_;
    Array<BehaviourInstance> instances_;
    CyModuleInit init_{};
    const ModuleManifest* manifest_ = nullptr;
};

}  // namespace cy::abi
