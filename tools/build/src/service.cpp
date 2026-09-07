#include <cy/build/service.h>
#include <cy/core/memory/system_allocator.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ranges>

namespace cy::build {
namespace {

[[nodiscard]] Allocator& build_allocator() noexcept {
    return system_allocator(MemoryDomain::Assets);
}

[[nodiscard]] u64 now_ns() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

// --- The cache record's payload ------------------------------------------------------------------
//
// A node's outputs, by logical name and content digest. This is what a cache hit returns: the
// artefacts themselves are in the content-addressed store, and the cache holds only the mapping
// from "this computation" to "these artefacts". Keeping them separate is what makes the cache
// disposable — "deleting it SHALL never lose project content" — and what lets two nodes that
// produce identical bytes share one artefact.
//
// Fixed-width and little-endian, like every other record in this tree, so a shared cache populated
// by continuous integration is readable by a developer on another architecture.

void put_u32(std::string& out, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFFU));
    }
}

void put_u64(std::string& out, u64 value) {
    for (u32 shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFFU));
    }
}

[[nodiscard]] std::string encode_outputs(const std::vector<NodeOutput>& outputs) {
    std::string record;
    put_u32(record, static_cast<u32>(outputs.size()));
    for (const NodeOutput& output : outputs) {
        put_u32(record, static_cast<u32>(output.name.size()));
        record.append(output.name);
        record.append(reinterpret_cast<const char*>(output.digest.bytes),
                      assets::ContentHash::kByteLength);
        put_u64(record, output.size);
    }
    return record;
}

class Reader {
public:
    Reader(const u8* data, usize size) noexcept : data_(data), size_(size) {}

    [[nodiscard]] bool take(void* out, usize size) noexcept {
        if (size_ - offset_ < size) {
            return false;
        }
        std::memcpy(out, data_ + offset_, size);
        offset_ += size;
        return true;
    }

    [[nodiscard]] bool take_u32(u32& out) noexcept {
        u8 bytes[4] = {};
        if (!take(bytes, sizeof(bytes))) {
            return false;
        }
        out = static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8) |
              (static_cast<u32>(bytes[2]) << 16) | (static_cast<u32>(bytes[3]) << 24);
        return true;
    }

    [[nodiscard]] bool take_u64(u64& out) noexcept {
        u8 bytes[8] = {};
        if (!take(bytes, sizeof(bytes))) {
            return false;
        }
        out = 0;
        for (u32 index = 0; index < 8; ++index) {
            out |= static_cast<u64>(bytes[index]) << (index * 8);
        }
        return true;
    }

    [[nodiscard]] bool take_string(u32 length, std::string& out) {
        if (size_ - offset_ < length) {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(data_ + offset_), length);
        offset_ += length;
        return true;
    }

private:
    const u8* data_;
    usize size_;
    usize offset_ = 0;
};

[[nodiscard]] bool decode_outputs(Span<const u8> payload, std::vector<NodeOutput>& out) {
    Reader reader(payload.data(), payload.size());
    u32 count = 0;
    if (!reader.take_u32(count)) {
        return false;
    }
    out.clear();
    out.reserve(count);
    for (u32 index = 0; index < count; ++index) {
        NodeOutput output;
        u32 length = 0;
        if (!reader.take_u32(length) || !reader.take_string(length, output.name) ||
            !reader.take(output.digest.bytes, assets::ContentHash::kByteLength) ||
            !reader.take_u64(output.size)) {
            return false;
        }
        out.push_back(std::move(output));
    }
    return true;
}

[[nodiscard]] std::vector<std::pair<std::string, assets::ContentHash>> as_pairs(
    const std::vector<NodeOutput>& outputs) {
    std::vector<std::pair<std::string, assets::ContentHash>> pairs;
    pairs.reserve(outputs.size());
    for (const NodeOutput& output : outputs) {
        pairs.emplace_back(output.name, output.digest);
    }
    return pairs;
}

}  // namespace

const char* node_outcome_name(NodeOutcome outcome) noexcept {
    switch (outcome) {
        case NodeOutcome::Ran:
            return "ran";
        case NodeOutcome::Cached:
            return "cached";
        case NodeOutcome::Rebuilt:
            return "rebuilt";
        case NodeOutcome::Skipped:
            return "skipped";
        case NodeOutcome::Failed:
            return "failed";
    }
    return "unknown";
}

const char* build_event_kind_name(BuildEventKind kind) noexcept {
    switch (kind) {
        case BuildEventKind::JobStarted:
            return "job-started";
        case BuildEventKind::Progress:
            return "progress";
        case BuildEventKind::Diagnostic:
            return "diagnostic";
        case BuildEventKind::ArtefactReady:
            return "artefact-ready";
        case BuildEventKind::JobCompleted:
            return "job-completed";
        case BuildEventKind::BuildCompleted:
            return "build-completed";
    }
    return "unknown";
}

const NodeResult* BuildReport::node(std::string_view name) const noexcept {
    for (const NodeResult& result : nodes) {
        if (result.name == name) {
            return &result;
        }
    }
    return nullptr;
}

/// Everything one build owns. Held on the calling thread's stack and shared with the pool through
/// `active_`; the mutex covers every field below.
struct BuildService::Run {
    std::vector<NodeResult> results;
    std::vector<u32> remaining;
    std::vector<bool> poisoned;
    std::deque<NodeId> ready;
    u32 completed = 0;
    u32 total = 0;
    u64 bytes_produced = 0;
    std::vector<Diagnostic> diagnostics;
    std::vector<AccessViolation> violations;
};

BuildService::BuildService() = default;

BuildService::~BuildService() {
    // Teardown under load, defined rather than hoped for. Three steps, in this order:
    //   1. cancel, so a build in flight stops taking new nodes;
    //   2. WAIT for it to leave the service's state — a worker still inside `evaluate` holds a
    //      reference to the caller's `Run`, and joining before it left would be M5.5's Jolt defect
    //      with a different subsystem's name on it;
    //   3. only then stop the pool.
    cancel();
    {
        std::unique_lock<std::mutex> lock(mutex_);
        progress_.wait(lock, [this] { return builds_in_flight_.load() == 0; });
        shutting_down_.store(true, std::memory_order_relaxed);
    }
    work_available_.notify_all();
    for (std::thread& worker : pool_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void BuildService::cancel() noexcept {
    cancelled_.store(true, std::memory_order_relaxed);
    progress_.notify_all();
}

void BuildService::reset_cancellation() noexcept {
    cancelled_.store(false, std::memory_order_relaxed);
}

Status BuildService::configure(BuildConfig config) {
    if (configured_) {
        return make_unexpected(
            Error{ErrorCode::AlreadyExists, "the build service is already configured", 0});
    }
    if (config.graph == nullptr || config.producers == nullptr || config.sources == nullptr) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a build needs a graph, producers and sources", 0});
    }
    if (!config.graph->is_finalized()) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "the graph is not finalised", 0});
    }
    if (config.toolchain.compiler.empty()) {
        config.toolchain = current_toolchain();
    }
    if (!toolchain_is_complete(config.toolchain)) {
        // The generated fingerprint lost its content. Refused loudly: a build that keyed against an
        // empty toolchain is precisely the defect design.md §1.3 found on the real importer.
        return make_unexpected(
            Error{ErrorCode::Internal, "the toolchain fingerprint is incomplete", 0});
    }
    // Every producer resolvable before anything runs, so a misspelled producer is a configure
    // error naming it rather than a failure two hours into a cook.
    for (u32 index = 0; index < config.graph->size(); ++index) {
        const NodeDesc& node = config.graph->node(static_cast<NodeId>(index));
        if (config.producers->find(node.producer) == nullptr) {
            return make_unexpected(Error{ErrorCode::NotFound, "no such producer", 0});
        }
    }
    if (Status stored = artefacts_.configure(config.artefact_root); !stored) {
        return stored;
    }

    config_ = std::move(config);
    if (config_.distributed) {
        // Distribution degrades to local rather than failing. Announced, so that a build which
        // believed it was distributing knows it was not.
        emit(BuildEventKind::Diagnostic, "",
             "no remote workers are reachable; every node runs locally", nullptr, 0, 0);
    }
    const u32 slots = config_.workers + 1;
    for (u32 slot = 0; slot < slots; ++slot) {
        auto cache = std::make_unique<assets::DerivedCache>();
        if (Status pointed = cache->configure(config_.cache); !pointed) {
            return pointed;
        }
        caches_.push_back(std::move(cache));
    }
    for (u32 worker = 0; worker < config_.workers; ++worker) {
        pool_.emplace_back([this, worker] { worker_loop(worker); });
    }
    configured_ = true;
    return ok();
}

void BuildService::emit(BuildEventKind kind, std::string_view node, std::string_view message,
                        const NodeResult* result, u32 completed, u32 total) const {
    if (config_.sink == nullptr) {
        return;
    }
    BuildEvent event;
    event.kind = kind;
    event.node = node;
    event.message = message;
    event.completed = completed;
    event.total = total;
    if (result != nullptr) {
        event.outcome = result->outcome;
        event.artefact = result->result;
    }
    config_.sink(config_.sink_user, event);
}

Expected<KeyInputs, Error> BuildService::key_inputs(Run& run, NodeId id) const {
    const BuildGraph& graph = *config_.graph;
    const NodeDesc& node = graph.node(id);

    KeyInputs inputs;
    inputs.node = &node;
    inputs.toolchain = &config_.toolchain;
    for (const std::string& source : node.sources) {
        const Expected<assets::ContentHash, Error> digest = config_.sources->digest(source);
        if (!digest) {
            return make_unexpected(digest.error());
        }
        inputs.sources.push_back(KeyedDigest{source, *digest});
    }
    for (const NodeId upstream : graph.upstreams(id)) {
        // The upstream's OUTPUT digest, not its key. design.md §1.5, and the whole of early cutoff.
        inputs.upstreams.push_back(
            KeyedDigest{graph.node(upstream).name, run.results[node_index(upstream)].result});
    }
    return inputs;
}

NodeResult BuildService::evaluate(Run& run, NodeId id, assets::DerivedCache& cache) {
    const NodeDesc& node = config_.graph->node(id);
    const u64 started = now_ns();

    NodeResult result;
    result.id = id;
    result.name = node.name;

    const Expected<KeyInputs, Error> inputs = key_inputs(run, id);
    if (!inputs) {
        result.outcome = NodeOutcome::Failed;
        result.reason = std::string("an input could not be read: ") + inputs.error().message;
        result.duration_ns = now_ns() - started;
        return result;
    }
    const Expected<assets::DerivationKey, Error> key = derivation_key(*inputs);
    if (!key) {
        result.outcome = NodeOutcome::Failed;
        result.reason =
            std::string("the derivation key could not be computed: ") + key.error().message;
        result.duration_ns = now_ns() - started;
        return result;
    }
    result.key = *key;

    struct Resolution {
        const SourceProvider* sources;
    } resolution{config_.sources};

    const auto resolver = [](void* user, std::string_view name, bool* found) noexcept {
        const Resolution& state = *static_cast<Resolution*>(user);
        const Expected<assets::ContentHash, Error> digest = state.sources->digest(name);
        *found = digest.has_value();
        return digest ? *digest : assets::ContentHash{};
    };

    assets::CacheResult lookup = cache.lookup(*key, resolver, &resolution);
    if (lookup.is_hit() && !decode_outputs(lookup.entry.payload(), result.outputs)) {
        // A record this build cannot read is a miss with a reason, never a failure: the cache is
        // disposable, and a corrupt entry that stopped a build would make the cache a thing that
        // can break you.
        result.outputs.clear();
        lookup.outcome = assets::CacheOutcome::Miss;
        lookup.reason = "the cache record could not be decoded";
    }
    if (lookup.is_hit()) {
        // A hit is only a hit if the artefacts it names are still in the store. The cache is
        // disposable and so is the store; they are deleted independently, and serving a manifest
        // whose artefacts are gone would be a "success" that produced nothing.
        const bool present = std::ranges::all_of(
            result.outputs, [this](const NodeOutput& o) { return artefacts_.contains(o.digest); });
        if (present) {
            result.outcome = NodeOutcome::Cached;
            result.tier = lookup.tier;
            result.result = result_digest(as_pairs(result.outputs));
            result.duration_ns = now_ns() - started;
            return result;
        }
        result.outputs.clear();
        lookup.outcome = assets::CacheOutcome::Miss;
        lookup.reason = "the cached artefacts are no longer in the store";
    }

    NodeResult produced = run_producer(run, id, *key, cache);
    produced.duration_ns = now_ns() - started;
    if (produced.outcome == NodeOutcome::Ran &&
        lookup.outcome == assets::CacheOutcome::Invalidated) {
        produced.outcome = NodeOutcome::Rebuilt;
        produced.reason =
            std::string("a recorded dependency changed: ") + std::string(lookup.stale);
    } else if (produced.outcome == NodeOutcome::Ran && produced.reason.empty()) {
        produced.reason = lookup.reason[0] != '\0' ? lookup.reason : "not in the cache";
    }
    return produced;
}

NodeResult BuildService::run_producer(Run& run, NodeId id, const assets::DerivationKey& key,
                                      assets::DerivedCache& cache) {
    const NodeDesc& node = config_.graph->node(id);
    const Producer* producer = config_.producers->find(node.producer);

    NodeResult result;
    result.id = id;
    result.name = node.name;
    result.key = key;

    NodeContext context(node, *config_.sources, config_.policy);
    context.bind_cancellation(&cancelled_);

    // Materialise every upstream output the node may read, under both the upstream NODE's name and
    // each output's own logical name. Two addressing styles, one binding step: a node that consumes
    // "cook:city" reads it by that name, and a node that wants one particular output of it reads
    // that output's name.
    Array<u8> bytes(build_allocator());
    for (const NodeId upstream : config_.graph->upstreams(id)) {
        const NodeResult& source = run.results[node_index(upstream)];
        for (usize index = 0; index < source.outputs.size(); ++index) {
            const NodeOutput& output = source.outputs[index];
            if (Status read = artefacts_.get(output.digest, bytes); !read) {
                result.outcome = NodeOutcome::Failed;
                result.reason = "an upstream artefact is missing from the store";
                return result;
            }
            std::string content(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (index == 0) {
                context.bind_upstream(config_.graph->node(upstream).name, content);
            }
            context.bind_upstream(output.name, std::move(content));
        }
    }

    const Status ran = producer->body(context);
    result.diagnostics = context.diagnostics();
    result.violations = context.violations();
    for (const Diagnostic& diagnostic : result.diagnostics) {
        if (diagnostic.severity == Severity::Error && result.reason.empty()) {
            result.reason = diagnostic.message;
        }
    }
    if (!ran) {
        result.outcome = NodeOutcome::Failed;
        if (result.reason.empty()) {
            result.reason = ran.error().message;
        }
        return result;
    }

    // Every declared output must exist. A node that silently produced fewer outputs than it
    // declared would leave a downstream node reading the previous build's artefact — the stale
    // output failure arriving through the outputs rather than through the inputs.
    for (const std::string& declared : node.outputs) {
        const bool written = std::ranges::any_of(
            context.outputs(),
            [&declared](const NodeOutput& output) { return output.name == declared; });
        if (!written) {
            result.outcome = NodeOutcome::Failed;
            result.reason = "the node did not write its declared output '" + declared + "'";
            return result;
        }
    }
    if (!context.violations().empty() && config_.policy == AccessPolicy::Enforce) {
        result.outcome = NodeOutcome::Failed;
        if (result.reason.empty()) {
            result.reason = "the node broke an access rule";
        }
        return result;
    }

    result.outputs = context.outputs();
    std::ranges::sort(result.outputs,
                      [](const NodeOutput& a, const NodeOutput& b) { return a.name < b.name; });
    for (const NodeOutput& output : result.outputs) {
        const std::string& content = context.output_bytes(output.name);
        if (!artefacts_.put(content.data(), content.size())) {
            result.outcome = NodeOutcome::Failed;
            result.reason = "the artefact could not be stored";
            return result;
        }
        result.bytes_produced += output.size;
    }

    const std::string record = encode_outputs(result.outputs);
    std::vector<assets::DerivedDependency> dependencies;
    dependencies.reserve(context.discoveries().size());
    for (const DiscoveredDependency& discovered : context.discoveries()) {
        dependencies.push_back(assets::DerivedDependency{discovered.name, discovered.hash});
    }

    assets::DerivedArtefact artefact;
    artefact.kind = derived_kind_of(node.kind);
    artefact.producer = node.producer;
    artefact.payload = Span<const u8>(reinterpret_cast<const u8*>(record.data()), record.size());
    artefact.dependencies =
        Span<const assets::DerivedDependency>(dependencies.data(), dependencies.size());
    if (Status stored = cache.store(key, artefact); !stored) {
        result.outcome = NodeOutcome::Failed;
        result.reason = "the cache entry could not be written";
        return result;
    }

    result.outcome = NodeOutcome::Ran;
    result.result = result_digest(as_pairs(result.outputs));
    return result;
}

void BuildService::complete(Run& run, NodeId id, NodeResult result) const {
    const BuildGraph& graph = *config_.graph;
    const bool failed = result.outcome == NodeOutcome::Failed;

    run.bytes_produced += result.bytes_produced;
    run.results[node_index(id)] = std::move(result);
    ++run.completed;

    for (const NodeId down : graph.downstreams(id)) {
        if (failed) {
            run.poisoned[node_index(down)] = true;
        }
        if (--run.remaining[node_index(down)] == 0) {
            run.ready.push_back(down);
        }
    }
}

NodeResult BuildService::skipped_result(NodeId id, bool poisoned) const {
    NodeResult result;
    result.id = id;
    result.name = config_.graph->node(id).name;
    result.outcome = NodeOutcome::Skipped;
    result.reason = poisoned ? "an upstream node failed" : "the build was cancelled";
    return result;
}

bool BuildService::pump(std::unique_lock<std::mutex>& lock, Run& run, u32 slot) {
    if (run.ready.empty()) {
        return false;
    }
    const NodeId id = run.ready.front();
    run.ready.pop_front();
    const bool poisoned = run.poisoned[node_index(id)];
    const u32 completed = run.completed;
    const u32 total = run.total;
    lock.unlock();

    const NodeDesc& node = config_.graph->node(id);
    emit(BuildEventKind::JobStarted, node.name, node.producer, nullptr, completed, total);

    NodeResult result = (poisoned || is_cancelled()) ? skipped_result(id, poisoned)
                                                     : evaluate(run, id, *caches_[slot]);
    for (const NodeOutput& output : result.outputs) {
        NodeResult carrier = NodeResult{};
        carrier.outcome = result.outcome;
        carrier.result = output.digest;
        emit(BuildEventKind::ArtefactReady, node.name, output.name, &carrier, completed, total);
    }
    for (const Diagnostic& diagnostic : result.diagnostics) {
        emit(BuildEventKind::Diagnostic, node.name, diagnostic.message, nullptr, completed, total);
    }
    emit(BuildEventKind::JobCompleted, node.name, result.reason, &result, completed + 1, total);

    lock.lock();
    complete(run, id, std::move(result));
    work_available_.notify_all();
    progress_.notify_all();
    return true;
}

void BuildService::participate(Run& run, u32 slot) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (run.completed < run.total) {
        if (!pump(lock, run, slot)) {
            // Nothing ready and the build is not finished, so something is in flight on another
            // thread. Waiting is correct; spinning would burn a core per idle worker.
            progress_.wait(lock);
        }
    }
}

void BuildService::worker_loop(u32 slot) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!shutting_down_.load(std::memory_order_relaxed)) {
        if (active_ == nullptr || !pump(lock, *active_, slot)) {
            work_available_.wait(lock);
        }
    }
}

BuildReport BuildService::finish_report(Run& run, u64 started_ns) const {
    BuildReport report;
    report.wall_ns = now_ns() - started_ns;
    report.cancelled = is_cancelled();
    report.bytes_produced = run.bytes_produced;
    report.nodes.reserve(run.total);

    // Evaluation order, never completion order: a report whose row order depended on which worker
    // finished first could not be diffed between two runs, and diffing two runs is how both of this
    // milestone's exit criteria are measured.
    for (const NodeId id : config_.graph->order()) {
        NodeResult& result = run.results[node_index(id)];
        switch (result.outcome) {
            case NodeOutcome::Ran:
                ++report.ran;
                break;
            case NodeOutcome::Cached:
                ++report.cached;
                break;
            case NodeOutcome::Rebuilt:
                ++report.rebuilt;
                break;
            case NodeOutcome::Skipped:
                ++report.skipped;
                break;
            case NodeOutcome::Failed:
                ++report.failed;
                break;
        }
        report.diagnostics.insert(report.diagnostics.end(), result.diagnostics.begin(),
                                  result.diagnostics.end());
        report.violations.insert(report.violations.end(), result.violations.begin(),
                                 result.violations.end());
        report.nodes.push_back(std::move(result));
    }
    return report;
}

Expected<BuildReport, Error> BuildService::build() {
    if (!configured_) {
        return make_unexpected(
            Error{ErrorCode::Unavailable, "the build service is not configured", 0});
    }
    const BuildGraph& graph = *config_.graph;
    const u64 started = now_ns();

    Run run;
    run.total = graph.size();
    run.results.resize(run.total);
    run.remaining.resize(run.total, 0);
    run.poisoned.assign(run.total, false);
    for (u32 index = 0; index < run.total; ++index) {
        run.remaining[index] = static_cast<u32>(graph.upstreams(static_cast<NodeId>(index)).size());
    }

    builds_in_flight_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const NodeId id : graph.order()) {
            if (run.remaining[node_index(id)] == 0) {
                run.ready.push_back(id);
            }
        }
        active_ = &run;
    }
    work_available_.notify_all();

    // The calling thread participates, so `workers == 0` is a complete build rather than a
    // degenerate one, and a deterministic gate can run the whole graph on one thread.
    participate(run, config_.workers);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = nullptr;
    }

    BuildReport report = finish_report(run, started);
    emit(BuildEventKind::BuildCompleted, "", report.succeeded() ? "succeeded" : "failed", nullptr,
         static_cast<u32>(report.ran + report.cached + report.rebuilt), run.total);

    {
        // LAST, and under the mutex. `~BuildService` waits on `progress_` for this counter to reach
        // zero and then frees the members this function has been touching, so the counter must not
        // drop until every one of those touches is behind us — and the decrement must happen under
        // the mutex, because a decrement outside it could land between the destructor's predicate
        // check and its wait, which is a lost wakeup and a destructor that hangs once in however
        // many runs. Both halves of that are the shape of defect M5.5's gate found in Jolt's job
        // bridge: a pool torn down underneath a worker that had not finished leaving.
        std::lock_guard<std::mutex> lock(mutex_);
        builds_in_flight_.fetch_sub(1, std::memory_order_relaxed);
    }
    progress_.notify_all();
    return report;
}

std::vector<std::string> BuildService::would_invalidate(std::string_view source) const {
    std::vector<std::string> names;
    if (config_.graph == nullptr) {
        return names;
    }
    for (const NodeId id : config_.graph->dependents_of_source(source)) {
        names.push_back(config_.graph->node(id).name);
    }
    return names;
}

}  // namespace cy::build
