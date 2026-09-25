// SPDX-License-Identifier: MIT
#include <cy/editor/material_service.h>

#include <cy/graph/material/canvas.h>
#include <cy/graph/material/lower_material.h>
#include <cy/graph/text.h>
#include <cy/rendering/material/compiler.h>
#if defined(CY_EDITOR_HAS_VFX)
#    include <cy/vfx/authoring.h>
#    include <cy/vfx/authoring_capabilities.h>
#    include <cy/vfx/catalogue.h>
#    include <cy/vfx/compile.h>
#    include <cy/vfx/interfaces.h>
#    include <cy/vfx/world.h>
#endif

#include <cmath>
#include <cstring>
#include <iterator>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#if defined(CY_EDITOR_HAS_VFX)
struct VfxPreviewState {
    explicit VfxPreviewState(cy::Allocator& allocator) noexcept : world(allocator) {}

    [[nodiscard]] cy::Status restart() noexcept {
        if (!system) {
            return cy::fail(cy::ErrorCode::Unavailable, "no VFX preview effect is loaded");
        }
        world.shutdown();
        cy::vfx::WorldDescription description;
        description.pool_bytes = 8ULL * 1024ULL * 1024ULL;
        description.max_instances = 1;
        if (cy::Status ready = world.initialize(description); !ready) {
            return ready;
        }
        cy::vfx::EffectSpawn spawn;
        spawn.position = {0.0F, 0.0F, -5.0F};
        auto started = world.play(*system, spawn);
        if (!started) {
            world.shutdown();
            return cy::make_unexpected(started.error());
        }
        handle = *started;
        time_seconds = 0.0F;
        return cy::ok();
    }

    [[nodiscard]] cy::Status load(cy::vfx::CompiledSystem&& cooked) noexcept {
        world.shutdown();
        system.emplace(std::move(cooked));
        playing = false;
        time_scale = 1.0F;
        return restart();
    }

    [[nodiscard]] cy::Status advance(cy::f32 dt) noexcept {
        if (!system) {
            return cy::fail(cy::ErrorCode::Unavailable, "no VFX preview effect is loaded");
        }
        if (!std::isfinite(dt) || dt < 0.0F || dt > 0.25F) {
            return cy::fail(cy::ErrorCode::InvalidArgument, "invalid VFX preview frame interval");
        }
        if (!playing || dt == 0.0F) {
            return cy::ok();
        }
        cy::vfx::StepReport report;
        const cy::f32 scaled = dt * time_scale;
        if (cy::Status stepped = world.step(scaled, report); !stepped) {
            return stepped;
        }
        time_seconds += scaled;
        return cy::ok();
    }

    [[nodiscard]] cy::Status seek(cy::f32 seconds) noexcept {
        if (!std::isfinite(seconds) || seconds < 0.0F || seconds > 30.0F) {
            return cy::fail(cy::ErrorCode::InvalidArgument,
                            "VFX preview scrub time is out of range");
        }
        if (cy::Status reset = restart(); !reset) {
            return reset;
        }
        constexpr cy::f32 kFrame = 1.0F / 60.0F;
        cy::vfx::StepReport report;
        while (time_seconds + kFrame <= seconds) {
            if (cy::Status stepped = world.step(kFrame, report); !stepped) {
                return stepped;
            }
            time_seconds += kFrame;
        }
        const cy::f32 remainder = seconds - time_seconds;
        if (remainder > 0.00001F) {
            if (cy::Status stepped = world.step(remainder, report); !stepped) {
                return stepped;
            }
        }
        time_seconds = seconds;
        playing = false;
        return cy::ok();
    }

    std::optional<cy::vfx::CompiledSystem> system;
    cy::vfx::SimulationWorld world;
    cy::vfx::EffectHandle handle = cy::vfx::kInvalidEffect;
    cy::f32 time_seconds = 0.0F;
    cy::f32 time_scale = 1.0F;
    bool playing = false;
};
#endif

struct CyServiceSession_T {
    explicit CyServiceSession_T(cy::Allocator& allocator) noexcept
        : request_payload(allocator),
          event_payload(allocator)
#if defined(CY_EDITOR_HAS_VFX)
          ,
          vfx_preview(allocator)
#endif
    {
    }

    cy::u64 request = 0;
    cy::u32 schema = 0;
    char operation[64] = {};
    cy::Array<cy::u8> request_payload;
    cy::Array<cy::u8> event_payload;
    bool pending = false;
    bool cancelled = false;
    bool failed_event = false;
    cy::u32 preview_generation[16] = {};
    bool preview_live[16] = {};
    cy::u64 preview_artefact[16] = {};
    cy::u32 preview_parameter_ids[16][32] = {};
    cy::u8 preview_parameter_types[16][32] = {};
#if defined(CY_EDITOR_HAS_VFX)
    VfxPreviewState vfx_preview;
#endif
};

namespace {

using cy::Array;
using cy::Status;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;

Status put_u8(Array<u8>& out, u8 value) noexcept {
    return out.push_back(value);
}

Status put_u32(Array<u8>& out, u32 value) noexcept {
    for (usize byte = 0; byte < 4; ++byte) {
        if (Status pushed = out.push_back(static_cast<u8>((value >> (byte * 8)) & 0xFFU));
            !pushed) {
            return pushed;
        }
    }
    return cy::ok();
}

Status put_u64(Array<u8>& out, u64 value) noexcept {
    for (usize byte = 0; byte < 8; ++byte) {
        if (Status pushed = out.push_back(static_cast<u8>((value >> (byte * 8)) & 0xFFU));
            !pushed) {
            return pushed;
        }
    }
    return cy::ok();
}

u64 read_u64(const Array<u8>& bytes, usize offset) noexcept {
    u64 value = 0;
    for (usize byte = 0; byte < 8; ++byte) {
        value |= static_cast<u64>(bytes[offset + byte]) << (byte * 8);
    }
    return value;
}

u32 read_u32(const Array<u8>& bytes, usize offset) noexcept {
    u32 value = 0;
    for (usize byte = 0; byte < 4; ++byte) {
        value |= static_cast<u32>(bytes[offset + byte]) << (byte * 8);
    }
    return value;
}

Status put_text(Array<u8>& out, std::string_view value) noexcept {
    if (Status length = put_u32(out, static_cast<u32>(value.size())); !length) {
        return length;
    }
    return out.append({reinterpret_cast<const u8*>(value.data()), value.size()});
}

CyResult failed(CyServiceSession_T& session, const char* code, const char* detail) noexcept {
    session.failed_event = true;
    session.event_payload.clear();
    if (!put_u32(session.event_payload, 1) || !put_text(session.event_payload, code) ||
        !put_text(session.event_payload, detail)) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    return CY_RESULT_OK;
}

#if defined(CY_EDITOR_HAS_VFX)
CyResult failed_vfx(CyServiceSession_T& session, const char* code, const char* message,
                    const cy::graph::DiagnosticSink& diagnostics) noexcept {
    session.failed_event = true;
    session.event_payload.clear();
    if (!put_u32(session.event_payload, 1) || !put_text(session.event_payload, code) ||
        !put_text(session.event_payload, message) ||
        !put_u32(session.event_payload, static_cast<u32>(diagnostics.entries().size()))) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    for (const cy::graph::Diagnostic& diagnostic : diagnostics.entries()) {
        if (!put_u8(session.event_payload, static_cast<u8>(diagnostic.severity)) ||
            !put_text(session.event_payload, diagnostic.code) ||
            !put_text(session.event_payload, diagnostic.message) ||
            !put_text(session.event_payload, diagnostic.detail.text()) ||
            !put_u64(session.event_payload, diagnostic.node) ||
            !put_text(session.event_payload, diagnostic.pin.text())) {
            return CY_RESULT_OUT_OF_MEMORY;
        }
    }
    return CY_RESULT_OK;
}

cy::Expected<cy::vfx::CompiledSystem, cy::Error> cook_vfx_document(
    std::string_view source, cy::Allocator& allocator, cy::graph::DiagnosticSink& diagnostics,
    cy::vfx::CompileReport& report, const char*& stage) noexcept {
    stage = "vfx.document.parse";
    auto asset = cy::vfx::read_authoring_document(source, allocator);
    if (!asset) {
        return cy::make_unexpected(asset.error());
    }
    cy::graph::NodeRegistry registry(allocator);
    cy::vfx::DataInterfaceRegistry interfaces(allocator);
    stage = "vfx.catalogue";
    if (Status registered = cy::vfx::register_vfx_nodes(registry); !registered) {
        return cy::make_unexpected(registered.error());
    }
    stage = "vfx.interfaces";
    if (Status registered = cy::vfx::register_builtin_interfaces(interfaces); !registered) {
        return cy::make_unexpected(registered.error());
    }
    asset->resolve(registry);
    stage = "vfx.compile";
    return cy::vfx::compile_system(*asset, registry, interfaces, cy::vfx::CompileOptions{},
                                   diagnostics, report);
}

CyResult compile_vfx(CyServiceSession_T& session, cy::Allocator& allocator) noexcept {
    const std::string_view source(reinterpret_cast<const char*>(session.request_payload.data()),
                                  session.request_payload.size());
    cy::graph::DiagnosticSink diagnostics(allocator);
    cy::vfx::CompileReport report(allocator);
    const char* stage = nullptr;
    auto compiled = cook_vfx_document(source, allocator, diagnostics, report, stage);
    if (!compiled) {
        return failed_vfx(session, stage, compiled.error().message, diagnostics);
    }
    session.event_payload.clear();
    if (!put_u32(session.event_payload, 1) ||
        !put_u64(session.event_payload, compiled->cook_key()) ||
        !put_u32(session.event_payload, report.kernels) ||
        !put_u32(session.event_payload, report.total_bytes_per_particle) ||
        !put_u32(session.event_payload, static_cast<u32>(compiled->emitters().size()))) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    for (usize index = 0; index < compiled->emitters().size(); ++index) {
        const cy::vfx::CompiledEmitter& emitter = compiled->emitters()[index];
        const cy::vfx::EmitterReport& details = report.emitters[index];
        if (!put_text(session.event_payload, emitter.name().text()) ||
            !put_u8(session.event_payload, static_cast<u8>(emitter.path())) ||
            !put_u32(session.event_payload, details.kernels) ||
            !put_u32(session.event_payload, details.bytes_per_particle) ||
            !put_u32(session.event_payload, details.max_population) ||
            !put_u64(session.event_payload, details.estimated_cost_units) ||
            !put_u32(session.event_payload, details.folded_constants) ||
            !put_u32(session.event_payload, static_cast<u32>(emitter.layout().slots().size()))) {
            return CY_RESULT_OUT_OF_MEMORY;
        }
        for (const cy::vfx::AttributeSlot& slot : emitter.layout().slots()) {
            if (!put_text(session.event_payload, slot.name.text()) ||
                !put_text(session.event_payload, cy::vfx::vfx_type_name(slot.type)) ||
                !put_u8(session.event_payload, static_cast<u8>(slot.precision)) ||
                !put_u32(session.event_payload, slot.array_offset) ||
                !put_u32(session.event_payload, slot.stride) ||
                !put_u8(session.event_payload, slot.elided ? 1U : 0U)) {
                return CY_RESULT_OUT_OF_MEMORY;
            }
        }
        if (!put_u32(session.event_payload, static_cast<u32>(emitter.sources().size()))) {
            return CY_RESULT_OUT_OF_MEMORY;
        }
        for (const cy::graph::GeneratedSource& generated : emitter.sources()) {
            if (!put_text(session.event_payload, {generated.text.data(), generated.text.size()})) {
                return CY_RESULT_OUT_OF_MEMORY;
            }
        }
    }
    return CY_RESULT_OK;
}

Status put_f32(Array<u8>& out, cy::f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return put_u32(out, bits);
}

cy::f32 read_f32(const Array<u8>& bytes, usize offset) noexcept {
    const u32 bits = read_u32(bytes, offset);
    cy::f32 value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

Status put_preview_sample(Array<u8>& out, const VfxPreviewState& preview,
                          const cy::vfx::EffectInstance* instance) noexcept {
    if (instance == nullptr) {
        return put_u8(out, 0);
    }
    for (u32 emitter = 0; emitter < preview.system->emitters().size(); ++emitter) {
        const auto flags = preview.world.alive_flags(instance->first_block + emitter);
        for (u32 particle = 0; particle < flags.size(); ++particle) {
            if (flags[particle] == 0) {
                continue;
            }
            const auto slots = preview.system->emitters()[emitter].layout().slots();
            u32 count = 0;
            for (const cy::vfx::AttributeSlot& slot : slots) {
                count += !slot.elided && count < 32U ? 1U : 0U;
            }
            if (!put_u8(out, 1) || !put_u32(out, emitter) || !put_u32(out, particle) ||
                !put_u32(out, count)) {
                return cy::fail(cy::ErrorCode::OutOfMemory, "VFX preview sample encoding failed");
            }
            for (const cy::vfx::AttributeSlot& slot : slots) {
                if (slot.elided || count == 0) {
                    continue;
                }
                if (!put_text(out, slot.name.text()) ||
                    !put_u8(out, static_cast<u8>(slot.components))) {
                    return cy::fail(cy::ErrorCode::OutOfMemory,
                                    "VFX preview attribute encoding failed");
                }
                for (u32 component = 0; component < slot.components; ++component) {
                    if (!put_f32(out, preview.world.read_attribute(*instance, emitter, particle,
                                                                   slot.name, component))) {
                        return cy::fail(cy::ErrorCode::OutOfMemory,
                                        "VFX preview attribute encoding failed");
                    }
                }
                --count;
            }
            return cy::ok();
        }
    }
    return put_u8(out, 0);
}

CyResult preview_snapshot(CyServiceSession_T& session) noexcept {
    const VfxPreviewState& preview = session.vfx_preview;
    if (!preview.system) {
        return failed(session, "vfx.preview.empty", "no VFX preview effect is loaded");
    }
    const cy::vfx::StepReport& step = preview.world.last_step();
    const cy::vfx::PoolReport& pool = preview.world.pool().report();
    session.event_payload.clear();
    if (!put_u32(session.event_payload, 2) ||
        !put_u64(session.event_payload, preview.system->cook_key()) ||
        !put_u8(session.event_payload, preview.playing ? 1U : 0U) ||
        !put_f32(session.event_payload, preview.time_seconds) ||
        !put_f32(session.event_payload, preview.time_scale) ||
        !put_u32(session.event_payload, step.live_particles) ||
        !put_u32(session.event_payload, step.spawned) ||
        !put_u32(session.event_payload, step.killed) ||
        !put_u32(session.event_payload, step.cpu_fallbacks) ||
        !put_u64(session.event_payload, pool.used_bytes) ||
        !put_u64(session.event_payload, pool.total_bytes) ||
        !put_u32(session.event_payload, preview.world.events().total_dropped()) ||
        !put_u32(session.event_payload, preview.world.events().total_truncated()) ||
        !put_u32(session.event_payload, static_cast<u32>(preview.system->emitters().size()))) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    const auto instances = preview.world.instances();
    const cy::vfx::EffectInstance* instance = instances.empty() ? nullptr : &instances[0];
    for (usize index = 0; index < preview.system->emitters().size(); ++index) {
        u32 live = 0;
        if (instance != nullptr) {
            for (u8 flag :
                 preview.world.alive_flags(instance->first_block + static_cast<u32>(index))) {
                live += flag != 0 ? 1U : 0U;
            }
        }
        if (!put_text(session.event_payload, preview.system->emitters()[index].name().text()) ||
            !put_u32(session.event_payload, live)) {
            return CY_RESULT_OUT_OF_MEMORY;
        }
    }
    u32 raised = 0;
    u32 delivered = 0;
    for (const cy::vfx::ChannelReport& channel : preview.world.events().reports()) {
        raised += channel.raised;
        delivered += channel.delivered;
    }
    if (!put_u32(session.event_payload, pool.shortfall_particles) ||
        !put_u32(session.event_payload, pool.reduced_requests) ||
        !put_u32(session.event_payload, raised) || !put_u32(session.event_payload, delivered) ||
        !put_u32(session.event_payload, preview.world.readback().report().deferred) ||
        !put_preview_sample(session.event_payload, preview, instance)) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    return CY_RESULT_OK;
}

CyResult preview_load(CyServiceSession_T& session, cy::Allocator& allocator) noexcept {
    const std::string_view source(reinterpret_cast<const char*>(session.request_payload.data()),
                                  session.request_payload.size());
    cy::graph::DiagnosticSink diagnostics(allocator);
    cy::vfx::CompileReport report(allocator);
    const char* stage = nullptr;
    auto compiled = cook_vfx_document(source, allocator, diagnostics, report, stage);
    if (!compiled) {
        return failed_vfx(session, stage, compiled.error().message, diagnostics);
    }
    if (Status loaded = session.vfx_preview.load(std::move(*compiled)); !loaded) {
        return failed(session, "vfx.preview.load", loaded.error().message);
    }
    return preview_snapshot(session);
}

CyResult preview_step(CyServiceSession_T& session) noexcept {
    if (session.request_payload.size() != 4) {
        return failed(session, "vfx.preview.step", "a frame interval is four bytes");
    }
    if (Status advanced = session.vfx_preview.advance(read_f32(session.request_payload, 0));
        !advanced) {
        return failed(session, "vfx.preview.step", advanced.error().message);
    }
    return preview_snapshot(session);
}

CyResult preview_control(CyServiceSession_T& session) noexcept {
    const auto& payload = session.request_payload;
    if (payload.empty()) {
        return failed(session, "vfx.preview.control", "a control action is required");
    }
    VfxPreviewState& preview = session.vfx_preview;
    if (!preview.system) {
        return failed(session, "vfx.preview.empty", "no VFX preview effect is loaded");
    }
    const u8 action = payload[0];
    if (action <= 2 && payload.size() != 1) {
        return failed(session, "vfx.preview.control", "unexpected control data");
    }
    if (action >= 3 && payload.size() != 5) {
        return failed(session, "vfx.preview.control", "a control value is four bytes");
    }
    if (action == 0) {
        preview.playing = true;
    } else if (action == 1) {
        preview.playing = false;
    } else if (action == 2) {
        if (Status restarted = preview.restart(); !restarted) {
            return failed(session, "vfx.preview.restart", restarted.error().message);
        }
        preview.playing = false;
    } else if (action == 3) {
        if (Status scrubbed = preview.seek(read_f32(payload, 1)); !scrubbed) {
            return failed(session, "vfx.preview.scrub", scrubbed.error().message);
        }
    } else if (action == 4) {
        const cy::f32 scale = read_f32(payload, 1);
        if (!std::isfinite(scale) || scale < 0.1F || scale > 4.0F) {
            return failed(session, "vfx.preview.scale", "time scale must be between 0.1 and 4");
        }
        preview.time_scale = scale;
    } else {
        return failed(session, "vfx.preview.control", "unknown VFX preview action");
    }
    return preview_snapshot(session);
}

CyResult preview_parameter_update(CyServiceSession_T& session) noexcept {
    const auto& payload = session.request_payload;
    if (payload.size() < 10) {
        return failed(session, "vfx.preview.parameter", "parameter name and value are required");
    }
    const u32 length = read_u32(payload, 0);
    if (length == 0 || length > 128 || length + 5U >= payload.size()) {
        return failed(session, "vfx.preview.parameter", "invalid parameter name length");
    }
    const usize count_offset = 4U + length;
    const u8 count = payload[count_offset];
    if (count == 0 || count > 4 || payload.size() != count_offset + 1U + (count * 4U)) {
        return failed(session, "vfx.preview.parameter", "invalid parameter component count");
    }
    cy::f32 values[4] = {};
    for (u8 index = 0; index < count; ++index) {
        values[index] = read_f32(payload, count_offset + 1U + (index * 4U));
        if (!std::isfinite(values[index])) {
            return failed(session, "vfx.preview.parameter", "parameter values must be finite");
        }
    }
    const std::string name(reinterpret_cast<const char*>(payload.data() + 4), length);
    if (Status updated = session.vfx_preview.world.set_parameter(
            session.vfx_preview.handle, cy::Name::intern(name), {values, count});
        !updated) {
        return failed(session, "vfx.preview.parameter", updated.error().message);
    }
    return preview_snapshot(session);
}
#endif

Status put_graph_location(Array<u8>& out, const cy::graph::Graph& graph,
                          cy::graph::NodeKey node_key, cy::Name pin_name) noexcept {
    const cy::graph::GraphNode* node = graph.find_node(node_key);
    const cy::graph::NodeType* type = node == nullptr ? nullptr : node->resolved;
    const cy::graph::PinDesc* pin = nullptr;
    if (type != nullptr && pin_name != cy::Name{}) {
        pin = type->find_pin(pin_name, cy::graph::PinDirection::Input);
        if (pin == nullptr) {
            pin = type->find_pin(pin_name, cy::graph::PinDirection::Output);
        }
    }
    return put_text(out, graph.name().text()) && put_u64(out, node_key) &&
                   put_u32(out, type == nullptr ? 0 : type->identity()) &&
                   put_u32(out, pin == nullptr ? 0 : pin->identity) &&
                   put_text(out, pin_name.text())
               ? cy::ok()
               : cy::fail(cy::ErrorCode::OutOfMemory, "a diagnostic location could not be encoded");
}

CyResult failed_material(CyServiceSession_T& session, const char* code, const char* message,
                         const char* detail = "") noexcept {
    session.failed_event = true;
    session.event_payload.clear();
    if (!put_u32(session.event_payload, 2) || !put_u32(session.event_payload, 1) ||
        !put_u8(session.event_payload, static_cast<u8>(cy::graph::Severity::Error)) ||
        !put_text(session.event_payload, code) || !put_text(session.event_payload, message) ||
        !put_text(session.event_payload, detail) || !put_text(session.event_payload, {}) ||
        !put_u64(session.event_payload, cy::graph::kInvalidNodeKey) ||
        !put_u32(session.event_payload, cy::graph::kInvalidNodeTypeId) ||
        !put_u32(session.event_payload, cy::graph::kInvalidPinId) ||
        !put_text(session.event_payload, {}) || !put_u32(session.event_payload, 0) ||
        !put_text(session.event_payload, {})) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    return CY_RESULT_OK;
}

CyResult failed_material(CyServiceSession_T& session, const cy::graph::Graph& graph,
                         const cy::graph::DiagnosticSink& diagnostics) noexcept {
    session.failed_event = true;
    session.event_payload.clear();
    if (!put_u32(session.event_payload, 2) ||
        !put_u32(session.event_payload, static_cast<u32>(diagnostics.entries().size()))) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    for (const cy::graph::Diagnostic& diagnostic : diagnostics.entries()) {
        const u32 related = diagnostic.related_node == cy::graph::kInvalidNodeKey ? 0U : 1U;
        if (!put_u8(session.event_payload, static_cast<u8>(diagnostic.severity)) ||
            !put_text(session.event_payload, diagnostic.code) ||
            !put_text(session.event_payload, diagnostic.message) ||
            !put_text(session.event_payload, diagnostic.detail.text()) ||
            !put_graph_location(session.event_payload, graph, diagnostic.node, diagnostic.pin) ||
            !put_u32(session.event_payload, related)) {
            return CY_RESULT_OUT_OF_MEMORY;
        }
        if (related != 0 && !put_graph_location(session.event_payload, graph,
                                                diagnostic.related_node, diagnostic.related_pin)) {
            return CY_RESULT_OUT_OF_MEMORY;
        }
        if (!put_text(session.event_payload, {})) {
            return CY_RESULT_OUT_OF_MEMORY;
        }
    }
    return CY_RESULT_OK;
}

Status put_material_dependencies(Array<u8>& out, const cy::graph::Graph& graph,
                                 cy::Allocator& allocator) noexcept {
    Array<cy::Name> dependencies(allocator);
    for (const cy::graph::GraphNode& node : graph.nodes()) {
        const cy::graph::Literal* texture = graph.property(node.key, cy::Name::intern("texture"));
        if (texture == nullptr || texture->text.is_empty()) {
            continue;
        }
        bool exists = false;
        for (const cy::Name dependency : dependencies) {
            exists = exists || dependency == texture->text;
        }
        if (!exists && !dependencies.push_back(texture->text)) {
            return cy::fail(cy::ErrorCode::OutOfMemory,
                            "a material dependency could not be retained");
        }
    }
    if (!put_u32(out, static_cast<u32>(dependencies.size()))) {
        return cy::fail(cy::ErrorCode::OutOfMemory,
                        "the material dependency count could not be encoded");
    }
    for (const cy::Name dependency : dependencies) {
        if (!put_text(out, dependency.text())) {
            return cy::fail(cy::ErrorCode::OutOfMemory,
                            "a material dependency could not be encoded");
        }
    }
    return cy::ok();
}

CyResult compile_material_result(CyServiceSession_T& session, const cy::graph::Graph& graph,
                                 const cy::rendering::material::Module& module,
                                 cy::editor::MaterialPreviewRuntime* preview_runtime,
                                 cy::Allocator& allocator) noexcept {
    cy::rendering::material::CompileOptions options;
    auto compiled = cy::rendering::material::compile_material(module, options, allocator);
    if (!compiled) {
        return failed_material(session, "material.compile", compiled.error().message);
    }
    const u64 artefact = compiled.value().cook_key();
    if (preview_runtime != nullptr) {
        if (Status published = preview_runtime->publish(artefact, compiled.value()); !published) {
            return failed_material(session, "material.publish-rejected", published.error().message);
        }
    }
    if (!put_u64(session.event_payload, artefact) ||
        !put_u64(session.event_payload, graph.semantic_digest()) ||
        !put_u32(session.event_payload, static_cast<u32>(compiled.value().programs().size()))) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    if (Status encoded = put_material_dependencies(session.event_payload, graph, allocator);
        !encoded) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    return CY_RESULT_OK;
}

CyResult author_graph_result(CyServiceSession_T& session, const cy::graph::Graph& graph,
                             Array<char>& canonical) noexcept {
    canonical.clear();
    if (Status written = cy::graph::write_graph(graph, canonical); !written) {
        return failed_material(session, "material.graph.write", written.error().message);
    }
    session.event_payload.clear();
    return put_u32(session.event_payload, 2) && put_u8(session.event_payload, 1) &&
                   put_text(session.event_payload, {canonical.data(), canonical.size()})
               ? CY_RESULT_OK
               : CY_RESULT_OUT_OF_MEMORY;
}

CyResult compile_graph(CyServiceSession_T& session,
                       cy::editor::MaterialPreviewRuntime* preview_runtime,
                       cy::Allocator& allocator, bool compile, bool author = false,
                       std::string_view input = {}) noexcept {
    cy::graph::NodeRegistry registry(allocator);
    if (Status status = cy::graph::material::register_material_nodes(registry); !status) {
        return failed(session, "catalogue-unavailable", status.error().message);
    }
    cy::graph::DiagnosticSink diagnostics(allocator);
    std::string_view text =
        input.empty()
            ? std::string_view(reinterpret_cast<const char*>(session.request_payload.data()),
                               session.request_payload.size())
            : input;
    Array<char> canonical(allocator);
    if (text.starts_with("cymatcanvas ")) {
        cy::graph::Graph authored(allocator, cy::Name::intern("editor_material"));
        auto canvas = cy::graph::material::read_canvas(text, authored);
        if (!canvas) {
            return failed_material(session, "material.canvas.parse", canvas.error().message);
        }
        if (Status written = cy::graph::write_graph(authored, canonical); !written) {
            return failed_material(session, "material.canvas.canonicalise",
                                   written.error().message);
        }
        text = {canonical.data(), canonical.size()};
    }
    auto graph = cy::graph::parse_graph(text, &registry, allocator, diagnostics);
    if (!graph) {
        return failed_material(session, "material.graph.parse", graph.error().message);
    }
    if (Status validated = cy::graph::validate(graph.value(), registry, nullptr, diagnostics);
        !validated) {
        return failed_material(session, "material.graph.validate", validated.error().message);
    }
    if (diagnostics.errors() != 0) {
        return failed_material(session, graph.value(), diagnostics);
    }
    cy::rendering::material::MaterialGraph lowered(allocator, cy::Name::intern("editor_material"));
    if (Status status = cy::graph::material::lower_material(graph.value(), lowered); !status) {
        return failed_material(session, "material.lowering", status.error().message);
    }
    auto module = cy::rendering::material::lower_graph(lowered, allocator);
    if (!module) {
        return failed_material(session, "material.validation", module.error().message);
    }

    session.event_payload.clear();
    if (!put_u32(session.event_payload, 2) || !put_u8(session.event_payload, 1)) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    if (author) {
        return author_graph_result(session, graph.value(), canonical);
    }
    if (!compile) {
        return CY_RESULT_OK;
    }
    return compile_material_result(session, graph.value(), module.value(), preview_runtime,
                                   allocator);
}

CyResult preview_authored_graph(CyServiceSession_T& session,
                                cy::editor::MaterialAuthoringRuntime* runtime,
                                cy::Allocator& allocator) noexcept {
    if (runtime == nullptr) {
        return failed_material(session, "material.preview.unavailable",
                               "this host has no authored scene material preview");
    }
    const auto& bytes = session.request_payload;
    if (bytes.size() < 8) {
        return failed_material(session, "material.preview.payload",
                               "missing graph reference or source");
    }
    const usize reference_size = read_u32(bytes, 0);
    if (reference_size == 0 || reference_size > bytes.size() - 8) {
        return failed_material(session, "material.preview.payload",
                               "invalid graph reference length");
    }
    const usize source_offset = 4 + reference_size;
    const usize source_size = read_u32(bytes, source_offset);
    if (source_size == 0 || source_size != bytes.size() - source_offset - 4) {
        return failed_material(session, "material.preview.payload", "invalid graph source length");
    }
    const std::string_view reference(reinterpret_cast<const char*>(bytes.data() + 4),
                                     reference_size);
    const std::string_view source(reinterpret_cast<const char*>(bytes.data() + source_offset + 4),
                                  source_size);
    if (!reference.ends_with(".cygraph") || reference.find("..") != std::string_view::npos ||
        reference.starts_with('/')) {
        return failed_material(session, "material.preview.reference",
                               "invalid project graph reference");
    }
    if (const CyResult result = compile_graph(session, nullptr, allocator, false, true, source);
        result != CY_RESULT_OK || session.failed_event) {
        return result;
    }
    const usize graph_size = read_u32(session.event_payload, 5);
    const std::string_view canonical(
        reinterpret_cast<const char*>(session.event_payload.data() + 9), graph_size);
    if (Status applied = runtime->preview(reference, canonical); !applied) {
        return failed_material(session, "material.preview.unsupported", applied.error().message);
    }
    session.event_payload.clear();
    return put_u32(session.event_payload, 2) && put_u8(session.event_payload, 1)
               ? CY_RESULT_OK
               : CY_RESULT_OUT_OF_MEMORY;
}

bool preview_slot(const CyServiceSession_T& session, u64 handle, usize& slot) noexcept {
    const u32 index = static_cast<u32>(handle & 0xFFFF'FFFFULL);
    const u32 generation = static_cast<u32>(handle >> 32U);
    if (index == 0 || index > 16) {
        return false;
    }
    slot = index - 1;
    return session.preview_live[slot] && session.preview_generation[slot] == generation;
}

CyResult preview_create(CyServiceSession_T& session,
                        cy::editor::MaterialPreviewRuntime* preview_runtime) noexcept {
    if (preview_runtime == nullptr) {
        return failed(session, "preview-runtime-unavailable",
                      "this host has no renderer-owned material preview runtime");
    }
    for (usize slot = 0; slot < 16; ++slot) {
        if (session.preview_live[slot]) {
            continue;
        }
        if (session.preview_generation[slot] == 0) {
            session.preview_generation[slot] = 1;
        }
        const u64 handle = (static_cast<u64>(session.preview_generation[slot]) << 32U) | (slot + 1);
        if (Status created = preview_runtime->create(handle); !created) {
            return failed(session, "preview-create-rejected", created.error().message);
        }
        session.preview_live[slot] = true;
        session.event_payload.clear();
        return put_u64(session.event_payload, handle) ? CY_RESULT_OK : CY_RESULT_OUT_OF_MEMORY;
    }
    return failed(session, "preview-limit", "this session already owns sixteen preview worlds");
}

CyResult preview_destroy(CyServiceSession_T& session,
                         cy::editor::MaterialPreviewRuntime* preview_runtime) noexcept {
    if (session.request_payload.size() != 8) {
        return failed(session, "preview-handle-invalid", "a preview handle is eight bytes");
    }
    usize slot = 0;
    const u64 handle = read_u64(session.request_payload, 0);
    if (!preview_slot(session, handle, slot)) {
        // Idempotent destruction: a stale/already-destroyed handle is still absent afterwards.
        session.event_payload.clear();
        return CY_RESULT_OK;
    }
    if (preview_runtime != nullptr) {
        if (Status destroyed = preview_runtime->destroy(handle); !destroyed) {
            return failed(session, "preview-destroy-rejected", destroyed.error().message);
        }
    }
    session.preview_live[slot] = false;
    session.preview_artefact[slot] = 0;
    std::memset(session.preview_parameter_types[slot], 0,
                sizeof(session.preview_parameter_types[slot]));
    std::memset(session.preview_parameter_ids[slot], 0,
                sizeof(session.preview_parameter_ids[slot]));
    ++session.preview_generation[slot];
    if (session.preview_generation[slot] == 0) {
        session.preview_generation[slot] = 1;
    }
    session.event_payload.clear();
    return CY_RESULT_OK;
}

CyResult preview_reload(CyServiceSession_T& session,
                        cy::editor::MaterialPreviewRuntime* preview_runtime) noexcept {
    if (session.request_payload.size() < 8) {
        return failed(session, "preview-handle-invalid", "the request has no preview handle");
    }
    usize slot = 0;
    if (!preview_slot(session, read_u64(session.request_payload, 0), slot)) {
        return failed(session, "preview-stale", "the preview world was destroyed or replaced");
    }
    if (session.request_payload.size() < 20) {
        return failed(session, "preview-reload-invalid",
                      "reload requires a preview, artefact and target count");
    }
    const u64 requested = read_u64(session.request_payload, 8);
    const u32 targets = read_u32(session.request_payload, 16);
    const usize expected = 20 + (static_cast<usize>(targets) * 20);
    if (requested == 0 || targets == 0 || session.request_payload.size() != expected) {
        return failed(session, "preview-reload-rejected",
                      "reload requires a non-zero artefact and at least one exact target binding");
    }
    cy::editor::MaterialPreviewTarget decoded[16] = {};
    if (targets > std::size(decoded)) {
        return failed(session, "preview-target-limit",
                      "a reload can address at most sixteen material bindings");
    }
    for (u32 target = 0; target < targets; ++target) {
        const usize offset = 20 + (static_cast<usize>(target) * 20);
        const u32 material_slot = read_u32(session.request_payload, offset + 16);
        if (material_slot >= 16) {
            return failed(session, "material-slot-unsupported",
                          "the runtime supports material slots zero through fifteen");
        }
        std::memcpy(decoded[target].entity, session.request_payload.data() + offset, 16);
        decoded[target].material_slot = material_slot;
    }
    const u64 preview = read_u64(session.request_payload, 0);
    if (preview_runtime != nullptr) {
        if (Status reloaded =
                preview_runtime->reload(preview, requested, {decoded, static_cast<usize>(targets)});
            !reloaded) {
            return failed(session, "preview-reload-rejected", reloaded.error().message);
        }
    }
    session.preview_artefact[slot] = requested;
    std::memset(session.preview_parameter_ids[slot], 0,
                sizeof(session.preview_parameter_ids[slot]));
    std::memset(session.preview_parameter_types[slot], 0,
                sizeof(session.preview_parameter_types[slot]));
    session.event_payload.clear();
    if (!put_u64(session.event_payload, requested) || !put_u64(session.event_payload, requested) ||
        !put_u32(session.event_payload, targets)) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    return session.event_payload.append(
               {session.request_payload.data() + 20, session.request_payload.size() - 20})
               ? CY_RESULT_OK
               : CY_RESULT_OUT_OF_MEMORY;
}

usize preview_parameter_slot(const CyServiceSession_T& session, usize preview,
                             u32 parameter) noexcept {
    usize empty = 32;
    for (usize index = 0; index < 32; ++index) {
        if (session.preview_parameter_ids[preview][index] == parameter) {
            return index;
        }
        if (empty == 32 && session.preview_parameter_ids[preview][index] == 0) {
            empty = index;
        }
    }
    return empty;
}

CyResult preview_parameter_update(CyServiceSession_T& session,
                                  cy::editor::MaterialPreviewRuntime* preview_runtime) noexcept {
    if (session.request_payload.size() < 21) {
        return failed(session, "parameter-payload-invalid",
                      "a parameter update requires preview, artefact, identity and type");
    }
    usize slot = 0;
    if (!preview_slot(session, read_u64(session.request_payload, 0), slot)) {
        return failed(session, "preview-stale", "the preview world was destroyed or replaced");
    }
    if (read_u64(session.request_payload, 8) != session.preview_artefact[slot]) {
        return failed(session, "artefact-stale",
                      "the parameter update does not address the applied artefact generation");
    }
    const u32 parameter = read_u32(session.request_payload, 16);
    const u8 kind = session.request_payload[20];
    if (parameter == 0 || kind == 0 || kind > 5) {
        return failed(session, "parameter-unsupported",
                      "the parameter identity or value type is not supported");
    }
    const usize value_size = session.request_payload.size() - 21;
    const bool valid_size =
        (kind == 1 && value_size == 1) || ((kind == 2 || kind == 3) && value_size == 8) ||
        (kind == 4 && value_size == 16) ||
        (kind == 5 && value_size >= 4 && read_u32(session.request_payload, 21) == value_size - 4);
    if (!valid_size) {
        return failed(session, "parameter-payload-invalid",
                      "the encoded value does not match its declared type");
    }
    const usize parameter_slot = preview_parameter_slot(session, slot, parameter);
    if (parameter_slot == 32) {
        return failed(session, "parameter-limit",
                      "this preview already tracks thirty-two live parameters");
    }
    u8& established = session.preview_parameter_types[slot][parameter_slot];
    if (session.preview_parameter_ids[slot][parameter_slot] == parameter && established != kind) {
        return failed(session, "parameter-type-mismatch",
                      "the parameter was previously established with another type");
    }
    if (preview_runtime != nullptr) {
        const cy::editor::MaterialParameterUpdate update{
            parameter, kind, {session.request_payload.data() + 21, value_size}};
        if (Status applied = preview_runtime->update(read_u64(session.request_payload, 0),
                                                     session.preview_artefact[slot], update);
            !applied) {
            return failed(session, "parameter-update-rejected", applied.error().message);
        }
    }
    session.preview_parameter_ids[slot][parameter_slot] = parameter;
    established = kind;
    session.event_payload.clear();
    if (!put_u32(session.event_payload, parameter) || !put_u8(session.event_payload, kind)) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    return CY_RESULT_OK;
}

CyResult capabilities(CyServiceSession_T& session,
                      const cy::editor::MaterialPreviewRuntime* preview_runtime,
                      const cy::editor::MaterialAuthoringRuntime* authoring_runtime) noexcept {
    constexpr const char* operations[] = {
        "capabilities.get", "material.catalogue.get",   "material.validate",
        "material.compile", "material.author",          "preview.create",
        "preview.destroy",  "preview.parameter.update", "preview.reload",
    };
#if defined(CY_EDITOR_HAS_VFX)
    constexpr u32 vfx_operations = 8;
#else
    constexpr u32 vfx_operations = 0;
#endif
    session.event_payload.clear();
    if (!put_u32(session.event_payload, 1) ||
        !put_u32(session.event_payload,
                 static_cast<u32>(std::size(operations) + (authoring_runtime != nullptr ? 1 : 0) +
                                  vfx_operations))) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    for (const char* operation : operations) {
        if (!put_text(session.event_payload, operation)) {
            return CY_RESULT_OUT_OF_MEMORY;
        }
    }
#if defined(CY_EDITOR_HAS_VFX)
    if (!put_text(session.event_payload, "vfx.catalogue.get") ||
        !put_text(session.event_payload, "vfx.authoring-capabilities.get") ||
        !put_text(session.event_payload, "vfx.compile") ||
        !put_text(session.event_payload, "vfx.preview.load") ||
        !put_text(session.event_payload, "vfx.preview.state") ||
        !put_text(session.event_payload, "vfx.preview.control") ||
        !put_text(session.event_payload, "vfx.preview.step") ||
        !put_text(session.event_payload, "vfx.preview.parameter.update")) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
#endif
    if (authoring_runtime != nullptr && !put_text(session.event_payload, "material.preview.set")) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    // Target feature bits: material compilation and preview lifecycle. Device-specific shader
    // features are queried by the compiler profile in later schema versions rather than guessed.
    const u64 features = 0x1U | (preview_runtime != nullptr ? 0x2U : 0U);
    return put_u64(session.event_payload, features) ? CY_RESULT_OK : CY_RESULT_OUT_OF_MEMORY;
}

}  // namespace

namespace cy::editor {

const vfx::SimulationWorld* MaterialService::vfx_preview_world(
    CyServiceSession session) const noexcept {
#if defined(CY_EDITOR_HAS_VFX)
    if (session != nullptr && session->vfx_preview.system &&
        session->vfx_preview.world.find(session->vfx_preview.handle) != nullptr) {
        return &session->vfx_preview.world;
    }
#else
    (void)session;
#endif
    return nullptr;
}

CyResult MaterialService::open(CyServiceSession* out_session) noexcept {
    void* memory = allocator_->allocate(sizeof(CyServiceSession_T), alignof(CyServiceSession_T));
    if (memory == nullptr) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    *out_session = new (memory) CyServiceSession_T(*allocator_);
    return CY_RESULT_OK;
}

void MaterialService::close(CyServiceSession session) noexcept {
    if (session == nullptr) {
        return;
    }
    if (preview_runtime_ != nullptr) {
        for (usize slot = 0; slot < std::size(session->preview_live); ++slot) {
            if (!session->preview_live[slot]) {
                continue;
            }
            const u64 handle =
                (static_cast<u64>(session->preview_generation[slot]) << 32U) | (slot + 1);
            (void)preview_runtime_->destroy(handle);
        }
    }
#if defined(CY_EDITOR_HAS_VFX)
    session->vfx_preview.world.shutdown();
#endif
    session->~CyServiceSession_T();
    allocator_->deallocate(session, sizeof(CyServiceSession_T), alignof(CyServiceSession_T));
}

CyResult MaterialService::submit(CyServiceSession session,
                                 const CyServiceRequest& request) noexcept {
    if (request.request_id == 0) {
        return CY_RESULT_INVALID_ARGUMENT;
    }
    if (session->pending) {
        return CY_RESULT_ALREADY_EXISTS;
    }
    const usize operation_size = std::strlen(request.operation);
    if (operation_size >= sizeof(session->operation)) {
        return CY_RESULT_INVALID_ARGUMENT;
    }
    std::memcpy(session->operation, request.operation, operation_size + 1);
    session->request_payload.clear();
    if (Status copied = session->request_payload.append({request.payload, request.payload_size});
        !copied) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    session->request = request.request_id;
    session->schema = request.schema_version;
    session->pending = true;
    session->cancelled = false;
    session->failed_event = false;
    return CY_RESULT_OK;
}

CyResult MaterialService::cancel(CyServiceSession session, u64 request_id) noexcept {
    if (!session->pending || session->request != request_id) {
        return CY_RESULT_NOT_FOUND;
    }
    session->cancelled = true;
    return CY_RESULT_OK;
}

CyResult MaterialService::poll(CyServiceSession session, CyServiceEvent& out_event,
                               bool& out_has_event) noexcept {
    out_has_event = session->pending;
    if (!session->pending) {
        return CY_RESULT_OK;
    }
    out_event = {};
    out_event.struct_size = sizeof(CyServiceEvent);
    out_event.request_id = session->request;
    out_event.schema_version = 1;
    out_event.kind = session->cancelled ? CY_SERVICE_EVENT_CANCELLED : CY_SERVICE_EVENT_COMPLETED;

    CyResult result = CY_RESULT_OK;
    const std::string_view operation(session->operation);
    if (!session->cancelled && session->schema != 1) {
        result = failed(*session, "schema-unsupported", "this operation supports schema 1");
        out_event.kind = CY_SERVICE_EVENT_FAILED;
    } else if (!session->cancelled && operation == "capabilities.get") {
        result = capabilities(*session, preview_runtime_, authoring_runtime_);
    } else if (!session->cancelled && operation == "material.catalogue.get") {
        if (Status encoded = graph::material::encode_material_catalogue(session->event_payload);
            !encoded) {
            result = CY_RESULT_OUT_OF_MEMORY;
        }
#if defined(CY_EDITOR_HAS_VFX)
    } else if (!session->cancelled && operation == "vfx.catalogue.get") {
        if (Status encoded = vfx::encode_vfx_catalogue(session->event_payload); !encoded) {
            result = CY_RESULT_OUT_OF_MEMORY;
        }
    } else if (!session->cancelled && operation == "vfx.authoring-capabilities.get") {
        if (Status encoded = vfx::encode_authoring_capabilities(session->event_payload, nullptr);
            !encoded) {
            result = CY_RESULT_OUT_OF_MEMORY;
        }
    } else if (!session->cancelled && operation == "vfx.compile") {
        result = compile_vfx(*session, *allocator_);
    } else if (!session->cancelled && operation == "vfx.preview.load") {
        result = preview_load(*session, *allocator_);
    } else if (!session->cancelled && operation == "vfx.preview.state") {
        result = preview_snapshot(*session);
    } else if (!session->cancelled && operation == "vfx.preview.control") {
        result = preview_control(*session);
    } else if (!session->cancelled && operation == "vfx.preview.step") {
        result = preview_step(*session);
    } else if (!session->cancelled && operation == "vfx.preview.parameter.update") {
        result = preview_parameter_update(*session);
#endif
    } else if (!session->cancelled && operation == "material.validate") {
        result = compile_graph(*session, preview_runtime_, *allocator_, false);
    } else if (!session->cancelled && operation == "material.compile") {
        result = compile_graph(*session, preview_runtime_, *allocator_, true);
    } else if (!session->cancelled && operation == "material.author") {
        result = compile_graph(*session, preview_runtime_, *allocator_, false, true);
    } else if (!session->cancelled && operation == "material.preview.set") {
        result = preview_authored_graph(*session, authoring_runtime_, *allocator_);
    } else if (!session->cancelled && operation == "preview.create") {
        result = preview_create(*session, preview_runtime_);
    } else if (!session->cancelled && operation == "preview.destroy") {
        result = preview_destroy(*session, preview_runtime_);
    } else if (!session->cancelled && operation == "preview.parameter.update") {
        result = preview_parameter_update(*session, preview_runtime_);
    } else if (!session->cancelled && operation == "preview.reload") {
        result = preview_reload(*session, preview_runtime_);
    } else if (!session->cancelled) {
        result = failed(*session, "operation-unsupported",
                        "this backend does not support the operation");
        out_event.kind = CY_SERVICE_EVENT_FAILED;
    }
    if (result != CY_RESULT_OK) {
        return result;
    }
    if (session->failed_event) {
        out_event.kind = CY_SERVICE_EVENT_FAILED;
    }
    out_event.payload = session->event_payload.data();
    out_event.payload_size = session->event_payload.size();
    session->pending = false;
    return CY_RESULT_OK;
}

}  // namespace cy::editor
