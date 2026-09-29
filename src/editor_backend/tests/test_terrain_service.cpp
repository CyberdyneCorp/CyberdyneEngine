// SPDX-License-Identifier: MIT
// `terrain.evaluate` through the C ABI the editor's live bridge reaches: the engine evaluates the
// editor's brush stack, a stroke changes only its footprint, sending the stack without it (an undo)
// answers the first reply's bytes again, holes reach rendering and collision, and every edited
// region is flagged stale for navigation. Issue #29, "Terrain (finish)".

#include <cy/abi/cy_abi.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/editor/material_service.h>
#include <cy/editor/terrain_service.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Scripting);
}

enum Op : cy::u8 { kRaise = 0, kLower, kSmooth, kFlatten, kPaint, kHole };

struct Dab {
    float x;
    float z;
    float pressure;
};

struct Stroke {
    cy::u64 id;
    Op op;
    bool enabled;
    cy::u8 layer;
    float radius;
    float strength;
    float falloff;
    std::vector<Dab> dabs;
};

void put_u32(std::vector<cy::u8>& out, cy::u32 value) {
    for (int byte = 0; byte < 4; ++byte) {
        out.push_back(static_cast<cy::u8>(value >> (byte * 8)));
    }
}

void put_u64(std::vector<cy::u8>& out, cy::u64 value) {
    put_u32(out, static_cast<cy::u32>(value));
    put_u32(out, static_cast<cy::u32>(value >> 32U));
}

void put_f32(std::vector<cy::u8>& out, float value) {
    cy::u32 bits = 0;
    std::memcpy(&bits, &value, 4);
    put_u32(out, bits);
}

std::vector<cy::u8> request(const std::vector<Stroke>& strokes, cy::u64 terrain = 0x29) {
    std::vector<cy::u8> out;
    put_u32(out, 1);
    put_u64(out, terrain);
    put_u64(out, 0);
    put_u32(out, 2);       // tiles
    put_f32(out, 128.0F);  // extent: one metre between samples
    put_f32(out, 100.0F);  // base height
    put_u32(out, static_cast<cy::u32>(strokes.size()));
    for (const Stroke& stroke : strokes) {
        put_u64(out, stroke.id);
        put_u64(out, 0);
        out.push_back(stroke.op);
        out.push_back(stroke.enabled ? 1 : 0);
        out.push_back(stroke.layer);
        put_f32(out, stroke.radius);
        put_f32(out, stroke.strength);
        put_f32(out, stroke.falloff);
        put_u32(out, static_cast<cy::u32>(stroke.dabs.size()));
        for (const Dab& dab : stroke.dabs) {
            put_f32(out, dab.x);
            put_f32(out, dab.z);
            put_f32(out, dab.pressure);
        }
    }
    return out;
}

cy::u32 read_u32(const cy::u8* bytes) noexcept {
    return static_cast<cy::u32>(bytes[0]) | (static_cast<cy::u32>(bytes[1]) << 8U) |
           (static_cast<cy::u32>(bytes[2]) << 16U) | (static_cast<cy::u32>(bytes[3]) << 24U);
}

float read_f32(const cy::u8* bytes) noexcept {
    const cy::u32 bits = read_u32(bytes);
    float value = 0.0F;
    std::memcpy(&value, &bits, 4);
    return value;
}

/// The reply, decoded the way the editor's `TerrainEvaluation::decode` reads it.
struct Reply {
    cy::u64 generation = 0;
    cy::u32 edge = 0;
    cy::u32 triangles = 0;
    cy::u32 hole_quads = 0;
    cy::u32 collision_holes = 0;
    std::vector<float> stale;  // four per region
    std::vector<cy::u8> heights;
    std::vector<cy::u8> texels;
    std::vector<cy::u8> holes;

    [[nodiscard]] cy::u16 height(cy::u32 x, cy::u32 z) const noexcept {
        const std::size_t at = ((static_cast<std::size_t>(z) * edge) + x) * 2;
        return static_cast<cy::u16>(heights[at] | (heights[at + 1] << 8U));
    }
};

Reply decode(const CyServiceEvent& event) {
    Reply reply;
    const cy::u8* bytes = event.payload;
    CY_REQUIRE(event.payload_size >= 48U);
    CY_REQUIRE_EQ(read_u32(bytes), 1U);
    reply.generation = static_cast<cy::u64>(read_u32(bytes + 4)) |
                       (static_cast<cy::u64>(read_u32(bytes + 8)) << 32U);
    reply.edge = read_u32(bytes + 12);
    reply.triangles = read_u32(bytes + 28);
    reply.hole_quads = read_u32(bytes + 32);
    reply.collision_holes = read_u32(bytes + 36);
    const cy::u32 stale = read_u32(bytes + 40);
    std::size_t cursor = 44;
    for (cy::u32 index = 0; index < stale * 4; ++index) {
        reply.stale.push_back(read_f32(bytes + cursor));
        cursor += 4;
    }
    const std::size_t heights = static_cast<std::size_t>(reply.edge) * reply.edge * 2;
    const std::size_t quads = static_cast<std::size_t>(reply.edge - 1) * (reply.edge - 1);
    CY_REQUIRE_EQ(event.payload_size, cursor + heights + (quads * 8) + quads);
    reply.heights.assign(bytes + cursor, bytes + cursor + heights);
    cursor += heights;
    reply.texels.assign(bytes + cursor, bytes + cursor + (quads * 8));
    cursor += quads * 8;
    reply.holes.assign(bytes + cursor, bytes + cursor + quads);
    return reply;
}

/// One ABI session over the backend, as the hosted runtime holds it.
class Session {
public:
    Session() : host_(allocator()), service_(allocator()) {
        host_.bind_editor_service(&service_);
        api_ = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
        CY_REQUIRE(api_ != nullptr);
        CY_REQUIRE_EQ(api_->service_open(&host_, &session_), CY_RESULT_OK);
    }
    ~Session() { api_->service_close(&host_, session_); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    CyServiceEvent submit(const std::vector<cy::u8>& payload) {
        const CyServiceRequest submitted{sizeof(CyServiceRequest),
                                         1,
                                         ++request_,
                                         "terrain.evaluate",
                                         payload.data(),
                                         payload.size()};
        CY_REQUIRE_EQ(api_->service_submit(&host_, session_, &submitted), CY_RESULT_OK);
        CyServiceEvent event{};
        bool present = false;
        CY_REQUIRE_EQ(api_->service_poll(&host_, session_, &event, &present), CY_RESULT_OK);
        CY_REQUIRE(present);
        return event;
    }

    Reply evaluate(const std::vector<Stroke>& strokes) {
        const CyServiceEvent event = submit(request(strokes));
        CY_REQUIRE_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
        return decode(event);
    }

    [[nodiscard]] CyServiceSession raw() const noexcept { return session_; }

private:
    cy::abi::Host host_;
    cy::editor::MaterialService service_;
    const CyInterface* api_ = nullptr;
    CyServiceSession session_ = nullptr;
    cy::u64 request_ = 0;
};

Stroke stroke(cy::u64 id, Op op) {
    return Stroke{id,   op,   true, op == kPaint ? cy::u8{1} : cy::u8{0},
                  8.0F, 0.8F, 0.5F, {{0.4F, 0.3F, 1.0F}, {0.5F, 0.35F, 1.0F}, {0.6F, 0.4F, 0.7F}}};
}

/// Samples farther than the radius from every dab, in metres over the 128 m region.
bool outside(const Stroke& brush, cy::u32 x, cy::u32 z) noexcept {
    return std::ranges::all_of(brush.dabs, [&](const Dab& dab) {
        const double dx = static_cast<double>(x) - (static_cast<double>(dab.x) * 128.0);
        const double dz = static_cast<double>(z) - (static_cast<double>(dab.z) * 128.0);
        return std::sqrt((dx * dx) + (dz * dz)) >= static_cast<double>(brush.radius);
    });
}

}  // namespace

CY_TEST_CASE("terrain.evaluate: the engine answers the brush stack with its heights and texels") {
    Session session;
    const Reply flat = session.evaluate({});
    CY_CHECK_EQ(flat.edge, 129U);
    CY_CHECK_EQ(flat.generation, 1U);
    CY_CHECK_EQ(flat.triangles, 4U * 64U * 64U * 2U);
    CY_CHECK_EQ(flat.hole_quads, 0U);
    CY_CHECK(flat.stale.empty());
    CY_CHECK_EQ(flat.height(0, 0), flat.height(128, 128));  // no noise: one flat base

    for (const Op op : {kRaise, kLower}) {
        const Stroke brush = stroke(1, op);
        const Reply after = session.evaluate({brush});
        cy::u32 inside = 0;
        cy::u32 outside_changed = 0;
        for (cy::u32 z = 0; z < after.edge; ++z) {
            for (cy::u32 x = 0; x < after.edge; ++x) {
                if (after.height(x, z) == flat.height(x, z)) {
                    continue;
                }
                if (outside(brush, x, z)) {
                    ++outside_changed;
                } else {
                    ++inside;
                    CY_REQUIRE((op == kRaise) == (after.height(x, z) > flat.height(x, z)));
                }
            }
        }
        CY_CHECK_EQ(outside_changed, 0U);
        CY_CHECK_GT(inside, 100U);
    }
}

CY_TEST_CASE("terrain.evaluate: the stack without a stroke answers the earlier bytes exactly") {
    Session session;
    const std::vector<Stroke> base{stroke(1, kRaise), stroke(2, kPaint)};
    const Reply before = session.evaluate(base);
    for (const Op op : {kRaise, kLower, kSmooth, kFlatten, kPaint, kHole}) {
        std::vector<Stroke> stroked = base;
        stroked.push_back(stroke(3, op));
        stroked.back().dabs = {{0.45F, 0.33F, 1.0F}, {0.55F, 0.36F, 1.0F}};
        const Reply after = session.evaluate(stroked);
        const Reply undone = session.evaluate(base);
        CY_TEST_MESSAGE(static_cast<int>(op));
        const bool changed = (after.heights != before.heights) || (after.texels != before.texels) ||
                             (after.holes != before.holes);
        CY_CHECK(changed);
        CY_CHECK(undone.heights == before.heights);
        CY_CHECK(undone.texels == before.texels);
        CY_CHECK(undone.holes == before.holes);
        CY_CHECK_GT(undone.generation, after.generation);
    }
}

CY_TEST_CASE("terrain.evaluate: a hole stroke leaves rendering and collision open") {
    Session session;
    const Reply before = session.evaluate({});
    const Reply after = session.evaluate({stroke(1, kHole)});
    cy::u32 holes = 0;
    for (const cy::u8 hole : after.holes) {
        holes += hole;
    }
    CY_CHECK_GT(holes, 100U);
    CY_CHECK_EQ(after.hole_quads, holes);
    CY_CHECK_EQ(before.triangles - after.triangles, 2U * holes);
    CY_CHECK_GT(after.collision_holes, 0U);
    CY_CHECK(after.heights == before.heights);
}

CY_TEST_CASE("terrain.evaluate: every edited region is flagged stale for navigation") {
    Session session;
    CY_CHECK(session.evaluate({}).stale.empty());  // nothing baked yet to be stale against
    const Stroke brush = stroke(7, kRaise);
    const Reply stroked = session.evaluate({brush});
    CY_REQUIRE_EQ(stroked.stale.size(), 4U);
    // The stroke's reach: its dabs' extent plus the radius, in metres.
    CY_CHECK_NEAR(stroked.stale[0], (0.4F * 128.0F) - 8.0F, 0.001F);
    CY_CHECK_NEAR(stroked.stale[1], (0.3F * 128.0F) - 8.0F, 0.001F);
    CY_CHECK_NEAR(stroked.stale[2], (0.6F * 128.0F) + 8.0F, 0.001F);
    CY_CHECK_NEAR(stroked.stale[3], (0.4F * 128.0F) + 8.0F, 0.001F);

    // An unchanged stack marks nothing more; an undo marks the same region, already stale.
    CY_CHECK_EQ(session.evaluate({brush}).stale.size(), 4U);
    CY_CHECK_EQ(session.evaluate({}).stale.size(), 4U);

    // A second stroke elsewhere adds its own region.
    Stroke far = stroke(8, kLower);
    far.dabs = {{0.97F, 0.97F, 1.0F}};
    const Reply two = session.evaluate({far});
    CY_REQUIRE_EQ(two.stale.size(), 8U);
    CY_CHECK_EQ(two.stale[6], 128.0F);  // clipped to the region

    // A stroke present at the first evaluation marks nothing; removing it marks its reach.
    Session other;
    CY_CHECK(other.evaluate({brush}).stale.empty());
    CY_CHECK_EQ(other.evaluate({}).stale.size(), 4U);

    // The engine keeps the stale set for the host, which #28's rebake will clear.
    const cy::editor::TerrainPreview* preview =
        cy::editor::MaterialService::terrain_preview(session.raw());
    CY_REQUIRE(preview != nullptr);
    CY_CHECK_EQ(preview->stale_navigation().size(), 2U);
    CY_CHECK_EQ(preview->generation(), two.generation);
}

CY_TEST_CASE("terrain.evaluate: flatten levels toward the ground under its first dab") {
    Session session;
    // Not named `raise`: on non-x86 Linux doctest breaks into the debugger with raise(SIGTRAP).
    Stroke mound = stroke(1, kRaise);
    mound.radius = 20.0F;
    mound.strength = 1.0F;
    mound.falloff = 1.0F;  // soft all the way in, so the hill has a slope under the flatten
    mound.dabs = {{0.5F, 0.5F, 1.0F}};
    const Reply hill = session.evaluate({mound});
    Stroke flatten = stroke(2, kFlatten);
    flatten.strength = 1.0F;
    flatten.falloff = 0.0F;
    flatten.dabs = {{0.5F, 0.5F, 1.0F}, {0.56F, 0.5F, 1.0F}};
    const Reply level = session.evaluate({mound, flatten});
    const cy::u16 target = hill.height(64, 64);
    // Inside the flatten's hard disc every sample is the ground under its first dab.
    CY_CHECK_EQ(level.height(64, 64), target);
    CY_CHECK_EQ(level.height(70, 64), target);
    CY_CHECK_NE(hill.height(70, 64), target);
}

CY_TEST_CASE("terrain.evaluate: a malformed request is refused by name and changes nothing") {
    Session session;
    const Reply first = session.evaluate({stroke(1, kRaise)});
    for (const std::vector<cy::u8>& bad : {std::vector<cy::u8>{1, 0, 0},
                                           [] {
                                               std::vector<cy::u8> out =
                                                   request({stroke(1, kRaise)});
                                               out.push_back(0);  // trailing byte
                                               return out;
                                           }(),
                                           [] {
                                               Stroke outside_region = stroke(1, kRaise);
                                               outside_region.dabs = {{1.5F, 0.5F, 1.0F}};
                                               return request({outside_region});
                                           }(),
                                           [] {
                                               Stroke wide = stroke(1, kRaise);
                                               wide.radius = 0.0F;
                                               return request({wide});
                                           }()}) {
        const CyServiceEvent event = session.submit(bad);
        CY_CHECK_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_FAILED));
        CY_REQUIRE(event.payload_size >= 8U);
        const std::string_view code(reinterpret_cast<const char*>(event.payload + 8),
                                    read_u32(event.payload + 4));
        CY_CHECK_EQ(code, std::string_view("terrain.evaluate"));
    }
    const Reply next = session.evaluate({stroke(1, kRaise)});
    CY_CHECK_EQ(next.generation, first.generation + 1);
    CY_CHECK(next.heights == first.heights);
}

namespace {

std::vector<cy::u8> read_fixture(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    CY_REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}  // namespace

// The editor's panel snapshots draw this reply: the engine's answer to the requests the editor
// sends for its scripted strokes (`cy-editor-shell/tests/panel_snapshots.rs`, which checks the
// requests). This case holds the committed reply to what the engine computes, so the picture cannot
// drift from the engine. `CY_TERRAIN_FIXTURE=write` rewrites it instead.
CY_TEST_CASE("terrain.evaluate: the committed panel fixture is what the engine answers") {
    const std::string directory =
        std::string(CY_SOURCE_DIR) + "/editor/crates/cy-editor-shell/tests/fixtures/";
    Session session;
    const CyServiceEvent before =
        session.submit(read_fixture(directory + "terrain-tools-before.request"));
    CY_REQUIRE_EQ(before.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
    const CyServiceEvent after = session.submit(read_fixture(directory + "terrain-tools.request"));
    CY_REQUIRE_EQ(after.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
    const std::vector<cy::u8> reply(after.payload, after.payload + after.payload_size);
    const Reply decoded = decode(after);
    CY_CHECK_EQ(decoded.stale.size(), 4U);  // the last stroke, a hole, made its reach stale
    CY_CHECK_GT(decoded.hole_quads, 0U);
    const char* mode = std::getenv("CY_TERRAIN_FIXTURE");
    if (mode != nullptr && std::string_view(mode) == "write") {
        std::ofstream output(directory + "terrain-tools.reply", std::ios::binary);
        output.write(reinterpret_cast<const char*>(reply.data()),
                     static_cast<std::streamsize>(reply.size()));
        CY_REQUIRE(output.good());
        return;
    }
    CY_CHECK(read_fixture(directory + "terrain-tools.reply") == reply);
}
