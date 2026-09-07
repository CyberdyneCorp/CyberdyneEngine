#ifndef CY_BUILD_SERVICE_H
#define CY_BUILD_SERVICE_H
// The build service: the thing that owns the graph, the cache and the workers. M6 tasks 7.3
// and 7.4.
//
// `build-and-packaging` — "Build service": "Build execution SHALL be provided by a build service
// that owns the dependency graph, watches source files, maintains the cache, and schedules workers.
// The editor and the command-line tools SHALL both be **clients** of that service … Communication
// SHALL be a **structured protocol** — job started, progress, diagnostic, artefact ready, job
// completed — and SHALL NOT be shell invocation with output parsing. The service SHALL support
// cancellation."
//
// --- WHY THIS OWNS ITS OWN THREADS RATHER THAN USING cy::jobs::JobSystem -------------------------
//
// `core-jobs-and-concurrency` is explicit that the engine creates exactly one JobSystem owning all
// general-purpose worker threads, and `job_system.h` enforces it. This pool is not one of those,
// and the reason is in the same header: **a worker never blocks on I/O.** `begin_blocking_region`
// refuses on any thread executing a job, and a build node is almost entirely blocking I/O — read a
// source, hash it, read a cache record, write an artefact. Running cooks on the engine's workers
// would either trip that rule on every node or force it to be relaxed for everyone.
//
// So `cy_build` is a tool-time process with a tool-time pool, in the same position as the dedicated
// I/O thread `async.h` describes: a documented dedicated pool rather than a second general-purpose
// one. Nothing here is linked into a shipped runtime.
//
// --- TEARDOWN IS PART OF THE INTERFACE, NOT AN AFTERTHOUGHT --------------------------------------
//
// M5.5's gate found this project's first real engine defect by destroying a subsystem while a
// worker was still inside it — one run in forty, a SIGTRAP with no relevant frame on the stack. M6
// creates and destroys build services continuously, so destruction here is DEFINED rather than
// merely discouraged: `~BuildService` cancels any build in flight, waits for it to leave the
// service's state, and only then joins the pool. Destroying a service mid-build is a supported
// operation with a defined outcome, and `integration.build_service` executes it in a loop.
//
// --- PRECISE INVALIDATION IS A CONSEQUENCE, NOT A FEATURE ----------------------------------------
//
// "Invalidation SHALL be **content-based**, not timestamp-based: a file rewritten with identical
// content SHALL NOT invalidate anything." Nothing here reads a timestamp. A node's key is a
// function of content, options, platform, profile and the toolchain; a rewritten-identical file
// produces the same key and therefore a hit. And because a downstream key holds its upstream's
// OUTPUT digest rather than its key (design.md §1.5), a node that re-ran and produced identical
// bytes stops the propagation dead — which is the difference between the spike's three rebuilt
// nodes and its one.

#include <cy/build/artefact_store.h>
#include <cy/build/graph.h>
#include <cy/build/key.h>
#include <cy/build/producer.h>
#include <cy/build/toolchain.h>
#include <cy/core/assets/derivation.h>
#include <cy/core/assets/derived_cache.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cy::build {

/// What happened to one node in one build.
enum class NodeOutcome : u8 {
    /// The producer ran and its outputs were stored.
    Ran = 0,
    /// The key was in the cache; the node did not run.
    Cached = 1,
    /// A recorded dependency changed, so the entry was rebuilt. Distinct from `Ran` because the
    /// report has to be able to say WHY a node ran, which is the specification's requirement that
    /// "the tooling SHALL show which input changed".
    Rebuilt = 2,
    /// The build was cancelled, or an upstream failed, before this node started.
    Skipped = 3,
    /// The producer failed, or the node broke an access rule under `Enforce`.
    Failed = 4,
};

[[nodiscard]] const char* node_outcome_name(NodeOutcome outcome) noexcept;

/// What one node did.
struct NodeResult {
    NodeId id = NodeId::Invalid;
    std::string name;
    assets::DerivationKey key{};
    /// What downstream keys hold: a digest over this node's outputs, by name and content.
    assets::ContentHash result{};
    std::vector<NodeOutput> outputs;
    NodeOutcome outcome = NodeOutcome::Skipped;
    assets::CacheTier tier = assets::CacheTier::None;
    /// One line naming why the node ran, or why it failed. Empty on a plain cache hit.
    std::string reason;
    u64 duration_ns = 0;
    u64 bytes_produced = 0;
    /// What the node had to say, and what it did that it should not have. Carried on the result
    /// rather than accumulated globally, so a report can attribute both to a node without a second
    /// lookup — `build-and-packaging` requires a diagnostic to carry "a location naming a source
    /// file and position, an asset, or a graph node".
    std::vector<Diagnostic> diagnostics;
    std::vector<AccessViolation> violations;
};

/// What a whole build did. Deterministic: nodes appear in evaluation order regardless of which
/// worker finished first, so two runs of one graph produce comparable reports.
struct BuildReport {
    std::vector<NodeResult> nodes;
    std::vector<Diagnostic> diagnostics;
    std::vector<AccessViolation> violations;
    u64 ran = 0;
    u64 cached = 0;
    u64 rebuilt = 0;
    u64 skipped = 0;
    u64 failed = 0;
    u64 bytes_produced = 0;
    u64 wall_ns = 0;
    bool cancelled = false;
    /// How many nodes ran on a remote worker. Always zero in this milestone, and REPORTED rather
    /// than assumed: `build-and-packaging` requires that "WHEN no remote workers are reachable THEN
    /// the build SHALL execute locally with no change in result", and a degradation nobody can
    /// observe is a degradation nobody notices has become permanent.
    u64 distributed_nodes = 0;

    [[nodiscard]] bool succeeded() const noexcept { return failed == 0 && !cancelled; }
    /// The node's result, or null when the graph has no such node.
    [[nodiscard]] const NodeResult* node(std::string_view name) const noexcept;
};

/// The structured protocol `build-and-packaging` requires instead of shell output.
enum class BuildEventKind : u8 {
    JobStarted = 0,
    Progress = 1,
    Diagnostic = 2,
    ArtefactReady = 3,
    JobCompleted = 4,
    BuildCompleted = 5,
};

[[nodiscard]] const char* build_event_kind_name(BuildEventKind kind) noexcept;

/// One message. Valid only for the duration of the sink call, which is what lets it carry views.
struct BuildEvent {
    BuildEventKind kind = BuildEventKind::Progress;
    std::string_view node;
    std::string_view message;
    Severity severity = Severity::Info;
    NodeOutcome outcome = NodeOutcome::Skipped;
    /// For `ArtefactReady`: the artefact's identity.
    assets::ContentHash artefact{};
    u32 completed = 0;
    u32 total = 0;
};

/// Called on whichever thread produced the event, so an implementation must be thread-safe. The
/// service holds no lock while calling it.
using BuildEventSink = void (*)(void* user, const BuildEvent& event);

/// How a build is set up. Everything the service needs and nothing it can discover for itself.
struct BuildConfig {
    /// Finalised before `configure`; borrowed, and must outlive the service.
    const BuildGraph* graph = nullptr;
    const ProducerRegistry* producers = nullptr;
    const SourceProvider* sources = nullptr;
    /// Where immutable artefacts live.
    std::string artefact_root;
    /// The derived data cache's tiers. A `local` of "" disables caching entirely, which is the
    /// clean-build configuration a determinism gate wants.
    assets::DerivedCacheTiers cache;
    AccessPolicy policy = AccessPolicy::Enforce;
    /// Worker threads besides the calling one. Zero runs everything on the caller, which is the
    /// mode a deterministic gate uses and the mode a debugger is bearable in.
    u32 workers = 0;
    /// Whether distributable nodes may be handed to remote workers. There is no remote worker in
    /// this milestone; the flag is read and reported so that the degradation
    /// `build-and-packaging` requires — "WHEN no remote workers are reachable THEN the build SHALL
    /// execute locally with no change in result" — is a measured fact rather than a promise.
    bool distributed = false;
    BuildEventSink sink = nullptr;
    void* sink_user = nullptr;
    /// What produced the artefacts. Defaults to the toolchain that compiled this binary; a
    /// cross-compile or a remote worker supplies its own.
    ToolchainFingerprint toolchain;
};

/// The service. One per process is the ordinary shape; nothing prevents several.
class BuildService {
public:
    BuildService();
    ~BuildService();

    BuildService(const BuildService&) = delete;
    BuildService& operator=(const BuildService&) = delete;

    /// Take the configuration and start the pool. Fails on an unfinalised graph, a missing
    /// producer, an unconfigurable artefact store, or an incomplete toolchain fingerprint.
    [[nodiscard]] Status configure(BuildConfig config);

    /// Run the graph. Blocking; the calling thread participates, so `workers == 0` is a complete
    /// build rather than a degenerate one.
    [[nodiscard]] Expected<BuildReport, Error> build();

    /// Ask the build to stop. Unstarted nodes are skipped; nodes already finished keep their
    /// artefacts and their cache entries, which is the specification's "cancellation preserves
    /// work". Safe from any thread, including from inside a producer.
    void cancel() noexcept;

    /// Clear the cancellation, so one service can run a second build after a cancelled one.
    void reset_cancellation() noexcept;

    [[nodiscard]] bool is_cancelled() const noexcept {
        return cancelled_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] ArtefactStore& artefacts() noexcept { return artefacts_; }
    [[nodiscard]] const ArtefactStore& artefacts() const noexcept { return artefacts_; }

    /// Which nodes a change to one source would reach, without building anything. The
    /// "explain an invalidation" half of precise invalidation.
    [[nodiscard]] std::vector<std::string> would_invalidate(std::string_view source) const;

private:
    struct Run;

    /// `slot` selects this thread's own `DerivedCache`. Workers take 0..n-1 and the calling thread
    /// takes the last, so no two threads share one cache and none of them needs a lock.
    void worker_loop(u32 slot);
    void participate(Run& run, u32 slot);
    /// Const because it touches only the RUN, never the service: the run belongs to the caller and
    /// the service is shared between the workers that call this.
    void complete(Run& run, NodeId id, NodeResult result) const;
    /// Take one ready node and run it, if there is one. False means there was nothing to do, and
    /// the caller waits. Called with `lock` held; releases it while the node runs.
    [[nodiscard]] bool pump(std::unique_lock<std::mutex>& lock, Run& run, u32 slot);
    [[nodiscard]] NodeResult skipped_result(NodeId id, bool poisoned) const;
    [[nodiscard]] BuildReport finish_report(Run& run, u64 started_ns) const;
    [[nodiscard]] NodeResult evaluate(Run& run, NodeId id, assets::DerivedCache& cache);
    [[nodiscard]] NodeResult run_producer(Run& run, NodeId id, const assets::DerivationKey& key,
                                          assets::DerivedCache& cache);
    [[nodiscard]] Expected<KeyInputs, Error> key_inputs(Run& run, NodeId id) const;
    void emit(BuildEventKind kind, std::string_view node, std::string_view message,
              const NodeResult* result, u32 completed, u32 total) const;

    BuildConfig config_;
    ArtefactStore artefacts_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> shutting_down_{false};
    std::atomic<u32> builds_in_flight_{0};

    mutable std::mutex mutex_;
    std::condition_variable work_available_;
    std::condition_variable progress_;
    Run* active_ = nullptr;
    std::vector<std::thread> pool_;
    /// One cache per worker over the same directories: `DerivedCache` is deliberately not
    /// thread-safe, and its own header names this as the supported shape — the store is
    /// content-addressed and written atomically, so two workers producing one artefact write the
    /// same bytes to the same name.
    std::vector<std::unique_ptr<assets::DerivedCache>> caches_;
    bool configured_ = false;
};

}  // namespace cy::build

#endif  // CY_BUILD_SERVICE_H
