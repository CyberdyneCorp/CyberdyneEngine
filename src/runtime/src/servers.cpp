#include <cy/runtime/servers.h>

#include <cy/core/base/assert.h>
#include <cy/core/base/diagnostic_sink.h>

#include <cstring>

namespace cy::runtime {

const char* server_kind_name(ServerKind kind) noexcept {
    switch (kind) {
        case ServerKind::Display:
            return "display";
        case ServerKind::Input:
            return "input";
        case ServerKind::Render:
            return "render";
        case ServerKind::Audio:
            return "audio";
        case ServerKind::Physics:
            return "physics";
        case ServerKind::Navigation:
            return "navigation";
        case ServerKind::Text:
            return "text";
        case ServerKind::Count:
            break;
    }
    return "unknown";
}

const char* backend_outcome_name(BackendOutcome outcome) noexcept {
    switch (outcome) {
        case BackendOutcome::NotRequested:
            return "not-requested";
        case BackendOutcome::Requested:
            return "requested";
        case BackendOutcome::Default:
            return "default";
        case BackendOutcome::NullFallback:
            return "null-fallback";
    }
    return "unknown";
}

ServerRegistry::ServerRegistry(Allocator& allocator) noexcept
    : allocator_(&allocator), registrations_(allocator), order_(allocator) {
    for (u32 index = 0; index < kServerCount; ++index) {
        slots_[index].kind = static_cast<ServerKind>(index);
        slots_[index].server = &nulls_[index];
    }
}

Status ServerRegistry::register_backend(ServerKind kind, const char* name, ServerFactory factory,
                                        void* user, bool is_default) noexcept {
    if (kind == ServerKind::Count) {
        return fail(ErrorCode::InvalidArgument, "ServerKind::Count is not a server");
    }
    if (name == nullptr || *name == '\0' || factory == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "a backend registration needs a name and a factory: the name is what a "
                    "configuration selects it by");
    }
    if (find(kind, name) != nullptr) {
        return fail(ErrorCode::AlreadyExists,
                    "a backend with this name is registered for this "
                    "server");
    }
    if (is_default && find_default(kind) != nullptr) {
        return fail(ErrorCode::AlreadyExists,
                    "this server already has a documented default backend; two would make which "
                    "one runs a function of registration order");
    }

    Registration registration;
    registration.kind = kind;
    registration.name = name;
    registration.factory = factory;
    registration.user = user;
    registration.is_default = is_default;
    return registrations_.push_back(registration);
}

const ServerRegistry::Registration* ServerRegistry::find(ServerKind kind,
                                                         const char* name) const noexcept {
    if (name == nullptr || *name == '\0') {
        return nullptr;
    }
    for (const Registration& registration : registrations_) {
        if (registration.kind == kind && std::strcmp(registration.name, name) == 0) {
            return &registration;
        }
    }
    return nullptr;
}

const ServerRegistry::Registration* ServerRegistry::find_default(ServerKind kind) const noexcept {
    for (const Registration& registration : registrations_) {
        if (registration.kind == kind && registration.is_default) {
            return &registration;
        }
    }
    return nullptr;
}

Server* ServerRegistry::bring_up(const Registration& registration) noexcept {
    const auto constructed = registration.factory(*allocator_, registration.user);
    if (!constructed) {
        emit_diagnosticf(DiagnosticSeverity::Warning, "runtime",
                         "the %s backend '%s' could not be constructed: %s",
                         server_kind_name(registration.kind), registration.name,
                         constructed.error().message);
        return nullptr;
    }
    Server* server = constructed.value();
    if (server == nullptr) {
        emit_diagnosticf(DiagnosticSeverity::Warning, "runtime",
                         "the %s backend '%s' returned no server",
                         server_kind_name(registration.kind), registration.name);
        return nullptr;
    }
    const Status started = server->initialize();
    if (!started) {
        emit_diagnosticf(
            DiagnosticSeverity::Warning, "runtime", "the %s backend '%s' failed to initialise: %s",
            server_kind_name(registration.kind), registration.name, started.error().message);
        server->shutdown();
        return nullptr;
    }
    return server;
}

Server* ServerRegistry::select_one(ServerKind kind, const char* wanted,
                                   BackendOutcome& outcome) noexcept {
    // `engine-architecture`'s chain, one step per paragraph: the requested backend, then the
    // documented default, then the null implementation. Written as straight-line steps rather than
    // a loop over candidates, because each step reports a different outcome and the outcome is the
    // part a diagnostic needs.
    const bool asked = wanted != nullptr && *wanted != '\0';
    outcome = BackendOutcome::NotRequested;

    const Registration* asked_for = find(kind, wanted);
    if (asked_for != nullptr) {
        if (Server* server = bring_up(*asked_for); server != nullptr) {
            outcome = BackendOutcome::Requested;
            return server;
        }
    } else if (asked) {
        emit_diagnosticf(DiagnosticSeverity::Warning, "runtime",
                         "no %s backend named '%s' is registered; falling back",
                         server_kind_name(kind), wanted);
    }

    // A backend that constructs and then fails to initialise is unavailable, and the requirement's
    // chain is about availability — so the default is owed a try either way.
    const Registration* fallback = find_default(kind);
    if (fallback != nullptr && fallback != asked_for) {
        if (Server* server = bring_up(*fallback); server != nullptr) {
            outcome = BackendOutcome::Default;
            return server;
        }
    }

    // `NullFallback` and `NotRequested` are distinguished because "you asked and could not have it"
    // and "nobody wanted one" are different facts about a build.
    outcome = (asked || asked_for != nullptr || fallback != nullptr) ? BackendOutcome::NullFallback
                                                                     : BackendOutcome::NotRequested;
    return nullptr;
}

Status ServerRegistry::select_all(const char* const* requested, u32 count) noexcept {
    for (u32 index = 0; index < kServerCount; ++index) {
        const auto kind = static_cast<ServerKind>(index);
        const char* wanted = (requested != nullptr && index < count) ? requested[index] : nullptr;

        ServerSlot& slot = slots_[index];
        slot.requested = (wanted != nullptr) ? wanted : "";

        BackendOutcome outcome = BackendOutcome::NotRequested;
        Server* server = select_one(kind, wanted, outcome);
        slot.outcome = outcome;

        if (server == nullptr) {
            // The null implementation keeps handle bookkeeping valid, so `get()` never returns null
            // and a caller has no null branch to forget.
            if (Status started = nulls_[index].initialize(); !started) {
                return started;
            }
            slot.server = &nulls_[index];
            continue;
        }

        slot.server = server;
        if (Status recorded = order_.push_back(kind); !recorded) {
            return recorded;
        }
    }
    return ok();
}

Server& ServerRegistry::get(ServerKind kind) noexcept {
    CY_ASSERT_MSG(kind != ServerKind::Count, "ServerKind::Count is not a server");
    return *slots_[static_cast<u32>(kind)].server;
}

const ServerSlot& ServerRegistry::slot(ServerKind kind) const noexcept {
    CY_ASSERT_MSG(kind != ServerKind::Count, "ServerKind::Count is not a server");
    return slots_[static_cast<u32>(kind)];
}

void ServerRegistry::shutdown_all() noexcept {
    // Exact reverse of bring-up, which is the same rule the runtime's stage journal follows and for
    // the same reason: a server torn down before one that holds its handles is a use-after-free
    // that only shows up on the machine where the order happened to differ.
    for (usize index = order_.size(); index > 0; --index) {
        const auto kind = static_cast<u32>(order_[index - 1]);
        slots_[kind].server->shutdown();
        slots_[kind].server = &nulls_[kind];
    }
    order_.clear();
}

u32 ServerRegistry::live_backends() const noexcept {
    u32 live = 0;
    for (const ServerSlot& slot : slots_) {
        if (slot.server != nullptr && !slot.server->is_null_backend()) {
            ++live;
        }
    }
    return live;
}

}  // namespace cy::runtime
