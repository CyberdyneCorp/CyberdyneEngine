#ifndef CY_BUILD_PRODUCER_H
#define CY_BUILD_PRODUCER_H
// What runs a node, and how its reads and writes are verified. M6 task 7.2.
//
// `build-and-packaging` — "Inputs are explicit": "Every node SHALL declare its inputs. **Reading an
// undeclared input SHALL be a defect**, because an undeclared read produces stale outputs that no
// invalidation can detect. The build SHALL be able to verify declared inputs — by sandboxing, by
// tracing file access, or by auditing — in at least one supported mode, and undeclared access SHALL
// be reported. Outputs SHALL likewise be declared, and a node writing outside them SHALL be
// reported."
//
// design.md §1.9 item 7 is blunt about why this has to be built rather than planned: **no key can
// catch an undeclared read.** The spike's E9 shows the build serving a stale artefact and reporting
// success, because the key is a perfect description of what the node DECLARED and says nothing
// about what it READ.
//
// --- THE MODE THIS FILE IMPLEMENTS: AUDITING, BY CONSTRUCTION ------------------------------------
//
// A producer is handed a `NodeContext` and no filesystem. Every read goes through `read` or
// `discover` and every write through `write`, so the audit is not a pass that runs afterwards and
// might miss something — a name that was not declared cannot be resolved at all. This is the same
// argument `ImportResolver` makes in `cy/import/importer.h`, and it is made here for the same
// reason: the alternative is letting a producer open files and asking it to please declare what it
// opened, which is what every pipeline with a "why did this not re-cook" problem does.
//
// --- DECLARED INPUTS AND DISCOVERED ONES ARE DIFFERENT, AND BOTH ARE LEGITIMATE ------------------
//
//   * `read` takes a name the node DECLARED — a source, or an upstream's output. Its content is in
//     the derivation key, so a change to it is known before the node runs.
//   * `discover` takes a name the producer could not have known before it ran: a shader's include,
//     a glTF material's texture. It cannot be in the key — a key computable only after doing the
//     work would never produce a hit — so it is recorded as a dependency with the digest it had,
//     and `DerivedCache::lookup` re-digests it. A change is an `Invalidated` outcome naming it.
//
// An undeclared read is neither of those: it is a name that was never declared and never recorded.
// It is reported as a violation, and under `AccessPolicy::Enforce` the read fails as well, so a
// producer cannot proceed on data the build cannot invalidate.

#include <cy/build/graph.h>
#include <cy/core/assets/hash.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

#include <atomic>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cy::build {

/// What the build does about access it did not expect.
enum class AccessPolicy : u8 {
    /// Report the violation and fail the read. The mode a shipping or continuous-integration build
    /// runs in: a node that reads what it did not declare produces an artefact no invalidation can
    /// reach, and shipping that is worse than failing.
    Enforce = 0,
    /// Report the violation and allow the read, so an existing producer can be brought into
    /// compliance by running the build and reading the report rather than by guessing.
    Audit = 1,
};

/// What kind of rule was broken.
enum class ViolationKind : u8 {
    /// A name that is neither a declared source, nor an upstream's output, nor a recorded
    /// discovery.
    UndeclaredRead = 0,
    /// A write to a name the node did not declare as an output.
    UndeclaredWrite = 1,
    /// The node completed without writing one of its declared outputs. A downstream node would then
    /// read an artefact from a previous build, which is the stale-output failure arriving from the
    /// other direction.
    MissingOutput = 2,
};

[[nodiscard]] const char* violation_kind_name(ViolationKind kind) noexcept;

/// One broken rule, named precisely enough to fix.
struct AccessViolation {
    ViolationKind kind = ViolationKind::UndeclaredRead;
    std::string node;
    std::string name;
};

/// How serious a diagnostic is. Mirrors `ImportSeverity`, deliberately: `build-and-packaging`
/// requires that "every diagnostic from every stage … SHALL share a structure".
enum class Severity : u8 { Info = 0, Warning = 1, Error = 2 };

[[nodiscard]] const char* severity_name(Severity severity) noexcept;

/// The structured diagnostic every stage emits.
///
/// `code` is a stable identifier a report groups by and a project suppresses — "undeclared-read"
/// rather than a sentence somebody will improve the wording of. `node` and `location` are the two
/// halves of "a location naming a source file and position, an asset, or a graph node".
struct Diagnostic {
    Severity severity = Severity::Info;
    std::string code;
    std::string message;
    std::string node;
    std::string location;
};

/// Resolve a project-relative logical name to bytes.
///
/// The seam between the build and where content actually lives. The default implementation reads a
/// directory; a test provides an in-memory one; a remote worker will fetch by hash. Nothing above
/// this interface knows which, which is what `build-and-packaging`'s distributed execution needs:
/// "inputs are fetched by hash and outputs stored by hash".
class SourceProvider {
public:
    SourceProvider() = default;
    SourceProvider(const SourceProvider&) = delete;
    SourceProvider& operator=(const SourceProvider&) = delete;
    virtual ~SourceProvider() = default;

    /// Read a source. `NotFound` when it does not exist, which the build reports as a diagnostic
    /// naming the node and the name rather than as a crash.
    [[nodiscard]] virtual Status read(std::string_view name, Array<u8>& out) const = 0;

    /// The source's content digest. Separate from `read` because keying a node needs the digest and
    /// not the bytes, and on a cache hit the bytes are never read at all.
    [[nodiscard]] virtual Expected<assets::ContentHash, Error> digest(
        std::string_view name) const = 0;

    [[nodiscard]] virtual bool exists(std::string_view name) const = 0;
};

/// A `SourceProvider` over a directory. Names are resolved beneath `root` and nowhere else.
class DirectorySourceProvider final : public SourceProvider {
public:
    explicit DirectorySourceProvider(std::string root) : root_(std::move(root)) {}

    [[nodiscard]] Status read(std::string_view name, Array<u8>& out) const override;
    [[nodiscard]] Expected<assets::ContentHash, Error> digest(std::string_view name) const override;
    [[nodiscard]] bool exists(std::string_view name) const override;

    [[nodiscard]] const std::string& root() const noexcept { return root_; }

private:
    /// Refuses a name that is not project-relative, so a producer cannot reach outside the project
    /// through a name the graph accepted by mistake.
    [[nodiscard]] Expected<std::string, Error> resolve(std::string_view name) const;

    std::string root_;
};

/// A `SourceProvider` over a map held in memory. For tests, and for a build description that
/// carries small inputs inline.
class MemorySourceProvider final : public SourceProvider {
public:
    void set(std::string name, std::string content);
    void erase(std::string_view name);

    [[nodiscard]] Status read(std::string_view name, Array<u8>& out) const override;
    [[nodiscard]] Expected<assets::ContentHash, Error> digest(std::string_view name) const override;
    [[nodiscard]] bool exists(std::string_view name) const override;

private:
    std::unordered_map<std::string, std::string> content_;
};

/// One input a producer discovered while running, with the digest it had at the time.
struct DiscoveredDependency {
    std::string name;
    assets::ContentHash hash;
};

/// What a node produced.
struct NodeOutput {
    std::string name;
    assets::ContentHash digest;
    u64 size = 0;
};

/// What a producer is handed. Its only route to the world.
///
/// Constructed by the build service around one node; a producer never makes one. Not thread-safe:
/// one context belongs to one running node, and independent nodes have their own.
class NodeContext {
public:
    NodeContext(const NodeDesc& node, const SourceProvider& sources, AccessPolicy policy);

    NodeContext(const NodeContext&) = delete;
    NodeContext& operator=(const NodeContext&) = delete;

    [[nodiscard]] const NodeDesc& node() const noexcept { return *node_; }

    /// Read a declared input: a source, or an output of an upstream node.
    ///
    /// An undeclared name is a violation. Under `Enforce` it also fails with `PermissionDenied`,
    /// which is the error a producer should propagate rather than work around.
    [[nodiscard]] Status read(std::string_view name, Array<u8>& out);

    /// Read an input the producer discovered, recording it as a dependency in the same act.
    ///
    /// The engine side of the argument at the top of this file: there is no way to get the bytes
    /// without filing the dependency.
    [[nodiscard]] Status discover(std::string_view name, Array<u8>& out);

    /// Write one of the node's declared outputs. Writing the same output twice replaces it, so a
    /// producer that builds an output incrementally is not forced to buffer by hand; writing a name
    /// the node did not declare is a violation.
    [[nodiscard]] Status write(std::string_view name, const void* data, usize size);

    void diagnose(Severity severity, std::string code, std::string message,
                  std::string location = {});

    /// True once the build has been cancelled. A long-running producer polls it; one that never
    /// polls simply finishes, and `build-and-packaging` requires only that "cancelling a build
    /// SHALL stop unstarted and cancellable work".
    [[nodiscard]] bool is_cancelled() const noexcept;

    // --- What the service reads back out -------------------------------------------------------

    [[nodiscard]] const std::vector<NodeOutput>& outputs() const noexcept { return outputs_; }
    [[nodiscard]] const std::vector<DiscoveredDependency>& discoveries() const noexcept {
        return discoveries_;
    }
    [[nodiscard]] const std::vector<AccessViolation>& violations() const noexcept {
        return violations_;
    }
    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }
    /// The bytes of a written output, so the service can store them.
    [[nodiscard]] const std::string& output_bytes(std::string_view name) const;

    // --- What the service fills in before running the node --------------------------------------

    /// Make an upstream node's output readable under its logical name.
    void bind_upstream(std::string name, std::string bytes);
    /// Point the context at the cancellation flag the service owns.
    void bind_cancellation(const std::atomic<bool>* cancelled) noexcept;

private:
    [[nodiscard]] bool declares_source(std::string_view name) const noexcept;
    [[nodiscard]] bool declares_output(std::string_view name) const noexcept;
    void record(ViolationKind kind, std::string_view name);

    const NodeDesc* node_;
    const SourceProvider* sources_;
    AccessPolicy policy_;
    const std::atomic<bool>* cancelled_ = nullptr;
    std::unordered_map<std::string, std::string> upstream_;
    std::unordered_map<std::string, std::string> written_;
    std::vector<NodeOutput> outputs_;
    std::vector<DiscoveredDependency> discoveries_;
    std::vector<AccessViolation> violations_;
    std::vector<Diagnostic> diagnostics_;
};

/// What a producer does. A plain function pointer: a producer is a pure function of its context,
/// and a registry of function pointers is the smallest thing that expresses that.
using ProducerBody = Status (*)(NodeContext& context);

/// One registered producer.
struct Producer {
    std::string name;
    /// The version `build-and-packaging` requires in the key: "WHEN a cooker's version increases
    /// THEN the keys of every node invoking it SHALL change and their outputs SHALL be recooked."
    u32 version = 0;
    ProducerBody body = nullptr;
    /// Whether a remote worker may run it. Some tools are licensed or platform-bound.
    bool distributable = true;
};

/// The producers a build knows about.
class ProducerRegistry {
public:
    [[nodiscard]] Status add(Producer producer);
    [[nodiscard]] const Producer* find(std::string_view name) const noexcept;
    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(producers_.size()); }

    /// Register the producers `cy_build` ships with: `copy`, `normalise`, `concat` and `manifest`.
    /// They are deliberately small and deterministic — the graph is the subject here, and a
    /// producer with a large surface would be testing the producer instead.
    [[nodiscard]] Status add_builtins();

private:
    std::unordered_map<std::string, Producer> producers_;
};

}  // namespace cy::build

#endif  // CY_BUILD_PRODUCER_H
