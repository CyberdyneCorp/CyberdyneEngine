// `host.h` — the lifecycle, the extension registry, and type ownership.

#include <cy/core/plugins/host.h>

#include <cstdio>
#include <utility>

namespace cy::plugins {

const char* phase_name(Phase phase) noexcept {
    switch (phase) {
        case Phase::Load:
            return "load";
        case Phase::Initialise:
            return "initialise";
        case Phase::Register:
            return "register";
        case Phase::Start:
            return "start";
        case Phase::Stop:
            return "stop";
        case Phase::Unregister:
            return "unregister";
        case Phase::Shutdown:
            return "shutdown";
        case Phase::Unload:
            return "unload";
    }
    return "load";
}

// --- extension points --------------------------------------------------------------------------

Status ExtensionRegistry::declare(const ExtensionPoint& point) noexcept {
    if (point.name.is_empty()) {
        return fail(ErrorCode::InvalidArgument, "an extension point has no name");
    }
    for (const ExtensionPoint& existing : points_) {
        if (existing.name == point.name) {
            return fail(ErrorCode::AlreadyExists,
                        "an extension point is declared twice; two versions of one point is the "
                        "ambiguity the version number removes");
        }
    }
    return points_.push_back(point);
}

const ExtensionPoint* ExtensionRegistry::point(Name name) const noexcept {
    for (const ExtensionPoint& existing : points_) {
        if (existing.name == name) {
            return &existing;
        }
    }
    return nullptr;
}

Status ExtensionRegistry::bind(PluginId plugin,
                               const ExtensionRegistration& registration) noexcept {
    const ExtensionPoint* found = point(registration.point);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound,
                    "a plugin registers at an extension point this engine "
                    "does not offer");
    }
    if (found->interface_version != registration.interface_version) {
        // "plugins targeting the previous version SHALL be reported as incompatible rather than
        // loaded". Reported rather than adapted: an engine that accepted the previous version by
        // guessing what changed is an engine whose version number means nothing.
        return fail(ErrorCode::Unsupported,
                    "a plugin targets a different version of an extension point's interface");
    }
    return bindings_.push_back(
        ExtensionBinding{registration.point, plugin, registration.contribution});
}

u32 ExtensionRegistry::withdraw(PluginId plugin) noexcept {
    u32 withdrawn = 0;
    usize keep = 0;
    // Compacting in place rather than erasing one at a time: a withdrawal takes every binding a
    // plugin made, and n erasures would each move the tail.
    for (const ExtensionBinding& binding : bindings_) {
        if (binding.plugin == plugin) {
            withdrawn += 1;
            continue;
        }
        bindings_[keep] = binding;
        keep += 1;
    }
    while (bindings_.size() > keep) {
        bindings_.pop_back();
    }
    return withdrawn;
}

Status ExtensionRegistry::declare_standard_points(ExtensionRegistry& out) noexcept {
    // THE MINIMUM SET THE REQUIREMENT NAMES, and it is a list rather than a sentence so that a
    // point the specification names and the engine forgot is a missing row somebody can see.
    // Every one is at interface version 1 because none of them has changed yet; the day one does,
    // its number moves here and plugins targeting the old one are refused by `bind`.
    static constexpr std::string_view kPoints[] = {
        "physics-backend",
        "audio-backend",
        "network-transport",
        "asset-importer",
        "asset-processor",
        "render-feature",
        "material-node",
        "material-closure",
        "editor-panel",
        "property-editor",
        "gizmo",
        "viewport-tool",
        "editor-command",
        "build-step",
        "platform-target",
        "upscaler",
        "ray-tracing-backend",
        "virtual-texture-producer",
        "source-control",
        "streaming-producer",
        "residency-producer",
    };
    for (const std::string_view name : kPoints) {
        if (Status declared = out.declare(ExtensionPoint{Name::intern(name), 1}); !declared) {
            return declared;
        }
    }
    return ok();
}

// --- type ownership ----------------------------------------------------------------------------

Status TypeOwnership::record(PluginId plugin, Name type) noexcept {
    for (const Owned& owned : owners_) {
        if (owned.type == type) {
            if (owned.plugin == plugin) {
                return ok();
            }
            return fail(ErrorCode::AlreadyExists,
                        "a type is already owned by another plugin; two owners is two answers to "
                        "who may unload it");
        }
    }
    return owners_.push_back(Owned{type, plugin});
}

PluginId TypeOwnership::owner_of(Name type) const noexcept {
    for (const Owned& owned : owners_) {
        if (owned.type == type) {
            return owned.plugin;
        }
    }
    return Name{};
}

void TypeOwnership::withdraw(PluginId plugin) noexcept {
    usize keep = 0;
    for (const Owned& owned : owners_) {
        if (owned.plugin == plugin) {
            continue;
        }
        owners_[keep] = owned;
        keep += 1;
    }
    while (owners_.size() > keep) {
        owners_.pop_back();
    }
}

Status TypeOwnership::may_unload(PluginId plugin, InstanceCounter counter, void* user,
                                 Array<char>& out) const noexcept {
    if (counter == nullptr) {
        // A host with no way to count instances cannot answer the question, and answering "yes"
        // would be exactly the dangling data the requirement forbids.
        return fail(ErrorCode::Unavailable,
                    "an unload was asked for with no way to count live instances");
    }
    u32 blocking = 0;
    for (const Owned& owned : owners_) {
        if (!(owned.plugin == plugin)) {
            continue;
        }
        const u64 live = counter(user, owned.type);
        if (live == 0) {
            continue;
        }
        blocking += 1;
        char line[256] = {};
        const int written =
            std::snprintf(line, sizeof(line), "%s%.*s x%llu", blocking > 1 ? ", " : "",
                          static_cast<int>(owned.type.text().size()), owned.type.text().data(),
                          static_cast<unsigned long long>(live));
        if (written > 0) {
            if (Status appended = out.append(Span<const char>(line, static_cast<usize>(written)));
                !appended) {
                return appended;
            }
        }
    }
    if (blocking != 0) {
        return fail(ErrorCode::PermissionDenied,
                    "a plugin cannot be unloaded while instances of its types exist");
    }
    return ok();
}

// --- the host ----------------------------------------------------------------------------------

Status PluginHost::set_platform(std::string_view platform) noexcept {
    platform_ = Name::intern(platform);
    return ok();
}

HostedPlugin* PluginHost::locate(PluginId plugin) noexcept {
    for (HostedPlugin& hosted : plugins_) {
        if (hosted.manifest != nullptr && hosted.manifest->id == plugin) {
            return &hosted;
        }
    }
    return nullptr;
}

const HostedPlugin* PluginHost::find(PluginId plugin) const noexcept {
    for (const HostedPlugin& hosted : plugins_) {
        if (hosted.manifest != nullptr && hosted.manifest->id == plugin) {
            return &hosted;
        }
    }
    return nullptr;
}

Status PluginHost::add(const PluginManifest& manifest, const PluginRuntime& runtime, bool required,
                       bool trusted) noexcept {
    if (Status valid = validate_plugin_manifest(manifest); !valid) {
        return valid;
    }
    if (!manifest.supports_engine(engine_api_)) {
        // "WHEN a plugin declares an engine API range that excludes the current engine THEN it
        // SHALL be reported as incompatible and not loaded." Refused at `add` rather than skipped
        // at bring-up, so the caller learns about it where it named the plugin.
        return fail(ErrorCode::Unsupported, "a plugin's engine API range excludes this engine");
    }
    if (manifest.tier == TrustTier::TrustedNative && !trusted) {
        // "loading native mod code SHALL require an explicit trust decision that the product may
        // disallow entirely." The decision is the `trusted` argument; there is no default that
        // grants it, which is what makes it explicit.
        return fail(ErrorCode::PermissionDenied,
                    "a TrustedNative plugin needs an explicit trust decision and has none");
    }
    HostedPlugin hosted;
    hosted.manifest = &manifest;
    hosted.runtime = runtime;
    hosted.required = required;
    hosted.trusted = trusted;
    return plugins_.push_back(hosted);
}

/// What the HOST registers on a plugin's behalf, before the plugin's own `register` callback runs.
///
/// Before, so that a plugin whose callback reaches for one of its own extension points finds it
/// bound. Types first: a component type is what a world is built out of.
///
/// It is the host that does this rather than the plugin, and that is the point — a plugin that
/// forgot to call something would otherwise own no types and unload freely, which is the refusal
/// `TypeOwnership::may_unload` exists to make.
Status PluginHost::register_declarations(HostedPlugin& hosted) noexcept {
    for (const Name& type : hosted.manifest->types()) {
        if (Status owned = ownership_.record(hosted.manifest->id, type); !owned) {
            return owned;
        }
    }
    for (const ExtensionRegistration& registration : hosted.manifest->extensions()) {
        if (Status bound = extensions_.bind(hosted.manifest->id, registration); !bound) {
            return bound;
        }
    }
    return ok();
}

Status PluginHost::run_phase(HostedPlugin& hosted, Phase phase) noexcept {
    const PluginRuntime& runtime = hosted.runtime;
    switch (phase) {
        case Phase::Load:
            return runtime.load != nullptr ? runtime.load(runtime.user) : ok();
        case Phase::Initialise:
            return runtime.initialise != nullptr ? runtime.initialise(runtime.user) : ok();
        case Phase::Register: {
            if (Status declared = register_declarations(hosted); !declared) {
                return declared;
            }
            return runtime.register_types != nullptr ? runtime.register_types(runtime.user) : ok();
        }
        case Phase::Start:
            return runtime.start != nullptr ? runtime.start(runtime.user) : ok();
        case Phase::Stop:
            if (runtime.stop != nullptr) {
                runtime.stop(runtime.user);
            }
            return ok();
        case Phase::Unregister:
            if (runtime.unregister_types != nullptr) {
                runtime.unregister_types(runtime.user);
            }
            (void)extensions_.withdraw(hosted.manifest->id);
            ownership_.withdraw(hosted.manifest->id);
            return ok();
        case Phase::Shutdown:
            if (runtime.shutdown != nullptr) {
                runtime.shutdown(runtime.user);
            }
            return ok();
        case Phase::Unload:
            if (runtime.unload != nullptr) {
                runtime.unload(runtime.user);
            }
            return ok();
    }
    return ok();
}

Status PluginHost::disable(HostedPlugin& hosted, Phase phase, const Error& error,
                           PluginId because_of, bool cascaded) noexcept {
    hosted.disabled = true;
    hosted.running = false;
    // Everything the plugin managed to register is withdrawn, so a half-brought-up plugin does not
    // leave a binding behind that outlives it. This is the same call `Unregister` makes.
    (void)extensions_.withdraw(hosted.manifest->id);
    ownership_.withdraw(hosted.manifest->id);
    PluginFailure failure;
    failure.plugin = hosted.manifest->id;
    failure.phase = phase;
    failure.error = error;
    failure.because_of = because_of;
    failure.cascaded = cascaded;
    return failures_.push_back(failure);
}

/// The first dependency of `hosted` that is not running, or the empty name.
///
/// A dependency that is not running is a reason this plugin cannot start, and it is reported as a
/// DIFFERENT failure from one the plugin caused itself — blaming a plugin for its dependency's
/// defect sends somebody to the wrong file.
PluginId PluginHost::first_stopped_dependency(const HostedPlugin& hosted) const noexcept {
    for (const PluginDependency& dependency : hosted.manifest->dependencies()) {
        const HostedPlugin* provider = find(dependency.plugin);
        if (provider == nullptr || !provider->running) {
            return dependency.plugin;
        }
    }
    return PluginId{};
}

/// Take one plugin from Load to Start, or disable it where a phase refused.
///
/// Answers an error only when the FAILURE could not be recorded, which is out of memory and nothing
/// else; a plugin that failed a phase is reported through `failures()` and `hosted.running`.
Status PluginHost::start_one(HostedPlugin& hosted) noexcept {
    static constexpr Phase kForward[] = {Phase::Load, Phase::Initialise, Phase::Register,
                                         Phase::Start};
    const PluginId blocked_by = first_stopped_dependency(hosted);
    if (!blocked_by.is_empty()) {
        return disable(hosted, Phase::Load,
                       Error{ErrorCode::Unavailable, "a plugin this one depends on is not running"},
                       blocked_by, true);
    }
    for (const Phase phase : kForward) {
        const Status ran = run_phase(hosted, phase);
        if (!ran) {
            return disable(hosted, phase, ran.error(), PluginId{}, false);
        }
        hosted.reached = phase;
    }
    hosted.running = true;
    return ok();
}

Status PluginHost::bring_up(Span<const PluginId> order) noexcept {
    bool required_failed = false;
    for (const PluginId& plugin : order) {
        HostedPlugin* hosted = locate(plugin);
        if (hosted == nullptr || hosted->disabled) {
            continue;
        }
        if (!hosted->manifest->supports_platform(platform_.text())) {
            // Not a failure. A plugin with no binary for this platform is not a defect on it, and
            // recording one would make every cross-platform project's report full of them.
            hosted->disabled = true;
            continue;
        }
        if (Status started = start_one(*hosted); !started) {
            return started;
        }
        required_failed = required_failed || (hosted->required && !hosted->running);
    }
    if (required_failed) {
        return fail(ErrorCode::Unavailable, "a required plugin did not start");
    }
    return ok();
}

void PluginHost::tear_down(Span<const PluginId> order) noexcept {
    static constexpr Phase kReverse[] = {Phase::Stop, Phase::Unregister, Phase::Shutdown,
                                         Phase::Unload};
    for (usize index = order.size(); index > 0; --index) {
        HostedPlugin* hosted = locate(order[index - 1]);
        if (hosted == nullptr || !hosted->running) {
            continue;
        }
        for (const Phase phase : kReverse) {
            (void)run_phase(*hosted, phase);
        }
        hosted->running = false;
        hosted->reached = Phase::Unload;
    }
}

}  // namespace cy::plugins
