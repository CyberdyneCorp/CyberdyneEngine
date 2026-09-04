// Module registration levels, and the order the runtime brings them up and takes them down.
//
// Task 4.5. `engine-architecture`'s "Deterministic startup and shutdown" fixes eleven stages, four
// of which are *modules at a registration level* — Core, Servers, Scene, Editor. src/runtime/ owns
// the eleven; this owns the four, because the order within a level is a property of the project
// graph rather than of the runtime.
//
// THE ORDER IS RECORDED, NOT EMERGENT. A registry sorts what is added to it by (level, name) and
// journals what it actually started and stopped. Neither the order modules were added in, nor a
// pointer value, nor a static initialiser can reach the result: `add()` inserts into a sorted
// array, so two hosts that register the same modules in different orders start them in the same
// one. The journal is the evidence — src/runtime/tests/test_module_order.cpp compares it across a
// hundred processes, which is what makes "deterministic" a measurement rather than a claim.
//
// REGISTRATION IS EXPLICIT, NEVER A STATIC INITIALISER. Static initialisation order is link order,
// and link order is exactly the non-determinism this requirement exists to remove. A module is
// registered by a call, from a host that decided to make it.
//
// LEVELS COME UP IN ORDER AND GO DOWN IN REVERSE, and the registry refuses anything else: start()
// requires every lower level to be running, stop() requires every higher level to have stopped. An
// out-of-order call is a programmer error that returns an Error rather than tripping an assertion,
// because CY_ASSERT is compiled out of Profile and Shipping and this must hold in all four.

#pragma once

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <span>

namespace cy::config {

/// The registration levels, in initialisation order. The same four names, in the same order, as
/// cmake/modules.cmake's CY_REGISTRATION_LEVELS and the manifest's `registration_level`.
///
/// A level is not a layer. A level says *when* a module initialises; a layer says what it may
/// depend on. A `Scene`-level module may sit at the core layer.
enum class ModuleLevel : u8 {
    Core = 0,  // before the display server exists
    Servers,   // after the servers, before the world
    Scene,     // after the world exists
    Editor,    // tools builds only
};

inline constexpr usize kModuleLevelCount = 4;

/// The enumerator's own spelling, and the manifest's. Never null.
const char* module_level_name(ModuleLevel level) noexcept;

/// The inverse, for a manifest string. Unknown names are an error naming the four.
Expected<ModuleLevel, Error> module_level_from_name(const char* name);

/// What a module does at its level. Registering is fallible — a module that cannot register is
/// reported and the startup unwinds; unregistering is not, because it runs during teardown where
/// there is nothing left to report a failure to.
using ModuleRegisterFn = Status (*)(void* user);
using ModuleUnregisterFn = void (*)(void* user);

struct ModuleRegistration {
    /// Must outlive the registry: it is the key the order is decided by and the name a diagnostic
    /// prints. A string literal, or the manifest's own storage.
    const char* name = nullptr;
    ModuleLevel level = ModuleLevel::Core;
    ModuleRegisterFn on_register = nullptr;
    ModuleUnregisterFn on_unregister = nullptr;
    void* user = nullptr;
};

/// Sized so the registry is an ordinary member of the runtime rather than an allocation. A project
/// with more modules than this is a project that has outgrown a fixed array, and the failure says
/// so rather than truncating.
inline constexpr usize kMaxRegisteredModules = 64;

class ModuleRegistry {
public:
    ModuleRegistry() = default;
    ~ModuleRegistry() = default;

    ModuleRegistry(const ModuleRegistry&) = delete;
    ModuleRegistry& operator=(const ModuleRegistry&) = delete;

    /// Insert in (level, name) order. Rejects a null or empty name, a duplicate name, a level
    /// outside the four, and an addition made while any level is running.
    Status add(const ModuleRegistration& registration);

    /// Add every module the project graph declares, from the generated cy_project.h — descriptors
    /// only, with no hooks. This is what makes the runtime's order the *manifest's* order: a module
    /// that is in the graph is initialised at the level the graph gives it, whether or not any code
    /// has been bound to it yet. `bind()` attaches the hooks afterwards.
    Status add_project_modules();

    /// Attach hooks to a module already in the registry. Fails naming the module when it is not
    /// there, which is the diagnostic a typo in a module name deserves.
    Status bind(const char* name, ModuleRegisterFn on_register, ModuleUnregisterFn on_unregister,
                void* user = nullptr);

    [[nodiscard]] std::span<const ModuleRegistration> modules() const;
    [[nodiscard]] usize size() const { return count_; }
    [[nodiscard]] const ModuleRegistration* find(const char* name) const;

    /// Register every module at `level`, in order. A module whose registration fails unregisters
    /// the ones this call already registered, in reverse, and returns an error naming it — the
    /// level is left exactly as it was found.
    Status start(ModuleLevel level);

    /// Unregister every module at `level`, in the exact reverse of the order start() used.
    /// Idempotent: stopping a level that is not running does nothing.
    void stop(ModuleLevel level);

    [[nodiscard]] bool level_started(ModuleLevel level) const;

    /// The names of the modules registered, and unregistered, since the last clear_journals(), in
    /// the order it happened. Both are the evidence for the ordering claim; nothing else in this
    /// class reads them.
    [[nodiscard]] std::span<const char* const> start_journal() const;
    [[nodiscard]] std::span<const char* const> stop_journal() const;

    /// True when everything started has been stopped and the stop journal is the exact reverse of
    /// the start journal. Checked in every configuration, never only under an assertion.
    [[nodiscard]] bool journal_is_reversed() const;

    /// A journal longer than it can hold has stopped being evidence, and says so rather than
    /// silently reporting the prefix it kept.
    [[nodiscard]] bool journal_overflowed() const { return overflowed_; }

    void clear_journals();

private:
    static constexpr usize kJournalCapacity = kMaxRegisteredModules * 2;

    void record_start(const char* name);
    void record_stop(const char* name);

    ModuleRegistration modules_[kMaxRegisteredModules] = {};
    usize count_ = 0;

    bool started_[kModuleLevelCount] = {};

    const char* start_journal_[kJournalCapacity] = {};
    const char* stop_journal_[kJournalCapacity] = {};
    usize start_count_ = 0;
    usize stop_count_ = 0;
    bool overflowed_ = false;
};

}  // namespace cy::config
