// CyberGraph's deterministic textual source: the writer and the reader. Task 2.1.
//
// See text.h for why the form is canonical and what opaque preservation costs the reader.

#include <cy/graph/text.h>

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace cy::graph {
namespace {

[[nodiscard]] Error invalid(const char* message) noexcept {
    return Error{ErrorCode::InvalidArgument, message, 0};
}

/// Appends to an `Array<char>`, carrying one status rather than checking every call site.
class Writer {
public:
    explicit Writer(Array<char>& out) noexcept : out_(&out) {}

    void text(std::string_view value) noexcept {
        if (!status_) {
            return;
        }
        for (const char character : value) {
            if (Status pushed = out_->push_back(character); !pushed) {
                status_ = pushed;
                return;
            }
        }
    }

    void quoted(std::string_view value) noexcept {
        text("\"");
        for (const char character : value) {
            if (character == '"' || character == '\\') {
                text("\\");
            }
            text(std::string_view(&character, 1));
        }
        text("\"");
    }

    void unsigned_value(u64 value) noexcept {
        char buffer[24] = {};
        (void)std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
        text(buffer);
    }

    void number(f32 value) noexcept {
        char buffer[32] = {};
        (void)std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
        text(buffer);
    }

    [[nodiscard]] Status status() const noexcept { return status_; }

private:
    Array<char>* out_;
    Status status_ = ok();
};

template <typename Less>
void sort_indices(Array<u32>& order, Less&& less) noexcept {
    for (usize outer = 1; outer < order.size(); ++outer) {
        for (usize inner = outer; inner > 0 && less(order[inner], order[inner - 1]); --inner) {
            const u32 swap = order[inner - 1];
            order[inner - 1] = order[inner];
            order[inner] = swap;
        }
    }
}

[[nodiscard]] Status fill_indices(Array<u32>& order, usize count) noexcept {
    order.clear();
    for (usize index = 0; index < count; ++index) {
        if (Status pushed = order.push_back(static_cast<u32>(index)); !pushed) {
            return pushed;
        }
    }
    return ok();
}

constexpr Capability kAllCapabilities[] = {
    Capability::ReadWorld,     Capability::WriteWorld, Capability::SpawnEntity,
    Capability::DestroyEntity, Capability::Physics,    Capability::Audio,
    Capability::Network,       Capability::FileSystem, Capability::Randomness,
    Capability::WallClock,     Capability::NativeCall,
};

void write_pin(Writer& writer, const PinDesc& pin) noexcept {
    writer.text("interface ");
    writer.text(pin.direction == PinDirection::Input ? "in " : "out ");
    writer.quoted(pin.name.text());
    writer.text(" : ");
    writer.quoted(pin.type.text());
    writer.text(pin.execution ? " exec" : "");
    writer.text(pin.variadic ? " variadic" : "");
    writer.text(pin.required ? " required" : "");
    writer.text("\n");
}

void write_property(Writer& writer, const Property& property) noexcept {
    writer.text("    prop ");
    writer.quoted(property.name.text());
    writer.text(" : ");
    writer.quoted(property.value.type.text());
    writer.text(" = ");
    const Immediate& value = property.value.value;
    const bool numeric = value.x != 0.0F || value.y != 0.0F || value.z != 0.0F || value.w != 0.0F ||
                         value.mask != 0 || property.value.text == Name{};
    if (numeric) {
        writer.text("(");
        writer.number(value.x);
        writer.text(", ");
        writer.number(value.y);
        writer.text(", ");
        writer.number(value.z);
        writer.text(", ");
        writer.number(value.w);
        writer.text(", ");
        writer.unsigned_value(value.mask);
        writer.text(")");
    } else {
        writer.quoted(property.value.text.text());
    }
    writer.text("\n");
}

void write_node(Writer& writer, const Graph& graph, const GraphNode& node) noexcept {
    writer.text("node ");
    writer.unsigned_value(node.key);
    writer.text(" ");
    writer.quoted(node.type.text());
    writer.text(" v");
    writer.unsigned_value(node.version);
    if (node.muted) {
        writer.text(" muted");
    }
    if (node.subgraph != Name{}) {
        writer.text(" subgraph ");
        writer.quoted(node.subgraph.text());
    }
    writer.text(" {\n");

    const std::string_view opaque = graph.opaque_body(node.key);
    if (!opaque.empty()) {
        // VERBATIM. Whatever the reader could not understand is written back exactly as authored.
        writer.text(opaque);
        writer.text("}\n");
        return;
    }

    const Span<const Property> entries = graph.properties(node.key);
    Array<u32> order(graph.allocator());
    if (!fill_indices(order, entries.size())) {
        return;
    }
    sort_indices(order, [entries](u32 a, u32 b) noexcept {
        return entries[a].name.text() < entries[b].name.text();
    });
    for (const u32 index : order) {
        write_property(writer, entries[index]);
    }
    writer.text("}\n");
}

// --- Reading ------------------------------------------------------------------------------------

/// A cursor over the text, line by line. The grammar is line-oriented on purpose: a format whose
/// reader needs a parser generator is a format nobody edits by hand when a merge goes wrong.
class LineReader {
public:
    explicit LineReader(std::string_view text) noexcept : text_(text) {}

    [[nodiscard]] bool next(std::string_view& line) noexcept {
        while (cursor_ < text_.size()) {
            const usize begin = cursor_;
            usize end = begin;
            while (end < text_.size() && text_[end] != '\n') {
                ++end;
            }
            cursor_ = end < text_.size() ? end + 1 : end;
            raw_ = text_.substr(begin, end - begin);
            std::string_view trimmed = raw_;
            while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
                trimmed.remove_prefix(1);
            }
            while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == ' ')) {
                trimmed.remove_suffix(1);
            }
            if (trimmed.empty() || trimmed.starts_with("#")) {
                continue;
            }
            line = trimmed;
            return true;
        }
        return false;
    }

    /// The line exactly as it appeared, indentation included. Opaque preservation reads this.
    [[nodiscard]] std::string_view raw() const noexcept { return raw_; }

private:
    std::string_view text_;
    std::string_view raw_;
    usize cursor_ = 0;
};

/// A cursor over one line's tokens.
class Tokens {
public:
    explicit Tokens(std::string_view line) noexcept : line_(line) {}

    [[nodiscard]] bool word(std::string_view& out) noexcept {
        skip_spaces();
        if (cursor_ >= line_.size()) {
            return false;
        }
        const usize begin = cursor_;
        while (cursor_ < line_.size() && line_[cursor_] != ' ') {
            ++cursor_;
        }
        out = line_.substr(begin, cursor_ - begin);
        return true;
    }

    /// A quoted string with `\"` and `\\` unescaped into `scratch`.
    [[nodiscard]] bool quoted(Array<char>& scratch, std::string_view& out) noexcept {
        skip_spaces();
        if (cursor_ >= line_.size() || line_[cursor_] != '"') {
            return false;
        }
        ++cursor_;
        scratch.clear();
        while (cursor_ < line_.size() && line_[cursor_] != '"') {
            char character = line_[cursor_];
            if (character == '\\' && cursor_ + 1 < line_.size()) {
                ++cursor_;
                character = line_[cursor_];
            }
            if (!scratch.push_back(character)) {
                return false;
            }
            ++cursor_;
        }
        if (cursor_ >= line_.size()) {
            return false;
        }
        ++cursor_;
        out = std::string_view(scratch.data(), scratch.size());
        return true;
    }

    [[nodiscard]] bool rest(std::string_view& out) noexcept {
        skip_spaces();
        out = line_.substr(cursor_);
        cursor_ = line_.size();
        return !out.empty();
    }

private:
    void skip_spaces() noexcept {
        while (cursor_ < line_.size() && line_[cursor_] == ' ') {
            ++cursor_;
        }
    }

    std::string_view line_;
    usize cursor_ = 0;
};

[[nodiscard]] u64 parse_unsigned(std::string_view token) noexcept {
    u64 value = 0;
    for (const char character : token) {
        if (character < '0' || character > '9') {
            break;
        }
        value = (value * 10U) + static_cast<u64>(character - '0');
    }
    return value;
}

[[nodiscard]] f32 parse_float(std::string_view token) noexcept {
    char buffer[64] = {};
    const usize size = token.size() < sizeof(buffer) - 1 ? token.size() : sizeof(buffer) - 1;
    for (usize index = 0; index < size; ++index) {
        buffer[index] = token[index];
    }
    return std::strtof(buffer, nullptr);
}

/// `(x, y, z, w, mask)` — the numeric half of a property's value.
[[nodiscard]] bool parse_tuple(std::string_view text, Immediate& out) noexcept {
    if (text.size() < 2 || text.front() != '(' || text.back() != ')') {
        return false;
    }
    std::string_view inner = text.substr(1, text.size() - 2);
    f32 component[4] = {};
    for (u32 index = 0; index < 5U; ++index) {
        const usize comma = inner.find(',');
        const std::string_view token = inner.substr(0, comma);
        if (index < 4U) {
            component[index] = parse_float(token);
        } else {
            out.mask = static_cast<u32>(parse_unsigned(token));
        }
        if (comma == std::string_view::npos) {
            if (index != 4U) {
                return false;
            }
            break;
        }
        inner.remove_prefix(comma + 1);
        while (!inner.empty() && inner.front() == ' ') {
            inner.remove_prefix(1);
        }
    }
    out.x = component[0];
    out.y = component[1];
    out.z = component[2];
    out.w = component[3];
    return true;
}

struct ParseState {
    Graph* graph = nullptr;
    const NodeRegistry* registry = nullptr;
    DiagnosticSink* sink = nullptr;
    Array<char>* scratch = nullptr;
};

[[nodiscard]] Status parse_property(ParseState& state, NodeKey key,
                                    std::string_view line) noexcept {
    Tokens tokens(line);
    std::string_view keyword;
    if (!tokens.word(keyword) || keyword != "prop") {
        return make_unexpected(invalid("a node body holds properties"));
    }
    std::string_view text;
    if (!tokens.quoted(*state.scratch, text)) {
        return make_unexpected(invalid("a property needs a quoted name"));
    }
    const Name property = Name::intern(text);
    std::string_view colon;
    if (!tokens.word(colon) || colon != ":") {
        return make_unexpected(invalid("a property names its type after a colon"));
    }
    if (!tokens.quoted(*state.scratch, text)) {
        return make_unexpected(invalid("a property needs a quoted type"));
    }
    Literal value;
    value.type = Name::intern(text);
    std::string_view equals;
    if (!tokens.word(equals) || equals != "=") {
        return make_unexpected(invalid("a property needs a value"));
    }
    std::string_view remainder;
    if (!tokens.rest(remainder)) {
        return make_unexpected(invalid("a property needs a value"));
    }
    if (remainder.front() == '"') {
        Tokens value_tokens(remainder);
        std::string_view literal_text;
        if (!value_tokens.quoted(*state.scratch, literal_text)) {
            return make_unexpected(invalid("an unterminated quoted property value"));
        }
        value.text = Name::intern(literal_text);
    } else if (!parse_tuple(remainder, value.value)) {
        return make_unexpected(invalid("a numeric property value is (x, y, z, w, mask)"));
    }
    return state.graph->set_property(key, property, value);
}

/// One `node ... { ... }` block. `known` decides whether the body is parsed or preserved.
[[nodiscard]] Status parse_node(ParseState& state, LineReader& reader,
                                std::string_view header) noexcept {
    Tokens tokens(header);
    std::string_view token;
    (void)tokens.word(token);  // "node"
    if (!tokens.word(token)) {
        return make_unexpected(invalid("a node needs a key"));
    }
    const auto key = static_cast<NodeKey>(parse_unsigned(token));
    std::string_view text;
    if (!tokens.quoted(*state.scratch, text)) {
        return make_unexpected(invalid("a node needs a quoted type"));
    }
    const Name type = Name::intern(text);
    u32 version = 1;
    bool muted = false;
    Name subgraph;
    while (tokens.word(token)) {
        if (token == "{") {
            break;
        }
        if (token.starts_with("v")) {
            version = static_cast<u32>(parse_unsigned(token.substr(1)));
        } else if (token == "muted") {
            muted = true;
        } else if (token == "subgraph") {
            if (!tokens.quoted(*state.scratch, text)) {
                return make_unexpected(invalid("a subgraph instance needs a quoted graph name"));
            }
            subgraph = Name::intern(text);
        }
    }

    if (Status added = state.graph->add_node(key, type, version); !added) {
        return added;
    }
    if (muted) {
        if (Status set = state.graph->mute(key, true); !set) {
            return set;
        }
    }
    if (subgraph != Name{}) {
        if (Status set = state.graph->set_subgraph(key, subgraph); !set) {
            return set;
        }
    }

    const bool known = state.registry != nullptr && state.registry->find(type) != nullptr;
    Array<char> body(state.graph->allocator());
    std::string_view line;
    while (reader.next(line)) {
        if (line == "}") {
            break;
        }
        if (known) {
            if (Status parsed = parse_property(state, key, line); !parsed) {
                return parsed;
            }
            continue;
        }
        if (Status appended =
                body.append(Span<const char>(reader.raw().data(), reader.raw().size()));
            !appended) {
            return appended;
        }
        if (Status pushed = body.push_back('\n'); !pushed) {
            return pushed;
        }
    }
    if (known || subgraph != Name{}) {
        return ok();
    }
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Warning;
    diagnostic.node = key;
    diagnostic.detail = type;
    diagnostic.message =
        "this node's type is not registered; its body is PRESERVED verbatim and will be written "
        "back unchanged";
    state.sink->report(diagnostic);
    return state.graph->set_opaque_body(key, std::string_view(body.data(), body.size()));
}

[[nodiscard]] Status parse_interface(ParseState& state, std::string_view line) noexcept {
    Tokens tokens(line);
    std::string_view token;
    (void)tokens.word(token);  // "interface"
    if (!tokens.word(token)) {
        return make_unexpected(invalid("an interface pin needs a direction"));
    }
    PinDesc pin;
    pin.direction = token == "out" ? PinDirection::Output : PinDirection::Input;
    std::string_view text;
    if (!tokens.quoted(*state.scratch, text)) {
        return make_unexpected(invalid("an interface pin needs a quoted name"));
    }
    pin.name = Name::intern(text);
    std::string_view colon;
    if (!tokens.word(colon) || colon != ":") {
        return make_unexpected(invalid("an interface pin names its type after a colon"));
    }
    if (!tokens.quoted(*state.scratch, text)) {
        return make_unexpected(invalid("an interface pin needs a quoted type"));
    }
    pin.type = Name::intern(text);
    while (tokens.word(token)) {
        pin.execution = pin.execution || token == "exec";
        pin.variadic = pin.variadic || token == "variadic";
        pin.required = pin.required || token == "required";
    }
    return state.graph->declare_interface(pin);
}

[[nodiscard]] Status parse_link(ParseState& state, std::string_view line) noexcept {
    Tokens tokens(line);
    std::string_view token;
    (void)tokens.word(token);  // "link"
    if (!tokens.word(token)) {
        return make_unexpected(invalid("a wire needs a source node"));
    }
    const auto from = static_cast<NodeKey>(parse_unsigned(token));
    std::string_view text;
    if (!tokens.quoted(*state.scratch, text)) {
        return make_unexpected(invalid("a wire needs a quoted source pin"));
    }
    const Name from_pin = Name::intern(text);
    if (!tokens.word(token) || token != "->") {
        return make_unexpected(invalid("a wire is written source -> target"));
    }
    if (!tokens.word(token)) {
        return make_unexpected(invalid("a wire needs a target node"));
    }
    const auto to = static_cast<NodeKey>(parse_unsigned(token));
    if (!tokens.quoted(*state.scratch, text)) {
        return make_unexpected(invalid("a wire needs a quoted target pin"));
    }
    return state.graph->connect(from, from_pin, to, Name::intern(text));
}

[[nodiscard]] Status parse_layout(ParseState& state, std::string_view line) noexcept {
    Tokens tokens(line);
    std::string_view token;
    (void)tokens.word(token);  // "layout"
    if (!tokens.word(token)) {
        return make_unexpected(invalid("a layout entry needs a node key"));
    }
    NodeLayout layout;
    layout.key = static_cast<NodeKey>(parse_unsigned(token));
    if (!tokens.word(token) || token != "at") {
        return make_unexpected(invalid("a layout entry is written 'at x y'"));
    }
    if (!tokens.word(token)) {
        return make_unexpected(invalid("a layout entry needs an x"));
    }
    layout.x = parse_float(token);
    if (!tokens.word(token)) {
        return make_unexpected(invalid("a layout entry needs a y"));
    }
    layout.y = parse_float(token);
    while (tokens.word(token)) {
        if (token == "tint" && tokens.word(token)) {
            layout.tint = static_cast<u32>(parse_unsigned(token));
        } else if (token == "comment") {
            std::string_view text;
            if (tokens.quoted(*state.scratch, text)) {
                layout.comment = Name::intern(text);
            }
        }
    }
    return state.graph->set_layout(layout);
}

[[nodiscard]] Status parse_capabilities(ParseState& state, std::string_view line) noexcept {
    Tokens tokens(line);
    std::string_view token;
    (void)tokens.word(token);  // "capability"
    while (tokens.word(token)) {
        bool matched = false;
        for (const Capability capability : kAllCapabilities) {
            if (token == capability_name(capability)) {
                state.graph->grant(capability);
                matched = true;
            }
        }
        if (!matched) {
            return make_unexpected(invalid("no such capability"));
        }
    }
    return ok();
}

}  // namespace

Status write_graph(const Graph& graph, Array<char>& out, bool include_layout) noexcept {
    Writer writer(out);
    writer.text("cygraph ");
    writer.unsigned_value(kTextFormatVersion);
    writer.text("\ngraph ");
    writer.quoted(graph.name().text());
    writer.text(" version ");
    writer.unsigned_value(graph.version());
    writer.text("\n");

    writer.text("capability");
    for (const Capability capability : kAllCapabilities) {
        if (has_capability(graph.granted(), capability)) {
            writer.text(" ");
            writer.text(capability_name(capability));
        }
    }
    writer.text("\ndeterministic ");
    writer.text(graph.claims_deterministic() ? "true" : "false");
    writer.text("\n");

    for (const PinDesc& pin : graph.interface_pins()) {
        write_pin(writer, pin);
    }

    const Span<const GraphNode> nodes = graph.nodes();
    Array<u32> order(graph.allocator());
    if (Status filled = fill_indices(order, nodes.size()); !filled) {
        return filled;
    }
    sort_indices(order, [nodes](u32 a, u32 b) noexcept { return nodes[a].key < nodes[b].key; });
    for (const u32 index : order) {
        write_node(writer, graph, nodes[index]);
    }

    const Span<const Link> links = graph.links();
    Array<u32> link_order(graph.allocator());
    if (Status filled = fill_indices(link_order, links.size()); !filled) {
        return filled;
    }
    sort_indices(link_order, [links](u32 a, u32 b) noexcept {
        if (links[a].to != links[b].to) {
            return links[a].to < links[b].to;
        }
        if (links[a].to_pin != links[b].to_pin) {
            return links[a].to_pin.text() < links[b].to_pin.text();
        }
        if (links[a].from != links[b].from) {
            return links[a].from < links[b].from;
        }
        return links[a].from_pin.text() < links[b].from_pin.text();
    });
    for (const u32 index : link_order) {
        const Link& link = links[index];
        writer.text("link ");
        writer.unsigned_value(link.from);
        writer.text(" ");
        writer.quoted(link.from_pin.text());
        writer.text(" -> ");
        writer.unsigned_value(link.to);
        writer.text(" ");
        writer.quoted(link.to_pin.text());
        writer.text("\n");
    }

    if (include_layout) {
        const Span<const NodeLayout> entries = graph.layouts();
        Array<u32> layout_order(graph.allocator());
        if (Status filled = fill_indices(layout_order, entries.size()); !filled) {
            return filled;
        }
        sort_indices(layout_order,
                     [entries](u32 a, u32 b) noexcept { return entries[a].key < entries[b].key; });
        for (const u32 index : layout_order) {
            const NodeLayout& layout = entries[index];
            writer.text("layout ");
            writer.unsigned_value(layout.key);
            writer.text(" at ");
            writer.number(layout.x);
            writer.text(" ");
            writer.number(layout.y);
            if (layout.tint != 0) {
                writer.text(" tint ");
                writer.unsigned_value(layout.tint);
            }
            if (layout.comment != Name{}) {
                writer.text(" comment ");
                writer.quoted(layout.comment.text());
            }
            writer.text("\n");
        }
    }
    if (!writer.status()) {
        return fail(ErrorCode::OutOfMemory, "the graph's text could not be grown");
    }
    return ok();
}

Expected<Graph, Error> parse_graph(std::string_view text, const NodeRegistry* registry,
                                   Allocator& allocator, DiagnosticSink& sink) noexcept {
    LineReader reader(text);
    Array<char> scratch(allocator);
    std::string_view line;
    if (!reader.next(line)) {
        return make_unexpected(invalid("an empty buffer is not a graph"));
    }
    {
        Tokens tokens(line);
        std::string_view token;
        if (!tokens.word(token) || token != "cygraph") {
            return make_unexpected(invalid("this is not a CyberGraph source"));
        }
        if (!tokens.word(token) || parse_unsigned(token) != kTextFormatVersion) {
            return make_unexpected(
                invalid("this graph was written at a text format version this "
                        "build does not read"));
        }
    }
    if (!reader.next(line)) {
        return make_unexpected(invalid("a graph needs a name"));
    }
    Graph graph(allocator, Name{});
    {
        Tokens tokens(line);
        std::string_view token;
        if (!tokens.word(token) || token != "graph") {
            return make_unexpected(invalid("the second line names the graph"));
        }
        std::string_view graph_name;
        if (!tokens.quoted(scratch, graph_name)) {
            return make_unexpected(invalid("a graph needs a quoted name"));
        }
        graph.set_name(Name::intern(graph_name));
        if (tokens.word(token) && token == "version" && tokens.word(token)) {
            graph.set_version(static_cast<u32>(parse_unsigned(token)));
        }
    }

    ParseState state;
    state.graph = &graph;
    state.registry = registry;
    state.sink = &sink;
    state.scratch = &scratch;

    while (reader.next(line)) {
        Tokens tokens(line);
        std::string_view keyword;
        (void)tokens.word(keyword);
        Status handled = ok();
        if (keyword == "node") {
            handled = parse_node(state, reader, line);
        } else if (keyword == "link") {
            handled = parse_link(state, line);
        } else if (keyword == "layout") {
            handled = parse_layout(state, line);
        } else if (keyword == "interface") {
            handled = parse_interface(state, line);
        } else if (keyword == "capability") {
            handled = parse_capabilities(state, line);
        } else if (keyword == "deterministic") {
            std::string_view token;
            graph.set_claims_deterministic(!tokens.word(token) || token != "false");
        } else {
            return make_unexpected(
                invalid("this line begins with a word the format does not have"));
        }
        if (!handled) {
            return make_unexpected(handled.error());
        }
    }
    if (registry != nullptr) {
        graph.resolve(*registry);
    }
    return graph;
}

}  // namespace cy::graph
