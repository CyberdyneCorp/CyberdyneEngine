// SPDX-License-Identifier: MIT
// The gameplay graph editor, engine side (#29, visual scripting): the vocabulary, the compiler's
// answer, and Play's graphs, as the editor reaches them through `script.*`.
//
// THE WIRE, FROM BOTH SIDES. `data/script_unit_command_v1.cyscript` is the graph the Rust editor's
// MCP tools write for the acceptance scenario (its suites compare their file with this one byte
// for byte), and `data/script_raise_request_v1.wire` is the raise its encoder writes. This suite
// compiles and submits exactly those bytes. `data/script_catalogue_v1.wire`,
// `data/script_compile_v1.wire`, `data/script_compile_error_v1.wire` and
// `data/script_state_play_v1.wire` are the engine's answers, which the Rust suites decode and
// replay to an MCP client as the runtime's. So are the Play debugger's and hot reload's
// (#84): `data/script_debug_paused_v1.wire` (Play paused at a breakpoint, with a variable and two
// watched pins), `data/script_reload_v1.wire` and `data/script_reload_refused_v1.wire`, over
// `data/script_unit_counter_v1.cyscript`. Regenerate them with `CY_UPDATE_SCRIPT_WIRE=1` after a
// deliberate change to a reply.

#include <cy/abi/cy_abi.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/editor/material_service.h>
#include <cy/editor/script_service.h>
#include <cy/game_backend/graph_behaviours.h>
#include <cy/graph/text.h>
#include <cy/scene/node.h>
#include <cy/scene/tree.h>
#include <cy/test/test.h>

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
    return system_allocator(MemoryDomain::Scripting);
}

std::filesystem::path wire(std::string_view name) {
    return std::filesystem::path(CY_SCRIPT_WIRE_DIR) / name;
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

/// The committed reply; with `CY_UPDATE_SCRIPT_WIRE=1`, written from `reply` first.
std::string committed(std::string_view name, const Array<u8>& reply) {
    const char* update = std::getenv("CY_UPDATE_SCRIPT_WIRE");
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
        const std::string_view raw = take(4);
        u32 value = 0;
        std::memcpy(&value, raw.data(), 4);
        return value;
    }
    u64 u64v() {
        const std::string_view raw = take(8);
        u64 value = 0;
        std::memcpy(&value, raw.data(), 8);
        return value;
    }
    f32 f32v() {
        const u32 bits = u32v();
        f32 value = 0.0F;
        std::memcpy(&value, &bits, 4);
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

struct Diagnosed {
    u8 severity = 0;
    std::string code;
    u64 node = 0;
    std::string pin;
    std::string message;
    std::string detail;
};

struct Compiled {
    bool compiled = false;
    u32 instructions = 0;
    std::vector<std::pair<std::string, u64>> handlers;
    std::vector<std::string> externals;
    std::vector<Diagnosed> diagnostics;
    std::string listing;
};

Compiled decode_compile(std::string bytes) {
    Reply reply(std::move(bytes));
    Compiled out;
    CY_REQUIRE_EQ(reply.u32v(), 1U);
    out.compiled = reply.u8v() != 0;
    (void)reply.u64v();
    (void)reply.u64v();
    out.instructions = reply.u32v();
    (void)reply.u32v();
    (void)reply.u32v();
    (void)reply.u32v();
    for (u32 count = reply.u32v(); count > 0; --count) {
        std::string event = reply.text();
        const u64 node = reply.u64v();
        (void)reply.u32v();
        out.handlers.emplace_back(event, node);
    }
    for (u32 count = reply.u32v(); count > 0; --count) {
        out.externals.push_back(reply.text());
        (void)reply.u8v();
    }
    for (u32 count = reply.u32v(); count > 0; --count) {
        (void)reply.text();
        (void)reply.u8v();
    }
    for (u32 count = reply.u32v(); count > 0; --count) {
        Diagnosed diagnostic;
        diagnostic.severity = reply.u8v();
        diagnostic.code = reply.text();
        diagnostic.node = reply.u64v();
        diagnostic.pin = reply.text();
        diagnostic.message = reply.text();
        diagnostic.detail = reply.text();
        (void)reply.u64v();
        out.diagnostics.push_back(diagnostic);
    }
    out.listing = reply.text();
    CY_CHECK(reply.done());
    return out;
}

Array<u8> compile_request(std::string_view source) {
    Array<u8> request(allocator());
    const u32 header[2] = {1, static_cast<u32>(source.size())};
    CY_REQUIRE(request.append({reinterpret_cast<const u8*>(header), sizeof(header)}).has_value());
    CY_REQUIRE(
        request.append({reinterpret_cast<const u8*>(source.data()), source.size()}).has_value());
    return request;
}

constexpr u64 kUnitIdentity = 0x0123456789ABCDEFULL;

/// One cue, `unit.arrived`, and nothing else: what a project with one arrival sound offers.
class OneCue final : public abi::game::AudioBackend {
public:
    CyResult find_cue(const char* name, CyAudioCue& out_cue) noexcept override {
        out_cue = 7;
        return std::string_view(name) == "unit.arrived" ? CY_RESULT_OK : CY_RESULT_NOT_FOUND;
    }
    CyResult play(const CyAudioPlay& /*play*/, CyAudioVoice& out_voice) noexcept override {
        out_voice = ++plays;
        return CY_RESULT_OK;
    }
    CyResult stop(CyAudioVoice /*voice*/, f32 /*fade*/) noexcept override { return CY_RESULT_OK; }
    bool playing(CyAudioVoice /*voice*/) const noexcept override { return false; }
    CyResult find_bus(const char* /*name*/, CyAudioBus& /*bus*/) noexcept override {
        return CY_RESULT_NOT_FOUND;
    }
    CyResult set_bus_volume(CyAudioBus /*bus*/, f32 /*volume*/, f32 /*fade*/) noexcept override {
        return CY_RESULT_OK;
    }

    u64 plays = 0;
};

/// The world Play runs: one unit at the origin running the editor's graph.
///
/// Built outside `Play` on purpose: on Linux other than x86, doctest's assertions break into the
/// debugger through an unqualified `raise(SIGTRAP)`, which inside `Play` would find its `raise`.
/// For the same reason no local in this file is named `raise`.
struct PlayStage {
    explicit PlayStage(const char* file = "script_unit_command_v1.cyscript",
                       const char* name = "unit_command") {
        CY_REQUIRE(world.initialize().has_value());
        CY_REQUIRE(tree.initialize().has_value());
        const auto node = tree.create_node(Name::intern("Tank"), tree.root());
        CY_REQUIRE(node.has_value());
        unit = node->entity();
        graphs.start(tree, &audio);
        graph::DiagnosticSink sink(allocator());
        const auto loaded = graphs.load(Name::intern(name), read_file(file),
                                        game_backend::GraphBackend::Bytecode, sink);
        CY_REQUIRE(loaded.has_value());
        CY_REQUIRE(graphs.attach(*loaded, unit).has_value());
    }

    ecs::World world{system_allocator(MemoryDomain::World)};
    scene::SceneTree tree{world};
    OneCue audio;
    game_backend::GraphBehaviours graphs{allocator()};
    ecs::Entity unit;
};

/// A host's Play over that world.
class Play final : public editor::ScriptPlayRuntime, public PlayStage {
public:
    Play() = default;
    Play(const char* file, const char* name) : PlayStage(file, name) {}

    const game_backend::GraphBehaviours* behaviours() const noexcept override {
        return playing ? &graphs : nullptr;
    }
    ecs::Entity entity_for(u64 identity) const noexcept override {
        return identity == kUnitIdentity ? unit : ecs::Entity{};
    }
    u64 identity_of(ecs::Entity entity) const noexcept override {
        return entity == unit ? kUnitIdentity : 0;
    }
    Expected<u32, Error> raise(ecs::Entity entity, Name event,
                               Span<const f32> arguments) noexcept override {
        return graphs.raise(entity, event, arguments);
    }
    Status set_breakpoint(Name graph_name, u64 node, ecs::Entity entity,
                          bool enabled) noexcept override {
        return graphs.set_breakpoint(graph_name, node, entity, enabled);
    }
    Status debug(editor::ScriptDebugAction action) noexcept override {
        switch (action) {
            case editor::ScriptDebugAction::Pause:
                return graphs.debug_pause();
            case editor::ScriptDebugAction::Continue:
                return graphs.debug_continue();
            case editor::ScriptDebugAction::StepInto:
                return graphs.debug_step(game_backend::GraphStep::Into);
            case editor::ScriptDebugAction::StepOver:
                return graphs.debug_step(game_backend::GraphStep::Over);
        }
        return fail(ErrorCode::InvalidArgument, "no such action");
    }
    Expected<u32, Error> reload(std::string_view reference, std::string_view source,
                                graph::DiagnosticSink& sink) noexcept override {
        const std::string stem = std::filesystem::path(reference).stem().string();
        return graphs.reload(Name::intern(stem), source, sink);
    }

    bool playing = true;
};

/// A request the way the editor's encoder writes it: little-endian scalars, length-prefixed text.
/// Built in one expression from a temporary, so each step hands the request on.
class Request {
public:
    Request() : bytes_(allocator()) { put_u32(1); }

    Request&& u8v(u8 value) && {
        put(&value, 1);
        return std::move(*this);
    }
    Request&& u32v(u32 value) && {
        put_u32(value);
        return std::move(*this);
    }
    Request&& u64v(u64 value) && {
        put(&value, 8);
        return std::move(*this);
    }
    Request&& text(std::string_view value) && {
        put_u32(static_cast<u32>(value.size()));
        put(value.data(), value.size());
        return std::move(*this);
    }
    [[nodiscard]] const Array<u8>& bytes() const { return bytes_; }

private:
    void put_u32(u32 value) { put(&value, 4); }
    void put(const void* data, usize size) {
        CY_REQUIRE(bytes_.append({static_cast<const u8*>(data), size}).has_value());
    }

    Array<u8> bytes_;
};

constexpr const char* kCounterFile = "script_unit_counter_v1.cyscript";

/// The debug state's leading fields: whether Play runs, is being debugged, and where it paused.
struct DebugHead {
    bool playing = false;
    bool debugging = false;
    bool paused = false;
    u8 reason = 0;
    u64 entity = 0;
    std::string graph;
    u64 node = 0;
    u64 breakpoints = 0;
    u32 trace = 0;
};

/// Read the debug state up to the inspected instance; `reply` is left at its variables.
DebugHead debug_head(Reply& reply) {
    DebugHead head;
    CY_REQUIRE_EQ(reply.u32v(), 1U);
    head.playing = reply.u8v() != 0;
    head.debugging = reply.u8v() != 0;
    head.paused = reply.u8v() != 0;
    head.reason = reply.u8v();
    head.entity = reply.u64v();
    head.graph = reply.text();
    head.node = reply.u64v();
    (void)reply.u64v();
    (void)reply.u64v();
    for (u32 count = reply.u32v(); count > 0; --count) {
        (void)reply.text();
        (void)reply.u64v();
        (void)reply.u64v();
        ++head.breakpoints;
    }
    head.trace = reply.u32v();
    for (u32 count = head.trace; count > 0; --count) {
        (void)reply.u64v();
        (void)reply.u64v();
        (void)reply.u64v();
        (void)reply.text();
        (void)reply.u64v();
    }
    return head;
}

editor::ScriptRefusal ask(editor::ScriptPlayRuntime* play, std::string_view operation,
                          const Array<u8>& request, Array<u8>& reply) {
    return editor::answer_script(play, operation, request.span(), reply);
}

}  // namespace

CY_TEST_CASE("editor script: the catalogue is the engine's script vocabulary and declared names") {
    Array<u8> first(allocator());
    Array<u8> second(allocator());
    CY_REQUIRE(editor::encode_script_catalogue(first).has_value());
    CY_REQUIRE(editor::encode_script_catalogue(second).has_value());
    CY_CHECK(text_of(first) == text_of(second));
    CY_CHECK(committed("script_catalogue_v1.wire", first) == text_of(first));

    graph::NodeRegistry registry(allocator());
    CY_REQUIRE(game_backend::register_gameplay_graph_nodes(registry).has_value());
    Reply reply(text_of(first));
    CY_REQUIRE_EQ(reply.u32v(), 3U);
    (void)reply.u32v();
    const u32 nodes = reply.u32v();
    // Every registered script node but the single-entry one: an event graph starts at events.
    CY_CHECK_EQ(nodes, static_cast<u32>(registry.size()) - 1U);
    bool saw_event = false;
    std::vector<std::string> functions;
    for (u32 node = 0; node < nodes; ++node) {
        CY_CHECK_NE(reply.u32v(), 0U);
        (void)reply.u32v();
        const std::string name = reply.text();
        CY_CHECK_NE(name, "script.entry");
        saw_event = saw_event || name == "script.on_event";
        (void)reply.u8v();
        std::vector<u32> pins;
        for (u32 pin = reply.u32v(); pin > 0; --pin) {
            pins.push_back(reply.u32v());
            (void)reply.u8v();
            (void)reply.text();
            (void)reply.text();
        }
        for (usize index = 0; index < pins.size(); ++index) {
            CY_CHECK_EQ(pins[index], static_cast<u32>(index + 1));
        }
        for (u32 property = reply.u32v(); property > 0; --property) {
            (void)reply.u32v();
            (void)reply.u8v();
            const std::string property_name = reply.text();
            (void)reply.text();
            (void)reply.text();
            CY_CHECK(reply.text().starts_with("literal:"));
            (void)reply.text();
            std::vector<std::string> choices;
            for (u32 choice = reply.u32v(); choice > 0; --choice) {
                choices.push_back(reply.text());
            }
            if (name == "script.call") {
                functions = choices;
            }
            (void)reply.text();
            CY_CHECK_EQ(reply.text(), "visual-scripting");
            (void)reply.u64v();
            (void)reply.u8v();
            (void)reply.u8v();
            (void)reply.u64v();
            (void)reply.u64v();
            (void)reply.u64v();
            (void)property_name;
        }
    }
    CY_CHECK(reply.done());
    CY_CHECK(saw_event);
    CY_CHECK(functions == std::vector<std::string>{"unit.move_to", "unit.set_speed", "unit.stop"});
}

CY_TEST_CASE("editor script: the editor's graph is the engine's canonical text and compiles") {
    const std::string source = read_file("script_unit_command_v1.cyscript");
    graph::NodeRegistry registry(allocator());
    CY_REQUIRE(game_backend::register_gameplay_graph_nodes(registry).has_value());
    graph::DiagnosticSink sink(allocator());
    const auto parsed = graph::parse_graph(source, &registry, allocator(), sink);
    CY_REQUIRE(parsed.has_value());
    Array<char> written(allocator());
    CY_REQUIRE(graph::write_graph(*parsed, written).has_value());
    // BYTE FOR BYTE: the editor writes the engine's canonical form, so a diff is a semantic diff.
    CY_CHECK(std::string(written.data(), written.size()) == source);

    Array<u8> reply(allocator());
    const auto refusal = ask(nullptr, "script.compile", compile_request(source), reply);
    CY_REQUIRE(!refusal.refused());
    CY_CHECK(committed("script_compile_v1.wire", reply) == text_of(reply));
    const Compiled compiled = decode_compile(text_of(reply));
    CY_CHECK(compiled.compiled);
    CY_CHECK(compiled.diagnostics.empty());
    CY_REQUIRE_EQ(compiled.handlers.size(), 1U);
    CY_CHECK_EQ(compiled.handlers[0].first, "unit.command");
    CY_CHECK_EQ(compiled.handlers[0].second, 1U);
    CY_CHECK(compiled.externals ==
             std::vector<std::string>{"event.x", "event.z", "unit.move_to", "cue.unit.arrived"});
    CY_CHECK_GT(compiled.instructions, 0U);
    CY_CHECK(compiled.listing.find("suspend") != std::string::npos);
}

CY_TEST_CASE("editor script: a misspelled function is refused on the node that names it") {
    const std::string source =
        replaced(read_file("script_unit_command_v1.cyscript"), "unit.move_to", "unit.mvoe_to");
    Array<u8> reply(allocator());
    const auto refusal = ask(nullptr, "script.compile", compile_request(source), reply);
    CY_REQUIRE(!refusal.refused());
    CY_CHECK(committed("script_compile_error_v1.wire", reply) == text_of(reply));
    const Compiled compiled = decode_compile(text_of(reply));
    CY_CHECK(!compiled.compiled);
    CY_REQUIRE_EQ(compiled.diagnostics.size(), 1U);
    CY_CHECK_EQ(compiled.diagnostics[0].severity, 2U);
    CY_CHECK_EQ(compiled.diagnostics[0].code, "script.external.unknown");
    CY_CHECK_EQ(compiled.diagnostics[0].node, 4U);
    CY_CHECK_EQ(compiled.diagnostics[0].detail, "unit.mvoe_to");
    CY_CHECK(compiled.listing.empty());
}

CY_TEST_CASE("editor script: source that does not parse is a diagnostic, not a failure") {
    Array<u8> reply(allocator());
    const auto refusal = ask(nullptr, "script.compile", compile_request("not a graph\n"), reply);
    CY_REQUIRE(!refusal.refused());
    const Compiled compiled = decode_compile(text_of(reply));
    CY_CHECK(!compiled.compiled);
    CY_REQUIRE_EQ(compiled.diagnostics.size(), 1U);
    CY_CHECK_EQ(compiled.diagnostics[0].code, "script.source.invalid");
    CY_CHECK_EQ(compiled.diagnostics[0].node, 0U);

    Array<u8> malformed(allocator());
    CY_REQUIRE(malformed.push_back(1).has_value());
    CY_CHECK_EQ(std::string(ask(nullptr, "script.compile", malformed, reply).code),
                "script.request.malformed");
}

CY_TEST_CASE("editor script: the editor's raise reaches Play's graph and the state reports it") {
    Play play;
    const std::string raise_wire = read_file("script_raise_request_v1.wire");
    Array<u8> request(allocator());
    CY_REQUIRE(request.append({reinterpret_cast<const u8*>(raise_wire.data()), raise_wire.size()})
                   .has_value());

    Array<u8> reply(allocator());
    auto refusal = ask(&play, "script.event.raise", request, reply);
    CY_REQUIRE(!refusal.refused());
    Reply started(text_of(reply));
    CY_CHECK_EQ(started.u32v(), 1U);
    CY_CHECK_EQ(started.u32v(), 1U);    // format
    CY_CHECK_EQ(started.u8v(), 1U);     // playing
    CY_CHECK_EQ(started.u64v(), 0U);    // tick
    CY_REQUIRE_EQ(started.u32v(), 1U);  // instances
    CY_CHECK_EQ(started.u64v(), kUnitIdentity);
    CY_CHECK_EQ(started.text(), "unit_command");
    CY_CHECK_EQ(started.u8v(), 1U);  // waiting
    CY_CHECK_EQ(started.text(), "unit.arrived");

    for (u32 step = 0; step < 400; ++step) {
        CY_REQUIRE(play.graphs.update(1.0F / 60.0F).has_value());
    }
    CY_CHECK_EQ(play.audio.plays, 1U);
    Array<u8> empty(allocator());
    refusal = ask(&play, "script.state.get", empty, reply);
    CY_REQUIRE(!refusal.refused());
    CY_CHECK(committed("script_state_play_v1.wire", reply) == text_of(reply));
    Reply state(text_of(reply));
    CY_CHECK_EQ(state.u32v(), 1U);
    CY_CHECK_EQ(state.u8v(), 1U);
    CY_CHECK_EQ(state.u64v(), 400U);
    CY_REQUIRE_EQ(state.u32v(), 1U);
    CY_CHECK_EQ(state.u64v(), kUnitIdentity);
    (void)state.text();
    CY_CHECK_EQ(state.u8v(), 0U);  // idle again
    (void)state.text();
    CY_CHECK_EQ(state.f32v(), 6.0F);
    CY_CHECK_EQ(state.f32v(), 0.0F);
    CY_CHECK_EQ(state.f32v(), 8.0F);
    CY_CHECK_EQ(state.u8v(), 0U);
    CY_CHECK_EQ(state.u32v(), 1U);
    (void)state.text();
    CY_REQUIRE_EQ(state.u32v(), 1U);  // one cue
    CY_CHECK_EQ(state.u64v(), kUnitIdentity);
    CY_CHECK_EQ(state.text(), "unit.arrived");
}

CY_TEST_CASE("editor script: without Play the Play operations are refused by name") {
    Play play;
    play.playing = false;
    const std::string raise_wire = read_file("script_raise_request_v1.wire");
    Array<u8> request(allocator());
    CY_REQUIRE(request.append({reinterpret_cast<const u8*>(raise_wire.data()), raise_wire.size()})
                   .has_value());
    Array<u8> reply(allocator());
    CY_CHECK_EQ(std::string(ask(&play, "script.event.raise", request, reply).code),
                "script.play.unavailable");
    CY_CHECK_EQ(std::string(ask(nullptr, "script.event.raise", request, reply).code),
                "script.play.unavailable");
    Array<u8> empty(allocator());
    CY_REQUIRE(!ask(nullptr, "script.state.get", empty, reply).refused());
    Reply state(text_of(reply));
    CY_CHECK_EQ(state.u32v(), 1U);
    CY_CHECK_EQ(state.u8v(), 0U);
}

CY_TEST_CASE("editor script: the material service routes script.* and lists it") {
    abi::Host host(allocator());
    editor::MaterialService service(allocator());
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);
    const Array<u8> request = compile_request(read_file("script_unit_command_v1.cyscript"));
    const CyServiceRequest compile{
        sizeof(CyServiceRequest), 1, 1, "script.compile", request.data(), request.size()};
    CY_REQUIRE_EQ(api->service_submit(&host, session, &compile), CY_RESULT_OK);
    CyServiceEvent event{};
    bool present = false;
    CY_REQUIRE_EQ(api->service_poll(&host, session, &event, &present), CY_RESULT_OK);
    CY_REQUIRE(present);
    CY_CHECK_EQ(event.kind, static_cast<u32>(CY_SERVICE_EVENT_COMPLETED));
    const std::string payload(reinterpret_cast<const char*>(event.payload), event.payload_size);
    CY_CHECK(decode_compile(payload).compiled);

    const CyServiceRequest raise_request{sizeof(CyServiceRequest), 1,       2,
                                         "script.event.raise",     nullptr, 0};
    CY_REQUIRE_EQ(api->service_submit(&host, session, &raise_request), CY_RESULT_OK);
    CY_REQUIRE_EQ(api->service_poll(&host, session, &event, &present), CY_RESULT_OK);
    CY_CHECK_EQ(event.kind, static_cast<u32>(CY_SERVICE_EVENT_FAILED));
    api->service_close(&host, session);
}

CY_TEST_CASE("editor script: the debugger stops Play at a breakpoint and reads its watches") {
    Play play(kCounterFile, "unit_counter");
    Array<u8> reply(allocator());
    const Request breakpoint = Request().text("unit_counter").u64v(11).u64v(kUnitIdentity).u8v(1);
    if constexpr (!graph::script::kGraphDebuggerEnabled) {
        // Compiled out: the runtime cannot attach the debugger, so a pause is refused by name.
        CY_CHECK(!play.graphs.set_debugging(true).has_value());
        CY_CHECK_EQ(
            std::string(ask(&play, "script.debug.control", Request().u8v(0).bytes(), reply).code),
            "script.debug.refused");
        return;
    }
    CY_REQUIRE(play.graphs.set_debugging(true).has_value());
    CY_REQUIRE(!ask(&play, "script.debug.breakpoint", breakpoint.bytes(), reply).refused());
    {
        Reply set(text_of(reply));
        const DebugHead head = debug_head(set);
        CY_CHECK(head.debugging);
        CY_CHECK(!head.paused);
        CY_CHECK_EQ(head.breakpoints, 1U);
    }
    // The editor's order, and Play stops before the count is written.
    const std::string raise_wire = read_file("script_raise_request_v1.wire");
    Array<u8> order(allocator());
    CY_REQUIRE(order.append({reinterpret_cast<const u8*>(raise_wire.data()), raise_wire.size()})
                   .has_value());
    CY_REQUIRE(!ask(&play, "script.event.raise", order, reply).refused());
    CY_REQUIRE(play.graphs.paused());

    const Request watch =
        Request().u64v(0).text("").u32v(2).u64v(10).text("value").u64v(8).text("value");
    CY_REQUIRE(!ask(&play, "script.debug.get", watch.bytes(), reply).refused());
    CY_CHECK(committed("script_debug_paused_v1.wire", reply) == text_of(reply));
    Reply paused(text_of(reply));
    const DebugHead head = debug_head(paused);
    CY_CHECK(head.playing);
    CY_CHECK(head.paused);
    CY_CHECK_EQ(head.reason, 0U);  // a breakpoint
    CY_CHECK_EQ(head.entity, kUnitIdentity);
    CY_CHECK_EQ(head.graph, "unit_counter");
    CY_CHECK_EQ(head.node, 11U);
    CY_CHECK_GE(head.trace, 4U);
    CY_CHECK_EQ(paused.u64v(), kUnitIdentity);  // the paused instance is the one inspected
    CY_CHECK_EQ(paused.text(), "unit_counter");
    CY_REQUIRE_EQ(paused.u32v(), 1U);  // one variable
    CY_CHECK_EQ(paused.u64v(), 7U);
    CY_CHECK_EQ(paused.text(), "orders");
    CY_CHECK_EQ(paused.u8v(), static_cast<u8>(graph::script::ValueKind::Int));
    (void)paused.f32v();
    CY_CHECK_EQ(paused.u64v(), 0U);    // not written yet
    CY_REQUIRE_EQ(paused.u32v(), 2U);  // two watches, in the request's order
    CY_CHECK_EQ(paused.u64v(), 10U);
    CY_CHECK_EQ(paused.text(), "value");
    CY_CHECK_EQ(paused.u8v(), 1U);
    (void)paused.u8v();
    (void)paused.f32v();
    CY_CHECK_EQ(paused.u64v(), 1U);  // the sum about to be written
    CY_CHECK_EQ(paused.u64v(), 8U);
    CY_CHECK_EQ(paused.text(), "value");
    CY_CHECK_EQ(paused.u8v(), 1U);
    (void)paused.u8v();
    (void)paused.f32v();
    CY_CHECK_EQ(paused.u64v(), 0U);
    (void)paused.text();
    for (u32 field = 0; field < 7; ++field) {
        (void)paused.u32v();
    }
    CY_CHECK(paused.done());

    // Continue, and the write happens.
    CY_REQUIRE(!ask(&play, "script.debug.control", Request().u8v(1).bytes(), reply).refused());
    CY_CHECK(!play.graphs.paused());
    const Request inspect = Request().u64v(kUnitIdentity).text("unit_counter").u32v(0);
    CY_REQUIRE(!ask(&play, "script.debug.get", inspect.bytes(), reply).refused());
    Reply after(text_of(reply));
    CY_CHECK(!debug_head(after).paused);
    CY_CHECK_EQ(after.u64v(), kUnitIdentity);
    (void)after.text();
    CY_REQUIRE_EQ(after.u32v(), 1U);
    (void)after.u64v();
    (void)after.text();
    (void)after.u8v();
    (void)after.f32v();
    CY_CHECK_EQ(after.u64v(), 1U);

    // A step with nothing paused is refused, and says why; a bad action is malformed.
    CY_CHECK_EQ(
        std::string(ask(&play, "script.debug.control", Request().u8v(2).bytes(), reply).code),
        "script.debug.refused");
    CY_CHECK_EQ(
        std::string(ask(&play, "script.debug.control", Request().u8v(9).bytes(), reply).code),
        "script.request.malformed");
    CY_CHECK_EQ(
        std::string(ask(&play, "script.debug.breakpoint",
                        Request().text("unit_counter").u64v(11).u64v(99).u8v(1).bytes(), reply)
                        .code),
        "script.node.unknown");
    play.playing = false;
    CY_CHECK_EQ(
        std::string(ask(&play, "script.debug.control", Request().u8v(1).bytes(), reply).code),
        "script.play.unavailable");
    CY_REQUIRE(!ask(&play, "script.debug.get", watch.bytes(), reply).refused());
    Reply idle(text_of(reply));
    CY_CHECK(!debug_head(idle).playing);
}

CY_TEST_CASE("editor script: a reload keeps the running count, and a refused one names its node") {
    Play play(kCounterFile, "unit_counter");
    const f32 target[] = {1.0F, 0.0F, 1.0F};
    for (u32 order = 0; order < 2; ++order) {
        CY_REQUIRE(
            play.graphs.raise(play.unit, Name::intern("unit.command"), Span<const f32>(target, 3))
                .has_value());
    }
    const std::string source = read_file(kCounterFile);
    const std::string twice = replaced(source, R"(prop "value" : "int" = (0, 0, 0, 0, 1))",
                                       R"(prop "value" : "int" = (0, 0, 0, 0, 2))");
    Array<u8> reply(allocator());
    const Request accepted = Request().text("game/scripts/unit_counter.cyscript").text(twice);
    CY_REQUIRE(!ask(&play, "script.reload", accepted.bytes(), reply).refused());
    CY_CHECK(committed("script_reload_v1.wire", reply) == text_of(reply));
    {
        Reply staged(text_of(reply));
        CY_CHECK_EQ(staged.u32v(), 1U);
        CY_CHECK_EQ(staged.u8v(), 1U);
        CY_CHECK_EQ(staged.u32v(), 2U);
        CY_CHECK_EQ(staged.u32v(), 0U);
        CY_CHECK(staged.done());
    }
    CY_REQUIRE(play.graphs.update(1.0F / 60.0F).has_value());
    CY_REQUIRE(
        play.graphs.raise(play.unit, Name::intern("unit.command"), Span<const f32>(target, 3))
            .has_value());
    CY_CHECK_EQ(play.graphs.variable(0, 0).value.integer, 4);

    const std::string retyped =
        replaced(twice, R"(prop "type" : "name" = "int")", R"(prop "type" : "name" = "bool")");
    const Request refused_request =
        Request().text("game/scripts/unit_counter.cyscript").text(retyped);
    CY_REQUIRE(!ask(&play, "script.reload", refused_request.bytes(), reply).refused());
    CY_CHECK(committed("script_reload_refused_v1.wire", reply) == text_of(reply));
    Reply refused_reply(text_of(reply));
    CY_CHECK_EQ(refused_reply.u32v(), 1U);
    CY_CHECK_EQ(refused_reply.u8v(), 0U);
    CY_CHECK_EQ(refused_reply.u32v(), 0U);
    CY_REQUIRE_EQ(refused_reply.u32v(), 1U);
    CY_CHECK_EQ(refused_reply.u8v(), 2U);
    CY_CHECK_EQ(refused_reply.text(), "script.reload.type");
    CY_CHECK_EQ(refused_reply.u64v(), 7U);
    CY_CHECK_EQ(refused_reply.text(), "type");
    (void)refused_reply.text();
    CY_CHECK_EQ(refused_reply.text(), "int -> bool");
    (void)refused_reply.u64v();
    CY_CHECK(refused_reply.done());
    CY_REQUIRE(play.graphs.update(1.0F / 60.0F).has_value());
    CY_CHECK_EQ(play.graphs.generation(0), 2U);

    // A graph Play does not run cannot be reloaded, and that is not the graph's fault.
    const Request elsewhere = Request().text("game/scripts/other.cyscript").text(twice);
    CY_CHECK_EQ(std::string(ask(&play, "script.reload", elsewhere.bytes(), reply).code),
                "script.reload.unavailable");
    CY_CHECK_EQ(std::string(ask(nullptr, "script.reload", elsewhere.bytes(), reply).code),
                "script.play.unavailable");
}

CY_TEST_CASE("editor script: the counter graph is the engine's canonical text and compiles") {
    const std::string source = read_file(kCounterFile);
    graph::NodeRegistry registry(allocator());
    CY_REQUIRE(game_backend::register_gameplay_graph_nodes(registry).has_value());
    graph::DiagnosticSink sink(allocator());
    const auto parsed = graph::parse_graph(source, &registry, allocator(), sink);
    CY_REQUIRE(parsed.has_value());
    Array<char> written(allocator());
    CY_REQUIRE(graph::write_graph(*parsed, written).has_value());
    CY_CHECK(std::string(written.data(), written.size()) == source);
    Array<u8> reply(allocator());
    CY_REQUIRE(!ask(nullptr, "script.compile", compile_request(source), reply).refused());
    const Compiled compiled = decode_compile(text_of(reply));
    CY_CHECK(compiled.compiled);
    CY_CHECK(compiled.diagnostics.empty());
}
