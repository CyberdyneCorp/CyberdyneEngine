// SPDX-License-Identifier: MIT
#include <cy/graph/material/canvas.h>

#include <cy/rendering/material/ir.h>

#include <cstdlib>
#include <string>

namespace cy::graph::material {
namespace {

[[nodiscard]] std::string_view take(std::string_view& rest) noexcept {
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
        rest.remove_prefix(1);
    }
    const usize end = rest.find_first_of(" \t");
    const std::string_view token = rest.substr(0, end);
    rest.remove_prefix(end == std::string_view::npos ? rest.size() : end);
    return token;
}

[[nodiscard]] f32 to_float(std::string_view text) noexcept {
    const std::string held(text);
    return static_cast<f32>(std::strtod(held.c_str(), nullptr));
}

[[nodiscard]] u64 to_unsigned(std::string_view text) noexcept {
    const std::string held(text);
    return std::strtoull(held.c_str(), nullptr, 10);
}

[[nodiscard]] Expected<u32, Error> swizzle_mask_of(std::string_view letters) noexcept {
    u8 components[4] = {};
    if (letters.empty() || letters.size() > 4) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a swizzle is one to four of x, y, z, w", 0});
    }
    for (usize index = 0; index < letters.size(); ++index) {
        switch (letters[index]) {
            case 'x':
                components[index] = 0;
                break;
            case 'y':
                components[index] = 1;
                break;
            case 'z':
                components[index] = 2;
                break;
            case 'w':
                components[index] = 3;
                break;
            default:
                return make_unexpected(Error{ErrorCode::InvalidArgument,
                                             "a swizzle component that is not x, y, z or w", 0});
        }
    }
    return rendering::material::Builder::swizzle_mask(Span<const u8>(components, letters.size()));
}

[[nodiscard]] Status set_property(Graph& out, NodeKey key, std::string_view name,
                                  std::string_view rest) noexcept {
    Literal literal;
    if (name == "symbol" || name == "type" || name == "texture") {
        literal.type = Name::intern("name");
        literal.text = Name::intern(std::string(take(rest)));
        return out.set_property(key, Name::intern(std::string(name)), literal);
    }
    if (name == "swizzle") {
        auto mask = swizzle_mask_of(take(rest));
        if (!mask) {
            return make_unexpected(mask.error());
        }
        literal.type = Name::intern("swizzle");
        literal.value.mask = mask.value();
        return out.set_property(key, Name::intern("value"), literal);
    }
    if (name == "value" || name == "default" || name == "average") {
        literal.type = Name::intern("vec4");
        literal.value.x = to_float(take(rest));
        literal.value.y = to_float(take(rest));
        literal.value.z = to_float(take(rest));
        literal.value.w = to_float(take(rest));
        return out.set_property(key, Name::intern(std::string(name)), literal);
    }
    literal.type = Name::intern("bool");
    literal.value.mask = take(rest) == "true" ? 1U : 0U;
    return out.set_property(key, Name::intern(std::string(name)), literal);
}

}  // namespace

Expected<AuthoredCanvas, Error> read_canvas(std::string_view text, Graph& out) noexcept {
    AuthoredCanvas authored;
    usize line_number = 0;
    bool saw_header = false;
    std::string_view rest = text;
    while (!rest.empty()) {
        const usize newline = rest.find('\n');
        std::string_view line = rest.substr(0, newline);
        rest.remove_prefix(newline == std::string_view::npos ? rest.size() : newline + 1);
        ++line_number;
        const std::string_view keyword = take(line);
        if (keyword.empty() || keyword.front() == '#') {
            continue;
        }
        if (keyword == "cymatcanvas") {
            if (to_unsigned(take(line)) != kCanvasVersion) {
                return make_unexpected(
                    Error{ErrorCode::Unsupported,
                          "an interchange written at a version this build does not read",
                          static_cast<i64>(line_number)});
            }
            saw_header = true;
            continue;
        }
        if (!saw_header) {
            return make_unexpected(Error{ErrorCode::InvalidArgument,
                                         "the first line is not `cymatcanvas <version>`",
                                         static_cast<i64>(line_number)});
        }
        if (keyword == "material") {
            authored.name = Name::intern(std::string(take(line)));
            out.set_name(authored.name);
        } else if (keyword == "node") {
            const auto key = static_cast<NodeKey>(to_unsigned(take(line)));
            if (Status added = out.add_node(key, Name::intern(std::string(take(line)))); !added) {
                return make_unexpected(added.error());
            }
            ++authored.nodes;
        } else if (keyword == "prop") {
            const auto key = static_cast<NodeKey>(to_unsigned(take(line)));
            const std::string_view property = take(line);
            if (Status set = set_property(out, key, property, line); !set) {
                return make_unexpected(set.error());
            }
        } else if (keyword == "link") {
            const auto from = static_cast<NodeKey>(to_unsigned(take(line)));
            const std::string from_pin(take(line));
            const auto to = static_cast<NodeKey>(to_unsigned(take(line)));
            const std::string to_pin(take(line));
            if (Status wired = out.connect(from, Name::intern(from_pin), to, Name::intern(to_pin));
                !wired) {
                return make_unexpected(wired.error());
            }
            ++authored.links;
        } else {
            return make_unexpected(Error{ErrorCode::InvalidArgument,
                                         "a keyword the interchange does not define",
                                         static_cast<i64>(line_number)});
        }
    }
    if (authored.name.is_empty()) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "the interchange names no material", 0});
    }
    return authored;
}

}  // namespace cy::graph::material
