// SPDX-License-Identifier: MIT
#ifndef CY_UI_TOOLKIT_DOCUMENT_HOST_H
#define CY_UI_TOOLKIT_DOCUMENT_HOST_H
// A UI document made into widgets, kept in step with its text, and reloaded when its file changes.
// Issue #91, stage 5.
//
// --- INSTANTIATING ------------------------------------------------------------------------------
//
// `DocumentInstance::load` expands the document — every `use` replaced by the document it names,
// that document's `$params` by the `use` line's properties or their defaults — and makes a widget
// per element. Widget properties (`text`, `value`, `items`, ...) go to the widget, `.cyss`
// properties become its inline declarations, `bind=` names a game value through
// `DocumentBindings`, and the sheets the documents name become the toolkit's sheet.
//
// --- RELOADING KEEPS WHAT THE PLAYER DID --------------------------------------------------------
//
// `reload` is a DIFF, as `reconcile()` is: an element is matched to the widget it made by its id,
// or — without one — by its kind and its place among its siblings, which is the consequence
// `widgets.h` documents for keys. A matched widget is kept: its focus, its scroll, the value the
// player set. Only the properties whose text changed are applied again; new elements are made and
// vanished ones destroyed. So a designer's colour tweak repaints, and does not reset the slider
// the player is holding.
//
// --- HOT RELOAD ---------------------------------------------------------------------------------
//
// `DocumentHost` reads the document through the engine's virtual file system — the project mount
// the editor saves into, a package, or memory — and watches it, and every sheet and document it
// reaches, with the engine's polling `FileWatcher`. A change that has settled reloads. A document
// that no longer parses keeps the interface it had and reports why — `core-assets-and-io`'s "reload
// failure keeps the old asset". The editor saving the file is the change; nothing else needs to
// know a reload happened.

#include <cy/core/assets/vfs.h>
#include <cy/core/assets/watch.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/ui/toolkit/document.h>
#include <cy/ui/toolkit/toolkit.h>

#include <string_view>

namespace cy::ui {

/// The game values a document's `bind=name` reaches.
class DocumentBindings {
public:
    DocumentBindings() = default;
    virtual ~DocumentBindings() = default;
    DocumentBindings(const DocumentBindings&) = delete;
    DocumentBindings& operator=(const DocumentBindings&) = delete;
    DocumentBindings(DocumentBindings&&) = delete;
    DocumentBindings& operator=(DocumentBindings&&) = delete;

    /// The source a name binds to, or null for one the game does not have.
    [[nodiscard]] virtual BindingSource* find(std::string_view name) noexcept = 0;
};

/// What a load or a reload did.
struct DocumentReport {
    u32 created = 0;
    u32 updated = 0;
    u32 removed = 0;
    /// Widgets kept, with their state, because their element was matched.
    u32 preserved = 0;
    /// Properties applied to widgets.
    u32 properties = 0;
};

/// The default property a `bind=` drives for a kind: a slider's value, a toggle's state, a text
/// field's text, a list's selection.
[[nodiscard]] WidgetProperty default_bound_property(WidgetKind kind) noexcept;

class DocumentInstance {
public:
    DocumentInstance(Allocator& allocator, Toolkit& toolkit) noexcept;
    ~DocumentInstance();

    DocumentInstance(const DocumentInstance&) = delete;
    DocumentInstance& operator=(const DocumentInstance&) = delete;
    DocumentInstance(DocumentInstance&&) = delete;
    DocumentInstance& operator=(DocumentInstance&&) = delete;

    /// Make the document's widgets under `parent` (the toolkit's root when null). `resolver` reads
    /// `use` and `stylesheet` paths and may be null for a document with neither; `bindings` may be
    /// null. Fails, making nothing, when the text does not parse.
    [[nodiscard]] Status load(std::string_view text, WidgetId parent, DocumentResolver* resolver,
                              DocumentBindings* bindings) noexcept;
    /// Bring the widgets in line with new text, keeping every matched widget and its state. Fails,
    /// changing nothing, when the text does not parse.
    [[nodiscard]] Status reload(std::string_view text) noexcept;
    /// Destroy every widget the document made.
    [[nodiscard]] Status unload() noexcept;

    [[nodiscard]] Span<const WidgetId> roots() const noexcept { return roots_.span(); }
    /// The widget made for the element with this id, anywhere in the expanded document.
    [[nodiscard]] WidgetId find(std::string_view id) const noexcept;
    /// The widget made for the element on `line` of the document `load` was given — for a `use`
    /// line, the top of what it instantiated — or none: a comment, a kind that does not exist, a
    /// line past the end. What an editor that shows the document's lines maps them to widgets by.
    [[nodiscard]] WidgetId widget_at_line(u32 line) const noexcept;
    /// What the last load or reload reported, and why one failed.
    [[nodiscard]] Span<const DocumentDiagnostic> diagnostics() const noexcept {
        return diagnostics_.span();
    }
    [[nodiscard]] const DocumentReport& report() const noexcept { return report_; }
    /// Every path the expanded document read, for a watcher.
    [[nodiscard]] Span<const std::string_view> dependencies() const noexcept {
        return dependencies_.span();
    }
    /// The sheets' text, concatenated in the order the documents named them.
    [[nodiscard]] std::string_view stylesheet() const noexcept {
        return {sheet_.data(), sheet_.size()};
    }

    struct Expanded;
    struct Expansion;

private:
    [[nodiscard]] Status expand(std::string_view text, Expansion& out) noexcept;
    [[nodiscard]] Status make(Expansion& expansion, u32 node, WidgetId parent) noexcept;
    /// `make` for a node and everything under it, parents first.
    [[nodiscard]] Status make_subtree(Expansion& expansion, u32 node, WidgetId parent) noexcept;
    [[nodiscard]] Status apply(const Expansion& expansion, u32 node, WidgetId widget,
                               const Expansion* before, u32 old_node) noexcept;
    [[nodiscard]] Status sync(Expansion& next, u32 parent_new, WidgetId parent_widget,
                              u32 parent_old) noexcept;
    [[nodiscard]] Status adopt(Expansion& next) noexcept;

    Allocator* allocator_;
    Toolkit* toolkit_;
    DocumentResolver* resolver_ = nullptr;
    DocumentBindings* bindings_ = nullptr;
    WidgetId parent_;
    Expansion* current_ = nullptr;
    Array<WidgetId> roots_;
    Array<DocumentDiagnostic> diagnostics_;
    Array<std::string_view> dependencies_;
    Array<char> sheet_;
    DocumentReport report_;
};

/// Reads a document's paths through a virtual file system.
class VfsDocumentResolver final : public DocumentResolver {
public:
    explicit VfsDocumentResolver(const assets::VirtualFileSystem& files) noexcept
        : files_(&files) {}
    [[nodiscard]] Status read(std::string_view path, Array<char>& out) noexcept override;

private:
    const assets::VirtualFileSystem* files_;
};

/// A document loaded from the virtual file system and reloaded whenever it, or anything it
/// reaches, changes.
class DocumentHost {
public:
    /// `files` outlives the host. `settle_ns` is how long a change must hold still before it is
    /// reloaded — the watcher's rule against reading a file an editor is half-way through saving.
    DocumentHost(Allocator& allocator, Toolkit& toolkit, assets::VirtualFileSystem& files,
                 i64 settle_ns = 100'000'000) noexcept;

    DocumentHost(const DocumentHost&) = delete;
    DocumentHost& operator=(const DocumentHost&) = delete;
    DocumentHost(DocumentHost&&) = delete;
    DocumentHost& operator=(DocumentHost&&) = delete;
    ~DocumentHost() = default;

    /// Load `path` under `parent`, its sheets applied, and watch it from `now_ns`.
    [[nodiscard]] Status open(std::string_view path, WidgetId parent, DocumentBindings* bindings,
                              i64 now_ns = 0) noexcept;
    /// Poll the watcher at `now_ns`; reload on a settled change. True when a reload was applied; a
    /// reload that failed leaves the interface as it was, and `instance().diagnostics()` says why.
    [[nodiscard]] Expected<bool, Error> poll(i64 now_ns) noexcept;
    /// Reload now, whatever the watcher says — what an editor's "reload" command does.
    [[nodiscard]] Status reload(i64 now_ns = 0) noexcept;
    [[nodiscard]] DocumentInstance& instance() noexcept { return instance_; }
    [[nodiscard]] u32 reloads() const noexcept { return reloads_; }
    [[nodiscard]] u32 failed_reloads() const noexcept { return failed_; }

private:
    [[nodiscard]] Status read(Array<char>& out) noexcept;
    [[nodiscard]] Status watch(i64 now_ns) noexcept;

    Allocator* allocator_;
    assets::VirtualFileSystem* files_;
    DocumentInstance instance_;
    VfsDocumentResolver resolver_;
    assets::FileWatcher watcher_;
    i64 settle_ns_;
    Array<char> path_;
    u32 reloads_ = 0;
    u32 failed_ = 0;
};

}  // namespace cy::ui

#endif  // CY_UI_TOOLKIT_DOCUMENT_HOST_H
