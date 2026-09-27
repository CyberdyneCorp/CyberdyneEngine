// SPDX-License-Identifier: MIT
// `vfx` as a graph node. Issue #19.
//
// A VFX system maps each reusable module it uses to an explicit project-relative `.cyvfxmodule`
// path, and a module names the modules it uses in turn. Which files a system reaches is therefore
// known only once its document, and each module's own dependency list, has been read. So the node
// declares one source, the system, and every module is read through `NodeContext::discover`:
// reading a module IS recording it as a dependency with the digest it had, so editing a module
// re-runs every system node that reached it, and the node beside it that did not stays cached.
// That is the same argument `import`'s `NodeImportResolver` makes for a glTF's external buffer.
//
// The producer only collects sources. Validation, cycle and stage checks, composition and the
// cook key all belong to `resolve_authoring_modules` and `compile_system`, exactly as for the
// editor's `vfx.compile` service, so a module that cannot be used fails here under the same named
// diagnostic it fails under there.
//
// THE ARTEFACT. There is no serialised `CompiledSystem` in this tree yet (src/vfx/README.md says
// so), so the node's output is the cook's own record of what it produced: the content-addressed
// cook key and every emitter's generated Slang. A module edit that reaches the kernels changes
// these bytes, and the key alone is what a runtime loader would compare.

#include "vfx_producer.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/graph/cybergraph.h>
#include <cy/vfx/authoring.h>
#include <cy/vfx/compile.h>
#include <cy/vfx/interfaces.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace cy::build {
namespace {

/// Every module source a system reaches, read through the node's context and nowhere else.
/// A reference with no mapping, or a mapped file that does not exist, is left out on purpose:
/// `resolve_authoring_modules` then fails it by name instead of this producer guessing.
class ReachableModules {
public:
    explicit ReachableModules(NodeContext& context) noexcept : context_(&context) {}

    void collect(const vfx::VfxSystemAsset& asset, Allocator& allocator) {
        std::vector<Name> pending;
        for (const vfx::Emitter& emitter : asset.emitters()) {
            pending.insert(pending.end(), emitter.modules().begin(), emitter.modules().end());
        }
        while (!pending.empty()) {
            const Name name = pending.back();
            pending.pop_back();
            if (std::ranges::find(visited_, name) != visited_.end()) {
                continue;
            }
            visited_.push_back(name);
            const vfx::ModuleAssetRef* mapping = asset.find_module_asset(name);
            if (mapping == nullptr) {
                continue;
            }
            Array<u8> bytes(allocator);
            if (!context_->discover(mapping->path.text(), bytes)) {
                continue;
            }
            texts_.emplace_back(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            names_.push_back(name);
            auto module = vfx::read_authoring_module(texts_.back(), allocator);
            if (module) {
                pending.insert(pending.end(), module->dependencies.begin(),
                               module->dependencies.end());
            }
        }
    }

    [[nodiscard]] std::vector<vfx::ModuleSource> sources() const {
        std::vector<vfx::ModuleSource> out;
        out.reserve(names_.size());
        for (usize index = 0; index < names_.size(); ++index) {
            out.push_back({names_[index], texts_[index]});
        }
        return out;
    }

private:
    NodeContext* context_;
    std::vector<Name> visited_;
    std::vector<Name> names_;
    std::vector<std::string> texts_;
};

void append_hex(std::string& out, const char* label, u64 value) {
    char text[32] = {};
    std::snprintf(text, sizeof(text), "%s %016" PRIx64 "\n", label, value);
    out.append(text);
}

[[nodiscard]] std::string cooked_record(const vfx::CompiledSystem& system) {
    std::string out = "cyvfxcook 1\n";
    append_hex(out, "key", system.cook_key());
    for (const vfx::CompiledEmitter& emitter : system.emitters()) {
        out.append("emitter ");
        out.append(emitter.name().text());
        out.push_back('\n');
        for (const graph::GeneratedSource& source : emitter.sources()) {
            out.append("source ");
            out.append(std::to_string(source.text.size()));
            out.push_back('\n');
            out.append(source.text.data(), source.text.size());
            out.push_back('\n');
        }
    }
    return out;
}

void report_diagnostics(NodeContext& context, const graph::DiagnosticSink& sink) {
    for (const graph::Diagnostic& diagnostic : sink.entries()) {
        std::string location(diagnostic.detail.text());
        if (diagnostic.node != graph::kInvalidNodeKey) {
            location += "#" + std::to_string(diagnostic.node);
        }
        context.diagnose(Severity::Error, diagnostic.code, diagnostic.message, location);
    }
}

}  // namespace

Status produce_vfx(NodeContext& context) {
    const NodeDesc& node = context.node();
    if (node.sources.size() != 1 || node.outputs.size() != 1) {
        return fail(ErrorCode::InvalidArgument,
                    "a vfx node declares exactly one source and one output");
    }
    Allocator& allocator = default_allocator();
    Array<u8> bytes(allocator);
    if (Status read = context.read(node.sources.front(), bytes); !read) {
        return read;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    auto asset = vfx::read_authoring_document(text, allocator);
    if (!asset) {
        context.diagnose(Severity::Error, "vfx.document.parse", asset.error().message,
                         node.sources.front());
        return make_unexpected(asset.error());
    }

    ReachableModules modules(context);
    modules.collect(*asset, allocator);
    const std::vector<vfx::ModuleSource> sources = modules.sources();
    graph::DiagnosticSink sink(allocator);
    vfx::CompileReport report(allocator);
    if (Status resolved = vfx::resolve_authoring_modules(*asset, sources, sink, report, allocator);
        !resolved) {
        report_diagnostics(context, sink);
        return resolved;
    }

    graph::NodeRegistry registry(allocator);
    vfx::DataInterfaceRegistry interfaces(allocator);
    if (Status registered = vfx::register_vfx_nodes(registry); !registered) {
        return registered;
    }
    if (Status registered = vfx::register_builtin_interfaces(interfaces); !registered) {
        return registered;
    }
    asset->resolve(registry);
    auto system =
        vfx::compile_system(*asset, registry, interfaces, vfx::CompileOptions{}, sink, report);
    if (!system) {
        report_diagnostics(context, sink);
        return make_unexpected(system.error());
    }
    const std::string cooked = cooked_record(*system);
    return context.write(node.outputs.front(), cooked.data(), cooked.size());
}

}  // namespace cy::build
