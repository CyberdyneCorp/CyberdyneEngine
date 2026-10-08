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
// `data/animation_catalogue_v1.wire`, `animation_compile_v1.wire`, `animation_compile_cut_v1.wire`
// and `animation_preview_state_v1.wire` are the engine's answers, which the Rust suites decode and
// replay to an MCP client as the runtime's. Regenerate them with `CY_UPDATE_ANIMATION_WIRE=1` after
// a deliberate change to a reply.

#include <cy/abi/cy_abi.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/editor/animation_service.h>
#include <cy/editor/material_service.h>
#include <cy/graph/text.h>
#include <cy/test/test.h>
#if defined(CY_EDITOR_HAS_ANIMATION)
#    include <cy/animation/evaluate.h>
#    include <cy/editor/animation_preview.h>
#endif

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

    Request&& u8v(u8 value) && {
        put(&value, 1);
        return std::move(*this);
    }
    Request&& u32v(u32 value) && {
        put(&value, 4);
        return std::move(*this);
    }
    Request&& u64v(u64 value) && {
        put(&value, 8);
        return std::move(*this);
    }
    Request&& f32v(f32 value) && {
        put(&value, 4);
        return std::move(*this);
    }
    Request&& text(std::string_view value) && {
        const auto size = static_cast<u32>(value.size());
        put(&size, 4);
        put(value.data(), value.size());
        return std::move(*this);
    }
    [[nodiscard]] Array<u8> take() && { return std::move(bytes_); }

private:
    void put(const void* data, usize size) {
        CY_REQUIRE(bytes_.append({static_cast<const u8*>(data), size}).has_value());
    }

    Array<u8> bytes_;
};

Array<u8> compile_request(std::string_view source) {
    return Request().u32v(1).text(source).take();
}

Array<u8> preview_request(std::string_view source, u64 focus, f32 time,
                          std::vector<std::pair<std::string, f32>> parameters = {}) {
    Request request = Request().u32v(1).text(source).u64v(focus).f32v(time).u8v(0).u32v(
        static_cast<u32>(parameters.size()));
    for (const auto& [name, value] : parameters) {
        request = std::move(request).text(name).f32v(value);
    }
    return std::move(request).take();
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
    for (const Diagnosed& diagnostic : compiled.diagnostics) {
        if (diagnostic.code == code && diagnostic.node == node) {
            return true;
        }
    }
    return false;
}

}  // namespace

CY_TEST_CASE("editor animation: the acceptance graph is the engine's canonical text") {
    // The Rust editor writes it call by call; the engine reads it back to the same bytes, so a text
    // diff of the file is a semantic diff and the engine's merge reads it unchanged.
    for (const char* name : {kGraph, kEdited}) {
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
std::vector<Transform> evaluate_directly(const editor::AnimationPreview& character,
                                         const std::string& source, f32 time,
                                         std::vector<std::pair<std::string, f32>> parameters) {
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
        CY_CHECK_MESSAGE(
            std::memcmp(&shown[joint].rotation, &direct[joint].rotation, sizeof(Quat)) == 0,
            "joint ", joint);
    }
}

Shown preview(Character& character, const Array<u8>& request) {
    Array<u8> reply(allocator());
    const editor::AnimationRefusal refusal =
        ask(&character.preview, "animation.preview.set", request, reply);
    CY_REQUIRE_MESSAGE(!refusal.refused(), refusal.code, ": ", refusal.detail);
    return decode_state(text_of(reply));
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
        replaced(read_file(kGraph), "prop \"duration\" : \"float\" = (0.25, 0, 0, 0, 0)",
                 "prop \"duration\" : \"float\" = (0, 0, 0, 0, 0)");
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
        moved = moved || std::memcmp(&matrix, &identity, sizeof(Mat4)) != 0;
    }
    CY_CHECK(moved);
    // Stopping hides it.
    CY_REQUIRE_FALSE(
        ask(&character.preview, "animation.preview.stop", Array<u8>(allocator()), reply).refused());
    CY_CHECK_EQ(text_of(reply), committed("animation_preview_stopped_v1.wire", reply));
    CY_CHECK_FALSE(decode_state(text_of(reply)).active);
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

#endif
