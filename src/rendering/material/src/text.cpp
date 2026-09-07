// The text material front-end: lexer, parser, and lowering into the same builder. M7 task 6.1.
//
// See text.h. Nothing in this file knows anything the graph front-end does not: both call
// `Builder`, and every canonicalisation happens there.

#include <cy/rendering/material/text.h>

#include <cstdlib>
#include <cstring>
#include <utility>

namespace cy::rendering::material {
namespace {

enum class TokenKind : u8 { End = 0, Identifier, Number, Symbol, String };

struct Token {
    TokenKind kind = TokenKind::End;
    std::string_view text;
    f64 number = 0.0;
    char symbol = '\0';
    u32 line = 1;
    u32 column = 1;
};

[[nodiscard]] bool is_space(char character) noexcept {
    return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

[[nodiscard]] bool is_digit(char character) noexcept {
    return character >= '0' && character <= '9';
}

[[nodiscard]] bool is_identifier_start(char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           character == '_';
}

[[nodiscard]] bool is_identifier(char character) noexcept {
    return is_identifier_start(character) || is_digit(character);
}

/// One token at a time, with a line and column on every one so a diagnostic can point at it.
class Lexer {
public:
    explicit Lexer(std::string_view source) noexcept : source_(source) { advance(); }

    [[nodiscard]] const Token& peek() const noexcept { return token_; }

    Token take() noexcept {
        const Token current = token_;
        advance();
        return current;
    }

private:
    void skip_trivia() noexcept {
        while (cursor_ < source_.size()) {
            const char character = source_[cursor_];
            if (is_space(character)) {
                step();
                continue;
            }
            if (character == '/' && cursor_ + 1 < source_.size() && source_[cursor_ + 1] == '/') {
                while (cursor_ < source_.size() && source_[cursor_] != '\n') {
                    step();
                }
                continue;
            }
            return;
        }
    }

    void step() noexcept {
        if (source_[cursor_] == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        ++cursor_;
    }

    void advance() noexcept {
        skip_trivia();
        token_ = Token{};
        token_.line = line_;
        token_.column = column_;
        if (cursor_ >= source_.size()) {
            token_.kind = TokenKind::End;
            return;
        }
        const char character = source_[cursor_];
        if (is_identifier_start(character)) {
            const usize begin = cursor_;
            while (cursor_ < source_.size() && is_identifier(source_[cursor_])) {
                step();
            }
            token_.kind = TokenKind::Identifier;
            token_.text = source_.substr(begin, cursor_ - begin);
            return;
        }
        if (is_digit(character) ||
            (character == '.' && cursor_ + 1 < source_.size() && is_digit(source_[cursor_ + 1]))) {
            const usize begin = cursor_;
            while (cursor_ < source_.size() &&
                   (is_digit(source_[cursor_]) || source_[cursor_] == '.' ||
                    source_[cursor_] == 'e' || source_[cursor_] == 'E' ||
                    ((source_[cursor_] == '-' || source_[cursor_] == '+') && cursor_ > begin &&
                     (source_[cursor_ - 1] == 'e' || source_[cursor_ - 1] == 'E')))) {
                step();
            }
            char buffer[64] = {};
            const usize size = cursor_ - begin < sizeof(buffer) - 1 ? cursor_ - begin : 0;
            std::memcpy(buffer, source_.data() + begin, size);
            token_.kind = TokenKind::Number;
            token_.text = source_.substr(begin, cursor_ - begin);
            token_.number = std::strtod(buffer, nullptr);
            return;
        }
        if (character == '"') {
            step();
            const usize begin = cursor_;
            while (cursor_ < source_.size() && source_[cursor_] != '"') {
                step();
            }
            token_.kind = TokenKind::String;
            token_.text = source_.substr(begin, cursor_ - begin);
            if (cursor_ < source_.size()) {
                step();
            }
            return;
        }
        token_.kind = TokenKind::Symbol;
        token_.symbol = character;
        token_.text = source_.substr(cursor_, 1);
        step();
    }

    std::string_view source_;
    usize cursor_ = 0;
    u32 line_ = 1;
    u32 column_ = 1;
    Token token_;
};

/// A `let` binding, an attribute or a field, by name.
struct Binding {
    Name name;
    NodeId value = kInvalidNode;
};

[[nodiscard]] ValueType type_from_name(std::string_view text) noexcept {
    if (text == "float") {
        return ValueType::Float;
    }
    if (text == "float2") {
        return ValueType::Vec2;
    }
    if (text == "float3") {
        return ValueType::Vec3;
    }
    if (text == "float4") {
        return ValueType::Vec4;
    }
    if (text == "int") {
        return ValueType::Int;
    }
    if (text == "bool") {
        return ValueType::Bool;
    }
    return ValueType::Count;
}

/// The leaf closure a call name builds, or `Op::Count`.
[[nodiscard]] Op closure_from_name(std::string_view text) noexcept {
    if (text == "diffuse") {
        return Op::Diffuse;
    }
    if (text == "specular") {
        return Op::Specular;
    }
    if (text == "coat") {
        return Op::Coat;
    }
    if (text == "sheen") {
        return Op::Sheen;
    }
    if (text == "emission") {
        return Op::Emission;
    }
    if (text == "transmission") {
        return Op::Transmission;
    }
    if (text == "subsurface") {
        return Op::Subsurface;
    }
    return Op::Count;
}

/// The arithmetic op a call name builds, or `Op::Count`.
[[nodiscard]] Op call_from_name(std::string_view text) noexcept {
    if (text == "min") {
        return Op::Min;
    }
    if (text == "max") {
        return Op::Max;
    }
    if (text == "pow") {
        return Op::Pow;
    }
    if (text == "dot") {
        return Op::Dot;
    }
    if (text == "saturate") {
        return Op::Saturate;
    }
    if (text == "normalize") {
        return Op::Normalize;
    }
    if (text == "lerp") {
        return Op::Lerp;
    }
    if (text == "select") {
        return Op::Select;
    }
    if (text == "layer") {
        return Op::ClosureLayer;
    }
    return Op::Count;
}

class Parser {
public:
    Parser(std::string_view source, Allocator& allocator, ParseDiagnostic& diagnostic,
           const PassSwitches& switches) noexcept
        : lexer_(source),
          allocator_(&allocator),
          diagnostic_(&diagnostic),
          policy_(builder_policy(switches)),
          bindings_(allocator) {}

    [[nodiscard]] Expected<Module, Error> run() noexcept;

private:
    // --- Diagnostics ----------------------------------------------------------------------------

    [[nodiscard]] Error report(const Token& token, const char* message) noexcept {
        diagnostic_->line = token.line;
        diagnostic_->column = token.column;
        diagnostic_->message.clear();
        for (const char* cursor = message; *cursor != '\0'; ++cursor) {
            if (!diagnostic_->message.push_back(*cursor)) {
                break;
            }
        }
        if (!token.text.empty()) {
            const char* separator = ": ";
            for (const char* cursor = separator; *cursor != '\0'; ++cursor) {
                if (!diagnostic_->message.push_back(*cursor)) {
                    break;
                }
            }
            for (const char character : token.text) {
                if (!diagnostic_->message.push_back(character)) {
                    break;
                }
            }
        }
        return Error{ErrorCode::InvalidArgument, "the material definition could not be parsed", 0};
    }

    [[nodiscard]] bool at_symbol(char symbol) const noexcept {
        return lexer_.peek().kind == TokenKind::Symbol && lexer_.peek().symbol == symbol;
    }

    [[nodiscard]] bool at_keyword(std::string_view word) const noexcept {
        return lexer_.peek().kind == TokenKind::Identifier && lexer_.peek().text == word;
    }

    [[nodiscard]] Status expect_symbol(char symbol, const char* message) noexcept {
        if (!at_symbol(symbol)) {
            return make_unexpected(report(lexer_.peek(), message));
        }
        (void)lexer_.take();
        return ok();
    }

    // --- Declarations ---------------------------------------------------------------------------

    [[nodiscard]] Status parse_parameter(bool requested_static) noexcept;
    [[nodiscard]] Status parse_texture() noexcept;
    [[nodiscard]] Status parse_attribute(bool is_field) noexcept;
    [[nodiscard]] Status parse_let() noexcept;
    [[nodiscard]] Status parse_statement() noexcept;
    [[nodiscard]] Status parse_annotations() noexcept;

    // --- Expressions ----------------------------------------------------------------------------

    [[nodiscard]] Expected<NodeId, Error> parse_expression() noexcept;
    [[nodiscard]] Expected<NodeId, Error> parse_term() noexcept;
    [[nodiscard]] Expected<NodeId, Error> parse_postfix() noexcept;
    [[nodiscard]] Expected<NodeId, Error> parse_primary() noexcept;
    [[nodiscard]] Expected<NodeId, Error> parse_call(const Token& name) noexcept;
    [[nodiscard]] Status parse_arguments(NodeId* out, u32 max, u32& count) noexcept;
    [[nodiscard]] Expected<NodeId, Error> combine(const Token& op, NodeId left,
                                                  NodeId right) noexcept;
    [[nodiscard]] Expected<NodeId, Error> resolve(const Token& token) noexcept;
    [[nodiscard]] Expected<NodeId, Error> immediate_tuple(const Token& open) noexcept;

    [[nodiscard]] Status bind(Name name, NodeId value) noexcept;
    [[nodiscard]] NodeId lookup(Name name) const noexcept;

    Lexer lexer_;
    Allocator* allocator_;
    ParseDiagnostic* diagnostic_;
    Builder* builder_ = nullptr;
    BuilderPolicy policy_;
    Array<Binding> bindings_;
    /// The annotations `@microdetail` and friends collected for the next `let`.
    NodeFlags pending_ = NodeFlags::None;
};

Status Parser::bind(Name name, NodeId value) noexcept {
    for (Binding& binding : bindings_) {
        if (binding.name == name) {
            binding.value = value;
            return ok();
        }
    }
    return bindings_.push_back(Binding{name, value});
}

NodeId Parser::lookup(Name name) const noexcept {
    for (const Binding& binding : bindings_) {
        if (binding.name == name) {
            return binding.value;
        }
    }
    return kInvalidNode;
}

Status Parser::parse_parameter(bool requested_static) noexcept {
    const Token name = lexer_.take();
    if (name.kind != TokenKind::Identifier) {
        return make_unexpected(report(name, "a parameter needs a name"));
    }
    if (Status expected = expect_symbol(':', "a parameter needs a type"); !expected) {
        return expected;
    }
    const Token type_name = lexer_.take();
    const ValueType type = type_from_name(type_name.text);
    if (type == ValueType::Count) {
        return make_unexpected(report(type_name, "no such type"));
    }
    ParameterDecl decl;
    decl.name = Name::intern(name.text);
    decl.type = type;
    decl.requested_static = requested_static;
    if (at_symbol('=')) {
        (void)lexer_.take();
        auto literal = parse_expression();
        if (!literal) {
            return make_unexpected(literal.error());
        }
        const Node& node = builder_->node(literal.value());
        if (node.op != Op::Constant) {
            return make_unexpected(report(name, "a parameter's default must be a literal"));
        }
        decl.default_value = node.value;
    }
    if (Status declared = builder_->declare_parameter(decl); !declared) {
        return make_unexpected(report(name, "this parameter is declared twice"));
    }
    return expect_symbol(';', "a declaration ends with a semicolon");
}

Status Parser::parse_texture() noexcept {
    const Token name = lexer_.take();
    if (name.kind != TokenKind::Identifier) {
        return make_unexpected(report(name, "a texture needs a name"));
    }
    TextureDecl decl;
    decl.name = Name::intern(name.text);
    if (at_keyword("average")) {
        (void)lexer_.take();
        const Token open = lexer_.peek();
        auto average = parse_expression();
        if (!average) {
            return make_unexpected(average.error());
        }
        const Node& node = builder_->node(average.value());
        if (node.op != Op::Constant) {
            return make_unexpected(report(open, "a texture's average must be a literal"));
        }
        decl.average = node.value;
    }
    if (at_keyword("shadow_critical")) {
        (void)lexer_.take();
        decl.shadow_critical = true;
    }
    if (Status declared = builder_->declare_texture(decl); !declared) {
        return make_unexpected(report(name, "this texture is declared twice"));
    }
    return expect_symbol(';', "a declaration ends with a semicolon");
}

Status Parser::parse_attribute(bool is_field) noexcept {
    const Token name = lexer_.take();
    if (name.kind != TokenKind::Identifier) {
        return make_unexpected(report(name, "an attribute needs a name"));
    }
    if (Status expected = expect_symbol(':', "an attribute needs a type"); !expected) {
        return expected;
    }
    const Token type_name = lexer_.take();
    const ValueType type = type_from_name(type_name.text);
    if (type == ValueType::Count) {
        return make_unexpected(report(type_name, "no such type"));
    }
    const Name symbol = Name::intern(name.text);
    auto value = is_field ? builder_->field(symbol, type) : builder_->attribute(symbol, type);
    if (!value) {
        return make_unexpected(value.error());
    }
    if (Status bound = bind(symbol, value.value()); !bound) {
        return bound;
    }
    return expect_symbol(';', "a declaration ends with a semicolon");
}

Status Parser::parse_annotations() noexcept {
    while (at_symbol('@')) {
        (void)lexer_.take();
        const Token name = lexer_.take();
        if (name.text == "microdetail") {
            pending_ = pending_ | NodeFlags::Microdetail;
        } else if (name.text == "base_reflectance") {
            pending_ = pending_ | NodeFlags::BaseReflectance;
        } else if (name.text == "opacity_critical") {
            pending_ = pending_ | NodeFlags::OpacityCritical;
        } else {
            return make_unexpected(report(name, "no such annotation"));
        }
    }
    return ok();
}

Status Parser::parse_let() noexcept {
    const Token name = lexer_.take();
    if (name.kind != TokenKind::Identifier) {
        return make_unexpected(report(name, "a binding needs a name"));
    }
    if (Status expected = expect_symbol('=', "a binding needs a value"); !expected) {
        return expected;
    }
    auto value = parse_expression();
    if (!value) {
        return make_unexpected(value.error());
    }
    if (Status annotated = builder_->annotate(value.value(), pending_); !annotated) {
        return annotated;
    }
    pending_ = NodeFlags::None;
    if (Status bound = bind(Name::intern(name.text), value.value()); !bound) {
        return bound;
    }
    return expect_symbol(';', "a statement ends with a semicolon");
}

Status Parser::parse_statement() noexcept {
    if (Status annotated = parse_annotations(); !annotated) {
        return annotated;
    }
    const Token keyword = lexer_.peek();
    if (keyword.kind != TokenKind::Identifier) {
        return make_unexpected(report(keyword, "expected a declaration or an output"));
    }
    (void)lexer_.take();
    if (keyword.text == "static") {
        const Token next = lexer_.take();
        if (next.text != "param") {
            return make_unexpected(report(next, "`static` introduces a parameter"));
        }
        return parse_parameter(true);
    }
    if (keyword.text == "param") {
        return parse_parameter(false);
    }
    if (keyword.text == "texture") {
        return parse_texture();
    }
    if (keyword.text == "attribute") {
        return parse_attribute(false);
    }
    if (keyword.text == "field") {
        return parse_attribute(true);
    }
    if (keyword.text == "let") {
        return parse_let();
    }
    if (keyword.text == "surface" || keyword.text == "opacity") {
        if (Status expected = expect_symbol('=', "an output is assigned"); !expected) {
            return expected;
        }
        auto value = parse_expression();
        if (!value) {
            return make_unexpected(value.error());
        }
        const bool surface = keyword.text == "surface";
        Status set =
            surface ? builder_->set_surface(value.value()) : builder_->set_opacity(value.value());
        if (!set) {
            return make_unexpected(report(keyword, surface ? "the surface output must be a closure"
                                                           : "the opacity output must be a float"));
        }
        return expect_symbol(';', "a statement ends with a semicolon");
    }
    return make_unexpected(report(keyword, "expected a declaration or an output"));
}

Expected<NodeId, Error> Parser::resolve(const Token& token) noexcept {
    const Name symbol = Name::intern(token.text);
    const NodeId bound = lookup(symbol);
    if (bound != kInvalidNode) {
        return bound;
    }
    auto parameter = builder_->parameter(symbol);
    if (!parameter) {
        return make_unexpected(report(token, "no parameter, binding or attribute of this name"));
    }
    return parameter;
}

Status Parser::parse_arguments(NodeId* out, u32 max, u32& count) noexcept {
    count = 0;
    if (Status expected = expect_symbol('(', "a call needs arguments"); !expected) {
        return expected;
    }
    while (!at_symbol(')')) {
        auto argument = parse_expression();
        if (!argument) {
            return make_unexpected(argument.error());
        }
        if (count >= max) {
            return make_unexpected(report(lexer_.peek(), "too many arguments"));
        }
        out[count++] = argument.value();
        if (at_symbol(',')) {
            (void)lexer_.take();
            continue;
        }
        break;
    }
    return expect_symbol(')', "a call's arguments end with a bracket");
}

Expected<NodeId, Error> Parser::parse_call(const Token& name) noexcept {
    if (name.text == "sample") {
        if (Status expected = expect_symbol('(', "sample takes a texture and a coordinate");
            !expected) {
            return make_unexpected(expected.error());
        }
        const Token texture = lexer_.take();
        if (Status expected = expect_symbol(',', "sample takes a texture and a coordinate");
            !expected) {
            return make_unexpected(expected.error());
        }
        auto coordinate = parse_expression();
        if (!coordinate) {
            return coordinate;
        }
        if (Status expected = expect_symbol(')', "a call's arguments end with a bracket");
            !expected) {
            return make_unexpected(expected.error());
        }
        auto sampled = builder_->texture_sample(Name::intern(texture.text), coordinate.value());
        if (!sampled) {
            return make_unexpected(report(texture, "no texture of this name is declared"));
        }
        return sampled;
    }
    if (name.text == "custom") {
        if (Status expected = expect_symbol('(', "custom takes Slang text and its arguments");
            !expected) {
            return make_unexpected(expected.error());
        }
        const Token code = lexer_.take();
        NodeId arguments[4] = {};
        u32 count = 0;
        while (at_symbol(',')) {
            (void)lexer_.take();
            auto argument = parse_expression();
            if (!argument) {
                return argument;
            }
            if (count >= 4) {
                return make_unexpected(report(code, "too many arguments"));
            }
            arguments[count++] = argument.value();
        }
        if (Status expected = expect_symbol(')', "a call's arguments end with a bracket");
            !expected) {
            return make_unexpected(expected.error());
        }
        if (count < 1) {
            return make_unexpected(
                report(code, "a custom node takes its Slang text and at least one argument"));
        }
        auto made =
            builder_->make(Op::Custom, builder_->node(arguments[0]).type, Name::intern(code.text),
                           Immediate{}, Span<const NodeId>(arguments, count));
        if (!made) {
            return make_unexpected(report(code, "this custom node could not be built"));
        }
        return made;
    }

    NodeId arguments[4] = {};
    u32 count = 0;
    if (Status parsed = parse_arguments(arguments, 4, count); !parsed) {
        return make_unexpected(parsed.error());
    }
    const Span<const NodeId> span(arguments, count);

    const ValueType vector = type_from_name(name.text);
    if (vector != ValueType::Count) {
        return builder_->make(Op::Combine, vector, Name{}, Immediate{}, span);
    }
    const Op closure = closure_from_name(name.text);
    if (closure != Op::Count) {
        return builder_->make(closure, ValueType::Closure, Name{}, Immediate{}, span);
    }
    const Op call = call_from_name(name.text);
    if (call != Op::Count) {
        return builder_->make(call, span);
    }
    return make_unexpected(report(name, "no such function"));
}

Expected<NodeId, Error> Parser::immediate_tuple(const Token& open) noexcept {
    NodeId components[4] = {};
    u32 count = 0;
    while (true) {
        auto component = parse_expression();
        if (!component) {
            return component;
        }
        if (count >= 4) {
            return make_unexpected(report(open, "a literal has at most four components"));
        }
        components[count++] = component.value();
        if (!at_symbol(',')) {
            break;
        }
        (void)lexer_.take();
    }
    if (Status expected = expect_symbol(')', "a bracket is not closed"); !expected) {
        return make_unexpected(expected.error());
    }
    if (count == 1) {
        return components[0];
    }
    // `(a, b, c)` is a vector. When every component is a literal the fold turns it into one
    // constant, which is what makes a graph's `float3` constant node and this notation one value.
    ValueType type = ValueType::Vec2;
    if (count == 3) {
        type = ValueType::Vec3;
    } else if (count == 4) {
        type = ValueType::Vec4;
    }
    auto combined = builder_->make(Op::Combine, type, Name{}, Immediate{},
                                   Span<const NodeId>(components, count));
    if (!combined) {
        return combined;
    }
    bool literal = true;
    f32 value[4] = {};
    for (u32 index = 0; index < count; ++index) {
        const Node& node = builder_->node(components[index]);
        literal = literal && node.op == Op::Constant;
        value[index] = node.value.x;
    }
    if (!literal) {
        return combined;
    }
    return builder_->constant(type, Immediate{value[0], value[1], value[2], value[3], 0});
}

Expected<NodeId, Error> Parser::parse_primary() noexcept {
    const Token token = lexer_.take();
    if (token.kind == TokenKind::Number) {
        return builder_->constant_float(static_cast<f32>(token.number));
    }
    if (token.kind == TokenKind::Symbol && token.symbol == '(') {
        return immediate_tuple(token);
    }
    if (token.kind == TokenKind::Identifier) {
        if (at_symbol('(')) {
            return parse_call(token);
        }
        if (token.text == "true" || token.text == "false") {
            return builder_->constant(ValueType::Bool,
                                      Immediate{0, 0, 0, 0, token.text == "true" ? 1U : 0U});
        }
        return resolve(token);
    }
    return make_unexpected(report(token, "expected a value"));
}

Expected<NodeId, Error> Parser::parse_postfix() noexcept {
    auto value = parse_primary();
    if (!value) {
        return value;
    }
    while (at_symbol('.')) {
        (void)lexer_.take();
        const Token swizzle = lexer_.take();
        u8 components[4] = {};
        u32 count = 0;
        for (const char character : swizzle.text) {
            const char* found = std::strchr("xyzw", character);
            if (found == nullptr || count >= 4) {
                return make_unexpected(report(swizzle, "a swizzle selects x, y, z or w"));
            }
            components[count++] = static_cast<u8>(found - "xyzw");
        }
        Immediate mask;
        mask.mask = Builder::swizzle_mask(Span<const u8>(components, count));
        const NodeId operand[] = {value.value()};
        value = builder_->make(Op::Swizzle, ValueType::Count, Name{}, mask,
                               Span<const NodeId>(operand, 1));
        if (!value) {
            return make_unexpected(report(swizzle, "this value cannot be swizzled"));
        }
    }
    return value;
}

Expected<NodeId, Error> Parser::combine(const Token& op, NodeId left, NodeId right) noexcept {
    const bool left_closure = builder_->node(left).type == ValueType::Closure;
    const bool right_closure = builder_->node(right).type == ValueType::Closure;
    NodeId operands[2] = {left, right};
    // `+` between closures is a closure sum, and `*` between a closure and a float is its weight.
    // The notation an author reaches for is the notation the closure model wants; nothing else in
    // the language changes meaning with its operands' types.
    if (op.symbol == '+' && left_closure && right_closure) {
        return builder_->make(Op::ClosureAdd, Span<const NodeId>(operands, 2));
    }
    if (op.symbol == '*' && (left_closure || right_closure)) {
        if (right_closure) {
            operands[0] = right;
            operands[1] = left;
        }
        return builder_->make(Op::ClosureScale, Span<const NodeId>(operands, 2));
    }
    if (left_closure || right_closure) {
        return make_unexpected(report(op, "a closure supports only `+` and a scalar `*`"));
    }
    Op arithmetic = Op::Add;
    switch (op.symbol) {
        case '-':
            arithmetic = Op::Sub;
            break;
        case '*':
            arithmetic = Op::Mul;
            break;
        case '/':
            arithmetic = Op::Div;
            break;
        default:
            break;
    }
    auto made = builder_->make(arithmetic, Span<const NodeId>(operands, 2));
    if (!made) {
        return make_unexpected(report(op, "these operand types do not combine"));
    }
    return made;
}

Expected<NodeId, Error> Parser::parse_term() noexcept {
    auto left = parse_postfix();
    if (!left) {
        return left;
    }
    while (at_symbol('*') || at_symbol('/')) {
        const Token op = lexer_.take();
        auto right = parse_postfix();
        if (!right) {
            return right;
        }
        left = combine(op, left.value(), right.value());
        if (!left) {
            return left;
        }
    }
    return left;
}

Expected<NodeId, Error> Parser::parse_expression() noexcept {
    auto left = parse_term();
    if (!left) {
        return left;
    }
    while (at_symbol('+') || at_symbol('-')) {
        const Token op = lexer_.take();
        auto right = parse_term();
        if (!right) {
            return right;
        }
        left = combine(op, left.value(), right.value());
        if (!left) {
            return left;
        }
    }
    return left;
}

Expected<Module, Error> Parser::run() noexcept {
    const Token keyword = lexer_.take();
    if (keyword.text != "material") {
        return make_unexpected(report(keyword, "a definition begins with `material`"));
    }
    const Token name = lexer_.take();
    if (name.kind != TokenKind::Identifier) {
        return make_unexpected(report(name, "a material needs a name"));
    }
    Builder builder(*allocator_, Name::intern(name.text));
    builder.set_policy(policy_);
    builder_ = &builder;
    // The builder is this function's, and `builder_` must not outlive it. Nothing reads it after
    // `run` returns, but a member pointing at a destroyed local is a dangling pointer whether or
    // not anybody dereferences it — and the static analyser is right to say so.
    struct Release {
        explicit Release(Parser* owner) noexcept : parser(owner) {}
        ~Release() { parser->builder_ = nullptr; }
        Release(const Release&) = delete;
        Release& operator=(const Release&) = delete;
        Parser* parser;
    };
    const Release release(this);
    if (Status expected = expect_symbol('{', "a material's body is braced"); !expected) {
        return make_unexpected(expected.error());
    }
    while (!at_symbol('}') && lexer_.peek().kind != TokenKind::End) {
        if (Status parsed = parse_statement(); !parsed) {
            return make_unexpected(parsed.error());
        }
    }
    if (Status expected = expect_symbol('}', "a material's body is braced"); !expected) {
        return make_unexpected(expected.error());
    }
    return builder.finish();
}

}  // namespace

Expected<Module, Error> parse_material(std::string_view source, Allocator& allocator,
                                       ParseDiagnostic& diagnostic,
                                       const PassSwitches& switches) noexcept {
    Parser parser(source, allocator, diagnostic, switches);
    return parser.run();
}

}  // namespace cy::rendering::material
