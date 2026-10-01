// SPDX-License-Identifier: MIT
// cy/editor/script_service.h — the `script.*` operations of the editor backend service: the
// gameplay graph editor's engine side. Issue #29, visual scripting.
//
// The editor authors a gameplay graph as CyberGraph text (`cygraph 1`, `cy/graph/text.h`) and owns
// none of what that text means. The engine answers four questions about it:
//
//   script.catalogue.get  ()                       -> the node vocabulary: types, pins, properties
//   script.compile        (u32 1, text source)     -> the compiled program, or what refused it
//   script.event.raise    (u32 1, u64 node,        -> u32 handlers started, then the Play state
//                          text event, u32 n,
//                          n x f32 argument)
//   script.state.get      ()                       -> the Play state
//
// THE CATALOGUE is the material catalogue's schema 3 (`material.catalogue.get`), so one decoder
// reads both: per node a stable identity, `v1`, its name, a stage mask of 0, its pins (identity,
// direction, name, type) and its properties. Its node types are `cy::graph`'s script vocabulary
// minus `script.entry` — an event graph starts at events — and its property choices are the
// externals `game_backend::gameplay_graph_externals()` declares, so a palette offers exactly what
// the compiler will accept. A property's `semantic` is `literal:<type>`: the literal type the
// value is written at in the graph's text (`float`, `int`, `bool` or `name`).
//
// A COMPILE always completes. Its reply: u32 1, u8 compiled, u64 the source's semantic digest, u64
// the program's digest, u32 instructions, u32 blocks, u32 registers, u32 state slots, u32 handlers
// and per handler (text event, u64 node, u32 block), u32 externals and per external (text name,
// u8 kind), u32 accesses and per access (text resource, u8 mode: 0 read, 1 write), u32 diagnostics
// and per diagnostic (u8 severity: 0 info, 1 warning, 2 error; text code; u64 node; text pin;
// text message; text detail; u64 related node), and text listing (`script::disassemble`). Source
// that does not parse is one `script.source.invalid` diagnostic on node 0.
//
// THE PLAY STATE: u32 1, u8 playing, u64 tick, u32 instances and per instance (u64 node, text
// graph, u8 status: 0 idle, 1 waiting, 2 failed; text waiting; f32 x, y, z; u8 moving; u32 runs;
// text problem), u32 cues and per cue (u64 node, text cue, u64 tick, f32 x, y, z). `node` is the
// authored node's engine identity, the one the editor's mirror sends.
//
// Refusals (`u32 1, text code, text detail`): script.request.malformed, script.schema.unsupported,
// script.operation.unsupported, script.play.unavailable (no runtime seam, or Play is not running),
// script.node.unknown (raise names a node Play has no entity for), script.raise.failed.

#pragma once

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/ecs/entity.h>
#include <cy/game_backend/graph_behaviours.h>

#include <array>
#include <string_view>

namespace cy::editor {

/// The request and reply format every `script.*` payload begins with.
inline constexpr u32 kScriptWireFormat = 1;

/// Every operation `script.*` serves, in the order `capabilities.get` lists them.
inline constexpr std::array<std::string_view, 4> kScriptOperations{
    "script.catalogue.get", "script.compile", "script.event.raise", "script.state.get"};

/// The host's Play, as the gameplay graph editor reaches it. Implemented by the runtime that owns
/// the play session; the service never runs a graph itself.
class ScriptPlayRuntime {
public:
    ScriptPlayRuntime() = default;
    virtual ~ScriptPlayRuntime() = default;
    ScriptPlayRuntime(const ScriptPlayRuntime&) = delete;
    ScriptPlayRuntime& operator=(const ScriptPlayRuntime&) = delete;
    ScriptPlayRuntime(ScriptPlayRuntime&&) = delete;
    ScriptPlayRuntime& operator=(ScriptPlayRuntime&&) = delete;

    /// The graphs Play is running, or null when it is not.
    [[nodiscard]] virtual const game_backend::GraphBehaviours* behaviours() const noexcept = 0;
    /// The Play entity an authored node is simulating as, or a null entity.
    [[nodiscard]] virtual ecs::Entity entity_for(u64 identity) const noexcept = 0;
    /// The authored node a Play entity simulates, or zero.
    [[nodiscard]] virtual u64 identity_of(ecs::Entity entity) const noexcept = 0;
    /// Raise `event` on `entity`; answers the handlers started.
    [[nodiscard]] virtual Expected<u32, Error> raise(ecs::Entity entity, Name event,
                                                     Span<const f32> arguments) noexcept = 0;
};

/// Why a `script.*` request was refused, or an empty code when it was answered.
struct ScriptRefusal {
    const char* code = nullptr;
    const char* detail = nullptr;

    [[nodiscard]] bool refused() const noexcept { return code != nullptr; }
};

/// The node vocabulary, in the material catalogue's schema 3. Deterministic.
[[nodiscard]] Status encode_script_catalogue(Array<u8>& out) noexcept;

/// Compile `source` and encode the reply described above. Fails only for memory.
[[nodiscard]] Status encode_script_compile(std::string_view source, Array<u8>& out) noexcept;

/// Encode Play's state. `play` may be null, which encodes "not playing".
[[nodiscard]] Status encode_script_state(const ScriptPlayRuntime* play, Array<u8>& out) noexcept;

/// Answer one `script.*` request into `reply`.
[[nodiscard]] ScriptRefusal answer_script(ScriptPlayRuntime* play, std::string_view operation,
                                          Span<const u8> payload, Array<u8>& reply) noexcept;

}  // namespace cy::editor
