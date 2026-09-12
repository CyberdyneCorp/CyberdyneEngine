// River spline networks: resampling, junctions, continuity, and the flow geometry decides.
// M10 task 2.3.

#include <cy/water/river.h>

#include <algorithm>
#include <cmath>

namespace cy::water {

namespace {

constexpr f32 kMinChannelSize = 0.01F;

[[nodiscard]] Vec3 to_vec3(const world::WorldVec3d& value) noexcept {
    return Vec3{static_cast<f32>(value.x), static_cast<f32>(value.y), static_cast<f32>(value.z)};
}

/// Horizontal distance between two absolute positions. f64 throughout, because two points on one
/// river a hundred kilometres from the origin are metres apart and an f32 subtraction of their
/// coordinates would lose the difference before the square root saw it.
[[nodiscard]] f64 horizontal_distance(const world::WorldVec3d& a,
                                      const world::WorldVec3d& b) noexcept {
    const f64 dx = a.x - b.x;
    const f64 dz = a.z - b.z;
    return std::sqrt((dx * dx) + (dz * dz));
}

/// Catmull-Rom between `b` and `c`, with `a` and `d` as the neighbouring control points. The
/// centreline is a spline and not a polyline because curvature is what drives a bend's turbulence,
/// and a polyline's curvature is zero everywhere and infinite at the joints.
[[nodiscard]] world::WorldVec3d catmull_rom(const world::WorldVec3d& a, const world::WorldVec3d& b,
                                            const world::WorldVec3d& c, const world::WorldVec3d& d,
                                            f64 t) noexcept {
    const f64 t2 = t * t;
    const f64 t3 = t2 * t;
    const auto axis = [&](f64 pa, f64 pb, f64 pc, f64 pd) noexcept {
        return 0.5 * (((2.0 * pb) + ((-pa + pc) * t) +
                       (((2.0 * pa) - (5.0 * pb) + (4.0 * pc) - pd) * t2)) +
                      ((-pa + (3.0 * pb) - (3.0 * pc) + pd) * t3));
    };
    return world::WorldVec3d{axis(a.x, b.x, c.x, d.x), axis(a.y, b.y, c.y, d.y),
                             axis(a.z, b.z, c.z, d.z)};
}

[[nodiscard]] f32 lerp_f32(f32 a, f32 b, f32 t) noexcept {
    return a + ((b - a) * t);
}

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    return (value < 0.0F) ? 0.0F : ((value > 1.0F) ? 1.0F : value);
}

/// The bed's shape across the channel. `u` is the lateral offset as a fraction of the half width.
///
/// A natural channel is parabolic — deepest at the thalweg, zero at the banks — and a canal is a
/// box. `squareness` interpolates between them, which is what `RiverControlPoint::bed_profile`
/// means as one number: everything that consumes a bed profile wants a depth at a lateral offset.
[[nodiscard]] f32 bed_profile(f32 u, f32 squareness) noexcept {
    const f32 clamped = clamp01(std::fabs(u));
    const f32 parabolic = 1.0F - (clamped * clamped);
    return lerp_f32(parabolic, 1.0F, clamp01(squareness));
}

}  // namespace

RiverNetwork::RiverNetwork(Allocator& allocator) noexcept
    : allocator_(&allocator),
      authored_(allocator),
      obstacles_(allocator),
      sections_(allocator),
      vertices_(allocator) {}

Expected<u32, Error> RiverNetwork::add_section(const RiverSectionDesc& desc) noexcept {
    if (desc.points.size() < 2) {
        return fail(ErrorCode::InvalidArgument,
                    "water: a river section needs at least two control points");
    }
    for (const RiverControlPoint& point : desc.points) {
        if (point.width <= kMinChannelSize || point.depth <= kMinChannelSize) {
            return fail(ErrorCode::InvalidArgument,
                        "water: a river section's width and depth must be positive");
        }
    }
    if (desc.joins_section != kNoSection && desc.joins_section >= authored_.size()) {
        // A forward reference would make the network's shape depend on declaration order, which is
        // the one thing a network keyed by index must not do.
        return fail(ErrorCode::NotFound,
                    "water: a river section may only join a section already added");
    }

    Authored authored(*allocator_);
    authored.name = (desc.name == nullptr) ? "" : desc.name;
    authored.joins_section = desc.joins_section;
    authored.joins_at = clamp01(desc.joins_at);
    for (const RiverControlPoint& point : desc.points) {
        if (Status pushed = authored.points.push_back(point); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    if (Status pushed = authored_.push_back(std::move(authored)); !pushed) {
        return make_unexpected(pushed.error());
    }
    built_ = false;
    return static_cast<u32>(authored_.size() - 1);
}

Status RiverNetwork::add_obstacle(const RiverObstacle& obstacle) noexcept {
    if (obstacle.radius <= 0.0F || obstacle.influence < 0.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "water: an obstacle needs a positive radius and a non-negative influence");
    }
    built_ = false;
    return obstacles_.push_back(obstacle);
}

Status RiverNetwork::set_resample_metres(f32 metres) noexcept {
    if (metres <= 0.0F) {
        return fail(ErrorCode::InvalidArgument, "water: a resample step must be positive");
    }
    resample_metres_ = metres;
    built_ = false;
    return ok();
}

// --- Resampling ------------------------------------------------------------------------------

Status RiverNetwork::resample_sections(RiverBuildReport& report) noexcept {
    sections_.clear();
    vertices_.clear();

    for (const Authored& authored : authored_) {
        RiverSection section;
        section.name = authored.name;
        section.joins_section = authored.joins_section;
        section.joins_at = authored.joins_at;
        section.first_vertex = static_cast<u32>(vertices_.size());

        const usize count = authored.points.size();
        for (usize index = 0; index + 1 < count; ++index) {
            const RiverControlPoint& from = authored.points[index];
            const RiverControlPoint& to = authored.points[index + 1];
            // The neighbours a Catmull-Rom segment needs; the ends duplicate their own endpoint,
            // which is the standard clamped form and keeps the first and last control points on
            // the curve.
            const world::WorldVec3d& before =
                authored.points[(index == 0) ? 0 : (index - 1)].position;
            const world::WorldVec3d& after =
                authored.points[(index + 2 < count) ? (index + 2) : (count - 1)].position;

            const f64 chord = horizontal_distance(from.position, to.position);
            const auto steps = static_cast<u32>(
                std::max(1.0, std::ceil(chord / static_cast<f64>(resample_metres_))));
            for (u32 step = 0; step < steps; ++step) {
                const f64 t = static_cast<f64>(step) / static_cast<f64>(steps);
                RiverVertex vertex;
                vertex.position = catmull_rom(before, from.position, to.position, after, t);
                const auto blend = static_cast<f32>(t);
                vertex.width = lerp_f32(from.width, to.width, blend);
                vertex.depth = lerp_f32(from.depth, to.depth, blend);
                vertex.speed = lerp_f32(from.flow_speed, to.flow_speed, blend);
                vertex.turbulence = lerp_f32(from.turbulence, to.turbulence, blend);
                vertex.bed_squareness = lerp_f32(from.bed_squareness, to.bed_squareness, blend);
                vertex.material = (blend < 0.5F) ? from.material : to.material;
                if (Status pushed = vertices_.push_back(vertex); !pushed) {
                    return pushed;
                }
            }
        }
        // The last control point, which the loop above stops short of by construction.
        const RiverControlPoint& last = authored.points[count - 1];
        RiverVertex tail;
        tail.position = last.position;
        tail.width = last.width;
        tail.depth = last.depth;
        tail.speed = last.flow_speed;
        tail.turbulence = last.turbulence;
        tail.bed_squareness = last.bed_squareness;
        tail.material = last.material;
        if (Status pushed = vertices_.push_back(tail); !pushed) {
            return pushed;
        }

        section.vertex_count = static_cast<u32>(vertices_.size()) - section.first_vertex;
        if (Status pushed = sections_.push_back(section); !pushed) {
            return pushed;
        }
        ++report.sections;
    }
    report.vertices = static_cast<u32>(vertices_.size());
    return ok();
}

namespace {

/// Arc length, tangents, curvature and surface gradient over one section's vertices. Geometry, so
/// it is computed here rather than authored: the specification's turbulence "at bends and drops" is
/// a function of these two numbers and nothing else.
void derive_geometry(Span<RiverVertex> vertices, f32& length) noexcept {
    const usize count = vertices.size();
    f32 arc = 0.0F;
    for (usize index = 0; index < count; ++index) {
        if (index > 0) {
            arc += static_cast<f32>(
                horizontal_distance(vertices[index].position, vertices[index - 1].position));
        }
        vertices[index].arc = arc;
    }
    length = arc;

    for (usize index = 0; index < count; ++index) {
        const usize previous = (index == 0) ? 0 : (index - 1);
        const usize next = (index + 1 < count) ? (index + 1) : (count - 1);
        const Vec3 forward =
            to_vec3(vertices[next].position) - to_vec3(vertices[previous].position);
        vertices[index].tangent = normalized_or(Vec3{forward.x, 0.0F, forward.z}, Vec3{1, 0, 0});

        const f32 span = vertices[next].arc - vertices[previous].arc;
        if (span > 1e-4F) {
            // Curvature as the turn rate of the tangent: the angle between the incoming and
            // outgoing directions, divided by the arc it took. A straight reach is zero and a
            // hairpin is large, which is exactly the quantity a bend's turbulence scales with.
            const Vec3 incoming = normalized_or(
                Vec3{static_cast<f32>(vertices[index].position.x - vertices[previous].position.x),
                     0.0F,
                     static_cast<f32>(vertices[index].position.z - vertices[previous].position.z)},
                vertices[index].tangent);
            const Vec3 outgoing = normalized_or(
                Vec3{static_cast<f32>(vertices[next].position.x - vertices[index].position.x), 0.0F,
                     static_cast<f32>(vertices[next].position.z - vertices[index].position.z)},
                vertices[index].tangent);
            const f32 turn = std::acos(std::clamp(dot(incoming, outgoing), -1.0F, 1.0F));
            const f32 sign = (cross(incoming, outgoing).y >= 0.0F) ? 1.0F : -1.0F;
            vertices[index].curvature = sign * turn / span;
            vertices[index].gradient =
                static_cast<f32>((vertices[previous].position.y - vertices[next].position.y) /
                                 static_cast<f64>(span));
        }
    }
}

/// The bend and drop terms `water` requires, added to the authored baseline rather than replacing
/// it: a reach that is authored turbulent stays turbulent when it is straight.
void apply_turbulence(Span<RiverVertex> vertices) noexcept {
    for (RiverVertex& vertex : vertices) {
        const f32 bend = clamp01(std::fabs(vertex.curvature) * vertex.width * 2.0F) * 0.5F;
        const f32 drop = clamp01(std::max(vertex.gradient, 0.0F) * 12.0F) * 0.8F;
        vertex.turbulence = clamp01(vertex.turbulence + bend + drop);
    }
}

}  // namespace

// --- Junctions and discharge ------------------------------------------------------------------

namespace {

/// The vertex of a section nearest a given arc length, and the level there. A junction's
/// constraint is "the tributary's mouth is at the trunk's surface", and this is where that number
/// comes from.
[[nodiscard]] f64 level_at_arc(Span<const RiverVertex> vertices, f32 arc) noexcept {
    if (vertices.empty()) {
        return 0.0;
    }
    for (usize index = 1; index < vertices.size(); ++index) {
        if (vertices[index].arc >= arc) {
            const f32 span = vertices[index].arc - vertices[index - 1].arc;
            const f32 blend = (span > 1e-4F) ? ((arc - vertices[index - 1].arc) / span) : 0.0F;
            return vertices[index - 1].position.y +
                   (static_cast<f64>(blend) *
                    (vertices[index].position.y - vertices[index - 1].position.y));
        }
    }
    return vertices[vertices.size() - 1].position.y;
}

}  // namespace

Status RiverNetwork::resolve_junctions(RiverBuildReport& report) noexcept {
    for (RiverSection& section : sections_) {
        Span<RiverVertex> span(vertices_.data() + section.first_vertex, section.vertex_count);
        derive_geometry(span, section.length);
        const RiverVertex& head = vertices_[section.first_vertex];
        section.discharge = head.width * head.depth * head.speed;
        section.tributary_discharge = 0.0F;
    }

    for (usize index = 0; index < sections_.size(); ++index) {
        RiverSection& tributary = sections_[index];
        if (tributary.joins_section == kNoSection) {
            continue;
        }
        ++report.junctions;
        const RiverSection& trunk = sections_[tributary.joins_section];
        tributary.junction_arc = tributary.joins_at * trunk.length;
        const Span<const RiverVertex> trunk_vertices(vertices_.data() + trunk.first_vertex,
                                                     trunk.vertex_count);
        const f64 target = level_at_arc(trunk_vertices, tributary.junction_arc);

        RiverVertex& mouth = vertices_[tributary.first_vertex + tributary.vertex_count - 1];
        const f64 correction = target - mouth.position.y;
        const auto magnitude = static_cast<f32>(std::fabs(correction));
        if (magnitude > report.largest_junction_correction) {
            report.largest_junction_correction = magnitude;
        }

        // Blended back up the tributary over its last stretch rather than applied at the mouth
        // alone: a step at the last vertex would be a waterfall of exactly the authoring error.
        const f32 blend_length = std::min(20.0F, tributary.length);
        for (u32 offset = 0; offset < tributary.vertex_count; ++offset) {
            RiverVertex& vertex = vertices_[tributary.first_vertex + offset];
            const f32 from_mouth = tributary.length - vertex.arc;
            if (from_mouth > blend_length || blend_length <= 0.0F) {
                continue;
            }
            const f32 weight = 1.0F - (from_mouth / blend_length);
            vertex.position.y += correction * static_cast<f64>(weight);
        }

        // The surface moved, so its gradient did. Re-deriving is cheaper than reasoning about
        // which vertices the blend touched, and it keeps "gradient is a property of the built
        // centreline" true rather than true-except-near-a-junction.
        Span<RiverVertex> span(vertices_.data() + tributary.first_vertex, tributary.vertex_count);
        f32 length = tributary.length;
        derive_geometry(span, length);
        tributary.length = length;
    }
    return ok();
}

void RiverNetwork::propagate_discharge() noexcept {
    // Depth of each section in the forest of junctions: how many sections are between it and open
    // water. A tributary of a tributary must contribute to its own trunk BEFORE that trunk
    // contributes to the main stem, which is what ordering by decreasing depth buys.
    Array<u32> depth(*allocator_);
    if (Status sized = depth.resize(sections_.size()); !sized) {
        return;
    }
    for (usize index = 0; index < sections_.size(); ++index) {
        u32 steps = 0;
        u32 cursor = sections_[index].joins_section;
        // Bounded by the section count: a cycle in an authored network would otherwise spin here,
        // and `add_section()` forbids forward references but not a cycle through several hops.
        while (cursor != kNoSection && steps <= sections_.size()) {
            cursor = sections_[cursor].joins_section;
            ++steps;
        }
        depth[index] = steps;
    }

    Array<u32> order(*allocator_);
    for (u32 index = 0; index < sections_.size(); ++index) {
        if (Status pushed = order.push_back(index); !pushed) {
            return;
        }
    }
    std::stable_sort(order.begin(), order.end(),
                     [&depth](u32 a, u32 b) noexcept { return depth[a] > depth[b]; });

    for (const u32 index : order) {
        const RiverSection& section = sections_[index];
        if (section.joins_section == kNoSection) {
            continue;
        }
        sections_[section.joins_section].tributary_discharge +=
            section.discharge + section.tributary_discharge;
    }
}

void RiverNetwork::apply_continuity(RiverBuildReport& report) noexcept {
    for (usize index = 0; index < sections_.size(); ++index) {
        RiverSection& section = sections_[index];
        const f32 head_speed = vertices_[section.first_vertex].speed;
        for (u32 offset = 0; offset < section.vertex_count; ++offset) {
            RiverVertex& vertex = vertices_[section.first_vertex + offset];
            f32 discharge = section.discharge;
            for (const RiverSection& other : sections_) {
                // A tributary raises the discharge only DOWNSTREAM of where it joins. Above the
                // confluence the trunk carries what it always did, which is the difference between
                // a junction and a global scale factor.
                if (other.joins_section == index && other.junction_arc <= vertex.arc) {
                    discharge += other.discharge + other.tributary_discharge;
                }
            }
            const f32 area = vertex.width * vertex.depth;
            if (area > 1e-4F) {
                vertex.speed = discharge / area;
            }
            if (head_speed > 1e-4F) {
                const f32 ratio = vertex.speed / head_speed;
                if (ratio > report.largest_narrowing_ratio) {
                    report.largest_narrowing_ratio = ratio;
                }
            }
        }
        Span<RiverVertex> span(vertices_.data() + section.first_vertex, section.vertex_count);
        apply_turbulence(span);
    }
}

Status RiverNetwork::build(RiverBuildReport& report) noexcept {
    report = RiverBuildReport{};
    if (authored_.empty()) {
        built_ = true;
        return ok();
    }
    if (Status resampled = resample_sections(report); !resampled) {
        return resampled;
    }
    if (Status resolved = resolve_junctions(report); !resolved) {
        return resolved;
    }
    propagate_discharge();
    apply_continuity(report);
    built_ = true;
    return ok();
}

// --- Sampling --------------------------------------------------------------------------------

RiverNetwork::Projection RiverNetwork::project(const RiverSection& section,
                                               const world::WorldVec3d& at) const noexcept {
    Projection best;
    f32 best_distance = 0.0F;
    for (u32 offset = 0; offset + 1 < section.vertex_count; ++offset) {
        const RiverVertex& from = vertices_[section.first_vertex + offset];
        const RiverVertex& to = vertices_[section.first_vertex + offset + 1];
        const f64 ax = to.position.x - from.position.x;
        const f64 az = to.position.z - from.position.z;
        const f64 length_squared = (ax * ax) + (az * az);
        if (length_squared <= 1e-9) {
            continue;
        }
        const f64 px = at.x - from.position.x;
        const f64 pz = at.z - from.position.z;
        f64 blend = ((px * ax) + (pz * az)) / length_squared;
        blend = (blend < 0.0) ? 0.0 : ((blend > 1.0) ? 1.0 : blend);
        const f64 dx = px - (ax * blend);
        const f64 dz = pz - (az * blend);
        const auto lateral = static_cast<f32>(std::sqrt((dx * dx) + (dz * dz)));
        if (!best.valid || lateral < best_distance) {
            best.valid = true;
            best.vertex = offset;
            best.blend = static_cast<f32>(blend);
            best.lateral = lateral;
            best_distance = lateral;
        }
    }
    return best;
}

RiverVertex RiverNetwork::interpolate(const RiverSection& section,
                                      const Projection& projection) const noexcept {
    const RiverVertex& from = vertices_[section.first_vertex + projection.vertex];
    const u32 next = std::min<u32>(projection.vertex + 1U, section.vertex_count - 1U);
    const RiverVertex& to = vertices_[section.first_vertex + next];
    const f32 t = projection.blend;

    RiverVertex result = from;
    const auto blend = static_cast<f64>(t);
    result.position =
        world::WorldVec3d{from.position.x + ((to.position.x - from.position.x) * blend),
                          from.position.y + ((to.position.y - from.position.y) * blend),
                          from.position.z + ((to.position.z - from.position.z) * blend)};
    result.width = lerp_f32(from.width, to.width, t);
    result.depth = lerp_f32(from.depth, to.depth, t);
    result.speed = lerp_f32(from.speed, to.speed, t);
    result.turbulence = lerp_f32(from.turbulence, to.turbulence, t);
    result.bed_squareness = lerp_f32(from.bed_squareness, to.bed_squareness, t);
    result.curvature = lerp_f32(from.curvature, to.curvature, t);
    result.gradient = lerp_f32(from.gradient, to.gradient, t);
    result.arc = lerp_f32(from.arc, to.arc, t);
    result.tangent = normalized_or(lerp(from.tangent, to.tangent, t), from.tangent);
    return result;
}

Vec3 RiverNetwork::deflect(const world::WorldVec3d& at, Vec3 velocity) const noexcept {
    for (const RiverObstacle& obstacle : obstacles_) {
        const f64 dx = at.x - obstacle.centre.x;
        const f64 dz = at.z - obstacle.centre.z;
        const auto distance = static_cast<f32>(std::sqrt((dx * dx) + (dz * dz)));
        const f32 reach = obstacle.radius + obstacle.influence;
        if (distance >= reach) {
            continue;
        }
        const Vec3 outward = normalized_or(Vec3{static_cast<f32>(dx), 0.0F, static_cast<f32>(dz)},
                                           Vec3{1.0F, 0.0F, 0.0F});
        // Water does not flow into a rock: the inward component is removed entirely inside the
        // radius and progressively outside it, and the speed removed is returned along the
        // obstacle's tangent so the river carries the same discharge past it.
        const f32 falloff =
            1.0F - ((distance - obstacle.radius) / std::max(obstacle.influence, 1e-3F));
        const f32 weight = clamp01(falloff);
        const f32 inward = dot(velocity, outward);
        if (inward < 0.0F) {
            const Vec3 removed = outward * (inward * weight);
            velocity = velocity - removed;
            const Vec3 sideways =
                normalized_or(cross(Vec3{0.0F, 1.0F, 0.0F}, outward), Vec3{0.0F, 0.0F, 1.0F});
            const f32 sense = (dot(velocity, sideways) >= 0.0F) ? 1.0F : -1.0F;
            velocity = velocity + (sideways * (sense * std::fabs(inward) * weight));
        }
    }
    return velocity;
}

RiverSample RiverNetwork::sample(const world::WorldVec3d& at) const noexcept {
    RiverSample sample;
    if (!built_ || sections_.empty()) {
        return sample;
    }

    const RiverSection* best_section = nullptr;
    Projection best;
    f32 best_score = 0.0F;
    for (const RiverSection& section : sections_) {
        const Projection projection = project(section, at);
        if (!projection.valid) {
            continue;
        }
        const RiverVertex vertex = interpolate(section, projection);
        // Scored by lateral distance IN CHANNEL WIDTHS rather than in metres: where a narrow
        // tributary crosses a wide trunk's bounds, the point belongs to whichever channel actually
        // contains it, and a metre-based score would always answer "the trunk".
        const f32 score = projection.lateral / std::max(vertex.width * 0.5F, kMinChannelSize);
        if (best_section == nullptr || score < best_score) {
            best_section = &section;
            best = projection;
            best_score = score;
        }
    }
    if (best_section == nullptr) {
        return sample;
    }

    const RiverVertex vertex = interpolate(*best_section, best);
    const f32 half_width = vertex.width * 0.5F;
    sample.found = true;
    sample.section = static_cast<u32>(best_section - sections_.data());
    sample.inside = best.lateral <= half_width;
    sample.surface = vertex.position.y;
    sample.distance_to_centre = best.lateral;
    sample.distance_to_bank = half_width - best.lateral;
    sample.turbulence = vertex.turbulence;
    sample.width = vertex.width;
    sample.material = vertex.material;
    sample.bed =
        vertex.position.y - static_cast<f64>(vertex.depth * bed_profile(best.lateral / half_width,
                                                                        vertex.bed_squareness));
    sample.velocity = deflect(at, vertex.tangent * vertex.speed);
    return sample;
}

Status RiverNetwork::foam_sources(Array<RiverFoamSource>& out) const noexcept {
    if (!built_) {
        return fail(ErrorCode::Unavailable, "water: the river network has not been built");
    }
    for (usize index = 0; index < sections_.size(); ++index) {
        const RiverSection& section = sections_[index];
        for (u32 offset = 0; offset < section.vertex_count; ++offset) {
            const RiverVertex& vertex = vertices_[section.first_vertex + offset];
            // The threshold is what separates a rapid from a reach. Below it a river is moving
            // water and above it the surface is broken, which is the specification's "river
            // turbulence" and "waterfalls" as one condition over the same number.
            if (vertex.turbulence < 0.25F) {
                continue;
            }
            RiverFoamSource source;
            source.position = vertex.position;
            source.strength = vertex.turbulence;
            source.drift = vertex.tangent * vertex.speed;
            source.section = static_cast<u32>(index);
            if (Status pushed = out.push_back(source); !pushed) {
                return pushed;
            }
        }
        if (section.joins_section == kNoSection) {
            continue;
        }
        // A confluence foams because two flows meet, whatever the curvature of either says.
        const RiverVertex& mouth = vertices_[section.first_vertex + section.vertex_count - 1];
        RiverFoamSource source;
        source.position = mouth.position;
        source.strength = 0.5F;
        source.drift = mouth.tangent * mouth.speed;
        source.section = static_cast<u32>(index);
        if (Status pushed = out.push_back(source); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status RiverNetwork::bounds(f64& min_x, f64& min_z, f64& max_x, f64& max_z) const noexcept {
    if (!built_ || vertices_.empty()) {
        return fail(ErrorCode::Unavailable,
                    "water: the river network has not been built, so it has no extent");
    }
    bool first = true;
    for (const RiverVertex& vertex : vertices_) {
        const f64 half = static_cast<f64>(vertex.width) * 0.5;
        if (first) {
            min_x = vertex.position.x - half;
            max_x = vertex.position.x + half;
            min_z = vertex.position.z - half;
            max_z = vertex.position.z + half;
            first = false;
            continue;
        }
        min_x = std::min(min_x, vertex.position.x - half);
        max_x = std::max(max_x, vertex.position.x + half);
        min_z = std::min(min_z, vertex.position.z - half);
        max_z = std::max(max_z, vertex.position.z + half);
    }
    return ok();
}

}  // namespace cy::water
