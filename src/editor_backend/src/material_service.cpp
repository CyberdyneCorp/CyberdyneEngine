#include <cy/editor/material_service.h>

#include <cy/graph/material/canvas.h>
#include <cy/graph/material/lower_material.h>
#include <cy/graph/text.h>
#include <cy/rendering/material/compiler.h>

#include <cstring>
#include <new>
#include <string_view>

struct CyServiceSession_T {
    explicit CyServiceSession_T(cy::Allocator& allocator) noexcept
        : request_payload(allocator), event_payload(allocator) {}

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
                                 cy::Allocator& allocator) noexcept {
    cy::rendering::material::CompileOptions options;
    auto compiled = cy::rendering::material::compile_material(module, options, allocator);
    if (!compiled) {
        return failed_material(session, "material.compile", compiled.error().message);
    }
    if (!put_u64(session.event_payload, compiled.value().cook_key()) ||
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

CyResult compile_graph(CyServiceSession_T& session, cy::Allocator& allocator,
                       bool compile) noexcept {
    cy::graph::NodeRegistry registry(allocator);
    if (Status status = cy::graph::material::register_material_nodes(registry); !status) {
        return failed(session, "catalogue-unavailable", status.error().message);
    }
    cy::graph::DiagnosticSink diagnostics(allocator);
    std::string_view text(reinterpret_cast<const char*>(session.request_payload.data()),
                          session.request_payload.size());
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
    if (!compile) {
        return CY_RESULT_OK;
    }
    return compile_material_result(session, graph.value(), module.value(), allocator);
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

CyResult preview_create(CyServiceSession_T& session) noexcept {
    for (usize slot = 0; slot < 16; ++slot) {
        if (session.preview_live[slot]) {
            continue;
        }
        if (session.preview_generation[slot] == 0) {
            session.preview_generation[slot] = 1;
        }
        session.preview_live[slot] = true;
        session.event_payload.clear();
        const u64 handle = (static_cast<u64>(session.preview_generation[slot]) << 32U) | (slot + 1);
        return put_u64(session.event_payload, handle) ? CY_RESULT_OK : CY_RESULT_OUT_OF_MEMORY;
    }
    return failed(session, "preview-limit", "this session already owns sixteen preview worlds");
}

CyResult preview_destroy(CyServiceSession_T& session) noexcept {
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
    session.preview_live[slot] = false;
    ++session.preview_generation[slot];
    if (session.preview_generation[slot] == 0) {
        session.preview_generation[slot] = 1;
    }
    session.event_payload.clear();
    return CY_RESULT_OK;
}

CyResult preview_update(CyServiceSession_T& session, bool reload) noexcept {
    if (session.request_payload.size() < 8) {
        return failed(session, "preview-handle-invalid", "the request has no preview handle");
    }
    usize slot = 0;
    if (!preview_slot(session, read_u64(session.request_payload, 0), slot)) {
        return failed(session, "preview-stale", "the preview world was destroyed or replaced");
    }
    session.event_payload.clear();
    if (reload) {
        const u64 requested =
            session.request_payload.size() >= 16 ? read_u64(session.request_payload, 8) : 0;
        if (!put_u64(session.event_payload, requested) ||
            !put_u64(session.event_payload, requested)) {
            return CY_RESULT_OUT_OF_MEMORY;
        }
    }
    return CY_RESULT_OK;
}

CyResult capabilities(CyServiceSession_T& session) noexcept {
    constexpr const char* operations[] = {
        "capabilities.get",         "material.catalogue.get", "material.validate",
        "material.compile",         "preview.create",         "preview.destroy",
        "preview.parameter.update", "preview.reload",
    };
    session.event_payload.clear();
    if (!put_u32(session.event_payload, 1) ||
        !put_u32(session.event_payload, static_cast<u32>(std::size(operations)))) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    for (const char* operation : operations) {
        if (!put_text(session.event_payload, operation)) {
            return CY_RESULT_OUT_OF_MEMORY;
        }
    }
    // Target feature bits: material compilation and preview lifecycle. Device-specific shader
    // features are queried by the compiler profile in later schema versions rather than guessed.
    return put_u64(session.event_payload, 0x3U) ? CY_RESULT_OK : CY_RESULT_OUT_OF_MEMORY;
}

}  // namespace

namespace cy::editor {

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
        result = capabilities(*session);
    } else if (!session->cancelled && operation == "material.catalogue.get") {
        if (Status encoded = graph::material::encode_material_catalogue(session->event_payload);
            !encoded) {
            result = CY_RESULT_OUT_OF_MEMORY;
        }
    } else if (!session->cancelled && operation == "material.validate") {
        result = compile_graph(*session, *allocator_, false);
    } else if (!session->cancelled && operation == "material.compile") {
        result = compile_graph(*session, *allocator_, true);
    } else if (!session->cancelled && operation == "preview.create") {
        result = preview_create(*session);
    } else if (!session->cancelled && operation == "preview.destroy") {
        result = preview_destroy(*session);
    } else if (!session->cancelled && operation == "preview.parameter.update") {
        result = preview_update(*session, false);
    } else if (!session->cancelled && operation == "preview.reload") {
        result = preview_update(*session, true);
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
