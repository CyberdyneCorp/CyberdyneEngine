#pragma once
// Gameplay features. M8.b task 3.3.
//
// `gameplay-framework` — "Gameplay features": functionality is packageable as "**features**: named
// units declaring the components, systems, rules, tags, input contexts, assets, interface, and
// world layers they contribute." Features are "activatable and deactivatable per session and per
// world, with states installed, loaded, registered, activated, and deactivated". Game modes are
// "**compositions of features and rules** rather than subclasses". Activation is content-level,
// distinct from module and plugin loading. "Features SHALL declare dependencies on other features,
// resolved before activation."
//
// ================================================================================================
// WHAT A FEATURE DECLARES IS A LIST OF NAMES, AND THAT IS DELIBERATE
// ================================================================================================
//
// A feature does not hold the systems it contributes; it names them. The registry has no way to
// call a system, instantiate a component or load an asset, because the things being named live in
// four different modules and a feature that held them would drag all four into this header — which
// is how a content-level concept turns into a link-time one. The host resolves the names when it
// activates, and `contributions()` is what it reads.
//
// ================================================================================================
// DEPENDENCIES ARE RESOLVED BEFORE ACTIVATION, NOT DURING IT
// ================================================================================================
//
// Activating a feature activates what it depends on first, in dependency order, and refuses the
// whole activation if a dependency is missing or the graph has a cycle — before anything is
// activated. A partial activation is worse than a refusal: half a mode is a state nobody wrote
// code for.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/gameplay/tags.h>

namespace cy::gameplay {

/// The states the requirement names, in the order a feature passes through them.
enum class FeatureState : u8 {
    Installed = 0,
    Loaded,
    Registered,
    Activated,
    Deactivated,
    Count,
};

const char* feature_state_name(FeatureState state) noexcept;

/// What kind of thing a feature contributes. Named, not held — see the header comment.
enum class ContributionKind : u8 {
    Component = 0,
    System,
    Rule,
    Tag,
    InputContext,
    Asset,
    Interface,
    WorldLayer,
    Count,
};

const char* contribution_kind_name(ContributionKind kind) noexcept;

struct Contribution {
    ContributionKind kind = ContributionKind::Component;
    Name name;
    /// For `ContributionKind::Tag`, the tag itself.
    TagId tag = kInvalidTag;
};

using FeatureId = u32;
inline constexpr FeatureId kInvalidFeature = 0xFFFFFFFFU;

/// Where a feature is active. A feature may be on for the session and off for a preview world.
enum class FeatureScopeKind : u8 { Session = 0, World, Count };

/// Features, their dependencies, their contributions and their states.
class FeatureRegistry {
public:
    /// A session with more features than this is a session whose composition is not a list. The
    /// bound is what lets the dependency walk keep its marks on the stack, which is what keeps
    /// `resolve_order()` const and allocation-free.
    static constexpr u32 kMaxFeatures = 64;

    explicit FeatureRegistry(Allocator& allocator) noexcept;

    FeatureRegistry(const FeatureRegistry&) = delete;
    FeatureRegistry& operator=(const FeatureRegistry&) = delete;

    /// Register a feature. It arrives `Installed`; nothing is activated by registering.
    [[nodiscard]] Expected<FeatureId, Error> register_feature(
        Name name, FeatureScopeKind scope = FeatureScopeKind::Session) noexcept;
    [[nodiscard]] FeatureId find(Name name) const noexcept;

    [[nodiscard]] Status declare_dependency(FeatureId feature, Name dependency) noexcept;
    [[nodiscard]] Status contribute(FeatureId feature, ContributionKind kind, Name name,
                                    TagId tag = kInvalidTag) noexcept;
    /// The phases a feature's work is active in. `kInvalidTag` — the default — means every phase.
    [[nodiscard]] Status set_active_phase(FeatureId feature, TagId phase) noexcept;
    [[nodiscard]] TagId active_phase(FeatureId feature) const noexcept;

    /// Advance a feature to `Loaded` and then `Registered`. Separate from activation because the
    /// requirement names five states and collapsing them would make "loaded but not active" —
    /// which is what a downloadable mode is between download and selection — inexpressible.
    [[nodiscard]] Status load(FeatureId feature) noexcept;
    [[nodiscard]] Status register_contributions(FeatureId feature) noexcept;

    /// Activate, resolving dependencies first. Refuses — activating nothing — when a dependency is
    /// missing or the dependency graph has a cycle.
    [[nodiscard]] Status activate(FeatureId feature) noexcept;
    [[nodiscard]] Status deactivate(FeatureId feature) noexcept;

    [[nodiscard]] FeatureState state(FeatureId feature) const noexcept;
    [[nodiscard]] Name name_of(FeatureId feature) const noexcept;
    [[nodiscard]] FeatureScopeKind scope_of(FeatureId feature) const noexcept;
    [[nodiscard]] u32 count() const noexcept { return static_cast<u32>(features_.size()); }

    /// The contributions of one feature. What a host reads to know what to wire up.
    [[nodiscard]] u32 contributions(FeatureId feature, Contribution* out,
                                    u32 capacity) const noexcept;
    /// Every contribution of that kind across the ACTIVE features. What a host reads to know what
    /// the session currently is.
    [[nodiscard]] u32 active_contributions(ContributionKind kind, Contribution* out,
                                           u32 capacity) const noexcept;

    /// The order `activate(feature)` would use, dependencies first. Exposed so that a host can see
    /// the plan, and so that the cycle refusal is testable without activating anything.
    [[nodiscard]] Expected<u32, Error> resolve_order(FeatureId feature, FeatureId* out,
                                                     u32 capacity) const noexcept;

private:
    struct Feature {
        Name name;
        FeatureScopeKind scope = FeatureScopeKind::Session;
        FeatureState state = FeatureState::Installed;
        TagId active_phase = kInvalidTag;
        Array<Name> dependencies;
        Array<Contribution> contributions;
    };

    /// Depth-first, marking as it goes: 0 unvisited, 1 on the stack, 2 done. A feature reached
    /// while it is on the stack is a cycle, and the walk refuses rather than recursing.
    [[nodiscard]] Status visit(FeatureId feature, FeatureId* out, u32 capacity, u32& written,
                               u8* marks) const noexcept;

    Allocator* allocator_;
    Array<Feature> features_;
};

}  // namespace cy::gameplay
