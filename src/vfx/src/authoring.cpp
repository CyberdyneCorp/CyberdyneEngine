// SPDX-License-Identifier: MIT
#include <cy/vfx/authoring.h>

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

namespace cy::vfx {
namespace {

constexpr u32 kMaxItems = 4096;

[[nodiscard]] Error malformed(const char* reason) noexcept {
    return {ErrorCode::InvalidArgument, reason, 0};
}

[[nodiscard]] Name intern(std::string_view value) noexcept {
    return Name::intern(std::string(value));
}

[[nodiscard]] bool identifier(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    for (const char character : value) {
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '_')) {
            return false;
        }
    }
    return true;
}

class Reader {
public:
    explicit Reader(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool done() const noexcept { return cursor_ == bytes_.size(); }

    [[nodiscard]] Expected<u8, Error> byte() noexcept {
        if (cursor_ == bytes_.size()) {
            return make_unexpected(malformed("truncated VFX document"));
        }
        return bytes_[cursor_++];
    }

    [[nodiscard]] Expected<u32, Error> word() noexcept {
        if (bytes_.size() - cursor_ < 4) {
            return make_unexpected(malformed("truncated VFX document"));
        }
        u32 value = 0;
        for (u32 index = 0; index < 4; ++index) {
            value |= static_cast<u32>(bytes_[cursor_++]) << (index * 8);
        }
        return value;
    }

    [[nodiscard]] Expected<u32, Error> count() noexcept {
        auto value = word();
        if (!value || *value > kMaxItems) {
            return make_unexpected(malformed("too many VFX document entries"));
        }
        return *value;
    }

    [[nodiscard]] Expected<std::string_view, Error> text() noexcept {
        auto length = word();
        if (!length) {
            return make_unexpected(length.error());
        }
        if (*length > bytes_.size() - cursor_) {
            return make_unexpected(malformed("truncated VFX document text"));
        }
        const std::string_view value(reinterpret_cast<const char*>(bytes_.data() + cursor_),
                                     *length);
        cursor_ += *length;
        return value;
    }

    [[nodiscard]] Expected<Name, Error> name() noexcept {
        auto value = text();
        if (!value || !identifier(*value)) {
            return make_unexpected(malformed("invalid VFX identifier"));
        }
        return intern(*value);
    }

    [[nodiscard]] Expected<f32, Error> number() noexcept {
        auto bits = word();
        if (!bits) {
            return make_unexpected(bits.error());
        }
        f32 value = 0;
        std::memcpy(&value, &*bits, sizeof(value));
        if (!std::isfinite(value)) {
            return make_unexpected(malformed("non-finite VFX numeric value"));
        }
        return value;
    }

private:
    Span<const u8> bytes_;
    usize cursor_ = 0;
};

[[nodiscard]] std::string_view take(std::string_view& rest) noexcept {
    const usize start = rest.find_first_not_of(" \t");
    if (start == std::string_view::npos) {
        rest = {};
        return {};
    }
    rest.remove_prefix(start);
    const usize end = rest.find_first_of(" \t");
    const std::string_view result = rest.substr(0, end);
    rest.remove_prefix(end == std::string_view::npos ? rest.size() : end);
    return result;
}

[[nodiscard]] Expected<u64, Error> unsigned_number(std::string_view token) noexcept {
    u64 value = 0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (token.empty() || result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return make_unexpected(malformed("invalid VFX canvas integer"));
    }
    return value;
}

[[nodiscard]] Expected<f32, Error> canvas_number(std::string_view token) noexcept {
    const std::string held(token);
    char* end = nullptr;
    const f32 value = std::strtof(held.c_str(), &end);
    if (token.empty() || end != held.c_str() + held.size() || !std::isfinite(value)) {
        return make_unexpected(malformed("invalid VFX canvas number"));
    }
    return value;
}

[[nodiscard]] Status set_canvas_property(Graph& graph, graph::NodeKey key,
                                         std::string_view property,
                                         std::string_view value) noexcept {
    graph::Literal literal;
    if (property == "value") {
        f32 components[4] = {};
        u32 count = 0;
        while (!value.empty()) {
            if (count == 4) {
                return make_unexpected(malformed("too many VFX constant components"));
            }
            auto number = canvas_number(take(value));
            if (!number) {
                return make_unexpected(number.error());
            }
            components[count++] = *number;
        }
        if (count == 0) {
            return make_unexpected(malformed("empty VFX constant"));
        }
        constexpr const char* types[] = {"", "float", "vec2", "vec3", "vec4"};
        literal.type = Name::intern(types[count]);
        literal.value = graph::Immediate{components[0], components[1], components[2],
                                         components[3], 0};
    } else {
        const std::string_view name = take(value);
        if (!identifier(name) || !take(value).empty()) {
            return make_unexpected(malformed("invalid VFX canvas property"));
        }
        literal.type = Name::intern("name");
        literal.text = intern(name);
    }
    return graph.set_property(key, intern(property), literal);
}

[[nodiscard]] Status read_layout(Graph& graph, std::string_view line) noexcept {
    auto key = unsigned_number(take(line));
    auto x = canvas_number(take(line));
    auto y = canvas_number(take(line));
    if (!key || !x || !y || *key == 0 || !take(line).empty()) {
        return make_unexpected(malformed("invalid VFX node layout"));
    }
    return graph.set_layout({*key, *x, *y, 0, {}});
}

[[nodiscard]] Status read_node(Graph& graph, std::string_view line) noexcept {
    auto key = unsigned_number(take(line));
    const std::string_view type = take(line);
    if (!key || *key == 0 || type.empty() || !take(line).empty()) {
        return make_unexpected(malformed("invalid VFX canvas node"));
    }
    return graph.add_node(*key, intern(type));
}

[[nodiscard]] Status read_property(Graph& graph, std::string_view line) noexcept {
    auto key = unsigned_number(take(line));
    const std::string_view property = take(line);
    if (!key || !identifier(property)) {
        return make_unexpected(malformed("invalid VFX canvas property"));
    }
    return set_canvas_property(graph, *key, property, line);
}

[[nodiscard]] Status read_link(Graph& graph, std::string_view line) noexcept {
    auto from = unsigned_number(take(line));
    const std::string_view from_pin = take(line);
    auto to = unsigned_number(take(line));
    const std::string_view to_pin = take(line);
    if (!from || !to || !identifier(from_pin) || !identifier(to_pin) || !take(line).empty()) {
        return make_unexpected(malformed("invalid VFX canvas link"));
    }
    return graph.connect(*from, intern(from_pin), *to, intern(to_pin));
}

[[nodiscard]] Status read_canvas_statement(Graph& graph, std::string_view line) noexcept {
    const std::string_view keyword = take(line);
    if (keyword.empty()) {
        return ok();
    }
    if (keyword == "#") {
        return take(line) == "layout" ? read_layout(graph, line) : ok();
    }
    if (keyword == "node") {
        return read_node(graph, line);
    }
    if (keyword == "prop") {
        return read_property(graph, line);
    }
    if (keyword == "link") {
        return read_link(graph, line);
    }
    return make_unexpected(malformed("unknown VFX canvas statement"));
}

[[nodiscard]] Expected<Graph, Error> read_stage_canvas(std::string_view source, Name owner,
                                                       Stage stage, Allocator& allocator,
                                                       std::string_view owner_kind) noexcept {
    Graph graph(allocator, Name::intern(stage_name(stage)));
    graph.grant(graph::Capability::ReadWorld | graph::Capability::Randomness);
    if (!source.starts_with("cyvfxcanvas 1\n")) {
        return make_unexpected(malformed("unsupported VFX stage canvas"));
    }
    source.remove_prefix(sizeof("cyvfxcanvas 1\n") - 1);
    if (!source.starts_with(owner_kind) || source.size() <= owner_kind.size() ||
        source[owner_kind.size()] != ' ') {
        return make_unexpected(malformed("VFX stage canvas has no owner"));
    }
    const usize first_newline = source.find('\n');
    if (first_newline == std::string_view::npos ||
        source.substr(owner_kind.size() + 1, first_newline - (owner_kind.size() + 1)) !=
            owner.text()) {
        return make_unexpected(malformed("VFX stage canvas belongs to another asset"));
    }
    source.remove_prefix(first_newline + 1);
    while (!source.empty()) {
        const usize newline = source.find('\n');
        const std::string_view line = source.substr(0, newline);
        source.remove_prefix(newline == std::string_view::npos ? source.size() : newline + 1);
        if (Status parsed = read_canvas_statement(graph, line); !parsed) {
            return make_unexpected(parsed.error());
        }
    }
    return graph;
}

[[nodiscard]] Expected<Precision, Error> precision_of(std::string_view name) noexcept {
    constexpr const char* names[] = {"Auto", "Float32", "Float16", "Unorm8", "Snorm16"};
    for (u32 index = 0; index < 5; ++index) {
        if (name == names[index]) {
            return static_cast<Precision>(index);
        }
    }
    return make_unexpected(malformed("unknown VFX attribute precision"));
}

[[nodiscard]] Status read_attributes(Reader& reader, Emitter& emitter) noexcept {
    auto capacity = reader.word();
    auto count = reader.count();
    if (!capacity || *capacity == 0 || !count) {
        return make_unexpected(malformed("invalid VFX emitter capacity or attributes"));
    }
    emitter.set_capacity(*capacity);
    for (u32 index = 0; index < *count; ++index) {
        auto name = reader.name();
        auto type = reader.name();
        auto minimum = reader.number();
        auto maximum = reader.number();
        auto tolerance = reader.number();
        auto precision_name = reader.text();
        if (!name || !type || !minimum || !maximum || !tolerance || !precision_name ||
            *minimum > *maximum || *tolerance < 0) {
            return make_unexpected(malformed("invalid VFX attribute declaration"));
        }
        auto precision = precision_of(*precision_name);
        if (!precision) {
            return make_unexpected(precision.error());
        }
        if (Status declared = emitter.declare_attribute(
                {*name, *type, *minimum, *maximum, *tolerance, *precision});
            !declared) {
            return declared;
        }
    }
    return ok();
}

[[nodiscard]] Status read_emitter(Reader& reader, u32 version, VfxSystemAsset& asset,
                                  Allocator& allocator) noexcept {
    auto name = reader.name();
    auto path = reader.byte();
    auto renderer_name = reader.name();
    auto stage_count = reader.count();
    if (!name || !path || *path > 1 || !renderer_name || !stage_count) {
        return make_unexpected(malformed("invalid VFX emitter header"));
    }
    Emitter emitter(allocator, *name);
    emitter.set_path(static_cast<SimulationPath>(*path));
    bool found_renderer = false;
    for (u32 index = 0; index < kAssetRendererCount; ++index) {
        if (renderer_name->text() == asset_renderer_name(static_cast<u8>(index))) {
            emitter.set_renderer(static_cast<u8>(index));
            found_renderer = true;
            break;
        }
    }
    if (!found_renderer) {
        return make_unexpected(malformed("unknown VFX renderer"));
    }
    for (u32 index = 0; index < *stage_count; ++index) {
        auto stage_id = reader.byte();
        auto canvas = reader.text();
        if (!stage_id || *stage_id >= static_cast<u8>(Stage::Count) || !canvas ||
            emitter.has_stage(static_cast<Stage>(*stage_id))) {
            return make_unexpected(malformed("invalid or duplicate VFX stage"));
        }
        const Stage stage = static_cast<Stage>(*stage_id);
        auto graph = read_stage_canvas(*canvas, *name, stage, allocator, "emitter");
        if (!graph) {
            return make_unexpected(graph.error());
        }
        if (Status set = emitter.set_stage(stage, std::move(*graph)); !set) {
            return set;
        }
    }
    auto modules = reader.count();
    if (!modules) {
        return make_unexpected(modules.error());
    }
    for (u32 index = 0; index < *modules; ++index) {
        auto module = reader.name();
        if (!module) {
            return make_unexpected(module.error());
        }
        return make_unexpected(Error{ErrorCode::Unsupported,
                                     "VFX module asset resolution is not available", 0});
    }
    auto interfaces = reader.count();
    if (!interfaces) {
        return make_unexpected(interfaces.error());
    }
    for (u32 index = 0; index < *interfaces; ++index) {
        auto binding = reader.name();
        if (!binding) {
            return make_unexpected(binding.error());
        }
        if (Status bound = emitter.bind_interface(*binding); !bound) {
            return bound;
        }
    }
    if (version >= 2) {
        if (Status attributes = read_attributes(reader, emitter); !attributes) {
            return attributes;
        }
    }
    return asset.add_emitter(std::move(emitter));
}

[[nodiscard]] Status read_parameters(Reader& reader, VfxSystemAsset& asset) noexcept {
    auto count = reader.count();
    if (!count) {
        return make_unexpected(count.error());
    }
    for (u32 index = 0; index < *count; ++index) {
        ParameterDecl parameter;
        auto name = reader.name();
        auto type = reader.name();
        if (!name || !type) {
            return make_unexpected(malformed("invalid VFX parameter declaration"));
        }
        parameter.name = *name;
        parameter.type = *type;
        for (f32& value : parameter.value) {
            auto component = reader.number();
            if (!component) {
                return make_unexpected(component.error());
            }
            value = *component;
        }
        auto exposed = reader.byte();
        if (!exposed || *exposed > 1) {
            return make_unexpected(malformed("invalid VFX parameter exposure"));
        }
        parameter.exposed = *exposed != 0;
        if (Status declared = asset.declare_parameter(parameter); !declared) {
            return declared;
        }
    }
    return ok();
}

[[nodiscard]] Status read_channels(Reader& reader, VfxSystemAsset& asset) noexcept {
    auto count = reader.count();
    if (!count) {
        return make_unexpected(count.error());
    }
    for (u32 index = 0; index < *count; ++index) {
        auto name = reader.name();
        auto events = reader.word();
        auto depth = reader.word();
        auto readback = reader.byte();
        if (!name || !events || *events == 0 || !depth || *depth == 0 || !readback ||
            *readback > 1) {
            return make_unexpected(malformed("invalid VFX event channel"));
        }
        if (Status declared = asset.declare_channel({*name, *events, *depth, *readback != 0});
            !declared) {
            return declared;
        }
    }
    return ok();
}

[[nodiscard]] Expected<Array<u8>, Error> decode_hex(std::string_view source,
                                                    std::string_view header,
                                                    Allocator& allocator) noexcept {
    if (!source.starts_with(header) || source.size() <= header.size() ||
        (source.size() - header.size()) % 2 != 0) {
        return make_unexpected(malformed("unsupported VFX document envelope"));
    }
    source.remove_prefix(header.size());
    Array<u8> bytes(allocator);
    for (usize index = 0; index < source.size(); index += 2) {
        u32 value = 0;
        const auto result = std::from_chars(source.data() + index, source.data() + index + 2,
                                            value, 16);
        if (result.ec != std::errc{} || result.ptr != source.data() + index + 2) {
            return make_unexpected(malformed("invalid VFX document hexadecimal payload"));
        }
        if (Status pushed = bytes.push_back(static_cast<u8>(value)); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    return bytes;
}

}  // namespace

Expected<VfxSystemAsset, Error> read_authoring_document(std::string_view source,
                                                          Allocator& allocator) noexcept {
    auto bytes = decode_hex(source, "cyvfxdoc 1\n", allocator);
    if (!bytes) {
        return make_unexpected(bytes.error());
    }
    Reader reader(bytes->span());
    auto version = reader.word();
    auto name = reader.name();
    auto count = reader.count();
    if (!version || (*version != 1 && *version != 2) || !name || !count) {
        return make_unexpected(malformed("unsupported VFX document payload"));
    }
    VfxSystemAsset asset(allocator, *name);
    for (u32 index = 0; index < *count; ++index) {
        if (Status emitter = read_emitter(reader, *version, asset, allocator); !emitter) {
            return make_unexpected(emitter.error());
        }
    }
    if (Status parameters = read_parameters(reader, asset); !parameters) {
        return make_unexpected(parameters.error());
    }
    if (*version >= 2) {
        if (Status channels = read_channels(reader, asset); !channels) {
            return make_unexpected(channels.error());
        }
    }
    if (!reader.done()) {
        return make_unexpected(malformed("trailing VFX document bytes"));
    }
    return asset;
}

Expected<VfxModuleAsset, Error> read_authoring_module(std::string_view source,
                                                      Allocator& allocator) noexcept {
    auto bytes = decode_hex(source, "cyvfxmodule 1\n", allocator);
    if (!bytes) {
        return make_unexpected(bytes.error());
    }
    Reader reader(bytes->span());
    auto version = reader.word();
    auto name = reader.name();
    auto stage_id = reader.byte();
    auto input_count = reader.count();
    if (!version || *version != 1 || !name || !stage_id ||
        *stage_id >= static_cast<u8>(Stage::Count) || !input_count) {
        return make_unexpected(malformed("invalid VFX module header"));
    }
    Array<ModuleInputDecl> inputs(allocator);
    for (u32 index = 0; index < *input_count; ++index) {
        auto input_name = reader.name();
        auto type = reader.name();
        if (!input_name || !type) {
            return make_unexpected(malformed("invalid VFX module input"));
        }
        const std::string_view kind = type->text();
        if (kind != "float" && kind != "vec2" && kind != "vec3" && kind != "vec4" &&
            kind != "int" && kind != "bool") {
            return make_unexpected(malformed("unsupported VFX module input type"));
        }
        for (const ModuleInputDecl& prior : inputs) {
            if (prior.name == *input_name) {
                return make_unexpected(malformed("duplicate VFX module input"));
            }
        }
        if (Status pushed = inputs.push_back({*input_name, *type}); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    auto dependency_count = reader.count();
    if (!dependency_count) {
        return make_unexpected(dependency_count.error());
    }
    Array<Name> dependencies(allocator);
    for (u32 index = 0; index < *dependency_count; ++index) {
        auto dependency = reader.name();
        if (!dependency || *dependency == *name) {
            return make_unexpected(malformed("invalid or self-referencing VFX module dependency"));
        }
        for (Name prior : dependencies) {
            if (prior == *dependency) {
                return make_unexpected(malformed("duplicate VFX module dependency"));
            }
        }
        if (Status pushed = dependencies.push_back(*dependency); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    auto canvas = reader.text();
    if (!canvas || !reader.done()) {
        return make_unexpected(malformed("invalid VFX module canvas or trailing bytes"));
    }
    const Stage stage = static_cast<Stage>(*stage_id);
    auto graph = read_stage_canvas(*canvas, *name, stage, allocator, "module");
    if (!graph) {
        return make_unexpected(graph.error());
    }
    VfxModuleAsset module(allocator, *name, stage, std::move(*graph));
    module.inputs = std::move(inputs);
    module.dependencies = std::move(dependencies);
    return module;
}

}  // namespace cy::vfx
