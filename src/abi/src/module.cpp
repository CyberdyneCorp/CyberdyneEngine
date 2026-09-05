// Images, generations, and the reload sequence. Tasks 2.6 and 2.7.
//
// The algorithm is the one M4's spike measured, and the ordering below is not the obvious one. The
// obvious one — destroy, shut down, then load — cannot report a failed reload, because by the time
// `dlopen` fails there is nothing left to keep live. So the sequence here does everything that can
// fail BEFORE anything that cannot be undone:
//
//   1. open the new image and resolve its entry symbol            (fails: nothing has changed)
//   2. serialize every live instance through its own vtable       (fails: nothing has changed)
//   3. open the next generation and run the new entry point       (fails: abandon it, nothing lost)
//   4. check every saved type exists in the new generation and    (fails: abandon it, nothing lost)
//      that its schema is not older than the blob
//   5. destroy the old instances through their own vtables, and
//      shut the old image down                                    -- the point of no return
//   6. create instances from the new vtables and restore by name
//
// Steps 1 to 4 are exactly `native-abi`'s "Incompatible reload": "the reload SHALL be rejected with
// a diagnostic and the old module retained".
//
// AND THERE IS NO dlclose IN ANY OF THEM. See cy/abi/module.h for the measurements that decided
// that: dlclose of a Swift image is unsafe whenever the Swift runtime outlives the module, the next
// image is then mapped over the same addresses, and a 20-cycle test passed by luck when two images
// happened to be the same size. A retired image stays mapped for the process lifetime and its
// instances stay destroyable, which is why calling a retired generation's vtable is safe rather
// than merely untested.

#include <cy/abi/module.h>

#include <cy/abi/errors.h>
#include <cy/core/base/diagnostic_sink.h>

#include <cstring>

#if defined(_WIN32)
#    include <windows.h>
#else
#    include <dlfcn.h>
#endif

namespace cy::abi {
namespace {

// The init levels a game module is brought up through, in order. `Editor` is deliberately absent:
// it exists in the enum because `cy::config::ModuleLevel` has it and the two must agree, and a game
// module is not an editor module. When the editor arrives at M5 it brings its own level up itself.
constexpr CyInitLevel kLevels[] = {CY_INIT_LEVEL_CORE, CY_INIT_LEVEL_SERVERS, CY_INIT_LEVEL_SCENE};

void* open_library(const char* path) noexcept {
#if defined(_WIN32)
    // UNVERIFIED. This machine is Linux only; the Windows path is written from the documented API
    // and has never been executed. The reload findings behind this file are ELF and glibc specific
    // and must be re-measured on Windows before this branch is relied on.
    return static_cast<void*>(LoadLibraryA(path));
#else
    // RTLD_NOW because a lazy binding failure would surface as a crash inside a module callback
    // rather than as a failed load. RTLD_LOCAL because a module's symbols are its own: the spike
    // measured that two images can define identical mangled Swift symbols with no interposition,
    // and making them global would only add a way for that to stop being true.
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

void* find_symbol(void* handle, const char* name) noexcept {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

const char* library_error() noexcept {
#if defined(_WIN32)
    return "the dynamic loader refused the library";
#else
    const char* message = dlerror();
    return (message != nullptr) ? message : "the dynamic loader refused the library";
#endif
}

// A function pointer out of a `void*`. POSIX guarantees the round trip for `dlsym` and ISO C++ does
// not, which is why this is one named helper with one cast rather than a cast at each call site.
template <class Fn>
Fn function_cast(void* symbol) noexcept {
    Fn result = nullptr;
    std::memcpy(static_cast<void*>(&result), static_cast<void*>(&symbol), sizeof(result));
    return result;
}

}  // namespace

const char* reload_failure_name(ReloadFailure failure) noexcept {
    switch (failure) {
        case ReloadFailure::None:
            return "none";
        case ReloadFailure::ImageDidNotOpen:
            return "the image did not open";
        case ReloadFailure::EntryRefused:
            return "the entry point returned false";
        case ReloadFailure::TypeNotRegistered:
            return "a live type is not in the new module";
        case ReloadFailure::SchemaTooNew:
            return "the new module's schema predates the saved state";
        case ReloadFailure::RestoreFailed:
            return "an instance refused to restore";
    }
    return "unknown";
}

// --- ModuleImage
// ----------------------------------------------------------------------------------

ModuleImage::~ModuleImage() {
    // DELIBERATELY EMPTY. An image is retired, not unloaded — see the file header. `close()` is the
    // one way to unload, and it is called only for an image that registered nothing.
    handle_ = nullptr;
}

ModuleImage::ModuleImage(ModuleImage&& other) noexcept
    : handle_(other.handle_),
      entry_(other.entry_),
      shutdown_(other.shutdown_),
      generation_(other.generation_) {
    other.handle_ = nullptr;
    other.entry_ = nullptr;
    other.shutdown_ = nullptr;
}

ModuleImage& ModuleImage::operator=(ModuleImage&& other) noexcept {
    if (this != &other) {
        handle_ = other.handle_;
        entry_ = other.entry_;
        shutdown_ = other.shutdown_;
        generation_ = other.generation_;
        other.handle_ = nullptr;
        other.entry_ = nullptr;
        other.shutdown_ = nullptr;
    }
    return *this;
}

Expected<ModuleImage, Error> ModuleImage::open(const char* path,
                                               const char* entry_symbol) noexcept {
    if (path == nullptr || entry_symbol == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a module needs a path and an entry symbol");
    }
    void* handle = open_library(path);
    if (handle == nullptr) {
        return fail(ErrorCode::NotFound, library_error());
    }
    void* entry = find_symbol(handle, entry_symbol);
    if (entry == nullptr) {
        // NONE OF THIS IMAGE'S CODE HAS RUN. `dlopen` executed its initialisers, but a library
        // that does not export the entry symbol registered nothing, interned no type metadata under
        // the engine's name, and holds no instance — so this is the one image it is safe to unload.
        // Leaving it mapped would also be safe; closing it stops a typo in a manifest from costing
        // an image per attempt.
        ModuleImage image;
        image.handle_ = handle;
        image.close();
        return fail(ErrorCode::NotFound, "the module does not export its declared entry symbol");
    }

    ModuleImage image;
    image.handle_ = handle;
    image.entry_ = function_cast<CyModuleEntryFn>(entry);
    image.shutdown_ = function_cast<CyModuleShutdownFn>(find_symbol(handle, "cy_module_shutdown"));
    return image;
}

void ModuleImage::close() noexcept {
    if (handle_ == nullptr) {
        return;
    }
#if defined(_WIN32)
    (void)FreeLibrary(static_cast<HMODULE>(handle_));
#else
    (void)dlclose(handle_);
#endif
    handle_ = nullptr;
    entry_ = nullptr;
    shutdown_ = nullptr;
}

// --- BehaviourRuntime
// -----------------------------------------------------------------------------

BehaviourRuntime::BehaviourRuntime(Allocator& allocator, Host& host) noexcept
    : allocator_(allocator), host_(host), images_(allocator), instances_(allocator) {}

BehaviourRuntime::~BehaviourRuntime() {
    // Destroy through the vtable of the generation that created each instance. Retired images are
    // still mapped, so this is correct for an instance from generation 0 in a process that reached
    // generation 40 — the spike called a retired generation's vtable deliberately and got the right
    // answers.
    for (BehaviourInstance& live : instances_) {
        if (live.instance != nullptr && live.record != nullptr &&
            live.record->vtable.destroy != nullptr) {
            live.record->vtable.destroy(live.instance, live.record->vtable.user_data);
        }
        live.instance = nullptr;
    }
}

u32 BehaviourRuntime::live_instances() const noexcept {
    u32 count = 0;
    for (const BehaviourInstance& live : instances_) {
        count += (live.instance != nullptr) ? 1U : 0U;
    }
    return count;
}

const BehaviourInstance* BehaviourRuntime::instance(u32 slot) const noexcept {
    return (slot < instances_.size()) ? &instances_[slot] : nullptr;
}

Status BehaviourRuntime::bring_up(ModuleImage& image) noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    if (iface == nullptr) {
        return fail(ErrorCode::Internal, "the engine did not return its own interface table");
    }

    CyModuleInit init{};
    if (!image.entry()(iface, &host_, &init)) {
        // `native-abi`'s "Entry point returns false": reported, engine startup not aborted. The
        // module's own message, if it set one, is more useful than anything this layer could say.
        const char* detail = last_error_message();
        emit_diagnostic(DiagnosticSeverity::Error, "abi",
                        (detail[0] != '\0') ? detail : "a module's entry point returned false");
        return fail(ErrorCode::Unavailable, "the module's entry point returned false");
    }
    if (init.struct_size < sizeof(CyModuleInit)) {
        return fail(ErrorCode::Unsupported,
                    "the module filled in a CyModuleInit older than this engine's");
    }
    if (init.abi_major != CY_ABI_MAJOR) {
        return fail(ErrorCode::Unsupported, "the module was built against another ABI major");
    }

    init_ = init;
    image.set_generation(host_.generation);
    if (init_.initialize != nullptr) {
        for (const CyInitLevel level : kLevels) {
            init_.initialize(&host_, level, init_.user_data);
        }
    }
    return ok();
}

Expected<ReloadReport, Error> BehaviourRuntime::load(const ModuleManifest& manifest,
                                                     const char* library_path) noexcept {
    if (!images_.empty()) {
        return fail(ErrorCode::AlreadyExists, "this runtime already has a module loaded");
    }
    if (manifest.min_abi_major != CY_ABI_MAJOR || manifest.min_abi_minor > CY_ABI_MINOR) {
        return fail(ErrorCode::Unsupported,
                    "the manifest requires an ABI version this engine does not export");
    }

    Expected<ModuleImage, Error> opened = ModuleImage::open(library_path, manifest.entry_symbol);
    if (!opened) {
        return make_unexpected(opened.error());
    }
    if (Status reserved = images_.reserve(images_.size() + 1); !reserved) {
        return make_unexpected(reserved.error());
    }
    (void)images_.push_back(static_cast<ModuleImage&&>(opened.value()));

    if (Status up = bring_up(images_.back()); !up) {
        images_.pop_back();
        return make_unexpected(up.error());
    }

    manifest_ = &manifest;
    ReloadReport report;
    report.generation = host_.generation;
    return report;
}

Expected<u32, Error> BehaviourRuntime::create(const char* type_name, CyEntity entity) noexcept {
    CyBehaviourType record = host_.find_behaviour(type_name);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "no behaviour of that name in the current generation");
    }
    CyInstance created = record->vtable.create(&host_, entity, record->vtable.user_data);
    if (created == nullptr) {
        return fail(ErrorCode::Unavailable, "the module refused to create the behaviour");
    }

    BehaviourInstance live;
    live.instance = created;
    live.record = record;
    live.entity = entity;
    live.generation = record->generation;
    if (Status pushed = instances_.push_back(live); !pushed) {
        record->vtable.destroy(created, record->vtable.user_data);
        return make_unexpected(pushed.error());
    }
    return static_cast<u32>(instances_.size() - 1);
}

Status BehaviourRuntime::destroy(u32 slot) noexcept {
    if (slot >= instances_.size() || instances_[slot].instance == nullptr) {
        return fail(ErrorCode::NotFound, "no live instance in that slot");
    }
    BehaviourInstance& live = instances_[slot];
    live.record->vtable.destroy(live.instance, live.record->vtable.user_data);
    live.instance = nullptr;
    return ok();
}

void BehaviourRuntime::fixed_update(f32 dt) noexcept {
    for (BehaviourInstance& live : instances_) {
        if (live.instance != nullptr && live.record->vtable.fixed_update != nullptr) {
            live.record->vtable.fixed_update(live.instance, dt, live.record->vtable.user_data);
        }
    }
}

Expected<u32, Error> BehaviourRuntime::quiesce_and_save(Array<SavedInstance>& saved,
                                                        Array<u8>& blobs) noexcept {
    for (u32 slot = 0; slot < instances_.size(); ++slot) {
        BehaviourInstance& live = instances_[slot];
        if (live.instance == nullptr) {
            continue;
        }
        SavedInstance entry;
        entry.slot = slot;
        entry.schema = live.record->vtable.schema_version;
        entry.type_name = live.record->name;
        entry.entity = live.entity;
        entry.offset = static_cast<u32>(blobs.size());

        // THROUGH THE VTABLE OF THE GENERATION THAT CREATED IT. Not the current one: the spike ran
        // v2 code over a v1 object and read the raw bits of a Swift String as a Double, with no
        // trap and no diagnostic.
        u32 required = 0;
        if (live.record->vtable.serialize != nullptr) {
            required = live.record->vtable.serialize(live.instance, nullptr, 0,
                                                     live.record->vtable.user_data);
        }
        if (required != 0) {
            if (Status grown = blobs.resize(blobs.size() + required); !grown) {
                return make_unexpected(grown.error());
            }
            const u32 written =
                live.record->vtable.serialize(live.instance, blobs.data() + entry.offset, required,
                                              live.record->vtable.user_data);
            if (written != required) {
                return fail(ErrorCode::Internal,
                            "a behaviour's serialize disagreed with its own size query");
            }
        }
        entry.size = required;
        if (Status pushed = saved.push_back(entry); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    return static_cast<u32>(blobs.size());
}

ReloadFailure BehaviourRuntime::restore(const Array<SavedInstance>& saved, const Array<u8>& blobs,
                                        const char** detail) noexcept {
    for (const SavedInstance& entry : saved) {
        CyBehaviourType record = host_.find_behaviour(entry.type_name);
        // Pre-checked before the point of no return, so reaching a null here would be a defect in
        // this file rather than in a module.
        if (record == nullptr) {
            *detail = entry.type_name;
            return ReloadFailure::TypeNotRegistered;
        }
        CyInstance created = record->vtable.create(&host_, entry.entity, record->vtable.user_data);
        if (created == nullptr) {
            *detail = entry.type_name;
            return ReloadFailure::RestoreFailed;
        }
        BehaviourInstance& live = instances_[entry.slot];
        live.instance = created;
        live.record = record;
        live.generation = record->generation;

        if (entry.size != 0 && record->vtable.deserialize != nullptr) {
            const int32_t status =
                record->vtable.deserialize(created, blobs.data() + entry.offset, entry.size,
                                           entry.schema, record->vtable.user_data);
            if (status != CY_RESULT_OK) {
                *detail = entry.type_name;
                return (status == CY_RESULT_SCHEMA_TOO_NEW) ? ReloadFailure::SchemaTooNew
                                                            : ReloadFailure::RestoreFailed;
            }
        }
    }
    return ReloadFailure::None;
}

Expected<ReloadReport, Error> BehaviourRuntime::reload(const char* library_path) noexcept {
    if (images_.empty() || manifest_ == nullptr) {
        return fail(ErrorCode::Unavailable, "nothing is loaded to reload");
    }
    if (!manifest_->hot_reload) {
        return fail(ErrorCode::PermissionDenied, "this module is not declared hot-reloadable");
    }

    ReloadReport report;
    report.generation = host_.generation;

    // 1. The new image. A NEW FILE, always: see cy/abi/module.h item 3 — a unique path and, for a
    //    Swift module, a unique `-module-name` per generation, because name-based type lookup is
    //    process-global and first-registration-wins.
    Expected<ModuleImage, Error> opened = ModuleImage::open(library_path, manifest_->entry_symbol);
    if (!opened) {
        report.failure = ReloadFailure::ImageDidNotOpen;
        // The dynamic loader's own message, which on glibc lives in a per-thread buffer the next
        // `dlerror()` overwrites. It is a diagnostic to report now, not a string to keep.
        report.detail = opened.error().message;
        return report;
    }

    // 2. Serialize, with nothing destroyed yet.
    Array<SavedInstance> saved(allocator_);
    Array<u8> blobs(allocator_);
    Expected<u32, Error> serialized = quiesce_and_save(saved, blobs);
    if (!serialized) {
        return make_unexpected(serialized.error());
    }
    report.bytes_serialized = serialized.value();

    // 3. The next generation, and the new module's registrations into it.
    //
    // `bring_up` overwrites `init_` with the new module's, so the old module's shutdown entries are
    // captured first. Calling the new module's `shutdown` to shut the old one down would be a
    // plausible-looking way to run a module's teardown against state it never created.
    const CyModuleInit previous_init = init_;
    if (Status reserved = images_.reserve(images_.size() + 1); !reserved) {
        return make_unexpected(reserved.error());
    }
    host_.open_generation();
    (void)images_.push_back(static_cast<ModuleImage&&>(opened.value()));
    if (Status up = bring_up(images_.back()); !up) {
        images_.pop_back();
        host_.abandon_generation();
        init_ = previous_init;
        report.failure = ReloadFailure::EntryRefused;
        report.detail = up.error().message;
        report.generation = host_.generation;
        return report;
    }

    // 4. Every live type must exist in the new generation, with a schema that does not predate the
    //    blob. This is the last point at which the old generation can still be kept, so both checks
    //    happen here rather than being discovered halfway through restoring.
    for (const SavedInstance& entry : saved) {
        CyBehaviourType record = host_.find_behaviour(entry.type_name);
        if (record == nullptr) {
            report.failure = ReloadFailure::TypeNotRegistered;
            report.detail = entry.type_name;
        } else if (record->vtable.schema_version < entry.schema) {
            report.failure = ReloadFailure::SchemaTooNew;
            report.detail = entry.type_name;
        } else {
            continue;
        }
        images_.pop_back();
        host_.abandon_generation();
        init_ = previous_init;
        report.generation = host_.generation;
        emit_diagnosticf(DiagnosticSeverity::Error, "abi",
                         "reload refused: %s (%s); the previous generation is still live",
                         reload_failure_name(report.failure), report.detail);
        return report;
    }

    // 5. The point of no return. Old instances die through their own vtables, then the old image is
    //    shut down — and NOT unloaded.
    ModuleImage& previous = images_[images_.size() - 2];
    for (const SavedInstance& entry : saved) {
        BehaviourInstance& live = instances_[entry.slot];
        live.record->vtable.destroy(live.instance, live.record->vtable.user_data);
        live.instance = nullptr;
    }
    if (previous_init.shutdown != nullptr) {
        for (usize index = sizeof(kLevels) / sizeof(kLevels[0]); index > 0; --index) {
            previous_init.shutdown(&host_, kLevels[index - 1], previous_init.user_data);
        }
    }
    if (previous.shutdown() != nullptr) {
        previous.shutdown()();
    }

    // 6. Recreate and restore by name.
    report.failure = restore(saved, blobs, &report.detail);
    report.generation = host_.generation;
    report.instances = live_instances();
    if (report.failure != ReloadFailure::None) {
        emit_diagnosticf(DiagnosticSeverity::Error, "abi", "reload restored partially: %s (%s)",
                         reload_failure_name(report.failure), report.detail);
    }
    return report;
}

}  // namespace cy::abi
