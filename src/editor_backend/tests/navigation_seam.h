// SPDX-License-Identifier: MIT
#pragma once
// A NavigationSourceRuntime over in-memory fixtures, and the request and event plumbing the
// navigation and composite service suites share. Issue #28, task 2.3.

#include <cy/abi/cy_abi.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/editor/navigation_service.h>
#include <cy/navigation/bake.h>
#include <cy/test/test.h>

#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "nav_fixture.h"

namespace cy::editor::testing {

[[nodiscard]] inline Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

inline constexpr u32 kWorld = 7;
inline constexpr f32 kTile = 8.0F;

[[nodiscard]] inline Aabb box(f32 x0, f32 z0, f32 x1, f32 z1) noexcept {
    return Aabb::from_min_max(Vec3{x0, -1.0F, z0}, Vec3{x1, 3.0F, z1});
}

/// The bake settings the suites use: the engine back end, 8 m tiles and a 0.25 m cell.
[[nodiscard]] inline navigation::NavBakeSettings bake_settings() noexcept {
    navigation::NavBakeSettings out;
    out.backend = navigation::NavBuildBackend::Engine;
    out.cell_size = 0.25F;
    out.cell_height = 0.2F;
    out.agent_radius = 0.25F;
    out.agent_height = 2.0F;
    out.step_height = 0.4F;
    out.max_slope_degrees = 45.0F;
    out.tile_size = kTile;
    return out;
}

class FixtureSeam final : public NavigationSourceRuntime {
public:
    FixtureSeam() : geometry(allocator()) {}

    navigation::testing::SourceGeometry geometry;
    std::vector<navigation::NavSurfaceVolume> surfaces;
    std::vector<navigation::NavAreaVolume> areas;
    std::vector<navigation::NavObstacleShape> obstacle_list;
    std::vector<navigation::NavLink> link_list;
    std::map<u64, std::vector<u8>> sidecars;
    Ray ray;
    bool ray_available = true;
    u32 gathers = 0;

    Status worlds(Array<u32>& out) noexcept override { return out.push_back(kWorld); }

    Status gather(u32 world, NavSourceBuffers& out) noexcept override {
        ++gathers;
        if (world != kWorld) {
            return fail(ErrorCode::NotFound, "no such world");
        }
        if (Status copied = out.vertices.append(geometry.vertices.span()); !copied) {
            return copied;
        }
        if (Status copied = out.indices.append(geometry.indices.span()); !copied) {
            return copied;
        }
        if (Status copied = out.area.append(geometry.area.span()); !copied) {
            return copied;
        }
        for (const navigation::NavSurfaceVolume& surface : surfaces) {
            if (Status pushed = out.surfaces.push_back(surface); !pushed) {
                return pushed;
            }
        }
        for (const navigation::NavAreaVolume& area : areas) {
            if (Status pushed = out.areas.push_back(area); !pushed) {
                return pushed;
            }
        }
        return ok();
    }

    Status obstacles(u32, Array<navigation::NavObstacleShape>& out) noexcept override {
        for (const navigation::NavObstacleShape& shape : obstacle_list) {
            if (Status pushed = out.push_back(shape); !pushed) {
                return pushed;
            }
        }
        return ok();
    }

    Status links(u32, Array<navigation::NavLink>& out) noexcept override {
        for (const navigation::NavLink& link : link_list) {
            if (Status pushed = out.push_back(link); !pushed) {
                return pushed;
            }
        }
        return ok();
    }

    Status store_bake(u64 identity, Span<const u8> bytes, Array<char>& out_path) noexcept override {
        sidecars[identity] = std::vector<u8>(bytes.begin(), bytes.end());
        const std::string path = "navigation/" + std::to_string(identity) + ".cynavmesh";
        return out_path.append(Span<const char>(path.data(), path.size()));
    }

    Status load_bake(u64 identity, Array<u8>& out) noexcept override {
        const auto found = sidecars.find(identity);
        if (found == sidecars.end()) {
            return fail(ErrorCode::NotFound, "no sidecar under that identity");
        }
        return out.append(Span<const u8>(found->second.data(), found->second.size()));
    }

    Expected<Ray, Error> pick_ray(u32, u64, f32, f32) noexcept override {
        if (!ray_available) {
            return make_unexpected(Error{ErrorCode::Unavailable, "no frame"});
        }
        return ray;
    }
};

/// A 16 m square of ground and one including surface over it: four 8 m tiles.
inline void square_world(FixtureSeam& seam) {
    seam.geometry.ground(0.0F, 0.0F, 16, 16, 1.0F);
    seam.surfaces.push_back({1, box(0.0F, 0.0F, 16.0F, 16.0F), false});
}

/// A 4 m corridor along x over tiles (0,0) and (1,0).
inline void corridor_world(FixtureSeam& seam) {
    seam.geometry.ground(0.0F, 0.0F, 16, 4, 1.0F);
    seam.surfaces.push_back({1, box(0.0F, 0.0F, 16.0F, 4.0F), false});
}

// --- Payloads ---------------------------------------------------------------------------------

class Payload {
public:
    Payload& u8_(u8 value) {
        bytes.push_back(value);
        return *this;
    }
    Payload& u32_(u32 value) {
        for (u32 byte = 0; byte < 4; ++byte) {
            bytes.push_back(static_cast<u8>((value >> (byte * 8U)) & 0xFFU));
        }
        return *this;
    }
    Payload& u64_(u64 value) {
        u32_(static_cast<u32>(value & 0xFFFFFFFFU));
        return u32_(static_cast<u32>(value >> 32U));
    }
    Payload& f32_(f32 value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return u32_(bits);
    }
    Payload& vec3(Vec3 value) { return f32_(value.x).f32_(value.y).f32_(value.z); }
    Payload& aabb(const Aabb& value) { return vec3(value.min).vec3(value.max); }
    Payload& settings(const navigation::NavBakeSettings& value) {
        f32_(value.agent_radius).f32_(value.agent_height).f32_(value.max_slope_degrees);
        f32_(value.step_height).f32_(value.cell_size).f32_(value.cell_height).f32_(value.tile_size);
        return u64_(value.layers).u64_(value.tags).u8_(static_cast<u8>(value.backend));
    }

    std::vector<u8> bytes;
};

/// Reads an event payload. A read past the end sets `overrun`.
class Decoder {
public:
    explicit Decoder(const std::vector<u8>& bytes) : bytes_(&bytes) {}

    u8 u8_() {
        if (!take(1)) {
            return 0;
        }
        return (*bytes_)[cursor_ - 1];
    }
    u32 u32_() {
        if (!take(4)) {
            return 0;
        }
        u32 value = 0;
        for (u32 byte = 0; byte < 4; ++byte) {
            value |= static_cast<u32>((*bytes_)[cursor_ - 4 + byte]) << (byte * 8U);
        }
        return value;
    }
    i32 i32_() {
        const u32 bits = u32_();
        i32 value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    u64 u64_() {
        const u64 low = u32_();
        return low | (static_cast<u64>(u32_()) << 32U);
    }
    f32 f32_() {
        const u32 bits = u32_();
        f32 value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    Vec3 vec3() {
        const f32 x = f32_();
        const f32 y = f32_();
        return Vec3{x, y, f32_()};
    }
    std::string text() {
        const u32 length = u32_();
        if (!take(length)) {
            return {};
        }
        return {reinterpret_cast<const char*>(bytes_->data() + cursor_ - length), length};
    }
    [[nodiscard]] bool done() const { return !overrun && cursor_ == bytes_->size(); }

    bool overrun = false;

private:
    bool take(usize count) {
        if (overrun || bytes_->size() - cursor_ < count) {
            overrun = true;
            return false;
        }
        cursor_ += count;
        return true;
    }

    const std::vector<u8>* bytes_;
    usize cursor_ = 0;
};

/// One event, with its payload copied out of the service's buffer.
struct Event {
    u32 kind = 0;
    u64 request = 0;
    std::vector<u8> payload;

    [[nodiscard]] bool is(CyServiceEventKind expected) const {
        return kind == static_cast<u32>(expected);
    }
    /// The code of a FAILED event.
    [[nodiscard]] std::string code() const {
        Decoder decoder(payload);
        (void)decoder.u32_();
        return decoder.text();
    }
};

inline CyResult submit(abi::EditorServiceBackend& service, CyServiceSession session, u64 id,
                       const char* operation, const Payload& payload = {}) {
    const CyServiceRequest request{sizeof(CyServiceRequest), 1, id, operation, payload.bytes.data(),
                                   payload.bytes.size()};
    return service.submit(session, request);
}

/// One poll; `present` says whether there was an event.
inline Event poll_once(abi::EditorServiceBackend& service, CyServiceSession session,
                       bool& present) {
    CyServiceEvent raw{};
    present = false;
    CY_REQUIRE_EQ(service.poll(session, raw, present), CY_RESULT_OK);
    Event event;
    if (!present) {
        return event;
    }
    event.kind = raw.kind;
    event.request = raw.request_id;
    event.payload.assign(raw.payload, raw.payload + raw.payload_size);
    return event;
}

[[nodiscard]] inline bool is_terminal(const Event& event) {
    return event.is(CY_SERVICE_EVENT_COMPLETED) || event.is(CY_SERVICE_EVENT_FAILED) ||
           event.is(CY_SERVICE_EVENT_CANCELLED);
}

/// Polls until request `id` ends or `limit` polls pass, returning every event seen for it.
inline std::vector<Event> drain(abi::EditorServiceBackend& service, CyServiceSession session,
                                u64 id, u32 limit = 256) {
    std::vector<Event> events;
    for (u32 attempt = 0; attempt < limit; ++attempt) {
        bool present = false;
        Event event = poll_once(service, session, present);
        if (!present || event.request != id) {
            continue;
        }
        events.push_back(event);
        if (is_terminal(event)) {
            break;
        }
    }
    return events;
}

/// Submits `operation` and returns its terminal event.
inline Event call(abi::EditorServiceBackend& service, CyServiceSession session, u64 id,
                  const char* operation, const Payload& payload = {}) {
    CY_REQUIRE_EQ(submit(service, session, id, operation, payload), CY_RESULT_OK);
    std::vector<Event> events = drain(service, session, id);
    CY_REQUIRE_FALSE(events.empty());
    return events.empty() ? Event{} : events.back();
}

[[nodiscard]] inline Payload bake_request(const navigation::NavBakeSettings& settings) {
    Payload payload;
    payload.u32_(kWorld).settings(settings);
    return payload;
}

/// The capabilities list of a `capabilities.get` answer.
[[nodiscard]] inline std::vector<std::string> capability_names(const Event& event, u64& features) {
    Decoder decoder(event.payload);
    std::vector<std::string> names;
    if (decoder.u32_() != 1) {
        return names;
    }
    const u32 count = decoder.u32_();
    for (u32 index = 0; index < count && !decoder.overrun; ++index) {
        names.push_back(decoder.text());
    }
    features = decoder.u64_();
    CY_CHECK(decoder.done());
    return names;
}

}  // namespace cy::editor::testing
