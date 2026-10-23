// SPDX-License-Identifier: MIT
#ifndef CY_UI_TOOLKIT_DOCUMENT_H
#define CY_UI_TOOLKIT_DOCUMENT_H
// The UI document: an interface's widgets, their properties, styles and bindings, as text. Issue
// #91, stage 5.
//
// `ui-system`: "A UI document SHALL be the single asset representation of a UI tree, its styles,
// its bindings, and its animations", with "a text form for version control" where "WHEN one
// property of one element changes THEN the text diff SHALL show that single change", and "Documents
// SHALL be composable: a document may instantiate another as a sub-tree with parameters".
//
// ================================================================================================
// THE TEXT FORM — `.cyui`
// ================================================================================================
//
//     cyui 1
//     stylesheet "menu.cyss"
//     param title "Settings"
//     column #settings .panel gap=8 width=320
//       label #heading text=$title .title
//       slider #volume min=0 max=1 step=0.05 value=0.8 bind=volume
//       toggle #subtitles text="Subtitles" checked=true
//       dropdown #quality items=["Low", "Medium", "High"] selected=1
//       use "widgets/stat.cyui" #health label="Health" value=0.7
//       row gap=6 justify-content=end
//         button #cancel text="Cancel"
//         button #apply text="Apply" .primary
//
// ONE ELEMENT A LINE, its children indented two spaces under it, and ONE PROPERTY A TOKEN on that
// line: `#id`, `.class`, `key=value`. So the scenario holds by construction — changing a property
// changes one token on one line, and reordering nothing moves anything else. A value is a number,
// a word, a "string", a `$param`, or a [list]. A key is a widget property (`text`, `value`,
// `items`,
// ...), a binding (`bind`, `bind-mode`), or any `.cyss` property, which becomes an inline
// declaration — the closed property set and its diagnostics are the style sheet's.
//
// `use "path"` instantiates another document in place, its `param`s replaced by the properties on
// the `use` line; a `$name` with neither is a diagnostic. `stylesheet "path"` names a sheet the
// document is styled with. Lines starting `//` are comments. Writing a parsed document gives back
// exactly the text it was parsed from, comments included — numbers keep the spelling they had.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>

#include <string_view>

namespace cy::ui {

/// A value in a document. Strings, words and numbers' spellings live in the document's text pool.
struct DocumentValue {
    enum class Kind : u8 { Number = 0, String, Word, Parameter, List };

    Kind kind = Kind::Word;
    f32 number = 0.0F;
    /// The spelling, or the string's contents unescaped, or the parameter's name: `text(value)`.
    u32 first = 0;
    u32 length = 0;
    /// A list's items: `items(value)`.
    u32 first_item = 0;
    u32 item_count = 0;
};

struct DocumentProperty {
    Name key;
    DocumentValue value;
    u32 column = 0;
};

/// One line of a document: an element, a `use`, or a comment.
struct DocumentNode {
    /// The widget kind's name (`slider`), `use`, or empty for a comment line.
    Name kind;
    Name id;
    Name classes[4];
    u32 class_count = 0;
    /// The parent's index, or `kNoParent`.
    u32 parent = kNoParent;
    u32 depth = 0;
    u32 first_property = 0;
    u32 property_count = 0;
    /// A `use` line's path, or a comment's text.
    DocumentValue path;
    u32 line = 0;

    [[nodiscard]] bool is_use() const noexcept;
    [[nodiscard]] bool is_comment() const noexcept { return kind.is_empty(); }

    static constexpr u32 kNoParent = 0xFFFFFFFFU;
};

struct DocumentParameter {
    Name name;
    DocumentValue value;
};

struct DocumentDiagnostic {
    const char* message = "";
    Name detail;
    u32 line = 0;
    u32 column = 0;
};

/// A parsed document. Nodes are in the order of their lines: parents before children.
class Document {
public:
    explicit Document(Allocator& allocator) noexcept;

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&&) noexcept = default;
    Document& operator=(Document&&) noexcept = default;
    ~Document() = default;

    [[nodiscard]] Span<const DocumentNode> nodes() const noexcept { return nodes_.span(); }
    [[nodiscard]] Span<const DocumentProperty> properties(const DocumentNode& node) const noexcept;
    [[nodiscard]] Span<const DocumentParameter> parameters() const noexcept {
        return parameters_.span();
    }
    /// The sheets the document names, in order.
    [[nodiscard]] Span<const DocumentValue> stylesheets() const noexcept {
        return stylesheets_.span();
    }
    [[nodiscard]] Span<const DocumentDiagnostic> diagnostics() const noexcept {
        return diagnostics_.span();
    }
    /// The directives in the order they were written: a parameter's index with the top bit set, or
    /// a stylesheet's index. What the writer walks to put them back where they were.
    [[nodiscard]] Span<const u32> directives() const noexcept { return directive_lines_.span(); }
    [[nodiscard]] std::string_view text(const DocumentValue& value) const noexcept;
    [[nodiscard]] Span<const DocumentValue> items(const DocumentValue& value) const noexcept;
    /// A node's property, or null.
    [[nodiscard]] const DocumentProperty* find(u32 node, Name key) const noexcept;
    /// A parameter's default, or null.
    [[nodiscard]] const DocumentValue* parameter(Name name) const noexcept;

    // --- Editing, as code and a designer do ---------------------------------------------------

    /// Append an element under `parent` (`kNoParent` for a top-level one), after its last child.
    [[nodiscard]] Expected<u32, Error> add(u32 parent, std::string_view kind) noexcept;
    [[nodiscard]] Status set_id(u32 node, std::string_view id) noexcept;
    [[nodiscard]] Status add_class(u32 node, std::string_view name) noexcept;
    /// Set a property to a number, a string, or a word, replacing its value where it already is
    /// — in its place on the line — or appending it.
    [[nodiscard]] Status set_number(u32 node, std::string_view key, f32 value) noexcept;
    [[nodiscard]] Status set_string(u32 node, std::string_view key,
                                    std::string_view value) noexcept;
    [[nodiscard]] Status set_word(u32 node, std::string_view key, std::string_view value) noexcept;

    friend class DocumentParser;

private:
    [[nodiscard]] Expected<u32, Error> intern(std::string_view text) noexcept;
    [[nodiscard]] Status set_value(u32 node, std::string_view key,
                                   const DocumentValue& value) noexcept;

    Array<DocumentNode> nodes_;
    Array<DocumentProperty> properties_;
    Array<DocumentValue> items_;
    Array<DocumentParameter> parameters_;
    Array<DocumentValue> stylesheets_;
    Array<DocumentDiagnostic> diagnostics_;
    Array<char> pool_;
    /// The directives' lines, in order, so writing keeps where they were.
    Array<u32> directive_lines_;
};

/// Parse `.cyui`. A document with diagnostics still parses what it can: a cook fails on
/// `diagnostics()`, a hot reload reports them and keeps the interface it had.
[[nodiscard]] Status parse_document(std::string_view text, Document& out) noexcept;

/// The canonical text: `cyui 1`, the directives, then one line per node.
[[nodiscard]] Status write_document(const Document& document, Array<char>& out) noexcept;

/// Where a document's `use` and `stylesheet` paths are read from: a project's files, or a cooked
/// bundle.
class DocumentResolver {
public:
    DocumentResolver() = default;
    virtual ~DocumentResolver() = default;
    DocumentResolver(const DocumentResolver&) = delete;
    DocumentResolver& operator=(const DocumentResolver&) = delete;
    DocumentResolver(DocumentResolver&&) = delete;
    DocumentResolver& operator=(DocumentResolver&&) = delete;

    /// The bytes at `path`, appended to `out`.
    [[nodiscard]] virtual Status read(std::string_view path, Array<char>& out) noexcept = 0;
};

/// Reads paths relative to a directory on disk.
class FileDocumentResolver final : public DocumentResolver {
public:
    FileDocumentResolver(Allocator& allocator, std::string_view root) noexcept;
    void set_root(std::string_view root) noexcept;
    /// `root/path`, as a C string in `out`.
    [[nodiscard]] Status full_path(std::string_view path, Array<char>& out) const noexcept;
    [[nodiscard]] Status read(std::string_view path, Array<char>& out) noexcept override;

private:
    Array<char> root_;
};

// --- The cooked bundle ---------------------------------------------------------------------------
//
// What the build graph's `ui` node writes: the document and every document and sheet it reaches,
// each as `<kind> <path> <bytes>` and its bytes, after a `cyuicook 1` line. A runtime reads a
// bundle through `BundleResolver` and needs no project files.

/// One file in a cooked bundle.
struct BundleEntry {
    /// `document` or `stylesheet`.
    std::string_view kind;
    std::string_view path;
    std::string_view bytes;
};

/// Split a cooked bundle into its entries. The views point into `text`.
[[nodiscard]] Status parse_bundle(std::string_view text, Array<BundleEntry>& out) noexcept;

/// Append one entry to a bundle being written.
[[nodiscard]] Status append_bundle_entry(Array<char>& bundle, std::string_view kind,
                                         std::string_view path, std::string_view bytes) noexcept;

/// Resolves paths from a cooked bundle's entries.
class BundleResolver final : public DocumentResolver {
public:
    BundleResolver(Allocator& allocator, std::string_view bundle) noexcept;
    [[nodiscard]] Status status() const noexcept { return status_; }
    /// The bundle's first document: the one it was cooked from.
    [[nodiscard]] std::string_view main_document() const noexcept;
    [[nodiscard]] Status read(std::string_view path, Array<char>& out) noexcept override;

private:
    Array<BundleEntry> entries_;
    Status status_;
};

/// Every path a document names — its sheets and the documents it `use`s — appended to `paths` as
/// views into the document. What a cook reads through `NodeContext::discover`.
[[nodiscard]] Status document_dependencies(const Document& document,
                                           Array<std::string_view>& paths) noexcept;

}  // namespace cy::ui

#endif  // CY_UI_TOOLKIT_DOCUMENT_H
