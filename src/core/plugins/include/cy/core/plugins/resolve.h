#pragma once
// Plugin resolution and the lockfile. `project-and-plugins`, M11.b task 2.3.
//
// ================================================================================================
// WHAT THE REQUIREMENT ASKS FOR, AND WHY EACH HALF IS HERE
// ================================================================================================
//
// *"Plugin dependencies SHALL be resolved from declared version constraints — exact, minimum
// compatible, or bounded range — into a concrete set."* That is [`resolve`].
//
// *"The resolved set SHALL be written to a committed lockfile recording each plugin's identifier,
// resolved version, and content hash, so that a build is reproducible and a change of dependency is
// a reviewable diff."* That is [`Lockfile`], and the three fields are exactly those three: a
// lockfile that recorded a path would not survive a checkout on another machine, and one that
// omitted the hash would call two different builds of one version the same build.
//
// *"Resolution SHALL fail with an explanation when constraints cannot be satisfied, naming the
// conflicting requirements."* That is [`Resolution::conflict`], and it is the reason this returns a
// report rather than a `Status`: "unsatisfiable" with nothing else in it is the failure the
// scenario was written to forbid.
//
// *"Building SHALL use the lockfile; updating it SHALL be a deliberate action."* That is
// [`resolve_against_lock`], which never writes: a caller that wants the lock updated calls
// [`resolve`] and writes the result, and there is no path where a build quietly moves a version.
//
// ================================================================================================
// WHY THE RESOLVER IS AS SIMPLE AS IT IS
// ================================================================================================
//
// One version of each plugin is available per candidate set, because a project's plugins are files
// in a directory and not a registry with a version history. So resolution is not a SAT problem: it
// is reachability from the project's own list, plus a check that every constraint on each reached
// plugin is satisfied by the one version present. When a registry with several versions of one
// plugin arrives, the shape that changes is `Candidates`, and `Resolution` is what it must still
// produce.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/core/plugins/plugin.h>

namespace cy::plugins {

/// One entry of the lockfile: the three things the requirement names and nothing else.
struct LockedPlugin {
    PluginId plugin;
    Version version;
    /// A content hash of the plugin's own bytes, as the packager computed it. Opaque here: this
    /// module compares it and never recomputes it, because a hash this module could recompute would
    /// be a hash of what it already has rather than of what was shipped.
    u64 content_hash = 0;
};

/// The resolved set, as it is committed.
///
/// Sorted by plugin id text, so that the file a resolve writes is a function of the set and not of
/// the order the resolver happened to visit it in — otherwise "a change of dependency is a
/// reviewable diff" would be false for a change that reordered nothing.
class Lockfile {
public:
    explicit Lockfile(Allocator& allocator = current_allocator()) noexcept : entries_(allocator) {}

    Lockfile(const Lockfile&) = delete;
    Lockfile& operator=(const Lockfile&) = delete;
    Lockfile(Lockfile&&) noexcept = default;
    Lockfile& operator=(Lockfile&&) noexcept = default;
    ~Lockfile() = default;

    [[nodiscard]] Span<const LockedPlugin> entries() const noexcept { return entries_.span(); }
    [[nodiscard]] const LockedPlugin* find(PluginId plugin) const noexcept;

    /// Insert or replace one entry, keeping the array sorted by the plugin's text.
    [[nodiscard]] Status record(const LockedPlugin& entry) noexcept;

private:
    Array<LockedPlugin> entries_;
};

/// Write a lockfile's text form. The inverse of [`read_lockfile`].
[[nodiscard]] Status write_lockfile(const Lockfile& lock, Array<char>& out) noexcept;
/// Read one back.
[[nodiscard]] Status read_lockfile(std::string_view text, Lockfile& out) noexcept;

/// Why a resolution failed, in the terms the requirement asks for.
struct ResolutionConflict {
    /// The plugin both requirements are about.
    PluginId subject;
    /// The version actually available, when one is.
    Version available;
    bool has_available = false;
    /// Who required it and what they asked for. Two, because naming one explains nothing: the
    /// scenario is "two plugins require incompatible versions of a third".
    PluginId first_requirer;
    VersionConstraint first_constraint;
    PluginId second_requirer;
    VersionConstraint second_constraint;
    bool has_second = false;
};

/// What a resolution produced.
class Resolution {
public:
    explicit Resolution(Allocator& allocator = current_allocator()) noexcept : order_(allocator) {}

    Resolution(const Resolution&) = delete;
    Resolution& operator=(const Resolution&) = delete;
    Resolution(Resolution&&) noexcept = default;
    Resolution& operator=(Resolution&&) noexcept = default;
    ~Resolution() = default;

    /// Whether it succeeded. When false, `conflict` says why.
    [[nodiscard]] bool resolved() const noexcept { return resolved_; }
    [[nodiscard]] const ResolutionConflict& conflict() const noexcept { return conflict_; }

    /// The resolved plugins in LOAD ORDER: a topological order of the dependency graph, with ties
    /// broken by the plugin's own text.
    ///
    /// *"Load order SHALL follow the resolved dependency graph and SHALL be deterministic."* The
    /// tie-break is what makes the second half true — a topological order alone is not unique, and
    /// two hosts that inserted the same plugins in different orders would otherwise start them
    /// differently.
    [[nodiscard]] Span<const PluginId> order() const noexcept { return order_.span(); }

    friend Expected<Resolution, Error> resolve(Span<const PluginManifest* const> available,
                                               Span<const PluginId> requested,
                                               Allocator& allocator) noexcept;

private:
    Array<PluginId> order_;
    ResolutionConflict conflict_;
    bool resolved_ = false;
};

/// Resolve `requested` and everything they depend on, out of `available`.
///
/// Answers a `Resolution` whose `resolved()` is false, with a filled-in conflict, when the
/// constraints cannot be met — rather than an `Error`, because an unsatisfiable set is an answer a
/// caller reports to a person and not a failure of the resolver.
///
/// A genuine `Error` is returned only for what the resolver cannot proceed past at all: a requested
/// plugin that is not in `available`, or a cycle in the dependency graph.
[[nodiscard]] Expected<Resolution, Error> resolve(Span<const PluginManifest* const> available,
                                                  Span<const PluginId> requested,
                                                  Allocator& allocator) noexcept;

/// Check a resolution against a committed lockfile.
///
/// *"Building SHALL use the lockfile; updating it SHALL be a deliberate action."* So this reports a
/// resolution that differs from the lock as an error naming the plugin that moved, and **never
/// updates the lock**. There is deliberately no `resolve_and_update`: the update is a caller
/// writing the file, which is a line somebody has to write on purpose.
[[nodiscard]] Status resolve_against_lock(Span<const PluginManifest* const> available,
                                          const Resolution& resolution, const Lockfile& lock,
                                          Array<char>& diagnostic) noexcept;

}  // namespace cy::plugins
