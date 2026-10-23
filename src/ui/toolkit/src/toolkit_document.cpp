// SPDX-License-Identifier: MIT
// A UI document made into widgets, diffed on reload, and watched on disk. See document_host.h.

#include <cy/ui/toolkit/document_host.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace cy::ui {
namespace {

/// How deep `use` may nest: a document that reaches itself is a cycle, not a bigger interface.
constexpr u32 kMaxUseDepth = 8;

/// The properties a widget is CREATED with: changing one on reload makes the widget again.
constexpr std::string_view kCreationKeys[] = {
    "placeholder", "title", "columns", "page", "uv", "size", "max-length", "mask", "row-height"};

/// The properties a widget takes after it exists.
constexpr std::string_view kWidgetKeys[] = {
    "text",    "value",   "min",     "max",    "step",   "items", "selected", "checked",
    "enabled", "visible", "tooltip", "anchor", "offset", "bind",  "bind-mode"};

[[nodiscard]] bool is_creation_key(std::string_view key) noexcept {
    return std::ranges::find(kCreationKeys, key) != std::end(kCreationKeys);
}

[[nodiscard]] bool is_widget_key(std::string_view key) noexcept {
    return std::ranges::find(kWidgetKeys, key) != std::end(kWidgetKeys);
}

}  // namespace

/// One element of the expanded document: what it is, and the widget it made.
struct DocumentInstance::Expanded {
    Name kind;
    Name id;
    Name classes[4];
    u32 class_count = 0;
    u32 parent = DocumentNode::kNoParent;
    u32 first_property = 0;
    u32 property_count = 0;
    /// The line of the ROOT document this element came from — a `use` line for the top of what it
    /// instantiated — or zero for one a used document holds. What an editor maps its lines by.
    u32 source_line = 0;
    WidgetId widget;
};

/// A property of the expanded document: its value lives in `documents[document]`.
struct ExpandedProperty {
    Name key;
    DocumentValue value;
    u32 document = 0;
};

/// The documents a load read, and the tree they expand to.
struct DocumentInstance::Expansion {
    explicit Expansion(Allocator& allocator) noexcept
        : documents(allocator),
          nodes(allocator),
          properties(allocator),
          diagnostics(allocator),
          dependencies(allocator),
          sheet(allocator) {}

    Array<Document> documents;
    Array<Expanded> nodes;
    Array<ExpandedProperty> properties;
    Array<DocumentDiagnostic> diagnostics;
    Array<std::string_view> dependencies;
    Array<char> sheet;

    [[nodiscard]] std::string_view text(const ExpandedProperty& property) const noexcept {
        return documents[property.document].text(property.value);
    }
    [[nodiscard]] Span<const ExpandedProperty> properties_of(u32 node) const noexcept {
        return properties.span().subspan(nodes[node].first_property, nodes[node].property_count);
    }
    [[nodiscard]] const ExpandedProperty* find(u32 node, std::string_view key) const noexcept {
        for (const ExpandedProperty& property : properties_of(node)) {
            if (property.key.text() == key) {
                return &property;
            }
        }
        return nullptr;
    }
};

namespace {

/// The scope a `$param` resolves in: the `use` line's properties, then the document's defaults.
struct ParameterScope {
    const Array<ExpandedProperty>* overrides = nullptr;
    u32 document = 0;
};

/// Whether two values say the same thing: their kinds, their spellings, and their items.
[[nodiscard]] bool same_value(const DocumentInstance::Expansion& a, const ExpandedProperty& left,
                              const DocumentInstance::Expansion& b,
                              const ExpandedProperty& right) noexcept {
    if (left.value.kind != right.value.kind) {
        return false;
    }
    if (left.value.kind != DocumentValue::Kind::List) {
        return a.text(left) == b.text(right);
    }
    const Span<const DocumentValue> first = a.documents[left.document].items(left.value);
    const Span<const DocumentValue> second = b.documents[right.document].items(right.value);
    if (first.size() != second.size()) {
        return false;
    }
    for (usize index = 0; index < first.size(); ++index) {
        if (a.documents[left.document].text(first[index]) !=
            b.documents[right.document].text(second[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool truth(std::string_view word) noexcept {
    return word == "true" || word == "yes" || word == "on" || word == "1";
}

/// The text of every item of a list, appended to `out` as views into the document.
[[nodiscard]] Status list_texts(const DocumentInstance::Expansion& expansion,
                                const ExpandedProperty& property,
                                Array<std::string_view>& out) noexcept {
    const Document& document = expansion.documents[property.document];
    for (const DocumentValue& item : document.items(property.value)) {
        if (Status pushed = out.push_back(document.text(item)); !pushed) {
            return pushed;
        }
    }
    return ok();
}

[[nodiscard]] Status diagnose(Array<DocumentDiagnostic>& out, const char* message,
                              Name detail = Name{}) noexcept {
    DocumentDiagnostic diagnostic;
    diagnostic.message = message;
    diagnostic.detail = detail;
    return out.push_back(diagnostic);
}

/// The numbers of a list, into `out`; missing ones stay as they were.
void numbers_of(const DocumentInstance::Expansion& expansion, const ExpandedProperty& property,
                f32* out, usize count) noexcept {
    const Span<const DocumentValue> items =
        expansion.documents[property.document].items(property.value);
    for (usize index = 0; index < count && index < items.size(); ++index) {
        out[index] = items[index].number;
    }
}

}  // namespace

WidgetProperty default_bound_property(WidgetKind kind) noexcept {
    switch (kind) {
        case WidgetKind::Toggle:
        case WidgetKind::Checkbox:
            return WidgetProperty::Checked;
        case WidgetKind::Label:
        case WidgetKind::RichText:
        case WidgetKind::TextField:
        case WidgetKind::Button:
            return WidgetProperty::Text;
        case WidgetKind::RadioGroup:
        case WidgetKind::Dropdown:
        case WidgetKind::ListView:
        case WidgetKind::Tabs:
            return WidgetProperty::Selected;
        default:
            return WidgetProperty::Value;
    }
}

DocumentInstance::DocumentInstance(Allocator& allocator, Toolkit& toolkit) noexcept
    : allocator_(&allocator),
      toolkit_(&toolkit),
      roots_(allocator),
      diagnostics_(allocator),
      dependencies_(allocator),
      sheet_(allocator) {}

DocumentInstance::~DocumentInstance() {
    if (current_ != nullptr) {
        current_->~Expansion();
        allocator_->deallocate(current_, sizeof(Expansion), alignof(Expansion));
        current_ = nullptr;
    }
}

// --- Expanding -----------------------------------------------------------------------------------

namespace {

/// The expansion's recursive walk, kept out of the class so its helpers stay small.
class Expander {
public:
    Expander(DocumentInstance::Expansion& out, DocumentResolver* resolver,
             Allocator& allocator) noexcept
        : out_(&out), resolver_(resolver), allocator_(&allocator) {}

    /// Expand document `document` under expanded node `parent`. `root_id` and `root_classes`, from
    /// a `use` line, go on the document's first top-level element.
    [[nodiscard]] Status expand(u32 document, u32 parent, const ParameterScope& scope,
                                const DocumentNode* use_line, u32 depth, u32& first_top) noexcept {
        const usize count = out_->documents[document].nodes().size();
        Array<u32> mapped(*allocator_);
        if (Status sized = mapped.resize(count); !sized) {
            return sized;
        }
        first_top = DocumentNode::kNoParent;
        for (usize index = 0; index < count; ++index) {
            const DocumentNode node = out_->documents[document].nodes()[index];
            mapped[index] = DocumentNode::kNoParent;
            if (node.is_comment()) {
                continue;
            }
            const u32 under = node.parent == DocumentNode::kNoParent ? parent : mapped[node.parent];
            u32 made = DocumentNode::kNoParent;
            if (node.is_use()) {
                if (Status used = use(document, node, under, scope, depth, made); !used) {
                    return used;
                }
            } else if (Status element = add_element(document, node, under, scope, made); !element) {
                return element;
            }
            mapped[index] = made;
            if (document == 0 && made != DocumentNode::kNoParent) {
                out_->nodes[made].source_line = node.line;
            }
            if (node.parent == DocumentNode::kNoParent && first_top == DocumentNode::kNoParent &&
                made != DocumentNode::kNoParent) {
                first_top = made;
                if (use_line != nullptr) {
                    apply_use_identity(*use_line, made);
                }
            }
        }
        return ok();
    }

private:
    void apply_use_identity(const DocumentNode& use_line, u32 node) noexcept {
        DocumentInstance::Expanded& expanded = out_->nodes[node];
        if (!use_line.id.is_empty()) {
            expanded.id = use_line.id;
        }
        for (u32 index = 0; index < use_line.class_count && expanded.class_count < 4; ++index) {
            expanded.classes[expanded.class_count++] = use_line.classes[index];
        }
    }

    /// A property's value with its parameter resolved, or false for an unresolvable one.
    [[nodiscard]] bool resolve(u32 document, const DocumentValue& value,
                               const ParameterScope& scope, ExpandedProperty& out) noexcept {
        out.value = value;
        out.document = document;
        if (value.kind != DocumentValue::Kind::Parameter) {
            return true;
        }
        const std::string_view name = out_->documents[document].text(value);
        if (scope.overrides != nullptr) {
            for (const ExpandedProperty& given : scope.overrides->span()) {
                if (given.key.text() == name) {
                    out.value = given.value;
                    out.document = given.document;
                    return true;
                }
            }
        }
        const DocumentValue* fallback =
            out_->documents[scope.document].parameter(Name::intern(name));
        if (fallback == nullptr) {
            return false;
        }
        out.value = *fallback;
        out.document = scope.document;
        return true;
    }

    [[nodiscard]] Status add_element(u32 document, const DocumentNode& node, u32 parent,
                                     const ParameterScope& scope, u32& made) noexcept {
        DocumentInstance::Expanded expanded;
        expanded.kind = node.kind;
        expanded.id = node.id;
        expanded.class_count = node.class_count;
        for (u32 index = 0; index < node.class_count; ++index) {
            expanded.classes[index] = node.classes[index];
        }
        expanded.parent = parent;
        expanded.first_property = static_cast<u32>(out_->properties.size());
        if (widget_kind_from_name(node.kind.text()) == WidgetKind::Count) {
            if (Status reported = diagnose(out_->diagnostics, "no such widget kind", node.kind);
                !reported) {
                return reported;
            }
        }
        for (const DocumentProperty& property : out_->documents[document].properties(node)) {
            ExpandedProperty resolved;
            resolved.key = property.key;
            if (!resolve(document, property.value, scope, resolved)) {
                if (Status reported =
                        diagnose(out_->diagnostics, "a parameter with no value and no default",
                                 Name::intern(out_->documents[document].text(property.value)));
                    !reported) {
                    return reported;
                }
                continue;
            }
            if (Status pushed = out_->properties.push_back(resolved); !pushed) {
                return pushed;
            }
            ++expanded.property_count;
        }
        made = static_cast<u32>(out_->nodes.size());
        return out_->nodes.push_back(expanded);
    }

    [[nodiscard]] Status use(u32 document, const DocumentNode& node, u32 parent,
                             const ParameterScope& scope, u32 depth, u32& made) noexcept {
        if (depth >= kMaxUseDepth) {
            return diagnose(out_->diagnostics, "`use` nests too deep — a document reaches itself");
        }
        if (resolver_ == nullptr) {
            return diagnose(out_->diagnostics, "`use` with no resolver to read it");
        }
        const std::string_view path = out_->documents[document].text(node.path);
        Array<char> bytes(*allocator_);
        if (Status read = resolver_->read(path, bytes); !read) {
            return diagnose(out_->diagnostics, "`use` names a document that cannot be read",
                            Name::intern(path));
        }
        if (Status noted = out_->dependencies.push_back(path); !noted) {
            return noted;
        }
        // The `use` line's properties, resolved in the scope it sits in: they are the parameters
        // of the document it instantiates.
        Array<ExpandedProperty> overrides(*allocator_);
        for (const DocumentProperty& property : out_->documents[document].properties(node)) {
            ExpandedProperty resolved;
            resolved.key = property.key;
            if (resolve(document, property.value, scope, resolved)) {
                if (Status pushed = overrides.push_back(resolved); !pushed) {
                    return pushed;
                }
            }
        }
        Document parsed(*allocator_);
        if (Status ok_parse = parse_document(std::string_view(bytes.data(), bytes.size()), parsed);
            !ok_parse) {
            return ok_parse;
        }
        for (const DocumentDiagnostic& diagnostic : parsed.diagnostics()) {
            if (Status pushed = out_->diagnostics.push_back(diagnostic); !pushed) {
                return pushed;
            }
        }
        const auto sub = static_cast<u32>(out_->documents.size());
        if (Status pushed = out_->documents.push_back(std::move(parsed)); !pushed) {
            return pushed;
        }
        if (Status sheets = collect_sheets(sub); !sheets) {
            return sheets;
        }
        ParameterScope inner;
        inner.overrides = &overrides;
        inner.document = sub;
        return expand(sub, parent, inner, &node, depth + 1, made);
    }

public:
    [[nodiscard]] Status collect_sheets(u32 document) noexcept {
        for (const DocumentValue& sheet : out_->documents[document].stylesheets()) {
            const std::string_view path = out_->documents[document].text(sheet);
            if (resolver_ == nullptr) {
                return diagnose(out_->diagnostics, "a stylesheet with no resolver to read it");
            }
            if (Status read = resolver_->read(path, out_->sheet); !read) {
                if (Status reported = diagnose(
                        out_->diagnostics, "a stylesheet that cannot be read", Name::intern(path));
                    !reported) {
                    return reported;
                }
                continue;
            }
            if (Status newline = out_->sheet.push_back('\n'); !newline) {
                return newline;
            }
            if (Status noted = out_->dependencies.push_back(path); !noted) {
                return noted;
            }
        }
        return ok();
    }

private:
    DocumentInstance::Expansion* out_;
    DocumentResolver* resolver_;
    Allocator* allocator_;
};

}  // namespace

Status DocumentInstance::expand(std::string_view text, Expansion& out) noexcept {
    Document main(*allocator_);
    if (Status ok_parse = parse_document(text, main); !ok_parse) {
        return ok_parse;
    }
    for (const DocumentDiagnostic& diagnostic : main.diagnostics()) {
        if (Status pushed = out.diagnostics.push_back(diagnostic); !pushed) {
            return pushed;
        }
    }
    if (Status pushed = out.documents.push_back(std::move(main)); !pushed) {
        return pushed;
    }
    Expander expander(out, resolver_, *allocator_);
    if (Status sheets = expander.collect_sheets(0); !sheets) {
        return sheets;
    }
    ParameterScope scope;
    scope.document = 0;
    u32 first = 0;
    return expander.expand(0, DocumentNode::kNoParent, scope, nullptr, 0, first);
}

// --- Making widgets ------------------------------------------------------------------------------

Status DocumentInstance::make(Expansion& expansion, u32 node, WidgetId parent) noexcept {
    const Expanded& expanded = expansion.nodes[node];
    WidgetDesc desc;
    desc.kind = widget_kind_from_name(expanded.kind.text());
    if (desc.kind == WidgetKind::Count) {
        return ok();
    }
    desc.id = expanded.id.text();
    Array<char> classes(*allocator_);
    for (u32 index = 0; index < expanded.class_count; ++index) {
        const std::string_view name = expanded.classes[index].text();
        if (Status built = classes.append(Span<const char>(name.data(), name.size())); !built) {
            return built;
        }
        if (Status space = classes.push_back(' '); !space) {
            return space;
        }
    }
    desc.classes = std::string_view(classes.data(), classes.size());
    Array<std::string_view> items(*allocator_);
    for (const ExpandedProperty& property : expansion.properties_of(node)) {
        const std::string_view key = property.key.text();
        const std::string_view text = expansion.text(property);
        const f32 number = property.value.number;
        if (key == "text") {
            desc.text = text;
        } else if (key == "placeholder") {
            desc.placeholder = text;
        } else if (key == "title") {
            desc.title = text;
        } else if (key == "items") {
            if (Status listed = list_texts(expansion, property, items); !listed) {
                return listed;
            }
            desc.items = items.span();
        } else if (key == "value") {
            desc.value = number;
        } else if (key == "min") {
            desc.minimum = number;
        } else if (key == "max") {
            desc.maximum = number;
        } else if (key == "step") {
            desc.step = number;
        } else if (key == "columns") {
            desc.columns = static_cast<u16>(number);
        } else if (key == "page") {
            desc.image_page = static_cast<u16>(number);
        } else if (key == "uv") {
            f32 uv[4] = {0.0F, 0.0F, 1.0F, 1.0F};
            numbers_of(expansion, property, uv, 4);
            desc.image_uv = Rect{uv[0], uv[1], uv[2], uv[3]};
        } else if (key == "size") {
            f32 size[2] = {0.0F, 0.0F};
            numbers_of(expansion, property, size, 2);
            desc.image_size = Vec2{size[0], size[1]};
        } else if (key == "max-length") {
            desc.max_length = static_cast<u32>(number);
        } else if (key == "mask") {
            desc.mask = TextMask::Any;
            if (text == "digits") {
                desc.mask = TextMask::Digits;
            } else if (text == "decimal") {
                desc.mask = TextMask::Decimal;
            }
        } else if (key == "row-height") {
            desc.row_height = number;
        } else if (key == "checked") {
            desc.checked = truth(text);
        } else if (key == "enabled") {
            desc.enabled = truth(text);
        }
    }
    Expected<WidgetId, Error> made = toolkit_->create(parent, desc);
    if (!made) {
        return make_unexpected(made.error());
    }
    expansion.nodes[node].widget = *made;
    ++report_.created;
    return apply(expansion, node, *made, nullptr, 0);
}

Status DocumentInstance::make_subtree(Expansion& expansion, u32 node, WidgetId parent) noexcept {
    if (Status made = make(expansion, node, parent); !made) {
        return made;
    }
    // Nodes are in tree order, a parent before its children: a node is under `node` when its
    // chain of parents reaches it, and its parent's widget is made by the time it is reached.
    for (u32 index = node + 1; index < expansion.nodes.size(); ++index) {
        u32 walk = expansion.nodes[index].parent;
        while (walk != DocumentNode::kNoParent && walk != node) {
            walk = expansion.nodes[walk].parent;
        }
        if (walk != node) {
            continue;
        }
        const WidgetId under = expansion.nodes[expansion.nodes[index].parent].widget;
        if (Status made = make(expansion, index, under); !made) {
            return made;
        }
    }
    return ok();
}

Status DocumentInstance::apply(const Expansion& expansion, u32 node, WidgetId widget,
                               const Expansion* before, u32 old_node) noexcept {
    // Changed, or new: a property whose value is what it was is not applied again — which is what
    // keeps the value a player set when a reload did not touch it.
    const auto changed = [&](const ExpandedProperty& property) noexcept {
        if (before == nullptr) {
            return true;
        }
        const ExpandedProperty* old = before->find(old_node, property.key.text());
        return old == nullptr || !same_value(expansion, property, *before, *old);
    };
    Array<char> declarations(*allocator_);
    bool styles_changed = before == nullptr;
    f32 range[3] = {toolkit_->value(widget), 0.0F, 0.0F};
    bool range_changed = false;
    f32 anchors[8] = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    bool anchors_changed = false;
    BindingSource* bound = nullptr;
    BindingMode mode = BindingMode::OneWay;
    bool binding_changed = false;
    for (const ExpandedProperty& property : expansion.properties_of(node)) {
        const std::string_view key = property.key.text();
        const std::string_view text = expansion.text(property);
        const bool fresh = changed(property);
        const StyleProperty style = style_property_from_name(key);
        if (style != StyleProperty::Count) {
            // A `.cyss` property: an inline declaration, spelled as the sheet would spell it.
            styles_changed = styles_changed || fresh;
            for (const std::string_view part :
                 {key, std::string_view(": "), text, std::string_view("; ")}) {
                if (Status built = declarations.append(Span<const char>(part.data(), part.size()));
                    !built) {
                    return built;
                }
            }
            continue;
        }
        if (!is_widget_key(key) && !is_creation_key(key)) {
            if (Status reported = diagnose(diagnostics_, "no such property", property.key);
                !reported) {
                return reported;
            }
            continue;
        }
        if (!fresh || (before == nullptr && is_creation_key(key))) {
            continue;
        }
        ++report_.properties;
        Status set = ok();
        if (key == "text") {
            set = toolkit_->set_text(widget, text);
        } else if (key == "value") {
            range[0] = property.value.number;
            range_changed = true;
        } else if (key == "min" || key == "max" || key == "step") {
            range_changed = true;
        } else if (key == "items") {
            Array<std::string_view> items(*allocator_);
            set = list_texts(expansion, property, items);
            if (set) {
                set = toolkit_->set_items(widget, items.span());
            }
        } else if (key == "selected") {
            set = toolkit_->set_selected(widget, static_cast<u32>(property.value.number));
        } else if (key == "checked") {
            set = toolkit_->set_checked(widget, truth(text));
        } else if (key == "enabled") {
            set = toolkit_->set_enabled(widget, truth(text));
        } else if (key == "visible") {
            set = toolkit_->set_visible(widget, truth(text));
        } else if (key == "tooltip") {
            set = toolkit_->set_tooltip(widget, text);
        } else if (key == "anchor" || key == "offset") {
            anchors_changed = true;
        } else if (key == "bind" || key == "bind-mode") {
            binding_changed = true;
        }
        if (!set) {
            return set;
        }
    }
    // The ones that are read together.
    for (const ExpandedProperty& property : expansion.properties_of(node)) {
        const std::string_view key = property.key.text();
        if (key == "min") {
            range[1] = property.value.number;
        } else if (key == "max") {
            range[2] = property.value.number;
        } else if (key == "anchor") {
            numbers_of(expansion, property, anchors, 4);
        } else if (key == "offset") {
            numbers_of(expansion, property, anchors + 4, 4);
        } else if (key == "bind" && bindings_ != nullptr) {
            bound = bindings_->find(expansion.text(property));
        } else if (key == "bind-mode") {
            mode =
                expansion.text(property) == "two-way" ? BindingMode::TwoWay : BindingMode::OneWay;
        }
    }
    if (range_changed) {
        const ExpandedProperty* step = expansion.find(node, "step");
        const ExpandedProperty* maximum = expansion.find(node, "max");
        if (Status set = toolkit_->set_range(widget, range[1], maximum != nullptr ? range[2] : 1.0F,
                                             step != nullptr ? step->value.number : 0.0F);
            !set) {
            return set;
        }
        if (Status set = toolkit_->set_value(widget, range[0]); !set) {
            return set;
        }
    }
    if (anchors_changed) {
        if (Status set = toolkit_->set_anchors(
                widget, Vec2{anchors[0], anchors[1]}, Vec2{anchors[2], anchors[3]},
                Vec2{anchors[4], anchors[5]}, Vec2{anchors[6], anchors[7]});
            !set) {
            return set;
        }
    }
    if (binding_changed) {
        toolkit_->unbind(widget);
        if (bound != nullptr) {
            if (Status set = toolkit_->bind(widget, default_bound_property(toolkit_->kind(widget)),
                                            *bound, mode);
                !set) {
                return set;
            }
        } else if (bindings_ != nullptr) {
            const ExpandedProperty* name = expansion.find(node, "bind");
            if (name != nullptr) {
                if (Status reported = diagnose(diagnostics_, "`bind` names no game value",
                                               Name::intern(expansion.text(*name)));
                    !reported) {
                    return reported;
                }
            }
        }
    }
    // The inline declarations as a whole: one that was removed is gone, one that changed changed.
    if (before != nullptr && !styles_changed) {
        for (const ExpandedProperty& old : before->properties_of(old_node)) {
            if (style_property_from_name(old.key.text()) != StyleProperty::Count &&
                expansion.find(node, old.key.text()) == nullptr) {
                styles_changed = true;
            }
        }
    }
    if (styles_changed) {
        if (Status set = toolkit_->set_inline_style(
                widget, std::string_view(declarations.data(), declarations.size()));
            !set) {
            if (Status reported = diagnose(diagnostics_, set.error().message); !reported) {
                return reported;
            }
        }
    }
    // Classes are cheap and exact: set every time.
    if (before != nullptr) {
        const Expanded& expanded = expansion.nodes[node];
        Array<char> classes(*allocator_);
        for (u32 index = 0; index < expanded.class_count; ++index) {
            const std::string_view name = expanded.classes[index].text();
            (void)classes.append(Span<const char>(name.data(), name.size()));
            (void)classes.push_back(' ');
        }
        if (Status set =
                toolkit_->set_classes(widget, std::string_view(classes.data(), classes.size()));
            !set) {
            return set;
        }
    }
    return ok();
}

// --- Loading and reloading -----------------------------------------------------------------------

Status DocumentInstance::load(std::string_view text, WidgetId parent, DocumentResolver* resolver,
                              DocumentBindings* bindings) noexcept {
    if (current_ != nullptr) {
        if (Status unloaded = unload(); !unloaded) {
            return unloaded;
        }
    }
    resolver_ = resolver;
    bindings_ = bindings;
    parent_ = parent;
    report_ = DocumentReport{};
    void* storage = allocator_->allocate(sizeof(Expansion), alignof(Expansion));
    if (storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "ui document: no memory for the expansion");
    }
    auto* next = construct_at<Expansion>(storage, *allocator_);
    const auto discard = [this, next]() noexcept {
        next->~Expansion();
        allocator_->deallocate(next, sizeof(Expansion), alignof(Expansion));
    };
    Status expanded = expand(text, *next);
    diagnostics_.clear();
    (void)diagnostics_.append(next->diagnostics.span());
    if (!expanded || !next->diagnostics.empty()) {
        discard();
        return expanded ? fail(ErrorCode::InvalidArgument, "ui document: it has diagnostics")
                        : expanded;
    }
    current_ = next;
    for (u32 node = 0; node < current_->nodes.size(); ++node) {
        const u32 parent_node = current_->nodes[node].parent;
        const WidgetId under =
            parent_node == DocumentNode::kNoParent ? parent_ : current_->nodes[parent_node].widget;
        if (Status made = make(*current_, node, under); !made) {
            return made;
        }
    }
    return adopt(*current_);
}

Status DocumentInstance::adopt(Expansion& next) noexcept {
    roots_.clear();
    for (const Expanded& node : next.nodes.span()) {
        if (node.parent == DocumentNode::kNoParent && node.widget.is_valid()) {
            if (Status pushed = roots_.push_back(node.widget); !pushed) {
                return pushed;
            }
        }
    }
    dependencies_.clear();
    if (Status kept = dependencies_.append(next.dependencies.span()); !kept) {
        return kept;
    }
    const std::string_view sheet(next.sheet.data(), next.sheet.size());
    if (sheet != std::string_view(sheet_.data(), sheet_.size())) {
        sheet_.clear();
        if (Status kept = sheet_.append(next.sheet.span()); !kept) {
            return kept;
        }
        // An empty sheet is applied too: a document that stopped naming its sheet is no longer
        // styled by it.
        if (Status applied = toolkit_->set_stylesheet(sheet); !applied) {
            return applied;
        }
    }
    return ok();
}

Status DocumentInstance::sync(Expansion& next, u32 parent_new, WidgetId parent_widget,
                              u32 parent_old) noexcept {
    Expansion& old = *current_;
    Array<u32> fresh(*allocator_);
    Array<u32> stale(*allocator_);
    for (u32 node = 0; node < next.nodes.size(); ++node) {
        if (next.nodes[node].parent == parent_new) {
            if (Status pushed = fresh.push_back(node); !pushed) {
                return pushed;
            }
        }
    }
    for (u32 node = 0; node < old.nodes.size(); ++node) {
        if (old.nodes[node].parent == parent_old && old.nodes[node].widget.is_valid()) {
            if (Status pushed = stale.push_back(node); !pushed) {
                return pushed;
            }
        }
    }
    Array<WidgetId> order(*allocator_);
    for (const u32 node : fresh.span()) {
        const Expanded& wanted = next.nodes[node];
        // MATCHED BY ID, or — with none — by kind and place: the first unmatched sibling of the
        // same kind with no id. `widgets.h`'s rule for keys, and its consequence, hold here too.
        u32 match = DocumentNode::kNoParent;
        usize match_slot = 0;
        for (usize slot = 0; slot < stale.size(); ++slot) {
            const Expanded& candidate = old.nodes[stale[slot]];
            const bool same_kind = candidate.kind == wanted.kind;
            const bool same_key =
                wanted.id.is_empty() ? candidate.id.is_empty() : candidate.id == wanted.id;
            if (same_kind && same_key) {
                match = stale[slot];
                match_slot = slot;
                break;
            }
        }
        bool remake = match == DocumentNode::kNoParent;
        if (!remake) {
            // A property a widget is created with changed: it is made again.
            for (const std::string_view key : kCreationKeys) {
                const ExpandedProperty* now = next.find(node, key);
                const ExpandedProperty* was = old.find(match, key);
                if ((now == nullptr) != (was == nullptr) ||
                    (now != nullptr && !same_value(next, *now, old, *was))) {
                    remake = true;
                }
            }
        }
        if (remake) {
            if (match != DocumentNode::kNoParent) {
                stale.erase(match_slot);
                ++report_.removed;
                if (Status destroyed = toolkit_->destroy(old.nodes[match].widget); !destroyed) {
                    return destroyed;
                }
                old.nodes[match].widget = kNoElement;
            }
            if (Status made = make_subtree(next, node, parent_widget); !made) {
                return made;
            }
        } else {
            stale.erase(match_slot);
            const WidgetId widget = old.nodes[match].widget;
            next.nodes[node].widget = widget;
            ++report_.preserved;
            const u32 before = report_.properties;
            if (Status applied = apply(next, node, widget, &old, match); !applied) {
                return applied;
            }
            report_.updated += report_.properties != before ? 1U : 0U;
            if (Status children = sync(next, node, widget, match); !children) {
                return children;
            }
        }
        if (Status pushed = order.push_back(next.nodes[node].widget); !pushed) {
            return pushed;
        }
    }
    for (const u32 node : stale.span()) {
        ++report_.removed;
        if (Status destroyed = toolkit_->destroy(old.nodes[node].widget); !destroyed) {
            return destroyed;
        }
        old.nodes[node].widget = kNoElement;
    }
    return toolkit_->reorder(parent_widget, order.span());
}

Status DocumentInstance::reload(std::string_view text) noexcept {
    if (current_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "ui document: nothing is loaded to reload");
    }
    report_ = DocumentReport{};
    void* storage = allocator_->allocate(sizeof(Expansion), alignof(Expansion));
    if (storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "ui document: no memory for the expansion");
    }
    auto* next = construct_at<Expansion>(storage, *allocator_);
    Status expanded = expand(text, *next);
    diagnostics_.clear();
    (void)diagnostics_.append(next->diagnostics.span());
    if (!expanded || !next->diagnostics.empty()) {
        // RELOAD FAILURE KEEPS THE OLD INTERFACE: nothing has been touched yet.
        next->~Expansion();
        allocator_->deallocate(next, sizeof(Expansion), alignof(Expansion));
        return expanded ? fail(ErrorCode::InvalidArgument, "ui document: it has diagnostics")
                        : expanded;
    }
    Status synced = sync(*next, DocumentNode::kNoParent, parent_, DocumentNode::kNoParent);
    current_->~Expansion();
    allocator_->deallocate(current_, sizeof(Expansion), alignof(Expansion));
    current_ = next;
    if (!synced) {
        return synced;
    }
    return adopt(*current_);
}

Status DocumentInstance::unload() noexcept {
    if (current_ == nullptr) {
        return ok();
    }
    for (const WidgetId root : roots_.span()) {
        if (toolkit_->alive(root)) {
            if (Status destroyed = toolkit_->destroy(root); !destroyed) {
                return destroyed;
            }
        }
    }
    roots_.clear();
    current_->~Expansion();
    allocator_->deallocate(current_, sizeof(Expansion), alignof(Expansion));
    current_ = nullptr;
    return ok();
}

WidgetId DocumentInstance::widget_at_line(u32 line) const noexcept {
    if (current_ == nullptr || line == 0) {
        return kNoElement;
    }
    for (const Expanded& node : current_->nodes.span()) {
        if (node.source_line == line) {
            return node.widget;
        }
    }
    return kNoElement;
}

WidgetId DocumentInstance::find(std::string_view id) const noexcept {
    if (current_ == nullptr) {
        return kNoElement;
    }
    for (const Expanded& node : current_->nodes.span()) {
        if (node.id.text() == id && node.widget.is_valid()) {
            return node.widget;
        }
    }
    return kNoElement;
}

// --- The host ------------------------------------------------------------------------------------

Status VfsDocumentResolver::read(std::string_view path, Array<char>& out) noexcept {
    Expected<assets::VirtualPath, Error> virtual_path = assets::VirtualPath::normalise(path);
    if (!virtual_path) {
        return make_unexpected(virtual_path.error());
    }
    Array<u8> bytes(out.allocator());
    if (Status read_status = files_->read(*virtual_path, bytes); !read_status) {
        return read_status;
    }
    return out.append(Span<const char>(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

DocumentHost::DocumentHost(Allocator& allocator, Toolkit& toolkit, assets::VirtualFileSystem& files,
                           i64 settle_ns) noexcept
    : allocator_(&allocator),
      files_(&files),
      instance_(allocator, toolkit),
      resolver_(files),
      watcher_(allocator),
      settle_ns_(settle_ns),
      path_(allocator) {}

Status DocumentHost::read(Array<char>& out) noexcept {
    out.clear();
    return resolver_.read(std::string_view(path_.data(), path_.size()), out);
}

Status DocumentHost::watch(i64 now_ns) noexcept {
    // The document and everything it reached: an edit to a sheet or a component reloads too.
    watcher_.stop();
    assets::FileWatcherConfig config;
    config.settle_ns = settle_ns_;
    if (Status started = watcher_.start(*files_, config); !started) {
        return started;
    }
    const auto watch_path = [this](std::string_view path) noexcept -> Status {
        Expected<assets::VirtualPath, Error> virtual_path = assets::VirtualPath::normalise(path);
        if (!virtual_path) {
            return make_unexpected(virtual_path.error());
        }
        return watcher_.watch(*virtual_path);
    };
    if (Status watched = watch_path(std::string_view(path_.data(), path_.size())); !watched) {
        return watched;
    }
    for (const std::string_view dependency : instance_.dependencies()) {
        if (Status watched = watch_path(dependency); !watched) {
            return watched;
        }
    }
    // What is there now is what was loaded; only a change after it is news.
    return watcher_.prime(now_ns);
}

Status DocumentHost::open(std::string_view path, WidgetId parent, DocumentBindings* bindings,
                          i64 now_ns) noexcept {
    path_.clear();
    if (Status kept = path_.append(Span<const char>(path.data(), path.size())); !kept) {
        return kept;
    }
    Array<char> text(*allocator_);
    if (Status read_status = read(text); !read_status) {
        return read_status;
    }
    if (Status loaded = instance_.load(std::string_view(text.data(), text.size()), parent,
                                       &resolver_, bindings);
        !loaded) {
        return loaded;
    }
    return watch(now_ns);
}

Status DocumentHost::reload(i64 now_ns) noexcept {
    Array<char> text(*allocator_);
    if (Status read_status = read(text); !read_status) {
        ++failed_;
        return read_status;
    }
    if (Status reloaded = instance_.reload(std::string_view(text.data(), text.size())); !reloaded) {
        ++failed_;
        return reloaded;
    }
    ++reloads_;
    // What the reload reached may have changed — a new component, a sheet dropped.
    return watch(now_ns);
}

Expected<bool, Error> DocumentHost::poll(i64 now_ns) noexcept {
    // The watcher reports to an observer, and counts nothing without one.
    u32 seen = 0;
    Expected<u32, Error> changes = watcher_.poll(
        now_ns,
        [](void* user, const assets::WatchEvent& event) noexcept {
            (void)event;
            ++*static_cast<u32*>(user);
        },
        &seen);
    if (!changes) {
        return make_unexpected(changes.error());
    }
    if (*changes == 0) {
        return false;
    }
    // A reload that fails keeps the interface, and the watcher keeps watching: the next save is
    // the next attempt.
    return reload(now_ns).has_value();
}

}  // namespace cy::ui
