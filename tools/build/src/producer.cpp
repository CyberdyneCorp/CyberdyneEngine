#include <cy/build/producer.h>
#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>

#include <algorithm>
#include <ranges>

namespace cy::build {
namespace {

[[nodiscard]] Error denied(const char* message) noexcept {
    return Error{ErrorCode::PermissionDenied, message, 0};
}

[[nodiscard]] Allocator& build_allocator() noexcept {
    return system_allocator(MemoryDomain::Assets);
}

[[nodiscard]] Status fill(Array<u8>& out, std::string_view content) noexcept {
    out.clear();
    if (Status reserved = out.reserve(content.size()); !reserved) {
        return reserved;
    }
    for (const char character : content) {
        if (Status pushed = out.push_back(static_cast<u8>(character)); !pushed) {
            return pushed;
        }
    }
    return ok();
}

}  // namespace

const char* violation_kind_name(ViolationKind kind) noexcept {
    switch (kind) {
        case ViolationKind::UndeclaredRead:
            return "undeclared-read";
        case ViolationKind::UndeclaredWrite:
            return "undeclared-write";
        case ViolationKind::MissingOutput:
            return "missing-output";
    }
    return "unknown";
}

const char* severity_name(Severity severity) noexcept {
    switch (severity) {
        case Severity::Info:
            return "info";
        case Severity::Warning:
            return "warning";
        case Severity::Error:
            return "error";
    }
    return "unknown";
}

// --- DirectorySourceProvider --------------------------------------------------------------------

Expected<std::string, Error> DirectorySourceProvider::resolve(std::string_view name) const {
    if (!is_project_relative(name)) {
        return make_unexpected(denied("a source name must be project-relative"));
    }
    std::string path = root_;
    path += '/';
    path.append(name);
    return path;
}

Status DirectorySourceProvider::read(std::string_view name, Array<u8>& out) const {
    const Expected<std::string, Error> path = resolve(name);
    if (!path) {
        return make_unexpected(path.error());
    }
    return assets::fs::read_whole(path->c_str(), out);
}

Expected<assets::ContentHash, Error> DirectorySourceProvider::digest(std::string_view name) const {
    Array<u8> bytes(build_allocator());
    if (Status read = this->read(name, bytes); !read) {
        return make_unexpected(read.error());
    }
    return assets::content_hash(bytes.data(), bytes.size());
}

bool DirectorySourceProvider::exists(std::string_view name) const {
    const Expected<std::string, Error> path = resolve(name);
    return path && assets::fs::exists(path->c_str());
}

// --- MemorySourceProvider -----------------------------------------------------------------------

void MemorySourceProvider::set(std::string name, std::string content) {
    content_[std::move(name)] = std::move(content);
}

void MemorySourceProvider::erase(std::string_view name) {
    content_.erase(std::string(name));
}

Status MemorySourceProvider::read(std::string_view name, Array<u8>& out) const {
    const auto found = content_.find(std::string(name));
    if (found == content_.end()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such source", 0});
    }
    return fill(out, found->second);
}

Expected<assets::ContentHash, Error> MemorySourceProvider::digest(std::string_view name) const {
    const auto found = content_.find(std::string(name));
    if (found == content_.end()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such source", 0});
    }
    return assets::content_hash(found->second.data(), found->second.size());
}

bool MemorySourceProvider::exists(std::string_view name) const {
    return content_.contains(std::string(name));
}

// --- NodeContext --------------------------------------------------------------------------------

NodeContext::NodeContext(const NodeDesc& node, const SourceProvider& sources, AccessPolicy policy)
    : node_(&node), sources_(&sources), policy_(policy) {}

bool NodeContext::declares_source(std::string_view name) const noexcept {
    return std::ranges::find(node_->sources, name) != node_->sources.end();
}

bool NodeContext::declares_output(std::string_view name) const noexcept {
    return std::ranges::find(node_->outputs, name) != node_->outputs.end();
}

void NodeContext::record(ViolationKind kind, std::string_view name) {
    violations_.push_back(AccessViolation{kind, node_->name, std::string(name)});
    diagnose(Severity::Error, violation_kind_name(kind),
             std::string("the node did not declare '") + std::string(name) + "'",
             std::string(name));
}

Status NodeContext::read(std::string_view name, Array<u8>& out) {
    // An upstream's output first: it is the common case in a graph, and it is held in memory
    // because the service has already materialised it from the artefact store.
    const auto upstream = upstream_.find(std::string(name));
    if (upstream != upstream_.end()) {
        return fill(out, upstream->second);
    }
    if (declares_source(name)) {
        return sources_->read(name, out);
    }

    record(ViolationKind::UndeclaredRead, name);
    if (policy_ == AccessPolicy::Enforce) {
        return make_unexpected(denied("undeclared read"));
    }
    return sources_->read(name, out);
}

Status NodeContext::discover(std::string_view name, Array<u8>& out) {
    if (Status read = sources_->read(name, out); !read) {
        return read;
    }
    const assets::ContentHash hash = assets::content_hash(out.data(), out.size());
    // Recorded once per name. A producer that reads one include twice must not put two entries in
    // the cache record, or the record's size becomes a function of the producer's control flow.
    for (const DiscoveredDependency& existing : discoveries_) {
        if (existing.name == name) {
            return ok();
        }
    }
    discoveries_.push_back(DiscoveredDependency{std::string(name), hash});
    return ok();
}

Status NodeContext::write(std::string_view name, const void* data, usize size) {
    if (!declares_output(name)) {
        record(ViolationKind::UndeclaredWrite, name);
        if (policy_ == AccessPolicy::Enforce) {
            return make_unexpected(denied("undeclared write"));
        }
    }
    std::string bytes(static_cast<const char*>(data), size);
    const assets::ContentHash digest = assets::content_hash(bytes.data(), bytes.size());

    const std::string key(name);
    const auto existing =
        std::ranges::find_if(outputs_, [&key](const NodeOutput& o) { return o.name == key; });
    if (existing != outputs_.end()) {
        existing->digest = digest;
        existing->size = size;
    } else {
        outputs_.push_back(NodeOutput{key, digest, size});
    }
    written_[key] = std::move(bytes);
    return ok();
}

void NodeContext::diagnose(Severity severity, std::string code, std::string message,
                           std::string location) {
    diagnostics_.push_back(Diagnostic{severity, std::move(code), std::move(message), node_->name,
                                      std::move(location)});
}

bool NodeContext::is_cancelled() const noexcept {
    return cancelled_ != nullptr && cancelled_->load(std::memory_order_relaxed);
}

const std::string& NodeContext::output_bytes(std::string_view name) const {
    static const std::string kEmpty;
    const auto found = written_.find(std::string(name));
    return found == written_.end() ? kEmpty : found->second;
}

void NodeContext::bind_upstream(std::string name, std::string bytes) {
    upstream_[std::move(name)] = std::move(bytes);
}

void NodeContext::bind_cancellation(const std::atomic<bool>* cancelled) noexcept {
    cancelled_ = cancelled;
}

// --- ProducerRegistry ---------------------------------------------------------------------------

Status ProducerRegistry::add(Producer producer) {
    if (producer.name.empty() || producer.body == nullptr) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a producer needs a name and a body", 0});
    }
    const std::string name = producer.name;
    if (producers_.contains(name)) {
        return make_unexpected(Error{ErrorCode::AlreadyExists, "duplicate producer", 0});
    }
    producers_.emplace(name, std::move(producer));
    return ok();
}

const Producer* ProducerRegistry::find(std::string_view name) const noexcept {
    const auto found = producers_.find(std::string(name));
    return found == producers_.end() ? nullptr : &found->second;
}

namespace {

/// Read every declared input in declared order, in one place, so the four built-ins below cannot
/// each get the order subtly different — which would make two of them disagree about what "the
/// inputs" of a node are.
[[nodiscard]] Status gather(NodeContext& context, std::string& out) {
    Array<u8> bytes(build_allocator());
    for (const std::string& source : context.node().sources) {
        if (Status read = context.read(source, bytes); !read) {
            return read;
        }
        out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    for (const std::string& upstream : context.node().upstreams) {
        if (Status read = context.read(upstream, bytes); !read) {
            return read;
        }
        out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    return ok();
}

/// `copy` — every input concatenated, written to the first declared output. The identity producer,
/// and the one a graph uses when the interesting part is the graph.
[[nodiscard]] Status produce_copy(NodeContext& context) {
    std::string content;
    if (Status gathered = gather(context, content); !gathered) {
        return gathered;
    }
    return context.write(context.node().outputs.front(), content.data(), content.size());
}

/// `normalise` — the inputs with trailing whitespace on each line removed.
///
/// It exists to make design.md §1.5's early cutoff testable on real bytes: an edit that adds
/// trailing spaces changes this node's INPUT and not its OUTPUT, so everything downstream must hit
/// the cache. Under deep input keys it would rebuild instead, and the spike measured that as three
/// nodes against one.
[[nodiscard]] Status produce_normalise(NodeContext& context) {
    std::string content;
    if (Status gathered = gather(context, content); !gathered) {
        return gathered;
    }
    std::string normalised;
    normalised.reserve(content.size());
    usize line_start = 0;
    while (line_start <= content.size()) {
        const usize newline = std::min(content.find('\n', line_start), content.size());
        usize end = newline;
        while (end > line_start && (content[end - 1] == ' ' || content[end - 1] == '\t')) {
            --end;
        }
        normalised.append(content, line_start, end - line_start);
        if (newline < content.size()) {
            normalised += '\n';
        }
        line_start = newline + 1;
    }
    return context.write(context.node().outputs.front(), normalised.data(), normalised.size());
}

/// `concat` — every input joined by the `separator` option, so a node's options demonstrably reach
/// its output and a changed option demonstrably changes the key.
[[nodiscard]] Status produce_concat(NodeContext& context) {
    const std::string separator(context.node().option("separator", "\n"));
    Array<u8> bytes(build_allocator());
    std::string content;
    bool first = true;
    const auto append = [&](const std::string& name) -> Status {
        if (Status read = context.read(name, bytes); !read) {
            return read;
        }
        if (!first) {
            content += separator;
        }
        first = false;
        content.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return ok();
    };
    for (const std::string& source : context.node().sources) {
        if (Status appended = append(source); !appended) {
            return appended;
        }
    }
    for (const std::string& upstream : context.node().upstreams) {
        if (Status appended = append(upstream); !appended) {
            return appended;
        }
    }
    return context.write(context.node().outputs.front(), content.data(), content.size());
}

/// `manifest` — a sorted listing of every input and its digest.
///
/// The shape a real packaging node has, and deterministic for the reason a real one must be: the
/// listing is sorted by name, never by the order the inputs were read.
[[nodiscard]] Status produce_manifest(NodeContext& context) {
    std::vector<std::string> names;
    names.insert(names.end(), context.node().sources.begin(), context.node().sources.end());
    names.insert(names.end(), context.node().upstreams.begin(), context.node().upstreams.end());
    std::ranges::sort(names);

    Array<u8> bytes(build_allocator());
    std::string manifest = "cymanifest 1\n";
    for (const std::string& name : names) {
        if (Status read = context.read(name, bytes); !read) {
            return read;
        }
        const assets::ContentHash digest = assets::content_hash(bytes.data(), bytes.size());
        char text[assets::ContentHash::kTextLength + 1] = {};
        digest.format(text);
        manifest += "  entry \"";
        manifest += name;
        manifest += "\" ";
        manifest += text;
        manifest += '\n';
    }
    return context.write(context.node().outputs.front(), manifest.data(), manifest.size());
}

}  // namespace

Status ProducerRegistry::add_builtins() {
    const Producer builtins[] = {
        Producer{"copy", 1, produce_copy, true},
        Producer{"normalise", 1, produce_normalise, true},
        Producer{"concat", 1, produce_concat, true},
        Producer{"manifest", 1, produce_manifest, true},
    };
    for (const Producer& producer : builtins) {
        if (Status added = add(producer); !added) {
            return added;
        }
    }
    return ok();
}

}  // namespace cy::build
