#pragma once
// The plugin lifecycle, the extension points, and type ownership.
// `project-and-plugins`, M11.b tasks 2.1, 2.2 and 2.5.
//
// ================================================================================================
// EIGHT PHASES, AND WHY REGISTER IS SEPARATE FROM START
// ================================================================================================
//
// *"Plugin loading SHALL proceed through explicit, separable phases: load, initialise, register,
// start, stop, unregister, shutdown, unload."* They are separable because the requirement's first
// scenario needs them to be: *"WHEN a plugin contributes component types THEN it SHALL register
// them during the register phase, before any world is created."* A host that initialised and
// started in one call could not honour that, and a plugin that registered a component type in
// `start` would be registering it into a world that already exists.
//
// ================================================================================================
// A FAILING PLUGIN IS CONTAINED, AND CONTAINMENT IS THE DEFAULT
// ================================================================================================
//
// *"Failure in any phase SHALL be contained: the plugin SHALL be reported and disabled, and the
// engine SHALL continue where the plugin is not required."* So [`PluginHost::bring_up`] does not
// return on the first failure. It disables the plugin, records why, disables everything that
// depended on it — a dependent whose dependency never started is not a plugin that can start — and
// carries on. It fails as a whole only when a plugin marked required is one of them.
//
// ================================================================================================
// TYPE OWNERSHIP IS A REGISTRY, NOT A CONVENTION
// ================================================================================================
//
// *"The type registry SHALL record the owning module or plugin of every registered type"*, and
// *"attempted unload with outstanding instances SHALL be refused with a diagnostic naming the types
// and their counts"*. [`TypeOwnership`] is that record, and the refusal names the types and counts
// because a refusal that said "in use" would send somebody looking through every world by hand.
//
// The instance count comes from a caller-supplied [`InstanceCounter`], and it has to: this module
// is at the core layer and `cy::ecs` is above it. A host that guessed zero would be a host that
// permitted exactly the unload the requirement exists to refuse.
//
// ================================================================================================
// THE PUBLIC API IS THE ONLY API — TASK 2.5
// ================================================================================================
//
// *"WHEN a built-in editor is implemented THEN it SHALL use only the public plugin API"*, and
// *"the engine's own first-party features SHALL use the same extension points where one exists"*.
// The mechanism here is that there is nothing else to use: an extension is a row in
// [`ExtensionRegistry`] keyed by point and interface version, first-party and third-party
// registrations go through the same call, and `tools/layercheck` is what stops a built-in editor
// including a header this one does not name. The check that makes it load-bearing lives in
// `tools/plugins/selftest.py`, because a built-in editor reaching past this API is a BUILD failure
// and a program that links is a program whose layering was already accepted.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/core/plugins/plugin.h>
#include <cy/core/plugins/resolve.h>

namespace cy::plugins {

/// The phases, in order. Bring-up runs them forwards; tear-down runs their inverses backwards.
enum class Phase : u8 {
    Load = 0,
    Initialise,
    Register,
    Start,
    Stop,
    Unregister,
    Shutdown,
    Unload,
};

inline constexpr usize kPhaseCount = 8;

/// The enumerator's own spelling, for a diagnostic. Never null.
[[nodiscard]] const char* phase_name(Phase phase) noexcept;

/// What a plugin's implementation does at each phase.
///
/// Function pointers rather than a virtual interface, because the binary boundary this is destined
/// to cross is the engine's C ABI — *"a second plugin ABI SHALL NOT be introduced"* — and a vtable
/// is a C++ layout. A first-party plugin compiled with the engine fills these from a lambda; a
/// binary one fills them from its descriptor entry point.
struct PluginRuntime {
    /// Passed to every callback. The plugin's own state.
    void* user = nullptr;
    /// One per forward phase. A null entry is a phase the plugin does nothing at, which is not the
    /// same as a phase that failed.
    Status (*load)(void* user) = nullptr;
    Status (*initialise)(void* user) = nullptr;
    Status (*register_types)(void* user) = nullptr;
    Status (*start)(void* user) = nullptr;
    /// The reverse phases cannot fail: they run during tear-down, where there is nothing left to
    /// report a failure to. `cy::config::ModuleRegistration` takes the same position for the same
    /// reason.
    void (*stop)(void* user) = nullptr;
    void (*unregister_types)(void* user) = nullptr;
    void (*shutdown)(void* user) = nullptr;
    void (*unload)(void* user) = nullptr;
};

/// Why a plugin is not running.
struct PluginFailure {
    PluginId plugin;
    Phase phase = Phase::Load;
    Error error;
    /// Set when this plugin was disabled because something it depends on was, rather than because
    /// it failed itself. Distinguished because a report that blamed a plugin for its dependency's
    /// defect sends somebody to the wrong file.
    PluginId because_of;
    bool cascaded = false;
};

/// One extension point the engine offers.
struct ExtensionPoint {
    Name name;
    /// The interface version the engine offers today. A registration naming any other is refused.
    ///
    /// *"WHEN an extension interface changes incompatibly THEN its version SHALL increment, and
    /// plugins targeting the previous version SHALL be reported as incompatible rather than
    /// loaded."* Incremented here, and nowhere else.
    u32 interface_version = 1;
};

/// What was registered at a point, and by whom.
struct ExtensionBinding {
    Name point;
    PluginId plugin;
    Name contribution;
};

/// The engine's extension points and what is bound to them.
///
/// **The engine's own features go through this too**, which is the requirement's "dogfoods its
/// extension points" scenario: a first-party contribution registers with the same call and is
/// subject to the same version check, so an inadequacy in the interface is found by the engine
/// before a third party meets it.
class ExtensionRegistry {
public:
    explicit ExtensionRegistry(Allocator& allocator = current_allocator()) noexcept
        : points_(allocator), bindings_(allocator) {}

    ExtensionRegistry(const ExtensionRegistry&) = delete;
    ExtensionRegistry& operator=(const ExtensionRegistry&) = delete;
    ExtensionRegistry(ExtensionRegistry&&) noexcept = default;
    ExtensionRegistry& operator=(ExtensionRegistry&&) noexcept = default;
    ~ExtensionRegistry() = default;

    /// Declare a point the engine offers. Declaring the same name twice is an error: two versions
    /// of one point is the ambiguity the version number exists to remove.
    [[nodiscard]] Status declare(const ExtensionPoint& point) noexcept;

    /// The point a name identifies, or null.
    [[nodiscard]] const ExtensionPoint* point(Name name) const noexcept;

    /// Bind a registration. Refuses a point that does not exist and a version that is not the one
    /// offered, naming both numbers.
    [[nodiscard]] Status bind(PluginId plugin, const ExtensionRegistration& registration) noexcept;

    /// Withdraw everything `plugin` bound. What unregister does, and it answers how many went.
    [[nodiscard]] u32 withdraw(PluginId plugin) noexcept;

    [[nodiscard]] Span<const ExtensionBinding> bindings() const noexcept {
        return bindings_.span();
    }

    /// The minimum set the requirement names, declared onto `out` at version 1.
    ///
    /// It is a function rather than a constructor default so that a test can build a registry with
    /// nothing in it and see a bind refused, which is the case a default-populated registry could
    /// never reach.
    [[nodiscard]] static Status declare_standard_points(ExtensionRegistry& out) noexcept;

private:
    Array<ExtensionPoint> points_;
    Array<ExtensionBinding> bindings_;
};

/// How many live instances of a type exist. Supplied by the caller — see the header.
using InstanceCounter = u64 (*)(void* user, Name type);

/// Which plugin owns which type, and what that means for unloading.
class TypeOwnership {
public:
    explicit TypeOwnership(Allocator& allocator = current_allocator()) noexcept
        : owners_(allocator) {}

    TypeOwnership(const TypeOwnership&) = delete;
    TypeOwnership& operator=(const TypeOwnership&) = delete;
    TypeOwnership(TypeOwnership&&) noexcept = default;
    TypeOwnership& operator=(TypeOwnership&&) noexcept = default;
    ~TypeOwnership() = default;

    /// Record that `plugin` owns `type`. A type already owned by somebody else is an error naming
    /// both owners: two owners of one type is two answers to "who may unload it".
    [[nodiscard]] Status record(PluginId plugin, Name type) noexcept;

    /// Who owns `type`, or the empty name.
    [[nodiscard]] PluginId owner_of(Name type) const noexcept;

    /// Forget everything `plugin` owns.
    void withdraw(PluginId plugin) noexcept;

    /// Whether `plugin` may be unloaded, and if not, why.
    ///
    /// The diagnostic is APPENDED to `out` and names every type with a live instance and its count,
    /// because *"attempted unload with outstanding instances SHALL be refused with a diagnostic
    /// naming the types and their counts"* and a caller cannot name them if this does not.
    [[nodiscard]] Status may_unload(PluginId plugin, InstanceCounter counter, void* user,
                                    Array<char>& out) const noexcept;

private:
    struct Owned {
        Name type;
        PluginId plugin;
    };
    Array<Owned> owners_;
};

/// One plugin as the host holds it: its manifest, its implementation, and whether it is running.
struct HostedPlugin {
    const PluginManifest* manifest = nullptr;
    PluginRuntime runtime;
    /// Whether the engine cannot continue without it. A required plugin's failure fails bring-up.
    bool required = false;
    /// Whether the project has granted it the trust its tier needs.
    bool trusted = false;
    /// The last phase it completed. `Load` before anything has run means nothing has.
    Phase reached = Phase::Load;
    bool running = false;
    bool disabled = false;
};

/// The host: it owns the order, the phases, and the containment.
class PluginHost {
public:
    explicit PluginHost(Allocator& allocator = current_allocator()) noexcept
        : plugins_(allocator),
          failures_(allocator),
          extensions_(allocator),
          ownership_(allocator) {}

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;
    PluginHost(PluginHost&&) noexcept = default;
    PluginHost& operator=(PluginHost&&) noexcept = default;
    ~PluginHost() = default;

    /// The engine API version this host presents. A plugin whose declared range excludes it is
    /// reported incompatible and not added.
    void set_engine_api(const Version& version) noexcept { engine_api_ = version; }
    [[nodiscard]] const Version& engine_api() const noexcept { return engine_api_; }

    /// The platform being brought up on. A plugin that does not declare it is skipped rather than
    /// failed: a plugin with no Windows binary is not a defect on Linux.
    [[nodiscard]] Status set_platform(std::string_view platform) noexcept;

    /// Add a plugin. `manifest` must outlive the host.
    ///
    /// Refuses, by name, a manifest whose engine API range excludes this host's version — which is
    /// the requirement's "incompatible engine version" scenario — and one whose trust tier is
    /// below what its contents require.
    [[nodiscard]] Status add(const PluginManifest& manifest, const PluginRuntime& runtime,
                             bool required, bool trusted) noexcept;

    /// Run Load through Start over `order`, containing failures.
    ///
    /// Answers an error only when a REQUIRED plugin did not start. Everything else is in
    /// `failures()`, and the engine is expected to carry on.
    [[nodiscard]] Status bring_up(Span<const PluginId> order) noexcept;

    /// Run Stop through Unload over everything running, in reverse order. Cannot fail; a plugin
    /// with outstanding instances is refused by `TypeOwnership::may_unload` before this is called.
    void tear_down(Span<const PluginId> order) noexcept;

    [[nodiscard]] const HostedPlugin* find(PluginId plugin) const noexcept;
    [[nodiscard]] Span<const PluginFailure> failures() const noexcept { return failures_.span(); }
    [[nodiscard]] ExtensionRegistry& extensions() noexcept { return extensions_; }
    [[nodiscard]] const ExtensionRegistry& extensions() const noexcept { return extensions_; }
    [[nodiscard]] TypeOwnership& ownership() noexcept { return ownership_; }
    [[nodiscard]] const TypeOwnership& ownership() const noexcept { return ownership_; }

private:
    [[nodiscard]] HostedPlugin* locate(PluginId plugin) noexcept;
    [[nodiscard]] PluginId first_stopped_dependency(const HostedPlugin& hosted) const noexcept;
    [[nodiscard]] Status register_declarations(HostedPlugin& hosted) noexcept;
    [[nodiscard]] Status start_one(HostedPlugin& hosted) noexcept;
    [[nodiscard]] Status run_phase(HostedPlugin& hosted, Phase phase) noexcept;
    [[nodiscard]] Status disable(HostedPlugin& hosted, Phase phase, const Error& error,
                                 PluginId because_of, bool cascaded) noexcept;

    Array<HostedPlugin> plugins_;
    Array<PluginFailure> failures_;
    ExtensionRegistry extensions_;
    TypeOwnership ownership_;
    Version engine_api_{1, 0, 0};
    Name platform_;
};

}  // namespace cy::plugins
