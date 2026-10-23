// SPDX-License-Identifier: MIT
// `ui` as a graph node. Issue #91, stage 5.
//
// A `.cyui` document names the sheets it is styled by and the documents it `use`s, and a used
// document names its own. Which files one document reaches is known only once each has been read,
// so the node declares one source, the document, and reads every other file through
// `NodeContext::discover` — reading it IS recording it as a dependency with the digest it had, so
// saving a shared sheet re-cooks every document that reaches it and nothing else. The VFX producer
// makes the same argument for its modules.
//
// THE ARTEFACT is `cy::ui`'s cooked bundle (cy/ui/toolkit/document.h): the document and every file
// it reaches, each by the path it was named by, after a `cyuicook 1` line. A runtime loads it
// through `BundleResolver` and reads no project file. A document or a sheet that does not exist, or
// a document that does not parse, fails the node by name — the same mistake the editor's hot
// reload reports, found at build time instead of on a player's screen.

#include "ui_producer.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/ui/toolkit/document.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace cy::build {
namespace {

[[nodiscard]] bool is_stylesheet(std::string_view path) noexcept {
    return path.ends_with(".cyss");
}

/// A document's first mistake, as the node's diagnostic.
[[nodiscard]] Status refuse_document(NodeContext& context, const ui::Document& document,
                                     const Error& error, std::string_view path) {
    std::string message = error.message;
    if (!document.diagnostics().empty()) {
        const ui::DocumentDiagnostic& first = document.diagnostics()[0];
        message = "line " + std::to_string(first.line) + ": " + first.message + " " +
                  std::string(first.detail.text());
    }
    context.diagnose(Severity::Error, "ui.document.parse", message, std::string(path));
    return make_unexpected(error);
}

/// The bundle being cooked, and the files already in it.
class Cook {
public:
    explicit Cook(NodeContext& context) noexcept
        : context_(&context), bundle_(default_allocator()) {}

    /// Add one document — parsed, so a mistake fails the node — and queue what it names.
    [[nodiscard]] Status add_document(std::string_view path, std::string_view text) {
        ui::Document document(default_allocator());
        // A parse reports a mistake as a diagnostic and goes on, so the editor can show them all;
        // a cook refuses the first.
        Status parsed = ui::parse_document(text, document);
        if (parsed && !document.diagnostics().empty()) {
            parsed = fail(ErrorCode::InvalidArgument, "ui: the document has a mistake");
        }
        if (!parsed) {
            return refuse_document(*context_, document, parsed.error(), path);
        }
        if (Status added = ui::append_bundle_entry(bundle_, "document", path, text); !added) {
            return added;
        }
        Array<std::string_view> named(default_allocator());
        if (Status listed = ui::document_dependencies(document, named); !listed) {
            return listed;
        }
        for (const std::string_view dependency : named.span()) {
            pending_.emplace_back(dependency);
        }
        return ok();
    }

    /// Read every queued file through `discover`, depth first, each once.
    [[nodiscard]] Status drain() {
        while (!pending_.empty()) {
            const std::string path = pending_.back();
            pending_.pop_back();
            if (std::ranges::find(seen_, path) != seen_.end()) {
                continue;
            }
            seen_.push_back(path);
            if (Status added = add_discovered(path); !added) {
                return added;
            }
        }
        return ok();
    }

    [[nodiscard]] Status write() {
        return context_->write(context_->node().outputs.front(), bundle_.data(), bundle_.size());
    }

    void mark_seen(std::string_view path) { seen_.emplace_back(path); }

private:
    [[nodiscard]] Status add_discovered(const std::string& path) {
        Array<u8> bytes(default_allocator());
        if (Status read = context_->discover(path, bytes); !read) {
            context_->diagnose(Severity::Error, "ui.document.missing",
                               "a document or sheet it names does not exist", path);
            return read;
        }
        const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (is_stylesheet(path)) {
            return ui::append_bundle_entry(bundle_, "stylesheet", path, text);
        }
        return add_document(path, text);
    }

    NodeContext* context_;
    Array<char> bundle_;
    std::vector<std::string> pending_;
    std::vector<std::string> seen_;
};

}  // namespace

Status produce_ui(NodeContext& context) {
    const NodeDesc& node = context.node();
    if (node.sources.size() != 1 || node.outputs.size() != 1) {
        return fail(ErrorCode::InvalidArgument,
                    "a ui node declares exactly one source and one output");
    }
    Array<u8> bytes(default_allocator());
    if (Status read = context.read(node.sources.front(), bytes); !read) {
        return read;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    Cook cook(context);
    cook.mark_seen(node.sources.front());
    if (Status added = cook.add_document(node.sources.front(), text); !added) {
        return added;
    }
    if (Status drained = cook.drain(); !drained) {
        return drained;
    }
    return cook.write();
}

}  // namespace cy::build
