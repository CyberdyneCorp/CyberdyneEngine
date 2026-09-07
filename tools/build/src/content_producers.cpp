// `import` and `cook` as graph nodes. M7 task 1.4; see content_producers.h for the argument.

#include <cy/build/content_producers.h>

#include <cy/build/graph.h>
#include <cy/cook/pipeline.h>
#include <cy/core/assets/hash.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/memory/ownership.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/import/importer.h>
#include <cy/import/pipeline.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace cy::build {
namespace {

/// The world the `cook` producer emits blocks against.
///
/// A PROCESS-WIDE POINTER, and that is a real constraint rather than a convenience: a
/// `ProducerBody` is a plain function pointer, which is the right shape for a producer — it must be
/// a pure function of its `NodeContext` — and leaves nowhere to hang the one thing a cook needs and
/// cannot derive. So a process configures ONE component registry, and registering a second replaces
/// the first. `cy_build` registers one at startup; a caller that wanted two would need the producer
/// registry to carry per-producer state, which is a change to `producer.h` and not to this file.
///
/// Atomic because a worker thread reads it while the main thread may still be registering: the
/// write happens before the pool starts in every path that exists today, and an atomic says so
/// rather than relying on that staying true.
std::atomic<const ecs::World*> g_cook_world{nullptr};

[[nodiscard]] assets::VirtualPath virtual_path_or_root(std::string_view name) noexcept {
    auto path = assets::VirtualPath::normalise(name);
    return path ? path.value() : assets::VirtualPath{};
}

// --- import -------------------------------------------------------------------------------------

/// Routes every input the importer discovers through `NodeContext::discover`.
///
/// The two halves of the audit meet here. `ImportResolver` exists so that an importer cannot read a
/// file without recording it; `NodeContext::discover` exists so that a producer cannot read a name
/// without the build recording it. Composing them means a glTF's external `.bin` is a declared
/// dependency of the NODE, so editing it invalidates the node — which is the whole point of a
/// derivation graph and is exactly what running the importer beside the graph could not give.
class NodeImportResolver final : public import::ImportResolver {
public:
    NodeImportResolver(NodeContext& context, const assets::VirtualPath& source) noexcept
        : context_(&context), source_(source), bytes_(default_allocator()) {}

    [[nodiscard]] Expected<Span<const u8>, Error> read(std::string_view path) noexcept override {
        // Relative to the source's own directory, as `ImportResolver::read` promises. A
        // project-absolute path is used as it stands.
        std::string resolved;
        if (!path.empty() && path.front() == '/') {
            resolved.assign(path.substr(1));
        } else {
            const std::string_view parent = source_.parent();
            if (!parent.empty()) {
                resolved.assign(parent);
                resolved += '/';
            }
            resolved.append(path);
        }
        bytes_.clear();
        if (Status found = context_->discover(resolved, bytes_); !found) {
            return make_unexpected(found.error());
        }
        return Span<const u8>(bytes_.data(), bytes_.size());
    }

    [[nodiscard]] Status observe(std::string_view name,
                                 const assets::ContentHash& digest) noexcept override {
        // Nothing to read: the importer consulted something it resolved for itself. The digest is
        // recorded by the CACHE rather than by the graph, and the graph's own record of it is the
        // node's declared options — so this is accepted and not turned into a phantom dependency on
        // a name no source provider can serve.
        (void)name;
        (void)digest;
        return ok();
    }

private:
    NodeContext* context_;
    assets::VirtualPath source_;
    /// One buffer reused across reads. `read` returns a view into it, and `ImportResolver`'s
    /// contract is that the view is good until the next call.
    Array<u8> bytes_;
};

/// `build-and-packaging` requires that "every diagnostic from every stage … SHALL share a
/// structure", and these two enumerations are that structure seen from each end.
[[nodiscard]] Severity severity_of(import::ImportSeverity severity) noexcept {
    switch (severity) {
        case import::ImportSeverity::Error:
            return Severity::Error;
        case import::ImportSeverity::Warning:
            return Severity::Warning;
        case import::ImportSeverity::Info:
            break;
    }
    return Severity::Info;
}

[[nodiscard]] Status apply_declared_options(const NodeDesc& node,
                                            const import::OptionsSchema& schema,
                                            import::ImportOptions& out) noexcept {
    // `opt.<name>` on the node. Prefixed so that an importer option cannot collide with `variant`
    // or `profile`, and so that reading a node's declaration says which settings are the
    // importer's.
    constexpr std::string_view kPrefix = "opt.";
    for (const NodeOption& option : node.options) {
        if (!std::string_view(option.name).starts_with(kPrefix)) {
            continue;
        }
        const std::string_view name = std::string_view(option.name).substr(kPrefix.size());
        const import::OptionSpec* declared = schema.find(name);
        if (declared == nullptr) {
            return fail(ErrorCode::InvalidArgument, "a node option no importer schema declares");
        }
        import::OptionValue parsed;
        switch (declared->type) {
            case import::OptionType::Bool:
                parsed = import::OptionValue::of_bool(option.value == "true");
                break;
            case import::OptionType::Int:
                parsed =
                    import::OptionValue::of_int(std::strtoll(option.value.c_str(), nullptr, 10));
                break;
            case import::OptionType::Float:
                parsed = import::OptionValue::of_float(std::strtod(option.value.c_str(), nullptr));
                break;
            case import::OptionType::Text:
                // The view points into the node, which outlives the options: a node is immutable
                // once the graph is finalised.
                parsed = import::OptionValue::of_text(option.value);
                break;
            case import::OptionType::Enumeration:
                parsed = import::OptionValue::of_enumeration(option.value);
                break;
        }
        if (Status set = out.set(schema, name, parsed); !set) {
            return set;
        }
    }
    return ok();
}

/// `import` — one source asset in, one bundle of cooked sub-assets out.
///
/// The node declares exactly one source and exactly one output. One output rather than one per
/// sub-asset because sub-asset names are DISCOVERED — an importer names what it found — and a
/// node's outputs must be declared before it runs. The bundle is the same encoding the derived-data
/// cache stores, so there is one framing rather than two.
[[nodiscard]] Status produce_import(NodeContext& context) {
    const NodeDesc& node = context.node();
    if (node.sources.size() != 1 || node.outputs.size() != 1) {
        return fail(ErrorCode::InvalidArgument,
                    "an import node declares exactly one source and one output");
    }

    import::ImporterRegistry registry;
    if (Status registered = import::register_builtin_importers(registry); !registered) {
        return registered;
    }
    const assets::VirtualPath source = virtual_path_or_root(node.sources.front());
    import::Importer* importer = registry.find_for_source(source);
    if (importer == nullptr) {
        context.diagnose(Severity::Error, "no-importer",
                         "no importer claims this source's extension", node.sources.front());
        return fail(ErrorCode::NotFound, "no importer claims this source's extension");
    }

    Array<u8> bytes(default_allocator());
    if (Status read = context.read(node.sources.front(), bytes); !read) {
        return read;
    }

    const import::OptionsSchema schema = importer->schema();
    import::ImportOptions options;
    if (Status applied = apply_declared_options(node, schema, options); !applied) {
        return applied;
    }

    auto profile = import::cook_profile_from_name(node.profile);
    if (!profile) {
        return make_unexpected(profile.error());
    }
    auto variant = assets::VariantKey::parse(node.option("variant", "desktop"));
    if (!variant) {
        return make_unexpected(variant.error());
    }

    NodeImportResolver resolver(context, source);
    import::ImportRequest request;
    request.source = source;
    request.bytes = Span<const u8>(bytes.data(), bytes.size());
    request.options = &options;
    request.variant = variant.value();
    request.profile = profile.value();
    request.resolver = &resolver;

    import::ImportResult result;
    if (Status imported = importer->import(request, result); !imported) {
        return imported;
    }
    for (const import::ImportDiagnostic& reported : result.diagnostics()) {
        context.diagnose(severity_of(reported.severity), reported.code,
                         static_cast<const char*>(reported.detail),
                         static_cast<const char*>(reported.subject));
    }
    if (result.has_errors()) {
        // No output. `build-and-packaging` requires a failed node to produce no artefact, and an
        // import that reported an error about its source is a failed node — a cached failure is a
        // failure you cannot clear by fixing the source.
        return fail(ErrorCode::InvalidArgument, "the importer reported an error about this source");
    }

    Array<u8> bundle(default_allocator());
    if (Status encoded = import::encode_import_bundle(result, bundle); !encoded) {
        return encoded;
    }
    return context.write(node.outputs.front(), bundle.data(), bundle.size());
}

// --- cook ---------------------------------------------------------------------------------------

/// `cook` — every declared authoring document in, one `.cypak` out.
///
/// The documents are read through `NodeContext::read` into a `MemoryMount` and the cook is handed a
/// virtual filesystem over that mount alone, so a document the node did not declare does not exist
/// as far as the cook is concerned. That is the audit, by construction rather than by a pass
/// afterwards.
///
/// THE ONE PLACE A REAL PATH IS USED, and why. `PackageWriter::write` takes a native path — the
/// package format is written incrementally with its chunk table patched at the end — so the
/// producer writes to a scratch file, reads it back, and hands the bytes to `context.write`. Every
/// READ is through the context, which is where an undeclared access could produce a stale artefact;
/// the scratch write cannot, because nothing but this function ever names it.
[[nodiscard]] Status produce_cook(NodeContext& context) {
    const NodeDesc& node = context.node();
    if (node.outputs.size() != 1) {
        return fail(ErrorCode::InvalidArgument, "a cook node declares exactly one output");
    }
    const ecs::World* const world = g_cook_world.load(std::memory_order_acquire);
    if (world == nullptr) {
        return fail(ErrorCode::Unavailable,
                    "the cook producer was registered without a component registry, so a cooked "
                    "block would be emitted against a layout nothing agreed to");
    }

    assets::VirtualFileSystem files;
    auto mount = make_unique<assets::MemoryMount>(default_allocator(), "cook-node");
    if (!mount) {
        return make_unexpected(mount.error());
    }
    assets::MemoryMount& documents_mount = *mount.value();
    if (auto mounted = files.mount_owned(std::move(mount.value()), 0); !mounted) {
        return make_unexpected(mounted.error());
    }

    Array<u8> bytes(default_allocator());
    for (const std::string& name : node.sources) {
        bytes.clear();
        if (Status read = context.read(name, bytes); !read) {
            return read;
        }
        const assets::VirtualPath path = virtual_path_or_root(name);
        if (Status added = documents_mount.add(path, bytes.data(), bytes.size()); !added) {
            return added;
        }
    }

    auto variant = assets::VariantKey::parse(node.option("variant", "desktop"));
    if (!variant) {
        return make_unexpected(variant.error());
    }

    cook::CookRequest request;
    request.source = virtual_path_or_root(node.option("source", "assets"));
    request.world = world;
    request.variant = variant.value();
    request.shipping = node.option("shipping", "false") == "true";
    request.fail_on_conflicts = node.option("fail_on_conflicts", "true") == "true";

    Array<scene::serialization::Document> read_documents(default_allocator());
    if (Status read = cook::read_documents(files, request.source, read_documents); !read) {
        return read;
    }

    std::string scratch(node.option("scratch", "."));
    scratch += "/cy_cook_node.cypak";
    assets::PackageWriter package;
    cook::CookRunReport report(default_allocator());
    if (Status cooked = cook::cook_documents(request, read_documents, package, report); !cooked) {
        return cooked;
    }
    if (Status written = package.write(scratch.c_str()); !written) {
        return written;
    }

    Array<u8> produced(default_allocator());
    Status read_back = assets::fs::read_whole(scratch.c_str(), produced);
    (void)assets::fs::remove_file(scratch.c_str());
    if (!read_back) {
        return read_back;
    }
    return context.write(node.outputs.front(), produced.data(), produced.size());
}

}  // namespace

Status add_content_producers(ProducerRegistry& registry, const ecs::World* world) noexcept {
    g_cook_world.store(world, std::memory_order_release);
    if (Status added = registry.add(Producer{"import", kImportProducerVersion, produce_import,
                                             /*distributable=*/true});
        !added) {
        return added;
    }
    // NOT distributable. The cook emits blocks against the component registry of the world it was
    // handed, and a remote worker's binary may have registered a different set — which produces a
    // package the runtime rejects at the build-schema check, in a place nobody would look.
    return registry.add(Producer{"cook", kCookProducerVersion, produce_cook,
                                 /*distributable=*/false});
}

}  // namespace cy::build
