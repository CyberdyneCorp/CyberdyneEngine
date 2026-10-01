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
// replay to an MCP client as the runtime's. Regenerate them with `CY_UPDATE_SCRIPT_WIRE=1` after a
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
    PlayStage() {
        CY_REQUIRE(world.initialize().has_value());
        CY_REQUIRE(tree.initialize().has_value());
        const auto node = tree.create_node(Name::intern("Tank"), tree.root());
        CY_REQUIRE(node.has_value());
        unit = node->entity();
        graphs.start(tree, &audio);
        graph::DiagnosticSink sink(allocator());
        const auto loaded =
            graphs.load(Name::intern("unit_command"), read_file("script_unit_command_v1.cyscript"),
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

    bool playing = true;
};

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
