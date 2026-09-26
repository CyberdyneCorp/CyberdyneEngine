// The committed scene, and the content it names. M11.c section 7. See shot.h.

#include "shot.h"

#include "embers.h"

#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/import/primitive.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cy::sample::beauty {
namespace {

/// One whitespace-separated token, consumed from the front.
[[nodiscard]] std::string_view take(std::string_view& rest) noexcept {
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t' || rest.front() == '\r')) {
        rest.remove_prefix(1);
    }
    const usize end = rest.find_first_of(" \t\r");
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
    return std::strtoull(held.c_str(), nullptr, 0);
}

[[nodiscard]] Vec3 to_vec3(std::string_view& rest) noexcept {
    const f32 x = to_float(take(rest));
    const f32 y = to_float(take(rest));
    const f32 z = to_float(take(rest));
    return Vec3{x, y, z};
}

/// The material of this key, appending one if it is new. The file may name a material's graph and
/// its three textures on four separate lines, in any order.
[[nodiscard]] ShotMaterial& material_for(Shot& shot, std::string_view key) {
    for (ShotMaterial& material : shot.materials) {
        if (material.key == key) {
            return material;
        }
    }
    shot.materials.push_back(ShotMaterial{});
    shot.materials.back().key = std::string(key);
    return shot.materials.back();
}

void read_camera(Shot& shot, std::string_view field, std::string_view& rest) {
    if (field == "position") {
        shot.camera_position = to_vec3(rest);
    } else if (field == "target") {
        shot.camera_target = to_vec3(rest);
    } else if (field == "fov") {
        shot.field_of_view_degrees = to_float(take(rest));
    } else if (field == "near") {
        shot.near_plane = to_float(take(rest));
    }
}

void read_sun(Shot& shot, std::string_view field, std::string_view& rest) {
    if (field == "elevation") {
        shot.sun_elevation_degrees = to_float(take(rest));
    } else if (field == "azimuth") {
        shot.sun_azimuth_degrees = to_float(take(rest));
    } else if (field == "shadow-extent") {
        shot.shadow_extent_metres = to_float(take(rest));
    } else if (field == "shadow-bias") {
        shot.shadow_bias = to_float(take(rest));
    } else if (field == "shadow-normal-offset") {
        shot.shadow_normal_offset = to_float(take(rest));
    }
}

void read_occlusion(Shot& shot, std::string_view field, std::string_view& rest) {
    if (field == "radius") {
        shot.occlusion_radius = to_float(take(rest));
    } else if (field == "power") {
        shot.occlusion_power = to_float(take(rest));
    }
}

void read_sky(Shot& shot, std::string_view field, std::string_view& rest) {
    if (field == "cloud-cover") {
        shot.cloud_cover = to_float(take(rest));
    } else if (field == "cloud-density") {
        shot.cloud_density = to_float(take(rest));
    } else if (field == "cloud-steps") {
        shot.cloud_steps = static_cast<u32>(to_unsigned(take(rest)));
    } else if (field == "seed") {
        shot.seed = to_unsigned(take(rest));
    }
}

void read_bloom(Shot& shot, std::string_view field, std::string_view& rest) {
    if (field == "threshold-stops") {
        shot.bloom_threshold_stops = to_float(take(rest));
    } else if (field == "knee") {
        shot.bloom_knee = to_float(take(rest));
    } else if (field == "intensity") {
        shot.bloom_intensity = to_float(take(rest));
    } else if (field == "scatter") {
        shot.bloom_scatter = to_float(take(rest));
    } else if (field == "levels") {
        shot.bloom_levels = static_cast<u32>(to_unsigned(take(rest)));
    }
}

void read_material(Shot& shot, std::string_view& rest) {
    const std::string_view key = take(rest);
    const std::string_view field = take(rest);
    const std::string path(take(rest));
    ShotMaterial& material = material_for(shot, key);
    if (field == "graph") {
        material.graph_path = path;
    } else if (field == "albedo") {
        material.albedo_path = path;
    } else if (field == "normal") {
        material.normal_path = path;
    } else if (field == "data") {
        material.data_path = path;
    }
}

void read_instance(Shot& shot, std::string_view& rest) {
    Instance instance;
    instance.mesh = std::string(take(rest));
    instance.material = std::string(take(rest));
    while (!rest.empty()) {
        const std::string_view field = take(rest);
        if (field.empty()) {
            break;
        }
        if (field == "at") {
            instance.position = to_vec3(rest);
        } else if (field == "yaw") {
            instance.yaw_degrees = to_float(take(rest));
        } else if (field == "uv") {
            instance.uv_scale = to_float(take(rest));
        } else if (field == "normal") {
            instance.normal_strength = to_float(take(rest));
        }
    }
    shot.instances.push_back(instance);
}

/// Does the scene's camera and grade agree with `embers.h`'s copy of them?
///
/// A TOLERANCE OF ONE PART IN TEN THOUSAND rather than equality: both sides are decimal literals
/// that pass through `strtod` and a narrowing to `f32`, and a comparison that failed on the last
/// bit would refuse a file nobody had edited.
[[nodiscard]] bool near_enough(f32 a, f32 b) noexcept {
    const f32 delta = a > b ? a - b : b - a;
    return delta <= 1.0e-4F;
}

[[nodiscard]] bool camera_matches_the_air(const Shot& shot, std::string& problem) {
    const auto agree = [](Vec3 a, Vec3 b) noexcept {
        return near_enough(a.x, b.x) && near_enough(a.y, b.y) && near_enough(a.z, b.z);
    };
    const char* which = nullptr;
    if (!agree(shot.camera_position, kShotEye)) {
        which = "camera position";
    } else if (!agree(shot.camera_target, kShotTarget)) {
        which = "camera target";
    } else if (!near_enough(shot.field_of_view_degrees, kShotFovDegrees)) {
        which = "camera fov";
    } else if (!near_enough(shot.near_plane, kShotNearPlane)) {
        which = "camera near";
    } else if (!near_enough(shot.exposure_stops, kShotExposureStops)) {
        which = "exposure-stops";
    }
    if (which == nullptr) {
        return true;
    }
    problem = std::string("the scene's `") + which +
              "` disagrees with samples/12-beauty/embers.h, which holds the same numbers so that "
              "`render.vfx` can photograph the air from where this shot sees it. Move both.";
    return false;
}

}  // namespace

Expected<Shot, Error> Shot::read(const char* path, std::string& problem) {
    Allocator& memory = system_allocator(MemoryDomain::Assets);
    Array<u8> bytes(memory);
    if (Status opened = assets::fs::read_whole(path, bytes); !opened) {
        problem = std::string("cannot read ") + path;
        return make_unexpected(opened.error());
    }

    Shot shot;
    std::string_view rest(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    u32 line_number = 0;
    bool saw_header = false;
    while (!rest.empty()) {
        const usize newline = rest.find('\n');
        std::string_view line = rest.substr(0, newline);
        rest.remove_prefix(newline == std::string_view::npos ? rest.size() : newline + 1);
        ++line_number;
        const std::string_view keyword = take(line);
        if (keyword.empty() || keyword.front() == '#') {
            continue;
        }
        if (keyword == "cyshot") {
            saw_header = to_unsigned(take(line)) == 1;
            if (!saw_header) {
                problem = "a `.cyshot` written at a version this build does not read";
                return make_unexpected(Error{ErrorCode::Unsupported, "shot version", line_number});
            }
            continue;
        }
        if (!saw_header) {
            problem = "the first line is not `cyshot 1`";
            return make_unexpected(Error{ErrorCode::InvalidArgument, "shot header", line_number});
        }
        if (keyword == "name") {
            shot.name = std::string(line.substr(line.find_first_not_of(" \t")));
        } else if (keyword == "camera") {
            read_camera(shot, take(line), line);
        } else if (keyword == "sun") {
            read_sun(shot, take(line), line);
        } else if (keyword == "sky") {
            read_sky(shot, take(line), line);
        } else if (keyword == "ambient-occlusion") {
            const std::string_view field = take(line);
            read_occlusion(shot, field, line);
        } else if (keyword == "exposure-stops") {
            shot.exposure_stops = to_float(take(line));
        } else if (keyword == "bloom") {
            read_bloom(shot, take(line), line);
        } else if (keyword == "material") {
            read_material(shot, line);
        } else if (keyword == "mesh") {
            const std::string key(take(line));
            shot.meshes.emplace_back(key, std::string(take(line)));
        } else if (keyword == "instance") {
            read_instance(shot, line);
        } else {
            problem =
                std::string("a keyword the shot format does not define: ") + std::string(keyword);
            return make_unexpected(Error{ErrorCode::InvalidArgument, "shot keyword", line_number});
        }
    }

    // EVERY REFUSAL IS ABOUT SOMETHING THE PICTURE WOULD HAVE BEEN WRONG ABOUT rather than about
    // tidiness: an instance naming a mesh nobody declared draws nothing and leaves a hole, and an
    // instance naming a material nobody declared would be shaded by whatever was bound last.
    if (shot.instances.empty()) {
        problem = "the shot places nothing";
        return make_unexpected(Error{ErrorCode::InvalidArgument, "empty shot", 0});
    }
    for (const Instance& instance : shot.instances) {
        bool found = false;
        for (const auto& mesh : shot.meshes) {
            found = found || mesh.first == instance.mesh;
        }
        if (!found) {
            problem = "an instance names a mesh the shot does not declare: " + instance.mesh;
            return make_unexpected(Error{ErrorCode::InvalidArgument, "unknown mesh", 0});
        }
        if (shot.material(instance.material) == nullptr) {
            problem =
                "an instance names a material the shot does not declare: " + instance.material;
            return make_unexpected(Error{ErrorCode::InvalidArgument, "unknown material", 0});
        }
    }
    for (const ShotMaterial& material : shot.materials) {
        if (material.graph_path.empty() || material.albedo_path.empty() ||
            material.normal_path.empty() || material.data_path.empty()) {
            problem =
                "material `" + material.key + "` is missing its graph or one of its three textures";
            return make_unexpected(Error{ErrorCode::InvalidArgument, "incomplete material", 0});
        }
    }

    // THE ONE PLACE THE SCENE AND THE AIR COULD DISAGREE, and it refuses rather than drifts.
    //
    // `embers.h` holds the camera and the grade as constants, because a suite has to photograph the
    // air from where the shot sees it without a working directory or a parser — and two copies of a
    // number that CAN drift are a defect waiting for somebody to move the camera. So this is the
    // check that makes them one number: move the camera in the scene file and the capture refuses,
    // naming the header to move it in as well.
    if (!camera_matches_the_air(shot, problem)) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "camera disagrees with embers.h", 0});
    }
    return shot;
}

const ShotMaterial* Shot::material(std::string_view key) const noexcept {
    for (const ShotMaterial& material : materials) {
        if (material.key == key) {
            return &material;
        }
    }
    return nullptr;
}

Expected<import::MeshData, Error> load_primitive(const char* path, std::string& problem) {
    Allocator& memory = system_allocator(MemoryDomain::Assets);
    Array<u8> bytes(memory);
    if (Status opened = assets::fs::read_whole(path, bytes); !opened) {
        problem = std::string("cannot read ") + path;
        return make_unexpected(opened.error());
    }
    u32 line = 0;
    auto spec = import::parse_primitive_source(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &line);
    if (!spec) {
        problem =
            std::string(path) + ": line " + std::to_string(line) + ": " + spec.error().message;
        return make_unexpected(spec.error());
    }

    import::MeshData mesh;
    if (Status built = import::build_primitive_mesh(spec.value(), mesh); !built) {
        problem = std::string(path) + ": " + built.error().message;
        return make_unexpected(built.error());
    }
    // THE TANGENT BASIS IS THE ENGINE'S and is generated here rather than assumed:
    // `generate_tangents` refuses a mesh with no normals or no UVs by name, and a normal map
    // applied against an arbitrary basis is "subtly wrong everywhere" — mesh.h's own words.
    if (mesh.normals.empty()) {
        if (Status made = import::generate_normals(mesh, 60.0F); !made) {
            problem = std::string(path) + ": " + made.error().message;
            return make_unexpected(made.error());
        }
    }
    if (Status made = import::generate_tangents(mesh); !made) {
        problem = std::string(path) + ": " + made.error().message;
        return make_unexpected(made.error());
    }
    return mesh;
}

}  // namespace cy::sample::beauty
