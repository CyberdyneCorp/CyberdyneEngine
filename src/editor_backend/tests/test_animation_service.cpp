// SPDX-License-Identifier: MIT
// The animation panel, engine side (#29, animation): the pose vocabulary, the compiler's answer,
// and the preview character, as the editor reaches them through `animation.*`.
//
// THE WIRE, FROM BOTH SIDES. `data/animation_locomotion_v1.cyanimgraph` is the graph the Rust
// editor's MCP tools write for the acceptance scenario, and `..._edited_v1.cyanimgraph` the same
// graph after its idle-to-walk blend was lengthened on the canvas; the Rust suites compare their
// files with these byte for byte. `data/animation_preview_request_v1.wire` is the preview its
// encoder writes. This suite compiles and previews exactly those bytes, and checks every previewed
// pose against the program evaluated DIRECTLY — compiled here, bound to the character's skeleton
// and clips, advanced and evaluated through `cy::animation` with no service in between.
// `data/animation_catalogue_v1.wire`, `animation_compile_v1.wire`, `animation_compile_cut_v1.wire`,
// `animation_preview_state_v1.wire` and `animation_preview_stopped_v1.wire` are the engine's
// answers, which the Rust suites decode and replay to an MCP client as the runtime's. Regenerate
// them with `CY_UPDATE_ANIMATION_WIRE=1` after a deliberate change to a reply.

#include <cy/abi/cy_abi.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/editor/animation_service.h>
#include <cy/editor/material_service.h>
#include <cy/graph/text.h>
#include <cy/test/test.h>
#if defined(CY_EDITOR_HAS_ANIMATION)
#    include <cy/animation/cooked.h>
#    include <cy/animation/evaluate.h>
#    include <cy/core/assets/cooked.h>
#    include <cy/editor/animation_preview.h>
#    include <cy/editor/animation_rig.h>
#endif
#if defined(CY_EDITOR_HAS_ANIMATION) && defined(CY_EDITOR_TEST_HAS_IMPORT)
#    include <cy/import/gltf.h>
#    include <cy/import/mesh.h>

#    include "animation_character_fixture.h"
#endif

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace cy;

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Animation);
}

constexpr const char* kGraph = "animation_locomotion_v1.cyanimgraph";
constexpr const char* kEdited = "animation_locomotion_edited_v1.cyanimgraph";

std::filesystem::path wire(std::string_view name) {
    return std::filesystem::path(CY_ANIMATION_WIRE_DIR) / name;
}

std::string read_file(std::string_view name) {
    std::ifstream input(wire(name), std::ios::binary);
    CY_REQUIRE(input.good());
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

std::string text_of(const Array<u8>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// The committed reply; with `CY_UPDATE_ANIMATION_WIRE=1`, written from `reply` first.
std::string committed(std::string_view name, const Array<u8>& reply) {
    const char* update = std::getenv("CY_UPDATE_ANIMATION_WIRE");
    if (update != nullptr && std::string_view(update) == "1") {
        std::ofstream(wire(name), std::ios::binary)
            .write(reinterpret_cast<const char*>(reply.data()),
                   static_cast<std::streamsize>(reply.size()));
    }
    return read_file(name);
}

std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const usize at = text.find(from);
    CY_REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}

/// A front-to-back reader over a reply, failing the case on a short read.
class Reply {
public:
    explicit Reply(std::string bytes) : bytes_(std::move(bytes)) {}

    u8 u8v() { return static_cast<u8>(take(1)[0]); }
    u32 u32v() {
        u32 value = 0;
        std::memcpy(&value, take(4).data(), 4);
        return value;
    }
    u64 u64v() {
        u64 value = 0;
        std::memcpy(&value, take(8).data(), 8);
        return value;
    }
    f32 f32v() {
        f32 value = 0.0F;
        std::memcpy(&value, take(4).data(), 4);
        return value;
    }
    f64 f64v() {
        f64 value = 0.0;
        std::memcpy(&value, take(8).data(), 8);
        return value;
    }
    std::string text() { return std::string(take(u32v())); }
    [[nodiscard]] bool done() const { return at_ == bytes_.size(); }

private:
    std::string_view take(usize count) {
        CY_REQUIRE(bytes_.size() - at_ >= count);
        const std::string_view out(bytes_.data() + at_, count);
        at_ += count;
        return out;
    }

    std::string bytes_;
    usize at_ = 0;
};

/// A request payload, little-endian, as the Rust editor writes it.
class Request {
public:
    Request() : bytes_(allocator()) {}

    Request& u8v(u8 value) {
        put(&value, 1);
        return *this;
    }
    Request& u32v(u32 value) {
        put(&value, 4);
        return *this;
    }
    Request& u64v(u64 value) {
        put(&value, 8);
        return *this;
    }
    Request& f32v(f32 value) {
        put(&value, 4);
        return *this;
    }
    Request& text(std::string_view value) {
        const auto size = static_cast<u32>(value.size());
        put(&size, 4);
        put(value.data(), value.size());
        return *this;
    }
    [[nodiscard]] Array<u8> take() { return std::move(bytes_); }

private:
    void put(const void* data, usize size) {
        CY_REQUIRE(bytes_.append({static_cast<const u8*>(data), size}).has_value());
    }

    Array<u8> bytes_;
};

Array<u8> compile_request(std::string_view source) {
    Request request;
    request.u32v(1).text(source);
    return request.take();
}

Array<u8> preview_request(std::string_view source, u64 focus, f32 time,
                          const std::vector<std::pair<std::string, f32>>& parameters = {}) {
    Request request;
    request.u32v(1).text(source).u64v(focus).f32v(time).u8v(0);
    request.u32v(static_cast<u32>(parameters.size()));
    for (const auto& [name, value] : parameters) {
        request.text(name).f32v(value);
    }
    return request.take();
}

struct Diagnosed {
    u8 severity = 0;
    std::string code;
    u64 node = 0;
    std::string detail;
};

struct Compiled {
    bool compiled = false;
    u64 program = 0;
    std::vector<std::string> states;
    std::vector<f32> durations;
    std::vector<std::pair<std::string, u32>> clips;
    std::vector<std::pair<std::string, u8>> parameters;
    std::vector<Diagnosed> diagnostics;
};

Compiled decode_compile(const std::string& bytes) {
    Reply reply(bytes);
    Compiled out;
    CY_REQUIRE_EQ(reply.u32v(), 1U);
    out.compiled = reply.u8v() != 0;
    (void)reply.u64v();
    out.program = reply.u64v();
    (void)reply.u32v();
    (void)reply.u32v();
    for (u32 count = reply.u32v(); count > 0; --count) {
        out.states.push_back(reply.text());
        (void)reply.u64v();
        (void)reply.u32v();
    }
    for (u32 count = reply.u32v(); count > 0; --count) {
        (void)reply.u64v();
        (void)reply.u32v();
        (void)reply.u32v();
        (void)reply.text();
        out.durations.push_back(reply.f32v());
        (void)reply.u32v();
        (void)reply.u8v();
    }
    for (u32 count = reply.u32v(); count > 0; --count) {
        std::string name = reply.text();
        (void)reply.f32v();
        (void)reply.u8v();
        (void)reply.u8v();
        out.clips.emplace_back(std::move(name), reply.u32v());
    }
    for (u32 count = reply.u32v(); count > 0; --count) {
        std::string name = reply.text();
        out.parameters.emplace_back(std::move(name), reply.u8v());
    }
    for (u32 count = reply.u32v(); count > 0; --count) {
        Diagnosed diagnostic;
        diagnostic.severity = reply.u8v();
        diagnostic.code = reply.text();
        diagnostic.node = reply.u64v();
        (void)reply.text();
        (void)reply.text();
        diagnostic.detail = reply.text();
        (void)reply.u64v();
        out.diagnostics.push_back(std::move(diagnostic));
    }
    CY_CHECK(reply.done());
    return out;
}

struct Fired {
    std::string name;
    f32 at = 0.0F;
};

struct Shown {
    bool active = false;
    bool playing = false;
    u64 focus = 0;
    std::string clip;
    f32 time = 0.0F;
    f32 length = 0.0F;
    u32 state = 0;
    std::string state_name;
    u32 target = 0;
    std::string target_name;
    f32 blend = 0.0F;
    u64 pose_digest = 0;
    std::vector<Transform> joints;
    std::vector<Fired> events;
};

Shown decode_state(const std::string& bytes) {
    Reply reply(bytes);
    Shown out;
    CY_REQUIRE_EQ(reply.u32v(), 1U);
    out.active = reply.u8v() != 0;
    out.playing = reply.u8v() != 0;
    out.focus = reply.u64v();
    out.clip = reply.text();
    out.time = reply.f32v();
    out.length = reply.f32v();
    out.state = reply.u32v();
    out.state_name = reply.text();
    out.target = reply.u32v();
    out.target_name = reply.text();
    out.blend = reply.f32v();
    (void)reply.u64v();
    out.pose_digest = reply.u64v();
    (void)reply.u64v();
    for (u32 count = reply.u32v(); count > 0; --count) {
        Transform joint;
        joint.translation = Vec3{reply.f32v(), reply.f32v(), reply.f32v()};
        joint.rotation.x = reply.f32v();
        joint.rotation.y = reply.f32v();
        joint.rotation.z = reply.f32v();
        joint.rotation.w = reply.f32v();
        joint.scale = Vec3{reply.f32v(), reply.f32v(), reply.f32v()};
        out.joints.push_back(joint);
    }
    for (u32 count = reply.u32v(); count > 0; --count) {
        Fired fired;
        (void)reply.u64v();
        fired.name = reply.text();
        (void)reply.f32v();
        fired.at = reply.f32v();
        out.events.push_back(std::move(fired));
    }
    CY_CHECK(reply.done());
    return out;
}

editor::AnimationRefusal ask(editor::AnimationPreviewRuntime* preview, std::string_view operation,
                             const Array<u8>& request, Array<u8>& reply) {
    return editor::answer_animation(preview, operation, request.span(), reply);
}

bool has_diagnostic(const Compiled& compiled, std::string_view code, u64 node) {
    return std::ranges::any_of(compiled.diagnostics, [&](const Diagnosed& diagnostic) {
        return diagnostic.code == code && diagnostic.node == node;
    });
}

/// Bit for bit, component by component: -0 and +0 are different answers here.
bool same_bits(f32 a, f32 b) {
    return std::bit_cast<u32>(a) == std::bit_cast<u32>(b);
}

bool same_bits(const Quat& a, const Quat& b) {
    return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z) && same_bits(a.w, b.w);
}

bool same_bits(const Mat4& a, const Mat4& b) {
    for (usize column = 0; column < 4; ++column) {
        const Vec4& x = a.columns[column];
        const Vec4& y = b.columns[column];
        if (!same_bits(x.x, y.x) || !same_bits(x.y, y.y) || !same_bits(x.z, y.z) ||
            !same_bits(x.w, y.w)) {
            return false;
        }
    }
    return true;
}

}  // namespace

CY_TEST_CASE("editor animation: the acceptance graph is the engine's canonical text") {
    // The Rust editor writes it call by call; the engine reads it back to the same bytes, so a text
    // diff of the file is a semantic diff and the engine's merge reads it unchanged.
    for (const char* name : {kGraph, kEdited, "animation_hero_v1.cyanimgraph"}) {
        const std::string source = read_file(name);
        graph::NodeRegistry registry(allocator());
        CY_REQUIRE(graph::pose::register_pose_nodes(registry).has_value());
        graph::DiagnosticSink sink(allocator());
        Expected<graph::Graph, Error> parsed =
            graph::parse_graph(source, &registry, allocator(), sink);
        CY_REQUIRE(parsed.has_value());
        Array<char> written(allocator());
        CY_REQUIRE(graph::write_graph(*parsed, written).has_value());
        CY_CHECK_EQ(std::string(written.data(), written.size()), source);
        sink.clear();
        CY_REQUIRE(graph::validate(*parsed, registry, nullptr, sink).has_value());
        CY_CHECK_EQ(sink.entries().size(), 0U);
    }
}

CY_TEST_CASE("editor animation: without a character the vocabulary is the pose graph's") {
    Array<u8> reply(allocator());
    const Array<u8> empty(allocator());
    CY_REQUIRE_FALSE(ask(nullptr, "animation.catalogue.get", empty, reply).refused());
    Reply catalogue(text_of(reply));
    CY_REQUIRE_EQ(catalogue.u32v(), 3U);
    (void)catalogue.u32v();
    const u32 count = catalogue.u32v();
    CY_CHECK_EQ(count, 9U);
    // Compiling needs no character: the program is checked against nothing it would play.
    CY_REQUIRE_FALSE(
        ask(nullptr, "animation.compile", compile_request(read_file(kGraph)), reply).refused());
    CY_CHECK(decode_compile(text_of(reply)).compiled);
    // A preview does.
    const editor::AnimationRefusal refusal =
        ask(nullptr, "animation.preview.set", preview_request(read_file(kGraph), 0, 0.1F), reply);
    CY_REQUIRE(refusal.refused());
    CY_CHECK_EQ(std::string_view(refusal.code), "animation.preview.unavailable");
}

CY_TEST_CASE("editor animation: the material service routes animation.* and lists it") {
    abi::Host host(allocator());
    editor::MaterialService service(allocator());
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);
    const CyServiceRequest capabilities{sizeof(CyServiceRequest), 1,       1,
                                        "capabilities.get",       nullptr, 0};
    CY_REQUIRE_EQ(api->service_submit(&host, session, &capabilities), CY_RESULT_OK);
    CyServiceEvent event{};
    bool present = false;
    CY_REQUIRE_EQ(api->service_poll(&host, session, &event, &present), CY_RESULT_OK);
    CY_REQUIRE(present);
    const std::string listed(reinterpret_cast<const char*>(event.payload), event.payload_size);
    for (const std::string_view operation : editor::kAnimationOperations) {
        CY_CHECK_MESSAGE(listed.find(operation) != std::string::npos, operation);
    }
    const Array<u8> request = compile_request(read_file(kGraph));
    const CyServiceRequest compile{
        sizeof(CyServiceRequest), 1, 2, "animation.compile", request.data(), request.size()};
    CY_REQUIRE_EQ(api->service_submit(&host, session, &compile), CY_RESULT_OK);
    CY_REQUIRE_EQ(api->service_poll(&host, session, &event, &present), CY_RESULT_OK);
    CY_REQUIRE(present);
    CY_CHECK_EQ(event.kind, static_cast<u32>(CY_SERVICE_EVENT_COMPLETED));
    const std::string payload(reinterpret_cast<const char*>(event.payload), event.payload_size);
    CY_CHECK(decode_compile(payload).compiled);
    api->service_close(&host, session);
}

CY_TEST_CASE("editor animation: a malformed request and an unknown operation are refused") {
    Array<u8> reply(allocator());
    Array<u8> truncated = compile_request("x");
    truncated.pop_back();
    CY_CHECK_EQ(std::string_view(ask(nullptr, "animation.compile", truncated, reply).code),
                "animation.request.malformed");
    const Array<u8> future = Request().u32v(2).text("x").take();
    CY_CHECK_EQ(std::string_view(ask(nullptr, "animation.compile", future, reply).code),
                "animation.schema.unsupported");
    const Array<u8> empty(allocator());
    CY_CHECK_EQ(std::string_view(ask(nullptr, "animation.dance", empty, reply).code),
                "animation.operation.unsupported");
}

CY_TEST_CASE("editor animation: events read as name@seconds, and anything else is refused") {
    Array<editor::AnimationClipEvent> events(allocator());
    const Name walk = Name::intern("walk");
    CY_REQUIRE(editor::parse_animation_events("footstep@0.25; footstep@0.75", walk, events));
    CY_REQUIRE_EQ(events.size(), 2U);
    CY_CHECK_EQ(events[1].event, Name::intern("footstep"));
    CY_CHECK_EQ(events[1].time, 0.75F);
    CY_CHECK(editor::parse_animation_events("", walk, events));
    CY_CHECK_EQ(events.size(), 2U);
    for (const char* bad : {"footstep", "foot step@0.2", "footstep@", "footstep@0.2x", "@0.2"}) {
        CY_CHECK_MESSAGE(!editor::parse_animation_events(bad, walk, events), bad);
    }
    CY_CHECK_EQ(events.size(), 2U);
}

CY_TEST_CASE("editor animation: an empty item between separators is skipped, as the editor does") {
    // The editor's `parse_events` skips empty items, so `animation.graph.set_property` accepted
    // `footstep@0.25;; footstep@0.5` and the compile then refused it as malformed. A trailing `;`
    // was already accepted here; a leading or doubled one is the same nothing.
    const Name walk = Name::intern("walk");
    for (const char* text : {"footstep@0.25;; footstep@0.5", "; footstep@0.25; footstep@0.5",
                             "footstep@0.25; ; footstep@0.5;"}) {
        Array<editor::AnimationClipEvent> events(allocator());
        CY_CHECK_MESSAGE(editor::parse_animation_events(text, walk, events), text);
        CY_CHECK_MESSAGE(events.size() == 2U, text);
    }
}

#if defined(CY_EDITOR_HAS_ANIMATION)

namespace {

/// The preview character, built.
struct Character {
    Character() : preview(allocator()) { CY_REQUIRE(preview.initialize().has_value()); }
    editor::AnimationPreview preview;
};

/// The program as a cook would compile it, for evaluating it directly.
graph::pose::PoseProgram compile_directly(const std::string& source, u32 joints) {
    graph::NodeRegistry registry(allocator());
    CY_REQUIRE(graph::pose::register_pose_nodes(registry).has_value());
    graph::DiagnosticSink sink(allocator());
    Expected<graph::Graph, Error> parsed = graph::parse_graph(source, &registry, allocator(), sink);
    CY_REQUIRE(parsed.has_value());
    Expected<graph::pose::PoseProgram, Error> program =
        graph::pose::compile_pose(*parsed, registry, joints, sink);
    CY_REQUIRE(program.has_value());
    return std::move(*program);
}

/// The graph's pose at `time`, evaluated with `cy::animation` alone: the program bound to the
/// character's skeleton and clips, the parameters set, advanced from zero in the preview's steps
/// (`kAnimationPreviewStep`, the last one shorter), and evaluated.
std::vector<Transform> evaluate_directly(
    const editor::AnimationPreview& character, const std::string& source, f32 time,
    const std::vector<std::pair<std::string, f32>>& parameters) {
    const graph::pose::PoseProgram program =
        compile_directly(source, character.skeleton().joint_count());
    std::vector<const animation::Clip*> table;
    for (const graph::pose::ClipRef& clip : program.clips()) {
        table.push_back(character.clip(clip.name));
    }
    animation::AnimationRig rig(allocator());
    CY_REQUIRE(rig.bind(character.skeleton(), program,
                        Span<const animation::Clip* const>(table.data(), table.size()))
                   .has_value());
    animation::AnimationInstance instance(allocator());
    CY_REQUIRE(instance.prepare(rig).has_value());
    for (const auto& [name, value] : parameters) {
        CY_REQUIRE(instance.set_parameter(rig, Name::intern(name), value).has_value());
    }
    f32 remaining = time;
    while (remaining > 0.0F) {
        const f32 step =
            remaining < editor::kAnimationPreviewStep ? remaining : editor::kAnimationPreviewStep;
        CY_REQUIRE(animation::advance(rig, instance, step, nullptr).has_value());
        remaining -= step;
    }
    animation::PoseScratch scratch(allocator());
    CY_REQUIRE(scratch.prepare(rig).has_value());
    std::vector<Transform> pose(character.skeleton().joint_count());
    character.skeleton().reference_pose(Span<Transform>(pose.data(), pose.size()));
    animation::EvaluationStats stats;
    CY_REQUIRE(animation::evaluate(rig, instance, 0, scratch,
                                   Span<Transform>(pose.data(), pose.size()), stats)
                   .has_value());
    return pose;
}

/// The clip's pose at `time`, sampled with nothing but the clip.
std::vector<Transform> sample_directly(const editor::AnimationPreview& character,
                                       std::string_view clip, f32 time) {
    const animation::Clip* sampled = character.clip(Name::intern(clip));
    CY_REQUIRE(sampled != nullptr);
    std::vector<Transform> pose(character.skeleton().joint_count());
    character.skeleton().reference_pose(Span<Transform>(pose.data(), pose.size()));
    animation::ClipCursor cursor(allocator());
    CY_REQUIRE(cursor.reset(sampled->track_count()).has_value());
    animation::SampleStats stats;
    CY_REQUIRE(sampled
                   ->sample_unwrapped(time, character.skeleton().retained(0), cursor,
                                      Span<Transform>(pose.data(), pose.size()), stats)
                   .has_value());
    return pose;
}

/// Bit for bit: an evaluation that agrees only approximately is a different evaluation.
void check_same_pose(const std::vector<Transform>& shown, const std::vector<Transform>& direct) {
    CY_REQUIRE_EQ(shown.size(), direct.size());
    CY_CHECK_EQ(editor::animation_pose_digest(Span<const Transform>(shown.data(), shown.size())),
                editor::animation_pose_digest(Span<const Transform>(direct.data(), direct.size())));
    for (usize joint = 0; joint < shown.size(); ++joint) {
        CY_CHECK_MESSAGE(same_bits(shown[joint].rotation, direct[joint].rotation), "joint ", joint);
    }
}

Shown preview(editor::AnimationPreview& previewed, const Array<u8>& request) {
    Array<u8> reply(allocator());
    const editor::AnimationRefusal refusal =
        ask(&previewed, "animation.preview.set", request, reply);
    CY_REQUIRE_MESSAGE(!refusal.refused(), refusal.code, ": ", refusal.detail);
    return decode_state(text_of(reply));
}

Shown preview(Character& character, const Array<u8>& request) {
    return preview(character.preview, request);
}

}  // namespace

CY_TEST_CASE("editor animation: the catalogue offers the character's clips on a clip node") {
    Character character;
    Array<u8> reply(allocator());
    const Array<u8> empty(allocator());
    CY_REQUIRE_FALSE(ask(&character.preview, "animation.catalogue.get", empty, reply).refused());
    CY_CHECK_EQ(text_of(reply), committed("animation_catalogue_v1.wire", reply));
    Reply catalogue(text_of(reply));
    CY_REQUIRE_EQ(catalogue.u32v(), 3U);
    (void)catalogue.u32v();
    bool state_output = false;
    std::vector<std::string> clip_choices;
    for (u32 nodes = catalogue.u32v(); nodes > 0; --nodes) {
        (void)catalogue.u32v();
        (void)catalogue.u32v();
        const std::string type = catalogue.text();
        (void)catalogue.u8v();
        for (u32 pins = catalogue.u32v(); pins > 0; --pins) {
            (void)catalogue.u32v();
            const u8 direction = catalogue.u8v();
            const std::string name = catalogue.text();
            const std::string pin_type = catalogue.text();
            state_output = state_output || (type == "pose.state" && direction == 1 &&
                                            name == "state" && pin_type == "state");
        }
        for (u32 properties = catalogue.u32v(); properties > 0; --properties) {
            (void)catalogue.u32v();
            (void)catalogue.u8v();
            const std::string name = catalogue.text();
            (void)catalogue.text();
            (void)catalogue.text();
            (void)catalogue.text();
            (void)catalogue.text();
            for (u32 choices = catalogue.u32v(); choices > 0; --choices) {
                std::string choice = catalogue.text();
                if (type == "pose.clip" && name == "clip") {
                    clip_choices.push_back(std::move(choice));
                }
            }
            (void)catalogue.text();
            (void)catalogue.text();
            (void)catalogue.u64v();
            (void)catalogue.u8v();
            (void)catalogue.u8v();
            (void)catalogue.f64v();
            (void)catalogue.f64v();
            (void)catalogue.f64v();
        }
    }
    CY_CHECK(catalogue.done());
    CY_CHECK(state_output);
    CY_CHECK_EQ(clip_choices, (std::vector<std::string>{"idle", "walk", "run", "wave"}));
}

CY_TEST_CASE("editor animation: the editor's graph compiles to two states and the walk's events") {
    Character character;
    Array<u8> reply(allocator());
    CY_REQUIRE_FALSE(
        ask(&character.preview, "animation.compile", compile_request(read_file(kGraph)), reply)
            .refused());
    CY_CHECK_EQ(text_of(reply), committed("animation_compile_v1.wire", reply));
    const Compiled compiled = decode_compile(text_of(reply));
    CY_CHECK(compiled.compiled);
    CY_CHECK_EQ(compiled.states, (std::vector<std::string>{"idle", "walk"}));
    CY_CHECK_EQ(compiled.durations, (std::vector<f32>{0.25F, 0.25F}));
    CY_REQUIRE_EQ(compiled.clips.size(), 2U);
    CY_CHECK_EQ(compiled.clips[1], (std::pair<std::string, u32>{"walk", 2U}));
    // The two clocks the runtime advances, and the two conditions the author sets.
    u32 clocks = 0;
    for (const auto& [name, kind] : compiled.parameters) {
        clocks += kind;
    }
    CY_CHECK_EQ(compiled.parameters.size(), 4U);
    CY_CHECK_EQ(clocks, 2U);
    CY_CHECK(compiled.diagnostics.empty());
    CY_CHECK_EQ(compiled.program, compile_directly(read_file(kGraph), 12).digest());
}

CY_TEST_CASE("editor animation: a zero-duration transition is refused on that transition") {
    Character character;
    const std::string cut =
        replaced(read_file(kGraph), R"(prop "duration" : "float" = (0.25, 0, 0, 0, 0))",
                 R"(prop "duration" : "float" = (0, 0, 0, 0, 0))");
    Array<u8> reply(allocator());
    CY_REQUIRE_FALSE(
        ask(&character.preview, "animation.compile", compile_request(cut), reply).refused());
    CY_CHECK_EQ(text_of(reply), committed("animation_compile_cut_v1.wire", reply));
    const Compiled compiled = decode_compile(text_of(reply));
    CY_CHECK_FALSE(compiled.compiled);
    CY_CHECK(has_diagnostic(compiled, "animation.transition.cut", 5));
    CY_CHECK_FALSE(has_diagnostic(compiled, "animation.transition.cut", 6));
    // And the preview will not show a graph that does not compile.
    const editor::AnimationRefusal refusal =
        ask(&character.preview, "animation.preview.set", preview_request(cut, 0, 0.1F), reply);
    CY_CHECK_EQ(std::string_view(refusal.code), "animation.preview.uncompiled");
}

CY_TEST_CASE("editor animation: what a compiled program cannot carry is named on its node") {
    Character character;
    std::string source = replaced(read_file(kGraph), "\"walk\"\n    prop \"duration\"",
                                  "\"walkk\"\n    prop \"duration\"");
    source = replaced(source, "footstep@0.75", "footstep@1.5");
    source = replaced(source, "link 4 \"state\" -> 6 \"from\"\n", "");
    source = replaced(source, "\"stopped\"", "(0, 0, 0, 0, 0)");
    Array<u8> reply(allocator());
    CY_REQUIRE_FALSE(
        ask(&character.preview, "animation.compile", compile_request(source), reply).refused());
    const Compiled compiled = decode_compile(text_of(reply));
    CY_CHECK_FALSE(compiled.compiled);
    CY_CHECK(has_diagnostic(compiled, "animation.clip.unknown", 3));
    CY_CHECK(has_diagnostic(compiled, "animation.transition.unwired", 6));
    CY_CHECK(has_diagnostic(compiled, "animation.transition.no-condition", 6));
    const std::string malformed = replaced(read_file(kGraph), "footstep@0.75", "footstep 0.75");
    CY_REQUIRE_FALSE(
        ask(&character.preview, "animation.compile", compile_request(malformed), reply).refused());
    CY_CHECK(has_diagnostic(decode_compile(text_of(reply)), "animation.event.malformed", 3));
    const std::string late = replaced(read_file(kGraph), "footstep@0.75", "footstep@1.5");
    CY_REQUIRE_FALSE(
        ask(&character.preview, "animation.compile", compile_request(late), reply).refused());
    CY_CHECK(has_diagnostic(decode_compile(text_of(reply)), "animation.event.outside", 3));
}

CY_TEST_CASE("editor animation: a wire the vocabulary does not have is refused on its node") {
    // The compiler reads a transition's input whatever pin it leaves; the vocabulary does not, and
    // a graph a canvas could not have drawn is refused rather than compiled as if it could.
    Character character;
    const std::string backwards = replaced(read_file(kGraph), R"(link 2 "state" -> 5 "from")",
                                           R"(link 2 "pose" -> 5 "from")");
    Array<u8> reply(allocator());
    CY_REQUIRE_FALSE(
        ask(&character.preview, "animation.compile", compile_request(backwards), reply).refused());
    const Compiled compiled = decode_compile(text_of(reply));
    CY_CHECK_FALSE(compiled.compiled);
    CY_CHECK(has_diagnostic(compiled, "graph.link.output-missing", 2));
}

CY_TEST_CASE("editor animation: a scrubbed clip shows the clip sampled directly at that time") {
    Character character;
    for (const f32 time : {0.0F, 0.37F, 0.5F, 1.0F}) {
        const Shown shown = preview(character, preview_request(read_file(kGraph), 3, time));
        CY_CHECK(shown.active);
        CY_CHECK_EQ(shown.clip, "walk");
        CY_CHECK_EQ(shown.time, time);
        CY_CHECK_EQ(shown.length, 1.0F);
        check_same_pose(shown.joints, sample_directly(character.preview, "walk", time));
    }
    // A different time is a different pose: the check above is not comparing two rest poses.
    const Shown early = preview(character, preview_request(read_file(kGraph), 3, 0.1F));
    const Shown late = preview(character, preview_request(read_file(kGraph), 3, 0.6F));
    CY_CHECK_NE(early.pose_digest, late.pose_digest);
}

CY_TEST_CASE("editor animation: the previewed machine is the program evaluated directly") {
    Character character;
    const std::vector<std::pair<std::string, f32>> moving{{"moving", 1.0F}};
    // The editor's own request, byte for byte, is what is previewed.
    const std::string request = read_file("animation_preview_request_v1.wire");
    Array<u8> bytes(allocator());
    CY_REQUIRE(
        bytes.append({reinterpret_cast<const u8*>(request.data()), request.size()}).has_value());
    CY_CHECK_EQ(text_of(bytes), text_of(preview_request(read_file(kGraph), 0, 0.4F, moving)));
    Array<u8> reply(allocator());
    CY_REQUIRE_FALSE(ask(&character.preview, "animation.preview.set", bytes, reply).refused());
    CY_CHECK_EQ(text_of(reply), committed("animation_preview_state_v1.wire", reply));
    const Shown shown = decode_state(text_of(reply));
    CY_CHECK_EQ(shown.focus, 0U);
    CY_CHECK_EQ(shown.state_name, "walk");
    CY_CHECK_EQ(shown.target, 0xFFFFU);
    check_same_pose(shown.joints,
                    evaluate_directly(character.preview, read_file(kGraph), 0.4F, moving));
    for (const f32 time : {0.05F, 0.2F, 1.3F}) {
        const Shown at = preview(character, preview_request(read_file(kGraph), 0, time, moving));
        check_same_pose(at.joints,
                        evaluate_directly(character.preview, read_file(kGraph), time, moving));
    }
    // With the condition closed, the machine stays idle: the parameter reached the engine.
    const Shown idle = preview(character, preview_request(read_file(kGraph), 0, 0.4F));
    CY_CHECK_EQ(idle.state_name, "idle");
}

CY_TEST_CASE("editor animation: an edited transition changes what the engine evaluates") {
    Character character;
    const std::vector<std::pair<std::string, f32>> moving{{"moving", 1.0F}};
    const std::string edited = read_file(kEdited);
    CY_CHECK_NE(edited, read_file(kGraph));
    const Shown before = preview(character, preview_request(read_file(kGraph), 0, 0.4F, moving));
    const Shown after = preview(character, preview_request(edited, 0, 0.4F, moving));
    // The blend into the walk takes 0.25 s before the edit and 0.625 s after: at 0.4 s one is done
    // and the other is two thirds through.
    CY_CHECK_EQ(before.target, 0xFFFFU);
    CY_CHECK_EQ(after.state_name, "idle");
    CY_CHECK_EQ(after.target_name, "walk");
    CY_CHECK_GT(after.blend, 0.5F);
    CY_CHECK_LT(after.blend, 0.75F);
    CY_CHECK_NE(before.pose_digest, after.pose_digest);
    check_same_pose(after.joints, evaluate_directly(character.preview, edited, 0.4F, moving));
}

CY_TEST_CASE("editor animation: scrubbing forward across an authored event fires it") {
    Character character;
    const std::string source = read_file(kGraph);
    (void)preview(character, preview_request(source, 3, 0.2F));
    const Shown crossed = preview(character, preview_request(source, 3, 0.3F));
    CY_REQUIRE_EQ(crossed.events.size(), 1U);
    CY_CHECK_EQ(crossed.events[0].name, "footstep");
    CY_CHECK_EQ(crossed.events[0].at, 0.25F);
    // Backwards crosses nothing; and an event moved on the timeline fires where it now is.
    CY_CHECK_EQ(preview(character, preview_request(source, 3, 0.1F)).events.size(), 1U);
    const std::string moved = replaced(source, "footstep@0.25", "footstep@0.5");
    (void)preview(character, preview_request(moved, 3, 0.1F));
    CY_CHECK_EQ(preview(character, preview_request(moved, 3, 0.3F)).events.size(), 1U);
    const Shown later = preview(character, preview_request(moved, 3, 0.55F));
    CY_REQUIRE_EQ(later.events.size(), 2U);
    CY_CHECK_EQ(later.events[1].at, 0.5F);
}

CY_TEST_CASE("editor animation: a playing preview runs on the engine's clock") {
    Character character;
    const std::string source = read_file(kGraph);
    Array<u8> request = Request().u32v(1).text(source).u64v(3).f32v(0.9F).u8v(1).u32v(0).take();
    Array<u8> reply(allocator());
    CY_REQUIRE_FALSE(ask(&character.preview, "animation.preview.set", request, reply).refused());
    // A looping clip wraps, and the footstep at 0.25 fires on the way.
    CY_REQUIRE(character.preview.tick(0.4F).has_value());
    CY_CHECK_NEAR(character.preview.state().time, 0.3F, 1e-5F);
    CY_REQUIRE_FALSE(character.preview.events().empty());
    CY_CHECK_EQ(character.preview.events().back().name, Name::intern("footstep"));
    check_same_pose(
        std::vector<Transform>(character.preview.pose().begin(), character.preview.pose().end()),
        sample_directly(character.preview, "walk", character.preview.state().time));
    // The skinning matrices move the mesh: the skinned pose is not the bind pose.
    const Mat4 identity = Mat4::identity();
    bool moved = false;
    for (const Mat4& matrix : character.preview.skinning_matrices()) {
        moved = moved || !same_bits(matrix, identity);
    }
    CY_CHECK(moved);
    // Stopping hides it.
    CY_REQUIRE_FALSE(
        ask(&character.preview, "animation.preview.stop", Array<u8>(allocator()), reply).refused());
    CY_CHECK_EQ(text_of(reply), committed("animation_preview_stopped_v1.wire", reply));
    CY_CHECK_FALSE(decode_state(text_of(reply)).active);
}

CY_TEST_CASE("editor animation: a playing state machine runs on in whole steps") {
    Character character;
    Request request;
    request.u32v(1).text(read_file(kGraph)).u64v(0).f32v(0.4F).u8v(1).u32v(1);
    request.text("moving").f32v(1.0F);
    Array<u8> reply(allocator());
    CY_REQUIRE_FALSE(
        ask(&character.preview, "animation.preview.set", request.take(), reply).refused());
    CY_REQUIRE(character.preview.tick(0.5F).has_value());
    // Half a second of wall time is whole sixtieths: what is left over waits for the next frame,
    // so the machine is at most one step short of 0.9 s and never past it.
    CY_CHECK_GT(character.preview.state().time, 0.9F - editor::kAnimationPreviewStep - 1e-4F);
    CY_CHECK_LE(character.preview.state().time, 0.9F + 1e-4F);
    CY_CHECK(character.preview.state().playing);
    CY_CHECK_EQ(character.preview.state().state_name, Name::intern("walk"));
}

CY_TEST_CASE("editor animation: the character's mesh is one box per bone, bound to that bone") {
    Character character;
    const editor::AnimationPreviewMesh& mesh = character.preview.mesh();
    CY_CHECK_EQ(mesh.positions.size(), 11U * 24U);
    CY_CHECK_EQ(mesh.indices.size(), 11U * 36U);
    CY_CHECK_EQ(mesh.joints.size(), mesh.positions.size() * 4U);
    CY_CHECK_EQ(mesh.weights.size(), mesh.positions.size() * 4U);
    for (const u16 joint : mesh.joints) {
        CY_CHECK_LT(joint, character.preview.joint_count());
    }
}

#    if defined(CY_EDITOR_TEST_HAS_IMPORT)

// --- A project's own character (#112's gaps) -----------------------------------------------------

namespace {

constexpr AssetId kSkeletonId{0x5e1e, 1};
constexpr AssetId kClipId{0x5e1e, 2};
constexpr AssetId kMeshId{0x5e1e, 3};
constexpr AssetId kForeignClipId{0x5e1e, 4};
constexpr const char* kModel = "characters/hero.fbx";

/// The cooked assets of a project, in memory: what `.cy/cooked/<id>.cyasset` holds, by id.
class MemoryAssets final : public editor::AnimationAssetSource {
public:
    void put(AssetId id, const std::vector<u8>& payload) {
        for (auto& [held, bytes] : records_) {
            if (held == id) {
                bytes = payload;
                return;
            }
        }
        records_.emplace_back(id, payload);
    }
    /// Bind every vertex this many joints further on: a mesh rigged to a bigger skeleton.
    u16 joint_offset = 0;

    Status read(AssetId id, Array<u8>& payload) noexcept override {
        for (const auto& [held, bytes] : records_) {
            if (held == id) {
                payload.clear();
                return payload.append(Span<const u8>(bytes.data(), bytes.size()));
            }
        }
        return fail(ErrorCode::NotFound, "no such cooked asset");
    }

    Status read_mesh(AssetId id, editor::AnimationPreviewMesh& out) noexcept override {
        Array<u8> record(allocator());
        if (Status read_ok = read(id, record); !read_ok) {
            return read_ok;
        }
        import::MeshData mesh;
        if (Status decoded = import::read_cooked_mesh(record.span(), mesh); !decoded) {
            return decoded;
        }
        if (mesh.skin.size() != mesh.positions.size()) {
            return fail(ErrorCode::InvalidArgument, "no skin");
        }
        if (mesh.normals.size() != mesh.positions.size()) {
            if (Status made = import::generate_normals(mesh, 180.0F); !made) {
                return made;
            }
        }
        out.clear();
        (void)out.positions.append(mesh.positions.span());
        (void)out.normals.append(mesh.normals.span());
        for (import::SkinInfluence influence : mesh.skin) {
            for (u16& joint : influence.joints) {
                joint = static_cast<u16>(joint + joint_offset);
            }
            (void)out.joints.append({influence.joints, import::kSkinInfluences});
            (void)out.weights.append({influence.weights, import::kSkinInfluences});
        }
        return out.indices.append(mesh.indices.span());
    }

private:
    std::vector<std::pair<AssetId, std::vector<u8>>> records_;
};

/// The imported hero, its cooked records in memory, and a preview reading them.
struct Project {
    Project() : hero(editor::testing::import_character()), preview(allocator()) {
        assets.put(kSkeletonId, hero.skeleton.payload);
        assets.put(kClipId, hero.clip.payload);
        assets.put(kMeshId, hero.mesh.payload);
        CY_REQUIRE(preview.initialize().has_value());
        preview.set_source(&assets);
    }

    editor::testing::ImportedCharacter hero;
    MemoryAssets assets;
    editor::AnimationPreview preview;
};

std::string id_text(AssetId id) {
    char text[AssetId::kTextLength + 1] = {};
    (void)id.format(text);
    return text;
}

/// A character block as the editor writes it.
void put_character(Request& request, std::string_view model, AssetId skeleton, AssetId mesh,
                   const std::vector<std::pair<std::string, AssetId>>& clips) {
    request.text(model).text(skeleton.is_nil() ? "" : id_text(skeleton));
    request.text(mesh.is_nil() ? "" : id_text(mesh)).u32v(static_cast<u32>(clips.size()));
    for (const auto& [name, id] : clips) {
        request.text(name).text(id_text(id));
    }
}

Array<u8> character_request(AssetId skeleton = kSkeletonId, AssetId mesh = kMeshId,
                            const std::vector<std::pair<std::string, AssetId>>& clips = {
                                {"hero", kClipId}}) {
    Request request;
    request.u32v(1);
    put_character(request, skeleton.is_nil() ? "" : kModel, skeleton, mesh, clips);
    return request.take();
}

struct Played {
    std::string model;
    bool project = false;
    u32 joints = 0;
    bool skinned = false;
    std::vector<std::pair<std::string, f32>> clips;
    std::vector<bool> looping;
    std::vector<std::pair<std::string, std::string>> refused;
};

Played decode_character(const std::string& bytes) {
    Reply reply(bytes);
    Played out;
    CY_REQUIRE_EQ(reply.u32v(), 1U);
    out.model = reply.text();
    out.project = reply.u8v() != 0;
    out.joints = reply.u32v();
    out.skinned = reply.u8v() != 0;
    for (u32 count = reply.u32v(); count > 0; --count) {
        std::string name = reply.text();
        out.clips.emplace_back(std::move(name), reply.f32v());
        out.looping.push_back(reply.u8v() != 0);
    }
    for (u32 count = reply.u32v(); count > 0; --count) {
        std::string clip = reply.text();
        out.refused.emplace_back(std::move(clip), reply.text());
    }
    CY_CHECK(reply.done());
    return out;
}

Played set_character(Project& project, const Array<u8>& request) {
    Array<u8> reply(allocator());
    const editor::AnimationRefusal refusal =
        ask(&project.preview, "animation.character.set", request, reply);
    CY_REQUIRE_MESSAGE(!refusal.refused(), refusal.code, ": ", refusal.detail);
    return decode_character(text_of(reply));
}

/// The hero's graph: one state playing its clip, with two events placed on the timeline — between
/// ticks of a 60 Hz clock, so the tick that crosses each is not a question of rounding.
std::string hero_graph(std::string_view events = "land@0.76; footstep@0.26",
                       std::string_view loop = "(0, 0, 0, 0, 1)") {
    std::string text =
        "cygraph 1\n"
        "graph \"hero\" version 1\n"
        "capability\n"
        "deterministic true\n"
        "node 1 \"pose.clip\" v1 {\n"
        "    prop \"clip\" : \"name\" = \"hero\"\n"
        "    prop \"duration\" : \"float\" = (1, 0, 0, 0, 0)\n";
    text.append(R"(    prop "events" : "name" = ")").append(events).append("\"\n");
    text.append(R"(    prop "loop" : "bool" = )").append(loop).append("\n");
    text +=
        "    prop \"time_parameter\" : \"name\" = (0, 0, 0, 0, 0)\n"
        "}\n"
        "node 2 \"pose.state\" v1 {\n"
        "    prop \"name\" : \"name\" = \"idle\"\n"
        "}\n"
        "link 1 \"pose\" -> 2 \"pose\"\n"
        "layout 1 at 16 16\n"
        "layout 2 at 230 16\n";
    return text;
}

/// The hero's skeleton and clip decoded straight from the importer's records, with no preview,
/// no character and no service in between.
struct Direct {
    Direct() : skeleton(allocator()), clip(allocator()) {
        const editor::testing::ImportedCharacter hero = editor::testing::import_character();
        animation::SkeletonProfile humanoid;
        CY_REQUIRE(animation::decode_skeleton(
                       Span<const u8>(hero.skeleton.payload.data(), hero.skeleton.payload.size()),
                       skeleton, humanoid)
                       .has_value());
        Array<Name> joints(allocator());
        CY_REQUIRE(
            animation::decode_clip(
                Span<const u8>(hero.clip.payload.data(), hero.clip.payload.size()), clip, joints)
                .has_value());
    }

    [[nodiscard]] std::vector<Transform> sample(f32 time) const {
        std::vector<Transform> pose(skeleton.joint_count());
        skeleton.reference_pose(Span<Transform>(pose.data(), pose.size()));
        animation::ClipCursor cursor(allocator());
        CY_REQUIRE(cursor.reset(clip.track_count()).has_value());
        animation::SampleStats stats;
        CY_REQUIRE(clip.sample_unwrapped(time, skeleton.retained(0), cursor,
                                         Span<Transform>(pose.data(), pose.size()), stats)
                       .has_value());
        return pose;
    }

    animation::Skeleton skeleton;
    animation::Clip clip;
};

}  // namespace

CY_TEST_CASE(
    "editor animation: an imported character plays its own clips, named as a graph names them") {
    Project project;
    Array<u8> answer(allocator());
    CY_REQUIRE_FALSE(
        ask(&project.preview, "animation.character.set", character_request(), answer).refused());
    // The Rust suites decode this reply and replay it as the runtime's, and their encoder writes
    // this request byte for byte from the project's import records.
    CY_CHECK_EQ(text_of(answer), committed("animation_character_v1.wire", answer));
    const Array<u8> asked = character_request();
    CY_CHECK_EQ(text_of(asked), committed("animation_character_request_v1.wire", asked));
    const Played played = decode_character(text_of(answer));
    CY_CHECK_EQ(played.model, kModel);
    CY_CHECK(played.project);
    CY_CHECK_EQ(played.joints, 3U);
    CY_CHECK(played.skinned);
    // The sub-asset's leaf, not the stack's `mixamo.com`.
    CY_REQUIRE_EQ(played.clips.size(), 1U);
    CY_CHECK_EQ(played.clips[0].first, "hero");
    CY_CHECK_NEAR(played.clips[0].second, 1.0F, 1e-5F);
    CY_CHECK(played.refused.empty());
    // The palette now offers the character's clips, and a compile checks against them.
    Array<u8> reply(allocator());
    CY_REQUIRE_FALSE(
        ask(&project.preview, "animation.catalogue.get", Array<u8>(allocator()), reply).refused());
    CY_CHECK_EQ(text_of(reply), committed("animation_catalogue_hero_v1.wire", reply));
    CY_CHECK_NE(text_of(reply).find("hero"), std::string::npos);
    CY_CHECK_EQ(text_of(reply).find("wave"), std::string::npos);
    CY_REQUIRE_FALSE(
        ask(&project.preview, "animation.compile", compile_request(hero_graph()), reply).refused());
    CY_CHECK_EQ(text_of(reply), committed("animation_compile_hero_v1.wire", reply));
    CY_CHECK(decode_compile(text_of(reply)).compiled);
    CY_REQUIRE_FALSE(
        ask(&project.preview, "animation.compile", compile_request(read_file(kGraph)), reply)
            .refused());
    const Compiled mannequin_graph = decode_compile(text_of(reply));
    CY_CHECK_FALSE(mannequin_graph.compiled);
    CY_CHECK(has_diagnostic(mannequin_graph, "animation.clip.unknown", 1));
}

CY_TEST_CASE("editor animation: the imported mesh is the character's skin, drawn by its joints") {
    Project project;
    (void)set_character(project, character_request());
    const editor::AnimationPreviewMesh& mesh = project.preview.mesh();
    // Two quads: four triangles over six vertices (the importer may split none of them).
    CY_CHECK_EQ(mesh.indices.size(), 12U);
    CY_CHECK_EQ(mesh.joints.size(), mesh.positions.size() * 4U);
    // Each row of vertices moves with its own bone, and the head row sits 0.6 m above the hips.
    f32 highest = 0.0F;
    for (usize vertex = 0; vertex < mesh.positions.size(); ++vertex) {
        highest = std::max(highest, mesh.positions[vertex].y);
        CY_CHECK_LT(mesh.joints[vertex * 4], 3U);
        CY_CHECK_EQ(mesh.weights[vertex * 4], 1.0F);
    }
    CY_CHECK_NEAR(highest, 1.6F, 1e-4F);
    // A skeleton with no mesh is drawn as its bones, one box each, bound to the bone it shows.
    (void)set_character(project, character_request(kSkeletonId, AssetId{}));
    CY_CHECK_FALSE(project.preview.character_skinned());
    CY_CHECK_EQ(project.preview.mesh().positions.size(), 3U * 24U);
}

CY_TEST_CASE("editor animation: a scrubbed imported clip is the importer's clip sampled directly") {
    Project project;
    (void)set_character(project, character_request());
    Direct direct;
    const std::string source = hero_graph();
    for (const f32 time : {0.0F, 0.3F, 0.5F, 1.0F}) {
        const Shown shown = preview(project.preview, preview_request(source, 1, time));
        CY_CHECK_EQ(shown.clip, "hero");
        CY_CHECK_EQ(shown.length, 1.0F);
        check_same_pose(shown.joints, direct.sample(time));
    }
    // The spine turns: halfway through it is about 22.5 degrees about Z.
    Array<u8> answer(allocator());
    CY_REQUIRE_FALSE(
        ask(&project.preview, "animation.preview.set", preview_request(source, 1, 0.5F), answer)
            .refused());
    CY_CHECK_EQ(text_of(answer), committed("animation_preview_hero_v1.wire", answer));
    const Shown half = decode_state(text_of(answer));
    CY_REQUIRE_EQ(half.joints.size(), 3U);
    const f32 angle = 2.0F * std::asin(half.joints[1].rotation.z) * 57.2957795F;
    CY_CHECK_NEAR(angle, 22.5F, 0.5F);
}

CY_TEST_CASE(
    "editor animation: the imported character's machine is the program evaluated directly") {
    Project project;
    (void)set_character(project, character_request());
    Direct direct;
    const std::string source = hero_graph();
    // The program bound to the importer's own skeleton and clip, decoded here, and advanced in
    // the preview's steps with `cy::animation` alone.
    const graph::pose::PoseProgram program =
        compile_directly(source, direct.skeleton.joint_count());
    const animation::Clip* table[] = {&direct.clip};
    animation::AnimationRig rig(allocator());
    CY_REQUIRE(rig.bind(direct.skeleton, program, Span<const animation::Clip* const>(table, 1))
                   .has_value());
    for (const f32 time : {0.2F, 0.7F, 1.4F}) {
        const Shown shown = preview(project.preview, preview_request(source, 0, time));
        CY_CHECK_EQ(shown.state_name, "idle");
        animation::AnimationInstance instance(allocator());
        CY_REQUIRE(instance.prepare(rig).has_value());
        f32 remaining = time;
        while (remaining > 0.0F) {
            const f32 step = std::fmin(remaining, editor::kAnimationPreviewStep);
            CY_REQUIRE(animation::advance(rig, instance, step, nullptr).has_value());
            remaining -= step;
        }
        animation::PoseScratch scratch(allocator());
        CY_REQUIRE(scratch.prepare(rig).has_value());
        std::vector<Transform> pose(direct.skeleton.joint_count());
        direct.skeleton.reference_pose(Span<Transform>(pose.data(), pose.size()));
        animation::EvaluationStats stats;
        CY_REQUIRE(animation::evaluate(rig, instance, 0, scratch,
                                       Span<Transform>(pose.data(), pose.size()), stats)
                       .has_value());
        check_same_pose(shown.joints, pose);
        // And it moved: the spine has turned by about 45 degrees a second, wrapped.
        const f32 angle = 2.0F * std::asin(shown.joints[1].rotation.z) * 57.2957795F;
        CY_CHECK_NEAR(angle, 45.0F * std::fmod(time, 1.0F), 1.0F);
    }
}

CY_TEST_CASE("editor animation: events placed on the timeline fire on the imported clip") {
    Project project;
    (void)set_character(project, character_request());
    const std::string source = hero_graph();
    (void)preview(project.preview, preview_request(source, 1, 0.1F));
    const Shown crossed = preview(project.preview, preview_request(source, 1, 0.8F));
    // In time order, whatever order they were written in.
    CY_REQUIRE_EQ(crossed.events.size(), 2U);
    CY_CHECK_EQ(crossed.events[0].name, "footstep");
    CY_CHECK_EQ(crossed.events[0].at, 0.26F);
    CY_CHECK_EQ(crossed.events[1].name, "land");
    CY_CHECK_EQ(crossed.events[1].at, 0.76F);
    // The clip the importer cooked carried none of them: they are the graph's.
    Direct direct;
    CY_CHECK(direct.clip.events().empty());
}

CY_TEST_CASE("editor animation: a clip cooked for another skeleton is refused by name") {
    Project project;
    const editor::testing::ImportedCharacter other =
        editor::testing::import_character(30.0, "characters/robot.fbx", "robot:");
    project.assets.put(kForeignClipId, other.clip.payload);
    const Played played = set_character(
        project,
        character_request(kSkeletonId, kMeshId, {{"hero", kClipId}, {"robot", kForeignClipId}}));
    CY_REQUIRE_EQ(played.clips.size(), 1U);
    CY_CHECK_EQ(played.clips[0].first, "hero");
    CY_REQUIRE_EQ(played.refused.size(), 1U);
    CY_CHECK_EQ(played.refused[0].first, "robot");
    CY_CHECK_NE(played.refused[0].second.find("another skeleton"), std::string::npos);
}

CY_TEST_CASE("editor animation: a character that does not load leaves the one that played") {
    Project project;
    (void)set_character(project, character_request());
    Array<u8> reply(allocator());
    // A skeleton id the project does not have.
    const editor::AnimationRefusal missing =
        ask(&project.preview, "animation.character.set", character_request(AssetId{9, 9}), reply);
    CY_REQUIRE(missing.refused());
    CY_CHECK_EQ(std::string_view(missing.code), "animation.character.failed");
    CY_CHECK_EQ(project.preview.character_model(), kModel);
    CY_CHECK_EQ(project.preview.joint_count(), 3U);
    // A mesh that is not a skin of it.
    const editor::AnimationRefusal unskinned = ask(&project.preview, "animation.character.set",
                                                   character_request(kSkeletonId, kClipId), reply);
    CY_CHECK_EQ(std::string_view(unskinned.code), "animation.character.failed");
    // An empty skeleton is the mannequin again, and a preview stops when the character changes.
    (void)preview(project.preview, preview_request(hero_graph(), 1, 0.2F));
    CY_CHECK(project.preview.state().active);
    const Played mannequin = set_character(project, character_request(AssetId{}, AssetId{}, {}));
    CY_CHECK_FALSE(mannequin.project);
    CY_CHECK_EQ(mannequin.joints, 12U);
    CY_CHECK_EQ(mannequin.clips.size(), 4U);
    CY_CHECK_FALSE(project.preview.state().active);
    // A host with no cooked assets plays only the mannequin, and says so.
    editor::AnimationPreview bare(allocator());
    CY_REQUIRE(bare.initialize().has_value());
    const editor::AnimationRefusal unhosted =
        ask(&bare, "animation.character.set", character_request(), reply);
    CY_CHECK_EQ(std::string_view(unhosted.code), "animation.character.failed");
    // A malformed id is the request's fault.
    Request bad;
    bad.u32v(1).text(kModel).text("not-an-id").text("").u32v(0);
    CY_CHECK_EQ(std::string_view(ask(&bare, "animation.character.set", bad.take(), reply).code),
                "animation.request.malformed");
}

// --- Baking a graph into the rig a game loads ----------------------------------------------------

namespace {

Array<u8> bake_request(std::string_view rig, std::string_view source,
                       AssetId skeleton = kSkeletonId) {
    Request request;
    request.u32v(1).text(rig).text(source);
    put_character(request, kModel, skeleton, kMeshId, {{"hero", kClipId}});
    return request.take();
}

struct Baked {
    bool baked = false;
    std::vector<std::pair<std::string, std::string>> files;
    std::vector<Diagnosed> diagnostics;

    [[nodiscard]] const std::string& file(std::string_view path) const {
        for (const auto& [name, bytes] : files) {
            if (name == path) {
                return bytes;
            }
        }
        CY_TEST_FAIL("no such baked file: ", path);
        static const std::string none;
        return none;
    }
};

Baked decode_bake(const std::string& bytes) {
    Reply reply(bytes);
    Baked out;
    CY_REQUIRE_EQ(reply.u32v(), 1U);
    out.baked = reply.u8v() != 0;
    for (u32 count = reply.u32v(); count > 0; --count) {
        std::string path = reply.text();
        out.files.emplace_back(std::move(path), reply.text());
    }
    for (u32 count = reply.u32v(); count > 0; --count) {
        Diagnosed diagnostic;
        diagnostic.severity = reply.u8v();
        diagnostic.code = reply.text();
        diagnostic.node = reply.u64v();
        (void)reply.text();
        (void)reply.text();
        diagnostic.detail = reply.text();
        (void)reply.u64v();
        out.diagnostics.push_back(std::move(diagnostic));
    }
    CY_CHECK(reply.done());
    return out;
}

Baked bake(Project& project, const Array<u8>& request, std::string_view committed_as = {}) {
    editor::AnimationRigBaker baker(allocator(), project.assets);
    Array<u8> reply(allocator());
    const editor::AnimationRefusal refusal =
        editor::answer_animation(nullptr, "animation.bake", request.span(), reply, &baker);
    CY_REQUIRE_MESSAGE(!refusal.refused(), refusal.code, ": ", refusal.detail);
    if (!committed_as.empty()) {
        CY_CHECK_EQ(text_of(reply), committed(committed_as, reply));
    }
    return decode_bake(text_of(reply));
}

/// A baked file's cooked record, its header checked.
Span<const u8> record_of(const std::string& file) {
    Expected<Span<const u8>, Error> payload =
        assets::read_cooked_payload(reinterpret_cast<const u8*>(file.data()), file.size(), true);
    CY_REQUIRE(payload.has_value());
    Expected<assets::CookedAssetHeader, Error> header =
        assets::read_cooked_header(reinterpret_cast<const u8*>(file.data()), file.size());
    CY_REQUIRE(header.has_value());
    CY_CHECK(header->kind == assets::AssetKind::Animation);
    return *payload;
}

}  // namespace

CY_TEST_CASE("editor animation: a project clip's own events give way to the graph's") {
    // A clip cooked with an event of its own (a v2 record): the graph's events replace it.
    Project project;
    animation::Clip clip(allocator());
    Array<Name> joints(allocator());
    CY_REQUIRE(animation::decode_clip(Span<const u8>(project.hero.clip.payload.data(),
                                                     project.hero.clip.payload.size()),
                                      clip, joints)
                   .has_value());
    CY_REQUIRE(clip.add_event(Name::intern("stale"), 0.5F).has_value());
    Array<u8> record(allocator());
    CY_REQUIRE(animation::encode_clip(clip, joints.span(), record).has_value());
    project.assets.put(kClipId, std::vector<u8>(record.begin(), record.end()));
    (void)set_character(project, character_request());
    const std::string source = hero_graph("footstep@0.26");
    (void)preview(project.preview, preview_request(source, 1, 0.1F));
    const Shown crossed = preview(project.preview, preview_request(source, 1, 0.9F));
    CY_REQUIRE_EQ(crossed.events.size(), 1U);
    CY_CHECK_EQ(crossed.events[0].name, "footstep");
    const Baked baked = bake(project, bake_request("hero", source));
    CY_REQUIRE(baked.baked);
    animation::Clip cooked(allocator());
    CY_REQUIRE(animation::decode_clip(record_of(baked.file("clips/0.cyasset")), cooked, joints)
                   .has_value());
    CY_REQUIRE_EQ(cooked.events().size(), 1U);
    CY_CHECK_EQ(cooked.events()[0].name, Name::intern("footstep"));
}

CY_TEST_CASE(
    "editor animation: two clips of one name, and a mesh rigged to other joints, are refused") {
    Project project;
    const Played played = set_character(
        project, character_request(kSkeletonId, kMeshId, {{"hero", kClipId}, {"hero", kClipId}}));
    CY_REQUIRE_EQ(played.clips.size(), 1U);
    CY_REQUIRE_EQ(played.refused.size(), 1U);
    CY_CHECK_NE(played.refused[0].second.find("already has this name"), std::string::npos);
    const u64 generation = project.preview.mesh_generation();
    project.assets.joint_offset = 3;
    Array<u8> reply(allocator());
    const editor::AnimationRefusal refusal =
        ask(&project.preview, "animation.character.set", character_request(), reply);
    CY_CHECK_EQ(std::string_view(refusal.code), "animation.character.failed");
    CY_CHECK_EQ(project.preview.mesh_generation(), generation);
    // A character that loads is a new mesh for the host to upload.
    project.assets.joint_offset = 0;
    (void)set_character(project, character_request());
    CY_CHECK_GT(project.preview.mesh_generation(), generation);
}

CY_TEST_CASE("editor animation: a project clip plays on or holds as its node says") {
    Project project;
    (void)set_character(project, character_request());
    // The node says hold: playing past the end stops on the last frame, as the bake writes it.
    const std::string held = hero_graph("", "(0, 0, 0, 0, 0)");
    Array<u8> request = Request().u32v(1).text(held).u64v(1).f32v(0.5F).u8v(1).u32v(0).take();
    Array<u8> reply(allocator());
    CY_REQUIRE_FALSE(ask(&project.preview, "animation.preview.set", request, reply).refused());
    CY_REQUIRE(project.preview.tick(1.0F).has_value());
    CY_CHECK_EQ(project.preview.state().time, 1.0F);
    CY_CHECK_FALSE(project.preview.state().playing);
    // The node says loop: it wraps.
    request = Request().u32v(1).text(hero_graph("")).u64v(1).f32v(0.5F).u8v(1).u32v(0).take();
    CY_REQUIRE_FALSE(ask(&project.preview, "animation.preview.set", request, reply).refused());
    CY_REQUIRE(project.preview.tick(1.0F).has_value());
    CY_CHECK_NEAR(project.preview.state().time, 0.5F, 1e-5F);
    CY_CHECK(project.preview.state().playing);
}

CY_TEST_CASE("editor animation: a clip two nodes sample is baked once") {
    Project project;
    std::string twice = hero_graph("footstep@0.26");
    twice = replaced(twice, "link 1 \"pose\" -> 2 \"pose\"\n",
                     "node 3 \"pose.clip\" v1 {\n"
                     "    prop \"clip\" : \"name\" = \"hero\"\n"
                     "    prop \"duration\" : \"float\" = (1, 0, 0, 0, 0)\n"
                     "    prop \"events\" : \"name\" = (0, 0, 0, 0, 0)\n"
                     "    prop \"loop\" : \"bool\" = (0, 0, 0, 0, 1)\n"
                     "    prop \"time_parameter\" : \"name\" = (0, 0, 0, 0, 0)\n"
                     "}\n"
                     "node 4 \"pose.state\" v1 {\n"
                     "    prop \"name\" : \"name\" = \"again\"\n"
                     "}\n"
                     "link 1 \"pose\" -> 2 \"pose\"\n"
                     "link 3 \"pose\" -> 4 \"pose\"\n");
    const Baked baked = bake(project, bake_request("hero", twice));
    CY_REQUIRE(baked.baked);
    CY_CHECK_EQ(baked.files.size(), 3U);
    editor::AnimationRigManifest manifest(allocator());
    CY_REQUIRE(editor::read_animation_rig(baked.file("rig.cyrig"), manifest).has_value());
    CY_CHECK_EQ(manifest.clips.size(), 1U);
}

CY_TEST_CASE("editor animation: a bake cooks the authored events into the clip a game loads") {
    Project project;
    // The graph is the one the Rust suites write, and the request the one their encoder sends.
    const std::string graph = hero_graph();
    Array<u8> graph_text(allocator());
    CY_REQUIRE(
        graph_text.append({reinterpret_cast<const u8*>(graph.data()), graph.size()}).has_value());
    CY_CHECK_EQ(graph, committed("animation_hero_v1.cyanimgraph", graph_text));
    const Array<u8> request = bake_request("hero", hero_graph());
    CY_CHECK_EQ(text_of(request), committed("animation_bake_request_v1.wire", request));
    const Baked baked = bake(project, request, "animation_bake_v1.wire");
    CY_REQUIRE(baked.baked);
    CY_CHECK(baked.diagnostics.empty());
    CY_REQUIRE_EQ(baked.files.size(), 3U);
    // The manifest names the character's own skeleton and mesh, and every file it baked.
    editor::AnimationRigManifest manifest(allocator());
    CY_REQUIRE(editor::read_animation_rig(baked.file("rig.cyrig"), manifest).has_value());
    CY_CHECK_EQ(manifest.rig, Name::intern("hero"));
    CY_CHECK_EQ(manifest.model, Name::intern(kModel));
    CY_CHECK(manifest.skeleton == kSkeletonId);
    CY_CHECK(manifest.mesh == kMeshId);
    CY_REQUIRE_EQ(manifest.clips.size(), 1U);
    CY_CHECK_EQ(manifest.clips[0].name, Name::intern("hero"));
    // The clip: named as the graph names it, the importer's motion untouched, the graph's events
    // in time order in place of none.
    animation::Clip clip(allocator());
    Array<Name> joints(allocator());
    CY_REQUIRE(
        animation::decode_clip(record_of(baked.file(manifest.clips[0].path.text())), clip, joints)
            .has_value());
    CY_CHECK_EQ(clip.name(), Name::intern("hero"));
    CY_REQUIRE_EQ(clip.events().size(), 2U);
    CY_CHECK_EQ(clip.events()[0].name, Name::intern("footstep"));
    CY_CHECK_EQ(clip.events()[0].time, 0.26F);
    CY_CHECK_EQ(clip.events()[1].name, Name::intern("land"));
    CY_CHECK_EQ(clip.events()[1].time, 0.76F);
    CY_CHECK(clip.loop_mode() == animation::LoopMode::Loop);
    Direct direct;
    u16 offending = 0;
    CY_CHECK(animation::clip_matches_skeleton(clip, joints.span(), direct.skeleton, offending));
    // The program loads without the compiler, binds to the hero's skeleton and that clip, and a
    // tick across 0.26 s emits the footstep — what a game's animation system delivers.
    Expected<graph::pose::PoseProgram, Error> program =
        animation::decode_program(allocator(), record_of(baked.file("program.cyasset")));
    CY_REQUIRE(program.has_value());
    const animation::Clip* table[] = {&clip};
    animation::AnimationRig rig(allocator());
    CY_REQUIRE(rig.bind(direct.skeleton, *program, Span<const animation::Clip* const>(table, 1))
                   .has_value());
    animation::AnimationInstance instance(allocator());
    CY_REQUIRE(instance.prepare(rig).has_value());
    animation::EventBuffer events(allocator());
    std::vector<std::pair<std::string, u32>> fired;
    for (u32 tick = 1; tick <= 60; ++tick) {
        events.clear();
        CY_REQUIRE(animation::advance(rig, instance, 1.0F / 60.0F, &events).has_value());
        for (const animation::EmittedEvent& event : events.events()) {
            fired.emplace_back(std::string(event.name.text()), tick);
        }
    }
    CY_REQUIRE_EQ(fired.size(), 2U);
    CY_CHECK_EQ(fired[0].first, "footstep");
    CY_CHECK_EQ(fired[0].second, 16U);
    CY_CHECK_EQ(fired[1].first, "land");
    CY_CHECK_EQ(fired[1].second, 46U);
}

CY_TEST_CASE("editor animation: a baked clip holds or loops as its node says") {
    Project project;
    const Baked held = bake(project, bake_request("hero", hero_graph("", "(0, 0, 0, 0, 0)")));
    CY_REQUIRE(held.baked);
    animation::Clip clip(allocator());
    Array<Name> joints(allocator());
    CY_REQUIRE(
        animation::decode_clip(record_of(held.file("clips/0.cyasset")), clip, joints).has_value());
    CY_CHECK(clip.loop_mode() == animation::LoopMode::None);
    CY_CHECK(clip.events().empty());
    // And its motion is the importer's, sample for sample.
    Direct direct;
    for (const f32 time : {0.0F, 0.4F, 1.0F}) {
        std::vector<Transform> pose(direct.skeleton.joint_count());
        direct.skeleton.reference_pose(Span<Transform>(pose.data(), pose.size()));
        animation::ClipCursor cursor(allocator());
        CY_REQUIRE(cursor.reset(clip.track_count()).has_value());
        animation::SampleStats stats;
        CY_REQUIRE(clip.sample_unwrapped(time, direct.skeleton.retained(0), cursor,
                                         Span<Transform>(pose.data(), pose.size()), stats)
                       .has_value());
        check_same_pose(pose, direct.sample(time));
    }
}

CY_TEST_CASE("editor animation: a graph with an error bakes nothing and says where") {
    Project project;
    const Baked refused = bake(project, bake_request("hero", hero_graph("footstep@1.5")));
    CY_CHECK_FALSE(refused.baked);
    CY_CHECK(refused.files.empty());
    CY_CHECK(std::ranges::any_of(refused.diagnostics, [](const Diagnosed& diagnostic) {
        return diagnostic.code == "animation.event.outside" && diagnostic.node == 1;
    }));
    // The mannequin's walk is not the hero's: the bake checks against the character it bakes for.
    const Baked unknown = bake(project, bake_request("hero", read_file(kGraph)));
    CY_CHECK_FALSE(unknown.baked);
}

CY_TEST_CASE("editor animation: a bake is refused without a cook, a character or a rig name") {
    Project project;
    Array<u8> reply(allocator());
    CY_CHECK_EQ(
        std::string_view(editor::answer_animation(nullptr, "animation.bake",
                                                  bake_request("hero", hero_graph()).span(), reply)
                             .code),
        "animation.bake.unavailable");
    editor::AnimationRigBaker baker(allocator(), project.assets);
    CY_CHECK_EQ(
        std::string_view(editor::answer_animation(
                             nullptr, "animation.bake",
                             bake_request("hero", hero_graph(), AssetId{}).span(), reply, &baker)
                             .code),
        "animation.bake.character");
    CY_CHECK_EQ(std::string_view(editor::answer_animation(
                                     nullptr, "animation.bake",
                                     bake_request("../hero", hero_graph()).span(), reply, &baker)
                                     .code),
                "animation.request.malformed");
    CY_CHECK_EQ(std::string_view(editor::answer_animation(
                                     nullptr, "animation.bake",
                                     bake_request("hero", hero_graph(), AssetId{7, 7}).span(),
                                     reply, &baker)
                                     .code),
                "animation.bake.failed");
}

CY_TEST_CASE("editor animation: a rig manifest reads back what was written, and nothing else") {
    editor::AnimationRigManifest manifest(allocator());
    manifest.rig = Name::intern("hero");
    manifest.model = Name::intern(kModel);
    manifest.skeleton = kSkeletonId;
    manifest.program = Name::intern("program.cyasset");
    CY_REQUIRE(manifest.clips
                   .push_back(editor::AnimationRigClip{Name::intern("Walking"),
                                                       Name::intern("clips/0.cyasset")})
                   .has_value());
    Array<char> text(allocator());
    CY_REQUIRE(editor::write_animation_rig(manifest, text).has_value());
    const std::string written(text.data(), text.size());
    CY_CHECK_NE(written.find("mesh \"\"\n"), std::string::npos);
    editor::AnimationRigManifest read(allocator());
    CY_REQUIRE(editor::read_animation_rig(written, read).has_value());
    CY_CHECK_EQ(read.rig, manifest.rig);
    CY_CHECK(read.skeleton == kSkeletonId);
    CY_CHECK(read.mesh.is_nil());
    CY_REQUIRE_EQ(read.clips.size(), 1U);
    CY_CHECK_EQ(read.clips[0].path, Name::intern("clips/0.cyasset"));
    for (const char* bad :
         {"cyrig 2\nrig \"a\"\n", "cyrig 1\nrig a\n", "cyrig 1\nrig \"a\"\nprogram \"p\"\n",
          "cyrig 1\nwhat \"a\"\n", "cyrig 1\nclip \"only-one\"\n"}) {
        CY_CHECK_MESSAGE(!editor::read_animation_rig(bad, read).has_value(), bad);
    }
    manifest.model = Name::intern("a\"b");
    CY_CHECK_FALSE(editor::write_animation_rig(manifest, text).has_value());
}

#    endif  // CY_EDITOR_TEST_HAS_IMPORT

#endif
