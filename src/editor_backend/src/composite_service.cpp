// SPDX-License-Identifier: MIT
// Longest-prefix routing over several editor-service backends. See cy/editor/composite_service.h.

#include <cy/editor/composite_service.h>

#include "service_wire.h"

#include <cstring>
#include <new>
#include <string_view>

namespace cy::editor {
namespace {

constexpr u32 kChildren = CompositeEditorService::kMaxRoutes;

enum class Ask : u8 { Pending, Asked, Done };

/// A `capabilities.get` being gathered from every child.
struct Merge {
    explicit Merge(Allocator& allocator) noexcept : names(allocator) {}

    bool active = false;
    bool cancelled = false;
    u64 request = 0;
    Ask ask[kChildren] = {};
    u32 count = 0;
    /// The merged operation names, each encoded as `put_text` writes it.
    Array<u8> names;
    u64 features = 0;
};

struct CompositeSession {
    explicit CompositeSession(Allocator& allocator) noexcept
        : event_payload(allocator), merge(allocator) {}

    CyServiceSession children[kChildren] = {};
    /// The request each child is working on for this session, or zero.
    u64 inflight[kChildren] = {};
    u32 next = 0;
    /// A request no route matches, owed its `operation-unsupported` event.
    u64 refused = 0;
    Array<u8> event_payload;
    Merge merge;
};

[[nodiscard]] CompositeSession* state_of(CyServiceSession session) noexcept {
    return reinterpret_cast<CompositeSession*>(session);
}

[[nodiscard]] bool terminal(u32 kind) noexcept {
    return kind == CY_SERVICE_EVENT_COMPLETED || kind == CY_SERVICE_EVENT_FAILED ||
           kind == CY_SERVICE_EVENT_CANCELLED;
}

void begin_event(CyServiceEvent& event, u64 request, u32 kind) noexcept {
    event = {};
    event.struct_size = sizeof(CyServiceEvent);
    event.request_id = request;
    event.schema_version = 1;
    event.kind = kind;
}

[[nodiscard]] bool listed(const Array<u8>& names, std::string_view name) noexcept {
    wire::Reader reader(names.span());
    while (!reader.complete()) {
        const std::string_view candidate = reader.read_text();
        if (candidate == name) {
            return true;
        }
        if (candidate.empty()) {
            return false;
        }
    }
    return false;
}

/// Adds a child's `capabilities.get` answer to the merge, skipping names already listed.
void absorb_capabilities(Merge& merge, const CyServiceEvent& event) noexcept {
    wire::Reader reader(Span<const u8>(event.payload, static_cast<usize>(event.payload_size)));
    const u32 version = reader.read_u32();
    const u32 count = reader.read_u32();
    if (version != 1) {
        return;
    }
    for (u32 index = 0; index < count; ++index) {
        const std::string_view name = reader.read_text();
        if (!name.empty() && !listed(merge.names, name) && wire::put_text(merge.names, name)) {
            ++merge.count;
        }
    }
    merge.features |= reader.read_u64();
}

}  // namespace

Status CompositeEditorService::route(std::string_view prefix,
                                     abi::EditorServiceBackend& backend) noexcept {
    if (open_sessions_ != 0) {
        return fail(ErrorCode::InvalidArgument, "routes are fixed once a session is open");
    }
    if (prefix.empty() || prefix.size() > kMaxPrefix || route_count_ == kMaxRoutes) {
        return fail(ErrorCode::InvalidArgument, "a route needs a prefix of 1 to 47 bytes");
    }
    for (u32 index = 0; index < route_count_; ++index) {
        if (std::string_view(routes_[index].prefix, routes_[index].length) == prefix) {
            return fail(ErrorCode::AlreadyExists, "that prefix is already routed");
        }
    }
    u32 slot = 0;
    while (slot < backend_count_ && backends_[slot] != &backend) {
        ++slot;
    }
    if (slot == backend_count_) {
        backends_[backend_count_++] = &backend;
    }
    Route& added = routes_[route_count_++];
    std::memcpy(added.prefix, prefix.data(), prefix.size());
    added.length = static_cast<u32>(prefix.size());
    added.backend = slot;
    return ok();
}

CyResult CompositeEditorService::open(CyServiceSession* out_session) noexcept {
    void* memory = allocator_->allocate(sizeof(CompositeSession), alignof(CompositeSession));
    if (memory == nullptr) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    auto* state = new (memory) CompositeSession(*allocator_);
    for (u32 index = 0; index < backend_count_; ++index) {
        if (const CyResult opened = backends_[index]->open(&state->children[index]);
            opened != CY_RESULT_OK) {
            ++open_sessions_;
            close(reinterpret_cast<CyServiceSession>(state));
            return opened;
        }
    }
    ++open_sessions_;
    *out_session = reinterpret_cast<CyServiceSession>(state);
    return CY_RESULT_OK;
}

void CompositeEditorService::close(CyServiceSession session) noexcept {
    if (session == nullptr) {
        return;
    }
    CompositeSession* state = state_of(session);
    for (u32 index = 0; index < backend_count_; ++index) {
        if (state->children[index] != nullptr) {
            backends_[index]->close(state->children[index]);
        }
    }
    state->~CompositeSession();
    allocator_->deallocate(state, sizeof(CompositeSession), alignof(CompositeSession));
    --open_sessions_;
}

CyServiceSession CompositeEditorService::child_session(
    CyServiceSession session, const abi::EditorServiceBackend& backend) const noexcept {
    if (session == nullptr) {
        return nullptr;
    }
    for (u32 index = 0; index < backend_count_; ++index) {
        if (backends_[index] == &backend) {
            return state_of(session)->children[index];
        }
    }
    return nullptr;
}

namespace {

void start_merge(Merge& merge, u64 request) noexcept {
    merge.active = true;
    merge.cancelled = false;
    merge.request = request;
    merge.count = 0;
    merge.features = 0;
    merge.names.clear();
    for (Ask& ask : merge.ask) {
        ask = Ask::Pending;
    }
}

[[nodiscard]] CyResult emit_refused(CompositeSession& state, CyServiceEvent& out_event) noexcept {
    begin_event(out_event, state.refused, CY_SERVICE_EVENT_FAILED);
    state.refused = 0;
    if (!wire::encode_failure(state.event_payload, "operation-unsupported",
                              "no editor service is routed for this operation")) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    out_event.payload = state.event_payload.data();
    out_event.payload_size = state.event_payload.size();
    return CY_RESULT_OK;
}

/// Asks every child that is idle for this session; a busy one is asked on a later poll.
void ask_children(CompositeSession& state, abi::EditorServiceBackend* const* backends,
                  u32 count) noexcept {
    Merge& merge = state.merge;
    for (u32 index = 0; index < count; ++index) {
        if (merge.ask[index] != Ask::Pending || state.inflight[index] != 0) {
            continue;
        }
        const CyServiceRequest ask{sizeof(CyServiceRequest), 1,       merge.request,
                                   "capabilities.get",       nullptr, 0};
        const CyResult asked = backends[index]->submit(state.children[index], ask);
        if (asked == CY_RESULT_OK) {
            merge.ask[index] = Ask::Asked;
            state.inflight[index] = merge.request;
        } else if (asked != CY_RESULT_ALREADY_EXISTS) {
            merge.ask[index] = Ask::Done;
        }
    }
}

/// Records a child's event. True when it is the caller's to forward, false when it belonged to a
/// capabilities merge and was absorbed.
[[nodiscard]] bool settle(CompositeSession& state, u32 index,
                          const CyServiceEvent& event) noexcept {
    const bool ends = terminal(event.kind);
    if (ends && state.inflight[index] == event.request_id) {
        state.inflight[index] = 0;
    }
    Merge& merge = state.merge;
    if (!merge.active || event.request_id != merge.request) {
        return true;
    }
    if (ends) {
        if (event.kind == CY_SERVICE_EVENT_COMPLETED) {
            absorb_capabilities(merge, event);
        }
        merge.ask[index] = Ask::Done;
    }
    return false;
}

/// Polls each child once, starting after the last one that answered, until one has an event to
/// forward.
[[nodiscard]] CyResult poll_children(CompositeSession& state,
                                     abi::EditorServiceBackend* const* backends, u32 count,
                                     CyServiceEvent& out_event, bool& out_has_event) noexcept {
    for (u32 step = 0; step < count; ++step) {
        const u32 index = (state.next + step) % count;
        bool present = false;
        if (const CyResult polled =
                backends[index]->poll(state.children[index], out_event, present);
            polled != CY_RESULT_OK) {
            return polled;
        }
        if (!present) {
            continue;
        }
        state.next = (index + 1) % count;
        if (settle(state, index, out_event)) {
            out_has_event = true;
            return CY_RESULT_OK;
        }
    }
    return CY_RESULT_OK;
}

/// Emits the merged `capabilities.get` once every child has answered or been skipped.
[[nodiscard]] CyResult finish_merge(CompositeSession& state, u32 count, CyServiceEvent& out_event,
                                    bool& out_has_event) noexcept {
    Merge& merge = state.merge;
    for (u32 index = 0; index < count; ++index) {
        if (merge.ask[index] != Ask::Done) {
            return CY_RESULT_OK;
        }
    }
    merge.active = false;
    begin_event(out_event, merge.request,
                merge.cancelled ? CY_SERVICE_EVENT_CANCELLED : CY_SERVICE_EVENT_COMPLETED);
    Array<u8>& out = state.event_payload;
    out.clear();
    if (!merge.cancelled &&
        (!wire::put_u32(out, 1) || !wire::put_u32(out, merge.count) ||
         !out.append(merge.names.span()) || !wire::put_u64(out, merge.features))) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    out_event.payload = out.data();
    out_event.payload_size = out.size();
    out_has_event = true;
    return CY_RESULT_OK;
}

}  // namespace

u32 CompositeEditorService::backend_for(std::string_view operation) const noexcept {
    u32 best = kMaxRoutes;
    u32 best_length = 0;
    for (u32 index = 0; index < route_count_; ++index) {
        const Route& candidate = routes_[index];
        if (candidate.length > best_length &&
            operation.starts_with(std::string_view(candidate.prefix, candidate.length))) {
            best = candidate.backend;
            best_length = candidate.length;
        }
    }
    return best;
}

CyResult CompositeEditorService::submit(CyServiceSession session,
                                        const CyServiceRequest& request) noexcept {
    CompositeSession* state = state_of(session);
    if (request.request_id == 0 || request.operation == nullptr) {
        return CY_RESULT_INVALID_ARGUMENT;
    }
    const std::string_view operation(request.operation);
    if (operation == "capabilities.get") {
        if (state->merge.active) {
            return CY_RESULT_ALREADY_EXISTS;
        }
        start_merge(state->merge, request.request_id);
        return CY_RESULT_OK;
    }
    const u32 backend = backend_for(operation);
    if (backend == kMaxRoutes) {
        if (state->refused != 0) {
            return CY_RESULT_ALREADY_EXISTS;
        }
        state->refused = request.request_id;
        return CY_RESULT_OK;
    }
    const CyResult result = backends_[backend]->submit(state->children[backend], request);
    if (result == CY_RESULT_OK && state->inflight[backend] == 0) {
        state->inflight[backend] = request.request_id;
    }
    return result;
}

CyResult CompositeEditorService::cancel(CyServiceSession session, u64 request_id) noexcept {
    CompositeSession* state = state_of(session);
    if (state->merge.active && state->merge.request == request_id) {
        state->merge.cancelled = true;
        return CY_RESULT_OK;
    }
    CyResult result = CY_RESULT_NOT_FOUND;
    for (u32 index = 0; index < backend_count_ && result != CY_RESULT_OK; ++index) {
        result = backends_[index]->cancel(state->children[index], request_id);
    }
    return result;
}

CyResult CompositeEditorService::poll(CyServiceSession session, CyServiceEvent& out_event,
                                      bool& out_has_event) noexcept {
    CompositeSession* state = state_of(session);
    out_has_event = false;
    if (state->refused != 0) {
        out_has_event = true;
        return emit_refused(*state, out_event);
    }
    if (state->merge.active) {
        ask_children(*state, backends_, backend_count_);
    }
    if (const CyResult polled =
            poll_children(*state, backends_, backend_count_, out_event, out_has_event);
        polled != CY_RESULT_OK || out_has_event || !state->merge.active) {
        return polled;
    }
    return finish_merge(*state, backend_count_, out_event, out_has_event);
}

}  // namespace cy::editor
