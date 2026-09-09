// Generated primitives: the source format, the five generators, and the importer. M8.a task 2.1.
//
// The argument for every structural decision here is in primitive.h. This file is the work.
//
// TWO THINGS TO KNOW BEFORE READING THE GENERATORS.
//
// A surface of revolution — a sphere, a cylinder's side, a capsule — is built ONCE, by
// `emit_ring_surface`, from a list of rows. Three shapes describing themselves as rows and sharing
// one triangulator is not brevity for its own sake: the pole rule (a row of zero radius produces
// one triangle per segment instead of two) and the winding are the two things easy to get wrong,
// and there is one copy of each to get right. A capsule is then literally a sphere's top rows, a
// cylinder's two rows, and a sphere's bottom rows, which is what a capsule is.
//
// The seam is DUPLICATED: a ring of `segments` divisions emits `segments + 1` vertices, the last
// coincident with the first and carrying u = 1 rather than u = 0. Welding keeps them apart because
// their texture coordinates differ, which is exactly the "a texture seam must stay split" case
// `weld` documents, and a ring that did not duplicate would wrap its last quad's UVs backwards.

#include <cy/import/primitive.h>

#include <cy/core/math/scalar.h>
#include <cy/import/model.h>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace cy::import {
namespace {

// --- The source format
// ----------------------------------------------------------------------------

constexpr std::string_view kMagic = "cyprim";

struct ShapeName {
    PrimitiveShape shape;
    std::string_view keyword;
    /// The default node name: the keyword, capitalised.
    std::string_view label;
};

constexpr ShapeName kShapes[] = {
    {PrimitiveShape::Box, "box", "Box"},
    {PrimitiveShape::Sphere, "sphere", "Sphere"},
    {PrimitiveShape::Cylinder, "cylinder", "Cylinder"},
    {PrimitiveShape::Plane, "plane", "Plane"},
    {PrimitiveShape::Capsule, "capsule", "Capsule"},
};

/// Every keyword a `.cyprim` may carry. `Unknown` is what an unrecognised one becomes, and it is an
/// error rather than a warning — see the header.
enum class Key : u8 {
    Shape,
    Name,
    Origin,
    Extent,
    Radius,
    Height,
    Segments,
    Rings,
    Subdivisions,
    Unknown,
};

struct KeyName {
    Key key;
    std::string_view keyword;
};

constexpr KeyName kKeys[] = {
    {Key::Shape, "shape"},       {Key::Name, "name"},     {Key::Origin, "origin"},
    {Key::Extent, "extent"},     {Key::Radius, "radius"}, {Key::Height, "height"},
    {Key::Segments, "segments"}, {Key::Rings, "rings"},   {Key::Subdivisions, "subdivisions"},
};

[[nodiscard]] Key key_of(std::string_view keyword) noexcept {
    for (const KeyName& entry : kKeys) {
        if (entry.keyword == keyword) {
            return entry.key;
        }
    }
    return Key::Unknown;
}

/// Which parameters each shape takes. The table in primitive.h, as code.
///
/// `shape`, `name` and `origin` are common to all five and are not listed; everything else is
/// refused by the shape that does not use it, so a `radius` on a box fails at the parse rather than
/// being silently ignored by the generator.
[[nodiscard]] bool shape_takes(PrimitiveShape shape, Key key) noexcept {
    switch (key) {
        case Key::Extent:
            return shape == PrimitiveShape::Box || shape == PrimitiveShape::Plane;
        case Key::Radius:
            return shape == PrimitiveShape::Sphere || shape == PrimitiveShape::Cylinder ||
                   shape == PrimitiveShape::Capsule;
        case Key::Height:
            return shape == PrimitiveShape::Cylinder || shape == PrimitiveShape::Capsule;
        case Key::Segments:
            return shape == PrimitiveShape::Sphere || shape == PrimitiveShape::Cylinder ||
                   shape == PrimitiveShape::Capsule;
        case Key::Rings:
            return shape == PrimitiveShape::Sphere || shape == PrimitiveShape::Capsule;
        case Key::Subdivisions:
            return shape == PrimitiveShape::Plane;
        case Key::Shape:
        case Key::Name:
        case Key::Origin:
            return true;
        case Key::Unknown:
            break;
    }
    return false;
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

/// Take the next whitespace-separated token off `rest`, or return false when there is none.
[[nodiscard]] bool next_token(std::string_view& rest, std::string_view& token) noexcept {
    rest = trim(rest);
    if (rest.empty()) {
        return false;
    }
    const usize end = rest.find_first_of(" \t");
    token = rest.substr(0, end);
    rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
    return true;
}

[[nodiscard]] Expected<f64, Error> parse_number(std::string_view token) noexcept {
    f64 value = 0.0;
    const char* first = token.data();
    const char* last = token.data() + token.size();
    const std::from_chars_result parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return fail(ErrorCode::InvalidArgument, "a number was expected");
    }
    if (!std::isfinite(value)) {
        return fail(ErrorCode::InvalidArgument, "a finite number was expected");
    }
    return value;
}

[[nodiscard]] Expected<u32, Error> parse_count(std::string_view token, u32 low, u32 high) noexcept {
    u64 value = 0;
    const char* first = token.data();
    const char* last = token.data() + token.size();
    const std::from_chars_result parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return fail(ErrorCode::InvalidArgument, "a whole number was expected");
    }
    if (value < low || value > high) {
        return fail(ErrorCode::InvalidArgument, "a count outside the range this parameter allows");
    }
    return static_cast<u32>(value);
}

[[nodiscard]] Expected<f32, Error> parse_extent(std::string_view token) noexcept {
    Expected<f64, Error> value = parse_number(token);
    if (!value) {
        return make_unexpected(value.error());
    }
    if (value.value() < static_cast<f64>(kPrimitiveMinExtent) ||
        value.value() > static_cast<f64>(kPrimitiveMaxExtent)) {
        return fail(ErrorCode::InvalidArgument, "a size outside the range this parameter allows");
    }
    return static_cast<f32>(value.value());
}

/// One `.cyprim`, accumulated line by line, with what has already been said.
struct Parser {
    PrimitiveSpec spec;
    bool shape_declared = false;
    bool seen[static_cast<usize>(Key::Unknown)] = {};

    [[nodiscard]] Status take(Key key) noexcept {
        auto& flag = seen[static_cast<usize>(key)];
        if (flag) {
            return fail(ErrorCode::InvalidArgument, "this parameter was already given");
        }
        flag = true;
        return {};
    }
};

[[nodiscard]] Status apply_name(Parser& parser, std::string_view rest) noexcept {
    const std::string_view name = trim(rest);
    if (name.empty() || name.size() > kPrimitiveMaxNameLength) {
        return fail(ErrorCode::InvalidArgument, "a name of one to ninety-six characters");
    }
    for (const char character : name) {
        if (static_cast<unsigned char>(character) < 0x20U || character == '"') {
            return fail(ErrorCode::InvalidArgument, "a name with no control characters or quotes");
        }
    }
    parser.spec.name.assign(name);
    return {};
}

[[nodiscard]] Status apply_origin(Parser& parser, std::string_view rest) noexcept {
    const std::string_view value = trim(rest);
    if (value == primitive_origin_name(PrimitiveOrigin::Centre)) {
        parser.spec.origin = PrimitiveOrigin::Centre;
        return {};
    }
    if (value == primitive_origin_name(PrimitiveOrigin::Base)) {
        parser.spec.origin = PrimitiveOrigin::Base;
        return {};
    }
    return fail(ErrorCode::InvalidArgument, "an origin of 'centre' or 'base'");
}

/// `extent` takes three numbers for a box and two — x and z — for a plane.
[[nodiscard]] Status apply_extent(Parser& parser, std::string_view rest) noexcept {
    const usize wanted = parser.spec.shape == PrimitiveShape::Plane ? 2U : 3U;
    f32 lanes[3] = {1.0f, 1.0f, 1.0f};
    for (usize lane = 0; lane < wanted; ++lane) {
        std::string_view token;
        if (!next_token(rest, token)) {
            return fail(ErrorCode::InvalidArgument, "an extent needs one number per axis");
        }
        Expected<f32, Error> value = parse_extent(token);
        if (!value) {
            return make_unexpected(value.error());
        }
        lanes[lane] = value.value();
    }
    std::string_view extra;
    if (next_token(rest, extra)) {
        return fail(ErrorCode::InvalidArgument, "an extent has one number per axis and no more");
    }
    parser.spec.parameters.extent = parser.spec.shape == PrimitiveShape::Plane
                                        ? Vec3{lanes[0], 1.0f, lanes[1]}
                                        : Vec3{lanes[0], lanes[1], lanes[2]};
    return {};
}

/// One number, in the extent range: `radius` and `height`.
[[nodiscard]] Status apply_size(std::string_view rest, f32& out) noexcept {
    std::string_view token;
    std::string_view extra;
    if (!next_token(rest, token) || next_token(rest, extra)) {
        return fail(ErrorCode::InvalidArgument, "this parameter takes exactly one number");
    }
    Expected<f32, Error> value = parse_extent(token);
    if (!value) {
        return make_unexpected(value.error());
    }
    out = value.value();
    return {};
}

[[nodiscard]] Status apply_count(std::string_view rest, u32 low, u32 high, u32& out) noexcept {
    std::string_view token;
    std::string_view extra;
    if (!next_token(rest, token) || next_token(rest, extra)) {
        return fail(ErrorCode::InvalidArgument, "this parameter takes exactly one whole number");
    }
    Expected<u32, Error> value = parse_count(token, low, high);
    if (!value) {
        return make_unexpected(value.error());
    }
    out = value.value();
    return {};
}

[[nodiscard]] Status apply_subdivisions(Parser& parser, std::string_view rest) noexcept {
    u32 lanes[2] = {1, 1};
    for (u32& lane : lanes) {
        std::string_view token;
        if (!next_token(rest, token)) {
            return fail(ErrorCode::InvalidArgument, "subdivisions take one number per axis");
        }
        Expected<u32, Error> value = parse_count(token, 1, kPrimitiveMaxSubdivisions);
        if (!value) {
            return make_unexpected(value.error());
        }
        lane = value.value();
    }
    std::string_view extra;
    if (next_token(rest, extra)) {
        return fail(ErrorCode::InvalidArgument, "subdivisions take two numbers and no more");
    }
    parser.spec.parameters.subdivisions_x = lanes[0];
    parser.spec.parameters.subdivisions_z = lanes[1];
    return {};
}

[[nodiscard]] Status apply_shape(Parser& parser, std::string_view rest) noexcept {
    Expected<PrimitiveShape, Error> shape = primitive_shape_from_name(trim(rest));
    if (!shape) {
        return make_unexpected(shape.error());
    }
    parser.spec.shape = shape.value();
    parser.shape_declared = true;
    return {};
}

[[nodiscard]] Status apply_parameter(Parser& parser, Key key, std::string_view rest) noexcept {
    PrimitiveParameters& parameters = parser.spec.parameters;
    switch (key) {
        case Key::Shape:
            return apply_shape(parser, rest);
        case Key::Name:
            return apply_name(parser, rest);
        case Key::Origin:
            return apply_origin(parser, rest);
        case Key::Extent:
            return apply_extent(parser, rest);
        case Key::Radius:
            return apply_size(rest, parameters.radius);
        case Key::Height:
            return apply_size(rest, parameters.height);
        case Key::Segments:
            return apply_count(rest, kPrimitiveMinSegments, kPrimitiveMaxSegments,
                               parameters.segments);
        case Key::Rings:
            return apply_count(rest, kPrimitiveMinRings, kPrimitiveMaxRings, parameters.rings);
        case Key::Subdivisions:
            return apply_subdivisions(parser, rest);
        case Key::Unknown:
            break;
    }
    return fail(ErrorCode::InvalidArgument, "a keyword this format does not define");
}

/// One line, already stripped of its comment and its surrounding space, and known to be non-empty.
[[nodiscard]] Status apply_line(Parser& parser, std::string_view line) noexcept {
    std::string_view rest = line;
    std::string_view keyword;
    (void)next_token(rest, keyword);
    const Key key = key_of(keyword);
    if (key == Key::Unknown) {
        return fail(ErrorCode::InvalidArgument, "a keyword this format does not define");
    }
    if (key != Key::Shape && !parser.shape_declared) {
        return fail(ErrorCode::InvalidArgument, "the shape must be declared before its parameters");
    }
    if (!shape_takes(parser.spec.shape, key)) {
        return fail(ErrorCode::InvalidArgument, "a parameter this shape does not take");
    }
    if (Status taken = parser.take(key); !taken) {
        return taken;
    }
    return apply_parameter(parser, key, rest);
}

[[nodiscard]] Status check_header(std::string_view line) noexcept {
    std::string_view rest = line;
    std::string_view magic;
    std::string_view version;
    if (!next_token(rest, magic) || magic != kMagic) {
        return fail(ErrorCode::InvalidArgument, "the first line of a primitive source is 'cyprim'");
    }
    if (!next_token(rest, version)) {
        return fail(ErrorCode::InvalidArgument, "the first line carries the format version");
    }
    Expected<u32, Error> number = parse_count(version, 0, 0xFFFFFFFFU);
    if (!number) {
        return make_unexpected(number.error());
    }
    if (number.value() != kPrimitiveSourceVersion) {
        return fail(ErrorCode::InvalidArgument,
                    "a primitive source version this build cannot read");
    }
    std::string_view extra;
    if (next_token(rest, extra)) {
        return fail(ErrorCode::InvalidArgument,
                    "the first line carries only 'cyprim' and a version");
    }
    return {};
}

// --- Mesh building
// --------------------------------------------------------------------------------

/// Positions, normals and texture coordinates into one `MeshData`, checking every allocation.
struct Builder {
    MeshData& mesh;

    [[nodiscard]] Status vertex(Vec3 position, Vec3 normal, Vec2 uv) const noexcept {
        if (Status pushed = mesh.positions.push_back(position); !pushed) {
            return pushed;
        }
        if (Status pushed = mesh.normals.push_back(normal); !pushed) {
            return pushed;
        }
        return mesh.uvs.push_back(uv);
    }

    [[nodiscard]] Status triangle(u32 a, u32 b, u32 c) const noexcept {
        if (Status pushed = mesh.indices.push_back(a); !pushed) {
            return pushed;
        }
        if (Status pushed = mesh.indices.push_back(b); !pushed) {
            return pushed;
        }
        return mesh.indices.push_back(c);
    }

    [[nodiscard]] u32 next() const noexcept { return static_cast<u32>(mesh.positions.size()); }
};

/// One latitude of a surface of revolution about the Y axis.
struct Ring {
    f32 y = 0.0f;
    f32 radius = 0.0f;
    /// The normal's radial and vertical components, already normalised as a pair.
    f32 normal_radial = 1.0f;
    f32 normal_y = 0.0f;
    /// The texture coordinate down the surface.
    f32 v = 0.0f;
};

/// The angle of segment `s`, and the unit direction at it. x = sin, z = cos, so segment 0 faces +Z
/// and the sweep runs towards +X — the direction that makes the winding below counter-clockwise
/// from outside.
void segment_direction(u32 segment, u32 segments, f32& sin_out, f32& cos_out) noexcept {
    const f32 angle = 2.0f * math::kPi * (static_cast<f32>(segment) / static_cast<f32>(segments));
    sin_out = std::sin(angle);
    cos_out = std::cos(angle);
    if (segment == 0 || segment == segments) {
        // The seam is closed exactly rather than to within a rounding of 2*pi, so that welding sees
        // one position and not two a nanometre apart.
        sin_out = 0.0f;
        cos_out = 1.0f;
    }
}

/// One ring's vertices, seam included.
[[nodiscard]] Status emit_ring_vertices(const Builder& builder, const Ring& ring,
                                        u32 segments) noexcept {
    for (u32 segment = 0; segment <= segments; ++segment) {
        f32 sine = 0.0f;
        f32 cosine = 0.0f;
        segment_direction(segment, segments, sine, cosine);
        const Vec3 position{ring.radius * sine, ring.y, ring.radius * cosine};
        const Vec3 normal{ring.normal_radial * sine, ring.normal_y, ring.normal_radial * cosine};
        const Vec2 uv{static_cast<f32>(segment) / static_cast<f32>(segments), ring.v};
        if (Status pushed = builder.vertex(position, normal, uv); !pushed) {
            return pushed;
        }
    }
    return {};
}

/// The band between two rings, `stride` vertices apart, with a pole's degenerate half omitted.
[[nodiscard]] Status emit_ring_band(const Builder& builder, u32 first, u32 stride, u32 segments,
                                    bool top_pole, bool bottom_pole) noexcept {
    for (u32 segment = 0; segment < segments; ++segment) {
        const u32 a = first + segment;
        const u32 b = a + stride;
        const u32 c = a + 1;
        const u32 d = b + 1;
        if (!top_pole) {
            if (Status pushed = builder.triangle(a, b, c); !pushed) {
                return pushed;
            }
        }
        if (!bottom_pole) {
            if (Status pushed = builder.triangle(b, d, c); !pushed) {
                return pushed;
            }
        }
    }
    return {};
}

/// Triangulate a stack of rings, duplicating the seam and collapsing a pole into one triangle.
[[nodiscard]] Status emit_ring_surface(const Builder& builder, const std::vector<Ring>& rings,
                                       u32 segments) noexcept {
    const u32 base = builder.next();
    const u32 stride = segments + 1;
    for (const Ring& ring : rings) {
        if (Status emitted = emit_ring_vertices(builder, ring, segments); !emitted) {
            return emitted;
        }
    }

    for (usize row = 0; row + 1 < rings.size(); ++row) {
        const bool top_pole = rings[row].radius == 0.0f;
        const bool bottom_pole = rings[row + 1].radius == 0.0f;
        if (top_pole && bottom_pole) {
            continue;
        }
        const u32 first = base + (static_cast<u32>(row) * stride);
        if (Status emitted =
                emit_ring_band(builder, first, stride, segments, top_pole, bottom_pole);
            !emitted) {
            return emitted;
        }
    }
    return {};
}

/// A flat disc closing a cylinder, facing up or down.
[[nodiscard]] Status emit_disc(const Builder& builder, f32 y, f32 radius, u32 segments,
                               bool upwards) noexcept {
    const Vec3 normal{0.0f, upwards ? 1.0f : -1.0f, 0.0f};
    const u32 centre = builder.next();
    if (Status pushed = builder.vertex(Vec3{0.0f, y, 0.0f}, normal, Vec2{0.5f, 0.5f}); !pushed) {
        return pushed;
    }
    for (u32 segment = 0; segment <= segments; ++segment) {
        f32 sine = 0.0f;
        f32 cosine = 0.0f;
        segment_direction(segment, segments, sine, cosine);
        const Vec2 uv{0.5f + (0.5f * sine), 0.5f + (0.5f * (upwards ? cosine : -cosine))};
        if (Status pushed = builder.vertex(Vec3{radius * sine, y, radius * cosine}, normal, uv);
            !pushed) {
            return pushed;
        }
    }
    for (u32 segment = 0; segment < segments; ++segment) {
        const u32 first = centre + 1 + segment;
        const Status pushed = upwards ? builder.triangle(centre, first, first + 1)
                                      : builder.triangle(centre, first + 1, first);
        if (!pushed) {
            return pushed;
        }
    }
    return {};
}

/// The six faces, each with its own normal and its own texture coordinates, which is what makes a
/// box's edges hard and its faces individually textured.
[[nodiscard]] Status build_box(const Builder& builder, Vec3 extent) noexcept {
    struct Face {
        Vec3 normal;
        /// The direction of increasing U, and of increasing V. `cross(u, v) == normal`, which is
        /// what makes the winding below counter-clockwise seen from outside.
        Vec3 u;
        Vec3 v;
        /// Which half-extent each of the three directions takes: 0 is x, 1 is y, 2 is z.
        usize normal_axis = 0;
        usize u_axis = 0;
        usize v_axis = 0;
    };
    static constexpr Face kFaces[] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}, 0, 2, 1}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}, 0, 2, 1},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}, 1, 0, 2}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}, 1, 0, 2},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, 2, 0, 1},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}, 2, 0, 1},
    };
    const f32 half[3] = {extent.x * 0.5f, extent.y * 0.5f, extent.z * 0.5f};

    for (const Face& face : kFaces) {
        const u32 base = builder.next();
        const Vec3 centre = face.normal * half[face.normal_axis];
        const Vec3 across = face.u * half[face.u_axis];
        const Vec3 up = face.v * half[face.v_axis];
        const Vec3 corners[4] = {centre - across - up, centre + across - up, centre + across + up,
                                 centre - across + up};
        static constexpr Vec2 kUvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (usize corner = 0; corner < 4; ++corner) {
            if (Status pushed = builder.vertex(corners[corner], face.normal, kUvs[corner]);
                !pushed) {
                return pushed;
            }
        }
        if (Status pushed = builder.triangle(base, base + 1, base + 2); !pushed) {
            return pushed;
        }
        if (Status pushed = builder.triangle(base, base + 2, base + 3); !pushed) {
            return pushed;
        }
    }
    return {};
}

[[nodiscard]] Status build_plane(const Builder& builder,
                                 const PrimitiveParameters& parameters) noexcept {
    const u32 along_x = parameters.subdivisions_x;
    const u32 along_z = parameters.subdivisions_z;
    const f32 size_x = parameters.extent.x;
    const f32 size_z = parameters.extent.z;
    const Vec3 normal{0.0f, 1.0f, 0.0f};

    for (u32 row = 0; row <= along_z; ++row) {
        const f32 t_z = static_cast<f32>(row) / static_cast<f32>(along_z);
        for (u32 column = 0; column <= along_x; ++column) {
            const f32 t_x = static_cast<f32>(column) / static_cast<f32>(along_x);
            const Vec3 position{(t_x - 0.5f) * size_x, 0.0f, (t_z - 0.5f) * size_z};
            if (Status pushed = builder.vertex(position, normal, Vec2{t_x, t_z}); !pushed) {
                return pushed;
            }
        }
    }

    const u32 stride = along_x + 1;
    for (u32 row = 0; row < along_z; ++row) {
        for (u32 column = 0; column < along_x; ++column) {
            const u32 a = (row * stride) + column;
            const u32 b = a + stride;
            if (Status pushed = builder.triangle(a, b, b + 1); !pushed) {
                return pushed;
            }
            if (Status pushed = builder.triangle(a, b + 1, a + 1); !pushed) {
                return pushed;
            }
        }
    }
    return {};
}

/// One latitude of a hemisphere, indexed from its pole (0) to its equator (`latitudes`).
///
/// `sign` is +1 for an upper hemisphere and -1 for a lower one; `centre_y` is where its equator
/// sits, which is 0 for a sphere and half the cylindrical section for a capsule.
[[nodiscard]] Ring hemisphere_ring(u32 polar, u32 latitudes, f32 radius, f32 centre_y, f32 sign,
                                   f32 v) noexcept {
    const f32 angle = 0.5f * math::kPi * (static_cast<f32>(polar) / static_cast<f32>(latitudes));
    Ring ring;
    ring.radius = radius * std::sin(angle);
    ring.y = centre_y + (sign * radius * std::cos(angle));
    ring.normal_radial = std::sin(angle);
    ring.normal_y = sign * std::cos(angle);
    ring.v = v;
    return ring;
}

/// Rows from the top pole down to the equator, inclusive.
///
/// ROWS MUST BE APPENDED IN DESCENDING ORDER OF Y, because `emit_ring_surface` triangulates
/// consecutive rows and takes the first of each pair to be the upper one. A hemisphere appended
/// pole-first for the bottom half looks right in a vertex dump and is inside out in every triangle
/// — which is what the winding and closed-surface cases below caught the first time.
void append_upper_hemisphere(std::vector<Ring>& rings, u32 latitudes, f32 radius, f32 centre_y,
                             f32 v_pole, f32 v_equator) {
    for (u32 polar = 0; polar <= latitudes; ++polar) {
        const f32 t = static_cast<f32>(polar) / static_cast<f32>(latitudes);
        rings.push_back(hemisphere_ring(polar, latitudes, radius, centre_y, 1.0f,
                                        v_pole + ((v_equator - v_pole) * t)));
    }
}

/// Rows below the equator down to the bottom pole. The equator row itself belongs to whatever is
/// above it — a sphere's upper hemisphere, a capsule's cylindrical section — and is not repeated.
void append_lower_hemisphere(std::vector<Ring>& rings, u32 latitudes, f32 radius, f32 centre_y,
                             f32 v_equator, f32 v_pole) {
    for (u32 step = 1; step <= latitudes; ++step) {
        const f32 t = static_cast<f32>(step) / static_cast<f32>(latitudes);
        rings.push_back(hemisphere_ring(latitudes - step, latitudes, radius, centre_y, -1.0f,
                                        v_equator + ((v_pole - v_equator) * t)));
    }
}

[[nodiscard]] Status build_sphere(const Builder& builder,
                                  const PrimitiveParameters& parameters) noexcept {
    // A sphere's latitudes are counted pole to pole, so each hemisphere takes half of them and an
    // odd count gives the top the extra row. The equator is emitted once, by the top hemisphere.
    const u32 top = (parameters.rings + 1) / 2;
    const u32 bottom = parameters.rings - top;
    std::vector<Ring> rings;
    rings.reserve(static_cast<usize>(parameters.rings) + 1);
    const f32 v_equator = static_cast<f32>(top) / static_cast<f32>(parameters.rings);
    append_upper_hemisphere(rings, top, parameters.radius, 0.0f, 0.0f, v_equator);
    append_lower_hemisphere(rings, bottom, parameters.radius, 0.0f, v_equator, 1.0f);
    return emit_ring_surface(builder, rings, parameters.segments);
}

[[nodiscard]] Status build_cylinder(const Builder& builder,
                                    const PrimitiveParameters& parameters) noexcept {
    const f32 half = parameters.height * 0.5f;
    const std::vector<Ring> rings = {
        Ring{half, parameters.radius, 1.0f, 0.0f, 0.0f},
        Ring{-half, parameters.radius, 1.0f, 0.0f, 1.0f},
    };
    if (Status emitted = emit_ring_surface(builder, rings, parameters.segments); !emitted) {
        return emitted;
    }
    if (Status emitted = emit_disc(builder, half, parameters.radius, parameters.segments, true);
        !emitted) {
        return emitted;
    }
    return emit_disc(builder, -half, parameters.radius, parameters.segments, false);
}

[[nodiscard]] Status build_capsule(const Builder& builder,
                                   const PrimitiveParameters& parameters) noexcept {
    const f32 radius = parameters.radius;
    const f32 half = parameters.height * 0.5f;
    // The texture runs down the surface by arc length, so the cylindrical section takes the share
    // of v its length is of the total. A capsule textured by latitude index instead would stretch
    // its caps at every aspect ratio but one.
    const f32 cap_arc = 0.5f * math::kPi * radius;
    const f32 total = (2.0f * cap_arc) + parameters.height;
    const f32 cap_v = cap_arc / total;

    std::vector<Ring> rings;
    rings.reserve((2 * static_cast<usize>(parameters.rings)) + 2);
    append_upper_hemisphere(rings, parameters.rings, radius, half, 0.0f, cap_v);
    rings.push_back(Ring{-half, radius, 1.0f, 0.0f, 1.0f - cap_v});
    append_lower_hemisphere(rings, parameters.rings, radius, -half, 1.0f - cap_v, 1.0f);
    return emit_ring_surface(builder, rings, parameters.segments);
}

/// The distance from the shape's centre to its lowest point, which is what `origin base` lifts by.
[[nodiscard]] f32 lowest_point(const PrimitiveSpec& spec) noexcept {
    const PrimitiveParameters& parameters = spec.parameters;
    switch (spec.shape) {
        case PrimitiveShape::Box:
            return parameters.extent.y * 0.5f;
        case PrimitiveShape::Sphere:
            return parameters.radius;
        case PrimitiveShape::Cylinder:
            return parameters.height * 0.5f;
        case PrimitiveShape::Plane:
            return 0.0f;
        case PrimitiveShape::Capsule:
            return (parameters.height * 0.5f) + parameters.radius;
    }
    return 0.0f;
}

}  // namespace

// --- The names
// ------------------------------------------------------------------------------------

const char* primitive_shape_name(PrimitiveShape shape) noexcept {
    for (const ShapeName& entry : kShapes) {
        if (entry.shape == shape) {
            return entry.keyword.data();
        }
    }
    return "box";
}

Expected<PrimitiveShape, Error> primitive_shape_from_name(std::string_view name) noexcept {
    for (const ShapeName& entry : kShapes) {
        if (entry.keyword == name) {
            return entry.shape;
        }
    }
    return fail(ErrorCode::InvalidArgument,
                "a shape of 'box', 'sphere', 'cylinder', 'plane' or 'capsule'");
}

const char* primitive_origin_name(PrimitiveOrigin origin) noexcept {
    return origin == PrimitiveOrigin::Base ? "base" : "centre";
}

// --- The source format
// ------------------------------------------------------------------------------

std::string primitive_number(f64 value) {
    char buffer[64] = {};
    const int written = std::snprintf(buffer, sizeof(buffer), "%.6f", value);
    if (written <= 0) {
        return "0";
    }
    std::string text(buffer, static_cast<usize>(written));
    const usize point = text.find('.');
    if (point != std::string::npos) {
        usize end = text.size();
        while (end > point + 1 && text[end - 1] == '0') {
            --end;
        }
        if (end == point + 1) {
            end = point;
        }
        text.resize(end);
    }
    return text == "-0" ? "0" : text;
}

namespace {

/// The next line of `text` from `cursor`, stripped of its comment and its surrounding space, with
/// `cursor` advanced past the newline. Empty when the line held nothing worth reading.
[[nodiscard]] std::string_view next_line(std::string_view text, usize& cursor) noexcept {
    const usize newline = text.find('\n', cursor);
    const std::string_view raw = text.substr(
        cursor, newline == std::string_view::npos ? std::string_view::npos : newline - cursor);
    cursor = newline == std::string_view::npos ? text.size() + 1 : newline + 1;
    const usize comment = raw.find('#');
    return trim(comment == std::string_view::npos ? raw : raw.substr(0, comment));
}

/// The default name of a shape, for a source file that declared none.
[[nodiscard]] std::string_view default_name(PrimitiveShape shape) noexcept {
    for (const ShapeName& entry : kShapes) {
        if (entry.shape == shape) {
            return entry.label;
        }
    }
    return "Primitive";
}

}  // namespace

Expected<PrimitiveSpec, Error> parse_primitive_source(std::string_view text,
                                                      u32* out_line) noexcept {
    const auto at = [out_line](u32 line, Error error) noexcept {
        if (out_line != nullptr) {
            *out_line = line;
        }
        return make_unexpected(error);
    };

    Parser parser;
    u32 number = 0;
    bool header = false;
    usize cursor = 0;
    while (cursor <= text.size()) {
        const std::string_view line = next_line(text, cursor);
        ++number;
        if (line.empty()) {
            continue;
        }
        if (!header) {
            if (Status checked = check_header(line); !checked) {
                return at(number, checked.error());
            }
            header = true;
            continue;
        }
        if (Status applied = apply_line(parser, line); !applied) {
            return at(number, applied.error());
        }
    }

    if (!header) {
        return at(1, Error{ErrorCode::InvalidArgument, "an empty file is not a primitive source"});
    }
    if (!parser.shape_declared) {
        return at(number, Error{ErrorCode::InvalidArgument, "a primitive source declares a shape"});
    }
    if (parser.spec.name.empty()) {
        parser.spec.name.assign(default_name(parser.spec.shape));
    }
    if (out_line != nullptr) {
        *out_line = 0;
    }
    return std::move(parser.spec);
}

Status write_primitive_source(const PrimitiveSpec& spec, std::string& out) {
    const PrimitiveParameters& parameters = spec.parameters;
    out.append(kMagic);
    out.append(" ").append(primitive_number(kPrimitiveSourceVersion)).append("\n");
    out.append("shape ").append(primitive_shape_name(spec.shape)).append("\n");
    out.append("name ").append(spec.name.empty() ? "Primitive" : spec.name).append("\n");
    out.append("origin ").append(primitive_origin_name(spec.origin)).append("\n");

    const auto number = [&out](std::string_view keyword, f64 value) {
        out.append(keyword).append(" ").append(primitive_number(value)).append("\n");
    };
    switch (spec.shape) {
        case PrimitiveShape::Box:
            out.append("extent ")
                .append(primitive_number(parameters.extent.x))
                .append(" ")
                .append(primitive_number(parameters.extent.y))
                .append(" ")
                .append(primitive_number(parameters.extent.z))
                .append("\n");
            break;
        case PrimitiveShape::Plane:
            out.append("extent ")
                .append(primitive_number(parameters.extent.x))
                .append(" ")
                .append(primitive_number(parameters.extent.z))
                .append("\n");
            out.append("subdivisions ")
                .append(primitive_number(parameters.subdivisions_x))
                .append(" ")
                .append(primitive_number(parameters.subdivisions_z))
                .append("\n");
            break;
        case PrimitiveShape::Sphere:
            number("radius", parameters.radius);
            number("segments", parameters.segments);
            number("rings", parameters.rings);
            break;
        case PrimitiveShape::Cylinder:
            number("radius", parameters.radius);
            number("height", parameters.height);
            number("segments", parameters.segments);
            break;
        case PrimitiveShape::Capsule:
            number("radius", parameters.radius);
            number("height", parameters.height);
            number("segments", parameters.segments);
            number("rings", parameters.rings);
            break;
    }
    return {};
}

// --- The generators
// ---------------------------------------------------------------------------------

Status build_primitive_mesh(const PrimitiveSpec& spec, MeshData& out) noexcept {
    out.clear();
    const Builder builder{out};
    const PrimitiveParameters& parameters = spec.parameters;

    Status built;
    switch (spec.shape) {
        case PrimitiveShape::Box:
            built = build_box(builder, parameters.extent);
            break;
        case PrimitiveShape::Sphere:
            built = build_sphere(builder, parameters);
            break;
        case PrimitiveShape::Cylinder:
            built = build_cylinder(builder, parameters);
            break;
        case PrimitiveShape::Plane:
            built = build_plane(builder, parameters);
            break;
        case PrimitiveShape::Capsule:
            built = build_capsule(builder, parameters);
            break;
    }
    if (!built) {
        return built;
    }

    if (spec.origin == PrimitiveOrigin::Base) {
        const f32 lift = lowest_point(spec);
        for (Vec3& position : out.positions) {
            position.y += lift;
        }
    }

    // One section, material 0: a primitive has one material, exactly as a single-material node of
    // an imported model does, and the cook step resolves the slot the same way for both.
    MeshSection section;
    section.first_index = 0;
    section.index_count = static_cast<u32>(out.indices.size());
    section.material = 0;
    if (Status pushed = out.sections.push_back(section); !pushed) {
        return pushed;
    }
    return out.validate();
}

// --- The importer
// -----------------------------------------------------------------------------------

namespace {

constexpr std::string_view kExtensions[] = {kPrimitiveExtension};
constexpr assets::AssetKind kProduces[] = {assets::AssetKind::Mesh, assets::AssetKind::Prefab};

constexpr std::string_view kCollisionChoices[] = {"none", "convex", "decompose", "triangle"};

// Every one of these is a model importer's option, under the same name and with the same default.
// See the note in primitive.h about what is deliberately absent.
constexpr OptionSpec kPrimitiveOptions[] = {
    {"weld-tolerance",
     OptionType::Float,
     OptionValue::of_float(1.0e-5),
     "How close two positions must be, in metres, to be merged into one vertex. A generated shape "
     "duplicates its seam and its hard edges on purpose, and the weld keeps them apart because "
     "their normals and texture coordinates differ rather than because it is switched off.",
     {},
     0.0,
     1.0},
    {"smoothing-angle",
     OptionType::Float,
     OptionValue::of_float(60.0),
     "The angle in degrees beyond which two faces are a hard edge. A generated shape supplies its "
     "own normals, so this is read only when a later step has to regenerate them.",
     {},
     0.0,
     180.0},
    {"generate-tangents",
     OptionType::Bool,
     OptionValue::of_bool(true),
     "Whether to generate a tangent basis. Needed by every normal-mapped material, and generated "
     "by the same function and in the same convention as an imported mesh's.",
     {},
     0.0,
     0.0},
    {"optimise",
     OptionType::Bool,
     OptionValue::of_bool(true),
     "Whether to reorder triangles for the post-transform vertex cache and for overdraw, and "
     "vertices for fetch locality. Off only to compare against an unoptimised generation.",
     {},
     0.0,
     0.0},
    {"overdraw-threshold",
     OptionType::Float,
     OptionValue::of_float(1.05),
     "How much of the vertex cache's efficiency the overdraw reorder may spend, as a ratio. 1 "
     "forbids any regression and disables the step in practice.",
     {},
     1.0,
     4.0},
    {"generate-lightmap-uvs",
     OptionType::Bool,
     OptionValue::of_bool(false),
     "Whether to unwrap a second texture coordinate set for lightmapping. A floor plane is the "
     "commonest thing anybody lightmaps, so this is the option a project turns on first.",
     {},
     0.0,
     0.0},
    {"lightmap-texel-density",
     OptionType::Float,
     OptionValue::of_float(16.0),
     "Texels per world unit for the generated lightmap atlas.",
     {},
     0.25,
     1024.0},
    {"lightmap-padding",
     OptionType::Int,
     OptionValue::of_int(2),
     "Texels left between charts so a bilinear tap at a chart's edge cannot reach its neighbour.",
     {},
     0.0,
     32.0},
    {"lod-count",
     OptionType::Int,
     OptionValue::of_int(0),
     "How many reduced levels of detail to generate after the full-detail mesh. Zero produces "
     "none, which is the right answer for a box and the wrong one for a 512-segment sphere.",
     {},
     0.0,
     8.0},
    {"lod-ratio",
     OptionType::Float,
     OptionValue::of_float(0.5),
     "The share of the previous level's triangles each level of detail keeps.",
     {},
     0.05,
     0.95},
    {"lod-error-bound",
     OptionType::Float,
     OptionValue::of_float(0.0),
     "Stop simplifying a level before a collapse whose squared error exceeds this, so a mesh that "
     "cannot be reduced safely comes back larger rather than damaged. Zero means no bound.",
     {},
     0.0,
     1.0e6},
    {"collision-suffix",
     OptionType::Text,
     OptionValue::of_text("_collision"),
     "A primitive whose name ends with this becomes a collider and is excluded from rendering — "
     "the same convention, and the same option, a model importer applies to a node's name. Empty "
     "disables it.",
     {},
     0.0,
     0.0},
    {"collision-mode", OptionType::Enumeration, OptionValue::of_enumeration("convex"),
     "What a collision primitive produces: nothing, one convex hull, a bounded set of hulls from a "
     "convex decomposition, or the triangle mesh as it stands.",
     Span<const std::string_view>(kCollisionChoices), 0.0, 0.0},
};

/// Read the schema into the shared options, or say that the schema and this function disagree.
[[nodiscard]] Expected<ModelBuildOptions, Error> read_build_options(
    const ImportRequest& request, const OptionsSchema& schema) noexcept {
    const auto option = [&](std::string_view name) noexcept {
        return request.option(schema, name);
    };
    Expected<OptionValue, Error> weld_tolerance = option("weld-tolerance");
    Expected<OptionValue, Error> smoothing = option("smoothing-angle");
    Expected<OptionValue, Error> tangents = option("generate-tangents");
    Expected<OptionValue, Error> optimise_option = option("optimise");
    Expected<OptionValue, Error> overdraw = option("overdraw-threshold");
    Expected<OptionValue, Error> lightmap = option("generate-lightmap-uvs");
    Expected<OptionValue, Error> density = option("lightmap-texel-density");
    Expected<OptionValue, Error> padding = option("lightmap-padding");
    Expected<OptionValue, Error> lod_count = option("lod-count");
    Expected<OptionValue, Error> lod_ratio = option("lod-ratio");
    Expected<OptionValue, Error> lod_error = option("lod-error-bound");
    Expected<OptionValue, Error> collision_suffix = option("collision-suffix");
    Expected<OptionValue, Error> collision_mode = option("collision-mode");
    if (!weld_tolerance || !smoothing || !tangents || !optimise_option || !overdraw || !lightmap ||
        !density || !padding || !lod_count || !lod_ratio || !lod_error || !collision_suffix ||
        !collision_mode) {
        return fail(ErrorCode::Internal,
                    "the primitive importer's own option schema is inconsistent");
    }

    ModelBuildOptions build;
    build.weld_tolerance = static_cast<f32>(weld_tolerance.value().as_float());
    build.smoothing_angle = static_cast<f32>(smoothing.value().as_float());
    build.generate_tangent_basis = tangents.value().as_bool();
    build.optimise = optimise_option.value().as_bool();
    build.overdraw_threshold = static_cast<f32>(overdraw.value().as_float());
    build.generate_lightmap_uvs = lightmap.value().as_bool();
    build.uv2.texel_density = static_cast<f32>(density.value().as_float());
    build.uv2.padding = static_cast<u32>(padding.value().as_int());
    build.lod_count = lod_count.value().as_int();
    build.lod_ratio = static_cast<f32>(lod_ratio.value().as_float());
    build.lod_error_bound = static_cast<f32>(lod_error.value().as_float());
    build.collision_suffix = collision_suffix.value().as_text();
    build.collision_mode = collision_mode.value().as_text();
    return build;
}

}  // namespace

OptionsSchema primitive_options() noexcept {
    return OptionsSchema(Span<const OptionSpec>(kPrimitiveOptions));
}

ImporterInfo PrimitiveImporter::info() const noexcept {
    ImporterInfo info;
    info.name = "primitive";
    info.version = 1;
    info.extensions = Span<const std::string_view>(kExtensions);
    info.produces = Span<const assets::AssetKind>(kProduces);
    // Steps 1 to 6 and 10, which is the hierarchy set without materials: a `.cyprim` declares no
    // material, so step 9 is one this format cannot express rather than one this importer skipped.
    // Declaring it here is what makes a cache HIT say the same thing a miss does — see the note on
    // `ModelImportStep`.
    info.steps = static_cast<ModelImportStepSet>(kHierarchyModelSteps &
                                                 ~step_bit(ModelImportStep::Materials));
    info.description =
        "Generates a box, sphere, cylinder, plane or capsule from a .cyprim source into a cooked "
        "mesh with levels of detail, a collision proxy from the same naming convention a model "
        "import uses, and the one-node hierarchy the cook step turns into a prefab. Every step "
        "after the shape itself is shared with the glTF and FBX importers, so nothing downstream "
        "can tell a generated mesh from an imported one.";
    return info;
}

OptionsSchema PrimitiveImporter::schema() const noexcept {
    return primitive_options();
}

Status PrimitiveImporter::import(const ImportRequest& request, ImportResult& out) noexcept {
    const OptionsSchema schema = this->schema();
    Expected<ModelBuildOptions, Error> options = read_build_options(request, schema);
    if (!options) {
        return make_unexpected(options.error());
    }
    const ModelBuildOptions& build = options.value();

    // --- 1. Parse. The one step that is this importer's own.
    const std::string_view text(reinterpret_cast<const char*>(request.bytes.data()),
                                request.bytes.size());
    u32 line = 0;
    Expected<PrimitiveSpec, Error> parsed = parse_primitive_source(text, &line);
    if (!parsed) {
        char subject[ImportDiagnostic::kSubjectCapacity] = {};
        (void)std::snprintf(subject, sizeof(subject), "%.*s:%u",
                            static_cast<int>(request.source.view().size()),
                            request.source.view().data(), line);
        return out.report(ImportSeverity::Error, "malformed-primitive", parsed.error().message,
                          subject);
    }
    const PrimitiveSpec& spec = parsed.value();

    MeshData mesh;
    if (Status generated = build_primitive_mesh(spec, mesh); !generated) {
        return out.report(ImportSeverity::Error, "ungeneratable-primitive",
                          generated.error().message, spec.name);
    }

    // --- 2 to 5. `model.h`'s, in `model.h`'s order, with no step of this importer's own between
    // them. This is what "no downstream system can tell it from an import" reduces to in code.
    SubAssetNames names;
    const std::string mesh_name = names.unique("mesh/", spec.name, 0);
    if (Status finished = finish_mesh(mesh, build, out, mesh_name); !finished) {
        return finished;
    }
    if (Status emitted = emit_mesh_with_lods(mesh, mesh_name, build, out); !emitted) {
        return emitted;
    }

    // --- 6. Collision, by the importers' naming convention rather than a parameter of its own.
    ImportedNode node;
    node.name = spec.name;
    node.parent = -1;
    node.mesh = 0;
    const bool collider = !build.collision_suffix.empty() &&
                          ends_with(spec.name, build.collision_suffix) &&
                          build.collision_mode != "none";
    if (collider) {
        node.collision_only = true;
        node.mesh = -1;
        const std::string collider_name = "collision/" + spec.name;
        Expected<usize, Error> emitted = emit_collision(mesh, collider_name, build, out);
        if (!emitted) {
            return make_unexpected(emitted.error());
        }
        if (emitted.value() != 0) {
            node.collision = 0;
        }
    }

    // --- 10. The hierarchy: one node, which is what a one-mesh source file is.
    return emit_prefab(Span<const ImportedNode>(&node, 1), out);
}

}  // namespace cy::import
