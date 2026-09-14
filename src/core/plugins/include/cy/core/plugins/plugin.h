#pragma once
// A plugin: what one is, how it is identified, and what it is allowed to contain.
// `project-and-plugins`, M11.b task 2.1.
//
// ================================================================================================
// WHAT WAS HERE BEFORE
// ================================================================================================
//
// Nothing. `find src tools -iname '*plugin*'` returned one layercheck fixture, and had since M5;
// four of `project-and-plugins`' eleven requirements — Plugins, Plugin lifecycle, Plugin resolution
// and lockfile, Trust tiers for extensions — had no implementation at all. What existed was
// `cy::config::ProjectPlugin`: a manifest row naming an id, a version and an engine API range, read
// by nothing.
//
// ================================================================================================
// WHY THIS IS SECTION 2 AND NOT SECTION 6
// ================================================================================================
//
// `editor-architecture`'s "Specialised editors" requirement is normative about the order:
// *"Each SHALL be a plugin using the same panel and undo infrastructure as user plugins, so the
// extension API is exercised by the engine's own tooling"*, with a scenario — *"WHEN a built-in
// editor is implemented THEN it SHALL use only the public plugin API"*. An extension API written
// after its first consumers is an API shaped by what they already did.
//
// ================================================================================================
// IDENTITY IS NOT A NAME AND NOT A PATH
// ================================================================================================
//
// *"Plugin identity SHALL NOT depend on its name or path, so a plugin can be renamed or relocated
// without invalidating projects that use it."* So [`PluginId`] is interned from a stable
// identifier the author chooses once, [`PluginManifest::display_name`] is a separate field, and
// nothing in this module ever compares display names. The scenario — renaming a plugin is safe —
// is a test rather than a convention, because a convention is what gets broken by the first person
// who finds `display_name` convenient.
//
// ================================================================================================
// WHAT THIS MODULE DELIBERATELY DOES NOT DO
// ================================================================================================
//
// IT DOES NOT `dlopen` ANYTHING. Loading a binary is `src/abi/`'s, which already carries the
// engine's reload model — *serialize, migrate by name, recreate, never `dlclose`* — and a second
// loader would be a second answer to "what happens to a retired image's string literals". This
// module decides WHICH plugins load, in what order, and what they may contain; `PluginHost` calls
// out to a loader through `PluginRuntime` and does not know what one is.
//
// IT DOES NOT SANDBOX. *"Native code loaded into the process has the process's privileges. The
// engine SHALL NOT claim to sandbox it, and SHALL instead make the trust decision explicit."*
// [`TrustTier::TrustedNative`] is therefore a decision recorded, not a boundary enforced, and the
// only thing this module guarantees about it is that nobody got there by accident.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>

#include <string_view>

namespace cy::plugins {

/// A plugin's stable identifier. Never its display name and never its path.
using PluginId = Name;

/// A semantic version. Three numbers and nothing else: a pre-release tag is a distribution concern
/// and this is what resolution compares.
struct Version {
    u32 major = 0;
    u32 minor = 0;
    u32 patch = 0;

    [[nodiscard]] friend constexpr bool operator==(const Version& left,
                                                   const Version& right) noexcept {
        return left.major == right.major && left.minor == right.minor && left.patch == right.patch;
    }

    [[nodiscard]] friend constexpr bool operator<(const Version& left,
                                                  const Version& right) noexcept {
        if (left.major != right.major) {
            return left.major < right.major;
        }
        if (left.minor != right.minor) {
            return left.minor < right.minor;
        }
        return left.patch < right.patch;
    }

    [[nodiscard]] friend constexpr bool operator<=(const Version& left,
                                                   const Version& right) noexcept {
        return left < right || left == right;
    }
};

/// Parse `1.4.2`. A missing minor or patch is zero, so `1` and `1.0.0` are the same version.
[[nodiscard]] Expected<Version, Error> parse_version(std::string_view text) noexcept;

/// How a dependency names the versions it accepts.
///
/// The three the requirement names — *"exact, minimum compatible, or bounded range"* — and no
/// fourth. A resolver with an open-ended constraint vocabulary is a resolver whose failures cannot
/// be explained, and explaining the failure is half of what the requirement asks for.
enum class ConstraintKind : u8 {
    /// Exactly this version.
    Exact,
    /// This version or any later one with the same major number. `^1.4.0`.
    MinimumCompatible,
    /// `low` inclusive to `high` exclusive.
    Range,
};

/// One version constraint.
struct VersionConstraint {
    ConstraintKind kind = ConstraintKind::MinimumCompatible;
    Version low;
    /// Exclusive, and read only by `ConstraintKind::Range`.
    Version high;

    /// Whether `candidate` satisfies this constraint.
    [[nodiscard]] bool satisfied_by(const Version& candidate) const noexcept;
};

/// Parse `=1.2.3`, `^1.2`, or `[1.2, 2.0)`.
[[nodiscard]] Expected<VersionConstraint, Error> parse_constraint(std::string_view text) noexcept;

/// What a plugin may contain, as a set of flags.
///
/// The requirement's own list: *"modules, content, editor extensions, schemas, importers, build
/// steps, and platform binaries"*. It is declared rather than discovered because it is what the
/// trust tier is checked against — a `DataOnly` plugin shipping a platform binary has to be a
/// refusal at the manifest and not a surprise at load.
enum class PluginContents : u16 {
    None = 0,
    Modules = 1U << 0U,
    Content = 1U << 1U,
    EditorExtensions = 1U << 2U,
    Schemas = 1U << 3U,
    Importers = 1U << 4U,
    BuildSteps = 1U << 5U,
    PlatformBinaries = 1U << 6U,
    Scripts = 1U << 7U,
};

[[nodiscard]] constexpr PluginContents operator|(PluginContents left,
                                                 PluginContents right) noexcept {
    return static_cast<PluginContents>(static_cast<u16>(left) | static_cast<u16>(right));
}

[[nodiscard]] constexpr bool contains(PluginContents set, PluginContents member) noexcept {
    return (static_cast<u16>(set) & static_cast<u16>(member)) != 0;
}

/// The spelling a manifest uses for one content kind, or null where the word is not one.
[[nodiscard]] Expected<PluginContents, Error> plugin_contents_of(std::string_view word) noexcept;

/// How much a distributable extension is trusted, and therefore what it may contain.
///
/// The requirement's table, in order of increasing privilege. The order is load-bearing: a tier
/// comparison is what `required_tier_for` answers with.
enum class TrustTier : u8 {
    /// Assets and configuration. Loaded freely.
    DataOnly = 0,
    /// Assets plus scripts running in the engine's scripting environment.
    Scripted = 1,
    /// Native binaries. Loaded only with an explicit user or project trust decision.
    TrustedNative = 2,
};

/// The enumerator's own spelling, for a diagnostic and for a manifest. Never null.
[[nodiscard]] const char* trust_tier_name(TrustTier tier) noexcept;
/// The inverse. An unknown word is an error naming the three.
[[nodiscard]] Expected<TrustTier, Error> trust_tier_of(std::string_view word) noexcept;

/// The lowest tier that may contain `contents`.
///
/// Native binaries and modules need `TrustedNative`; scripts need `Scripted`; everything else is
/// `DataOnly`. Stated as a function rather than checked inline at each site, so that "what needs
/// trust" is one sentence somebody can read and argue with.
[[nodiscard]] TrustTier required_tier_for(PluginContents contents) noexcept;

/// One plugin's dependency on another.
struct PluginDependency {
    PluginId plugin;
    VersionConstraint constraint;
};

/// One extension point a plugin registers at.
///
/// *"Each extension point SHALL be versioned independently of the engine's release version, so that
/// a plugin targets an interface rather than a patch release."* So a registration carries the
/// interface version it was built against, and [`ExtensionRegistry`] refuses one built against a
/// version it no longer offers rather than loading it and finding out.
struct ExtensionRegistration {
    /// Which point. One of the names `ExtensionRegistry` declares.
    Name point;
    /// The interface version this plugin targets.
    u32 interface_version = 0;
    /// What the plugin calls its contribution, for a diagnostic.
    Name contribution;
};

/// A plugin, as its manifest declares it.
///
/// Allocation-owning, because a manifest is read from a file at start-up and the arrays are sized
/// by the file. It is moved into the host and never copied.
class PluginManifest {
public:
    explicit PluginManifest(Allocator& allocator = current_allocator()) noexcept
        : modules_(allocator),
          types_(allocator),
          platforms_(allocator),
          dependencies_(allocator),
          extensions_(allocator) {}

    PluginManifest(const PluginManifest&) = delete;
    PluginManifest& operator=(const PluginManifest&) = delete;
    PluginManifest(PluginManifest&&) noexcept = default;
    PluginManifest& operator=(PluginManifest&&) noexcept = default;
    ~PluginManifest() = default;

    /// The stable identifier. Everything addresses a plugin by this.
    PluginId id;
    /// What a person reads. Changing it must not invalidate a project — see the header.
    Name display_name;
    Version version;
    /// The engine API range this plugin supports: `engine_api_min` inclusive to `engine_api_max`
    /// exclusive. A plugin outside it is reported incompatible and not loaded.
    Version engine_api_min;
    Version engine_api_max;
    /// What it contains.
    PluginContents contents = PluginContents::None;
    /// The tier it was distributed at. Checked against `required_tier_for(contents)`.
    TrustTier tier = TrustTier::DataOnly;
    /// Whether it supports hot reload.
    bool hot_reload = false;

    [[nodiscard]] Array<Name>& modules() noexcept { return modules_; }
    [[nodiscard]] const Array<Name>& modules() const noexcept { return modules_; }
    /// The types its modules own. What `TypeOwnership` is populated from.
    [[nodiscard]] Array<Name>& types() noexcept { return types_; }
    [[nodiscard]] const Array<Name>& types() const noexcept { return types_; }
    [[nodiscard]] Array<Name>& platforms() noexcept { return platforms_; }
    [[nodiscard]] const Array<Name>& platforms() const noexcept { return platforms_; }
    [[nodiscard]] Array<PluginDependency>& dependencies() noexcept { return dependencies_; }
    [[nodiscard]] const Array<PluginDependency>& dependencies() const noexcept {
        return dependencies_;
    }
    [[nodiscard]] Array<ExtensionRegistration>& extensions() noexcept { return extensions_; }
    [[nodiscard]] const Array<ExtensionRegistration>& extensions() const noexcept {
        return extensions_;
    }

    /// Whether this plugin declares support for `platform`. A manifest naming no platform at all
    /// supports every one, which is what a content-only plugin means by saying nothing.
    [[nodiscard]] bool supports_platform(std::string_view platform) const noexcept;

    /// Whether `engine` is inside the declared API range.
    [[nodiscard]] bool supports_engine(const Version& engine) const noexcept;

private:
    Array<Name> modules_;
    Array<Name> types_;
    Array<Name> platforms_;
    Array<PluginDependency> dependencies_;
    Array<ExtensionRegistration> extensions_;
};

/// Read a `cyplugin` manifest.
///
/// The text form is the engine's own — `TextScanner`'s words and indentation — because a third
/// grammar for a fourth kind of manifest is how a format ends up with three parsers that disagree
/// about escaping.
[[nodiscard]] Status read_plugin_manifest(std::string_view text, PluginManifest& out) noexcept;

/// Whether a manifest is internally consistent, as a diagnostic rather than a bool.
///
/// The checks that are about the manifest alone and not about the set it is resolved in: an
/// identity, a tier that covers what it contains, and an engine API range that is not empty.
[[nodiscard]] Status validate_plugin_manifest(const PluginManifest& manifest) noexcept;

}  // namespace cy::plugins
