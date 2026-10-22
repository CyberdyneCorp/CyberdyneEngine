// SPDX-License-Identifier: MIT
// map_source.cpp — the `.cymap` reader. See map_source.h.

#include "map_source.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

namespace sample::rts::map {
namespace {

using cy::Status;
using cy::terrain::BrushOp;
using cy::terrain::ModifierKind;

/// One statement's words, quotes removed.
class Words {
public:
    explicit Words(std::string_view line) {
        std::size_t at = 0;
        while (at < line.size()) {
            while (at < line.size() && (line[at] == ' ' || line[at] == '\t' || line[at] == '\r')) {
                ++at;
            }
            if (at >= line.size() || line[at] == '#') {
                break;
            }
            if (line[at] == '"') {
                const std::size_t close = line.find('"', at + 1);
                if (close == std::string_view::npos) {
                    error_ = "a quoted name is not closed";
                    return;
                }
                words_.emplace_back(line.substr(at + 1, close - at - 1));
                at = close + 1;
                continue;
            }
            std::size_t end = at;
            while (end < line.size() && line[end] != ' ' && line[end] != '\t' &&
                   line[end] != '\r') {
                ++end;
            }
            words_.emplace_back(line.substr(at, end - at));
            at = end;
        }
    }

    [[nodiscard]] bool empty() const noexcept { return words_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return words_.size(); }
    [[nodiscard]] const std::string& operator[](std::size_t index) const { return words_[index]; }
    [[nodiscard]] const char* error() const noexcept { return error_; }

    /// Word `index` as a number. Sets the error and returns zero when it is not one.
    f32 number(std::size_t index) {
        if (index >= words_.size()) {
            error_ = "a statement is missing a number";
            return 0.0F;
        }
        const char* text = words_[index].c_str();
        char* end = nullptr;
        errno = 0;
        const double value = std::strtod(text, &end);
        if (end == text || *end != '\0' || errno != 0) {
            error_ = "a word that should be a number is not one";
            return 0.0F;
        }
        return static_cast<f32>(value);
    }
    u64 integer(std::size_t index) {
        if (index >= words_.size()) {
            error_ = "a statement is missing a number";
            return 0;
        }
        const char* text = words_[index].c_str();
        char* end = nullptr;
        errno = 0;
        const unsigned long long value = std::strtoull(text, &end, 0);
        if (end == text || *end != '\0' || errno != 0) {
            error_ = "a word that should be a whole number is not one";
            return 0;
        }
        return value;
    }
    Vec3 vec3(std::size_t index) {
        const f32 x = number(index);
        const f32 y = number(index + 1);
        return Vec3{x, y, number(index + 2)};
    }
    /// Fail unless there are exactly `count` words.
    void expect(std::size_t count) {
        if (words_.size() != count && error_ == nullptr) {
            error_ = "a statement has the wrong number of words";
        }
    }
    void fail(const char* why) {
        if (error_ == nullptr) {
            error_ = why;
        }
    }
    /// The points after a ":" from word `from` on: x z pairs.
    std::vector<Vec2> points(std::size_t from) {
        std::vector<Vec2> out;
        if (from >= words_.size() || words_[from] != ":") {
            fail("a stroke's points must follow a ':'");
            return out;
        }
        if ((words_.size() - from - 1U) % 2U != 0U || words_.size() - from - 1U < 2U) {
            fail("a stroke's points are not x z pairs");
            return out;
        }
        for (std::size_t at = from + 1; at + 1 < words_.size(); at += 2) {
            const f32 x = number(at);
            out.push_back(Vec2{x, number(at + 1)});
        }
        return out;
    }

private:
    std::vector<std::string> words_;
    const char* error_ = nullptr;
};

// --- Statements ----------------------------------------------------------------------------------

using Handler = void (*)(Words& words, MapSource& out);

void on_map(Words& words, MapSource& out) {
    words.expect(2);
    out.name = words.size() > 1 ? words[1] : std::string{};
}

void on_seed(Words& words, MapSource& out) {
    words.expect(2);
    out.seed = words.integer(1);
}

void on_extent(Words& words, MapSource& out) {
    words.expect(5);
    out.origin_x = words.number(1);
    out.origin_z = words.number(2);
    out.size = words.number(3);
    out.tiles = static_cast<u32>(words.integer(4));
    if (!(out.size > 0.0F) || out.tiles == 0 || out.tiles > cy::terrain::kMaxRegionTiles) {
        words.fail("the extent's size or tile count is out of range");
    }
}

void on_generator(Words& words, MapSource& out) {
    words.expect(5);
    out.generator.period = words.number(1);
    out.generator.amplitude = words.number(2);
    out.generator.octaves = static_cast<u32>(words.integer(3));
    out.generator.base_height = words.number(4);
}

void on_water(Words& words, MapSource& out) {
    words.expect(2);
    out.water_level = words.number(1);
}

void on_sun(Words& words, MapSource& out) {
    words.expect(5);
    out.sun_travel = words.vec3(1);
    out.sun_lux = words.number(4);
}

void on_base(Words& words, MapSource& out) {
    words.expect(5);
    for (std::size_t index = 0; index < 4; ++index) {
        out.base[index] = words.number(index + 1);
    }
}

void on_company(Words& words, MapSource& out) {
    words.expect(3);
    out.company[0] = words.number(1);
    out.company[1] = words.number(2);
}

void on_texture(Words& words, MapSource& out) {
    words.expect(3);
    if (out.textures.size() >= kMaxContentTextures) {
        words.fail("too many textures");
        return;
    }
    out.textures.push_back(SourceTexture{words[1], words[2]});
}

void on_layer(Words& words, MapSource& out) {
    words.expect(9);
    SourceLayer layer;
    layer.index = static_cast<u8>(words.integer(1));
    layer.name = words.size() > 2 ? words[2] : std::string{};
    layer.colour = words.vec3(3);
    layer.detail = words.size() > 6 ? words[6] : std::string{};
    layer.metres = words.number(7);
    layer.contrast = words.number(8);
    if (layer.index != out.layers.size()) {
        words.fail("layers must be declared in index order from zero");
    }
    out.layers.push_back(layer);
}

[[nodiscard]] bool layer_known(const MapSource& out, u64 layer) noexcept {
    return layer < out.layers.size();
}

void on_slope_layer(Words& words, MapSource& out) {
    words.expect(4);
    const u64 layer = words.integer(1);
    if (!layer_known(out, layer)) {
        words.fail("the slope layer is not declared");
    }
    out.slope_layer = static_cast<u8>(layer);
    out.slope_from = words.number(2);
    out.slope_full = words.number(3);
}

void on_shore_layer(Words& words, MapSource& out) {
    words.expect(3);
    const u64 layer = words.integer(1);
    if (!layer_known(out, layer)) {
        words.fail("the shore layer is not declared");
    }
    out.shore_layer = static_cast<u8>(layer);
    out.shore_metres = words.number(2);
}

void on_noise(Words& words, MapSource& out) {
    words.expect(4);
    SourceModifier modifier;
    modifier.kind = ModifierKind::Noise;
    modifier.numbers[0] = words.number(1);
    modifier.numbers[1] = words.number(2);
    modifier.numbers[2] = words.number(3);
    modifier.count = 3;
    out.stack.push_back(modifier);
}

void on_mound(Words& words, MapSource& out) {
    words.expect(5);
    SourceModifier modifier;
    modifier.kind = ModifierKind::Crater;
    for (std::size_t index = 0; index < 4; ++index) {
        modifier.numbers[index] = words.number(index + 1);
    }
    modifier.count = 4;
    if (!(modifier.numbers[2] > 0.0F)) {
        words.fail("a mound's radius must be positive");
    }
    out.stack.push_back(modifier);
}

void on_flatten(Words& words, MapSource& out) {
    words.expect(6);
    SourceModifier modifier;
    modifier.kind = ModifierKind::Flatten;
    for (std::size_t index = 0; index < 5; ++index) {
        modifier.numbers[index] = words.number(index + 1);
    }
    modifier.count = 5;
    if (!(modifier.numbers[2] > modifier.numbers[0]) ||
        !(modifier.numbers[3] > modifier.numbers[1])) {
        words.fail("a flatten's box is empty");
    }
    out.stack.push_back(modifier);
}

void on_road(Words& words, MapSource& out) {
    SourceModifier modifier;
    modifier.kind = ModifierKind::RoadSpline;
    modifier.numbers[0] = words.number(1);
    const u64 layer = words.integer(2);
    if (!layer_known(out, layer)) {
        words.fail("a road's layer is not declared");
    }
    modifier.layer = static_cast<u8>(layer);
    modifier.points = words.points(3);
    out.stack.push_back(modifier);
}

/// `brush <op> <radius> <strength> <falloff> [layer n | height h | passes n] : <x z>...`
void on_brush(Words& words, MapSource& out) {
    if (words.size() < 2) {
        words.fail("a brush names no operation");
        return;
    }
    static constexpr struct {
        const char* name;
        BrushOp op;
        const char* argument;
    } kOps[] = {
        {"raise", BrushOp::Raise, nullptr},    {"lower", BrushOp::Lower, nullptr},
        {"smooth", BrushOp::Smooth, "passes"}, {"flatten", BrushOp::Flatten, "height"},
        {"paint", BrushOp::Paint, "layer"},
    };
    SourceModifier modifier;
    modifier.kind = ModifierKind::Brush;
    const char* argument = nullptr;
    bool found = false;
    for (const auto& op : kOps) {
        if (words[1] == op.name) {
            modifier.brush = op.op;
            argument = op.argument;
            found = true;
        }
    }
    if (!found) {
        words.fail("a brush's operation is not one the map supports");
        return;
    }
    modifier.numbers[0] = words.number(2);
    modifier.numbers[1] = words.number(3);
    modifier.numbers[2] = words.number(4);
    std::size_t next = 5;
    if (argument != nullptr) {
        if (words.size() <= next || words[next] != argument) {
            words.fail("a brush is missing its operation's argument");
            return;
        }
        modifier.numbers[3] = words.number(next + 1);
        next += 2;
    }
    modifier.count = 4;
    if (modifier.brush == BrushOp::Paint) {
        const auto layer = static_cast<u64>(modifier.numbers[3]);
        if (!layer_known(out, layer)) {
            words.fail("a paint stroke's layer is not declared");
        }
        modifier.layer = static_cast<u8>(layer);
    }
    modifier.points = words.points(next);
    out.stack.push_back(modifier);
}

void on_material(Words& words, MapSource& out) {
    if (words.size() < 7) {
        words.fail("a material needs a name, a colour, a roughness and a metallic");
        return;
    }
    MapMaterial material;
    material.name = words[1];
    material.base_colour = words.vec3(2);
    material.roughness = words.number(5);
    material.metallic = words.number(6);
    std::size_t at = 7;
    while (at < words.size()) {
        if (words[at] == "texture" && at + 1 < words.size()) {
            const std::string& name = words[at + 1];
            material.texture = kNoTexture;
            if (name == "albedo") {
                material.texture = kAlbedoTexture;
            }
            for (std::size_t index = 0; index < out.textures.size(); ++index) {
                if (out.textures[index].name == name) {
                    material.texture = static_cast<u32>(index);
                }
            }
            if (material.texture == kNoTexture) {
                words.fail("a material names a texture that is not declared");
            }
            at += 2;
        } else if (words[at] == "emission") {
            material.emission = words.vec3(at + 1);
            at += 4;
        } else if (words[at] == "transparent") {
            material.blend = Blend::Transparent;
            at += 1;
        } else {
            words.fail("a material has a word it does not know");
            return;
        }
    }
    if (out.materials.size() >= kMaxMaterials) {
        words.fail("too many materials");
        return;
    }
    out.materials.push_back(material);
}

void on_species(Words& words, MapSource& out) {
    words.expect(13);
    SourceSpecies species;
    species.name = words.size() > 1 ? words[1] : std::string{};
    species.mesh = words.size() > 2 ? words[2] : std::string{};
    species.klass = words.size() > 3 ? words[3] : std::string{};
    species.scale_min = words.number(4);
    species.scale_max = words.number(5);
    species.footprint = words.number(6);
    species.tint_a = words.vec3(7);
    species.tint_b = words.vec3(10);
    if (species.klass != "canopy" && species.klass != "understory" && species.klass != "scatter") {
        words.fail("a species' class is not canopy, understory or scatter");
    }
    if (out.species.size() >= kMaxSpecies) {
        words.fail("too many species");
        return;
    }
    out.species.push_back(species);
}

void on_rule(Words& words, MapSource& out) {
    if (words.size() < 5) {
        words.fail("a rule needs a species, a density, a spacing and a priority");
        return;
    }
    SourceRule rule;
    rule.species = words[1];
    bool known = false;
    for (const SourceSpecies& species : out.species) {
        known = known || species.name == rule.species;
    }
    if (!known) {
        words.fail("a rule names a species that is not declared");
    }
    rule.density = words.number(2);
    rule.spacing = words.number(3);
    rule.priority = static_cast<u32>(words.integer(4));
    std::size_t at = 5;
    while (at < words.size()) {
        if (words[at] == "noise-metres") {
            rule.noise_metres = words.number(at + 1);
            at += 2;
            continue;
        }
        if (words[at] != "slope" && words[at] != "altitude" && words[at] != "noise") {
            words.fail("a rule tests an input the map does not know");
            return;
        }
        SourceTest test;
        test.input = words[at];
        for (std::size_t corner = 0; corner < 4; ++corner) {
            test.corners[corner] = words.number(at + 1 + corner);
        }
        rule.tests.push_back(test);
        at += 5;
    }
    out.rules.push_back(rule);
}

void on_exclude(Words& words, MapSource& out) {
    if (words.size() != 5 && words.size() != 7) {
        words.fail("an exclusion is a box and an optional feather");
        return;
    }
    SourceExclusion zone;
    zone.min_x = words.number(1);
    zone.min_z = words.number(2);
    zone.max_x = words.number(3);
    zone.max_z = words.number(4);
    if (words.size() == 7) {
        if (words[5] != "feather") {
            words.fail("an exclusion's sixth word must be 'feather'");
        }
        zone.feather = words.number(6);
    }
    out.exclusions.push_back(zone);
}

void on_place(Words& words, MapSource& out) {
    if (words.size() < 5) {
        words.fail("a placement needs a mesh, a kind and a position");
        return;
    }
    static constexpr struct {
        const char* name;
        PlacementKind kind;
    } kKinds[] = {{"building", PlacementKind::Building},
                  {"resource", PlacementKind::Resource},
                  {"prop", PlacementKind::Prop},
                  {"water", PlacementKind::Water}};
    SourcePlacement placement;
    placement.mesh = words[1];
    bool found = false;
    for (const auto& kind : kKinds) {
        if (words[2] == kind.name) {
            placement.kind = kind.kind;
            found = true;
        }
    }
    if (!found) {
        words.fail("a placement's kind is not building, resource, prop or water");
    }
    placement.x = words.number(3);
    placement.z = words.number(4);
    std::size_t at = 5;
    while (at + 1 < words.size()) {
        if (words[at] == "yaw") {
            placement.yaw_degrees = words.number(at + 1);
        } else if (words[at] == "scale") {
            placement.scale = words.number(at + 1);
        } else if (words[at] == "team") {
            placement.team = static_cast<u8>(words.integer(at + 1));
        } else if (words[at] == "tag") {
            placement.tag = words[at + 1];
        } else {
            words.fail("a placement has a word it does not know");
            return;
        }
        at += 2;
    }
    if (at != words.size()) {
        words.fail("a placement's last word has no value");
    }
    out.placements.push_back(placement);
}

struct Statement {
    const char* name;
    Handler handler;
};

constexpr Statement kStatements[] = {
    {"map", &on_map},
    {"seed", &on_seed},
    {"extent", &on_extent},
    {"generator", &on_generator},
    {"water", &on_water},
    {"sun", &on_sun},
    {"base", &on_base},
    {"company", &on_company},
    {"texture", &on_texture},
    {"layer", &on_layer},
    {"slope-layer", &on_slope_layer},
    {"shore-layer", &on_shore_layer},
    {"noise", &on_noise},
    {"mound", &on_mound},
    {"flatten", &on_flatten},
    {"road", &on_road},
    {"brush", &on_brush},
    {"material", &on_material},
    {"species", &on_species},
    {"rule", &on_rule},
    {"exclude", &on_exclude},
    {"place", &on_place},
};

[[nodiscard]] Handler find_handler(const std::string& name) noexcept {
    for (const Statement& statement : kStatements) {
        if (name == statement.name) {
            return statement.handler;
        }
    }
    return nullptr;
}

}  // namespace

Status parse_map_source(std::string_view text, MapSource& out, u32& line) noexcept {
    out = MapSource{};
    line = 0;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        ++line;
        Words words(text.substr(at, end - at));
        at = end + 1;
        if (words.error() == nullptr && words.empty()) {
            if (end == text.size()) {
                break;
            }
            continue;
        }
        if (words.error() == nullptr) {
            const Handler handler = find_handler(words[0]);
            if (handler == nullptr) {
                return cy::fail(cy::ErrorCode::InvalidArgument, "an unknown statement");
            }
            handler(words, out);
        }
        if (words.error() != nullptr) {
            return cy::fail(cy::ErrorCode::InvalidArgument, words.error());
        }
        if (end == text.size()) {
            break;
        }
    }
    if (out.name.empty() || out.layers.empty() || out.materials.empty()) {
        return cy::fail(cy::ErrorCode::InvalidArgument,
                        "a map needs a name, at least one layer and at least one material");
    }
    return cy::ok();
}

}  // namespace sample::rts::map
