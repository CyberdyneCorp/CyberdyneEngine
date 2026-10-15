// SPDX-License-Identifier: MIT
// `illumination.report`. See cy/editor/illumination_service.h.

#include <cy/editor/illumination_service.h>

#include <cy/rendering/gi/diagnostics.h>
#include <cy/rendering/gi/emissive.h>
#include <cy/rendering/gi/gi_volume.h>
#include <cy/rendering/gi/scene.h>

#include <cmath>
#include <cstring>
#include <new>
#include <string_view>
#include <utility>

#include "service_wire.h"

namespace cy::editor {
namespace {

namespace gi = rendering::gi;

constexpr std::string_view kReport = "illumination.report";
constexpr std::string_view kCapabilities = "capabilities.get";
constexpr usize kOperationCapacity = 96;

struct IlluminationSession {
    explicit IlluminationSession(Allocator& allocator) noexcept
        : request_payload(allocator), event_payload(allocator) {}

    char operation[kOperationCapacity] = {};
    Array<u8> request_payload;
    Array<u8> event_payload;
    u64 request = 0;
    u32 schema = 0;
    /// A second request while one is pending, answered `illumination.busy` on the next poll.
    u64 busy_request = 0;
    bool pending = false;
    bool cancelled = false;
};

[[nodiscard]] IlluminationSession* state_of(CyServiceSession session) noexcept {
    return reinterpret_cast<IlluminationSession*>(session);
}

void begin_event(CyServiceEvent& event, u64 request, u32 kind) noexcept {
    event = {};
    event.struct_size = sizeof(CyServiceEvent);
    event.request_id = request;
    event.schema_version = 1;
    event.kind = kind;
}

[[nodiscard]] const char* mode_name(gi::GiMode mode) noexcept {
    return gi::gi_mode_name(mode);
}

void write_settings(wire::Writer& out, const gi::GiRuntimeSettings& settings) noexcept {
    out.text(mode_name(settings.mode))
        .text(gi::gi_quality_name(settings.quality))
        .text(gi::gi_tier_preference_name(settings.tier))
        .u8v(settings.hardware_ray_tracing ? 1U : 0U)
        .f32v(settings.budget_ms);
}

void write_profile(wire::Writer& out, const gi::GiProfile& profile) noexcept {
    out.u64v(profile.frame).f32v(profile.allocation_ms).f32v(profile.measured_ms);
    out.u32v(gi::kGiStageCount);
    for (u32 stage = 0; stage < gi::kGiStageCount; ++stage) {
        out.text(gi::gi_stage_name(static_cast<gi::GiStage>(stage))).f64v(profile.stage_ms[stage]);
    }
    out.u32v(profile.probes_updated)
        .u32v(profile.surface_pages_updated)
        .u32v(profile.screen_rays)
        .u32v(profile.software_rays)
        .u32v(profile.hardware_rays)
        .f32v(profile.surface_cache_hit_rate)
        .f32v(profile.radiance_cache_hit_rate);
}

void write_emissive(wire::Writer& out, const IlluminationReport& report) noexcept {
    const gi::EmissiveReport& totals = report.emissive;
    out.u32v(totals.emitters)
        .f32v(totals.total_power)
        .f32v(totals.cached_power)
        .f32v(totals.promoted_power)
        .f32v(totals.demoted_power)
        .f32v(totals.disabled_power);
    out.u32v(static_cast<u32>(report.emitters.size()));
    for (const IlluminationEmitterEntry& emitter : report.emitters) {
        out.u64v(emitter.node)
            .text(emitter.material)
            .text(gi::emitter_class_name(emitter.classification))
            .text(gi::emitter_override_name(emitter.override_mode))
            .u32v(emitter.cards)
            .f32v(emitter.area_m2)
            .f32v(emitter.power);
    }
}

void write_query(wire::Writer& out, const IlluminationReport& report) noexcept {
    out.u8v(report.explained ? 1U : 0U);
    if (!report.explained) {
        return;
    }
    const gi::IlluminationQuery& query = report.query;
    out.vec3(report.point)
        .vec3(report.normal)
        .text(gi::darkness_cause_name(query.cause))
        .vec3(query.diffuse.radiance)
        .f32v(query.diffuse.confidence)
        .text(gi::radiance_source_name(query.diffuse.dominant))
        .u32v(query.probes_contributing)
        .u32v(query.probes_in_reach)
        .u32v(query.leak_suspects)
        .f32v(query.leak_weight)
        .text(gi::radiance_source_name(query.world_tier))
        .f32v(query.convergence)
        .u32v(query.source_count);
    for (u32 index = 0; index < query.source_count; ++index) {
        const gi::SourceReport& source = query.sources[index];
        out.text(gi::radiance_source_name(source.source))
            .vec3(source.radiance)
            .f32v(source.confidence)
            .f32v(source.weight_share)
            .u8v(source.excluded ? 1U : 0U);
    }
}

[[nodiscard]] CyResult written(const Status& status) noexcept {
    return status ? CY_RESULT_OK : CY_RESULT_OUT_OF_MEMORY;
}

[[nodiscard]] CyResult failed(IlluminationSession& session, bool& failed_event,
                              std::string_view code, std::string_view detail) noexcept {
    failed_event = true;
    return written(wire::encode_failure(session.event_payload, code, detail));
}

[[nodiscard]] CyResult capabilities(IlluminationSession& session, bool seam) noexcept {
    wire::Writer out(session.event_payload);
    out.u32v(1).u32v(seam ? 2U : 1U).text(kCapabilities);
    if (seam) {
        out.text(kReport);
    }
    out.u64v(seam ? kIlluminationFeature : 0U);
    return written(out.status());
}

[[nodiscard]] CyResult report(IlluminationSession& session, IlluminationRuntime* runtime,
                              bool& failed_event) noexcept {
    IlluminationPreview* preview = runtime == nullptr ? nullptr : runtime->preview();
    if (preview == nullptr) {
        return failed(session, failed_event, "illumination.report.unavailable",
                      "the runtime host has no world lit by the GI");
    }
    wire::Reader in(session.request_payload.span());
    const u32 format = in.read_u32();
    const u8 pick = in.read_u8();
    u32 viewport = 0;
    u64 frame = 0;
    f32 x = 0.0F;
    f32 y = 0.0F;
    if (pick == 1U) {
        viewport = in.read_u32();
        frame = in.read_u64();
        x = in.read_f32();
        y = in.read_f32();
    }
    if (!in.complete() || format != kIlluminationReportFormat || pick > 1U || !std::isfinite(x) ||
        !std::isfinite(y)) {
        return failed(session, failed_event, "illumination.request.malformed",
                      "a report request is u32 1, u8 pick and, when picking, a viewport, a frame "
                      "and a finite pixel");
    }
    Ray ray;
    bool has_ray = false;
    if (pick == 1U) {
        Expected<Ray, Error> picked = runtime->pick_ray(viewport, frame, x, y);
        if (!picked) {
            return failed(session, failed_event, "illumination.pick.unknown-frame",
                          picked.error().message);
        }
        ray = picked.value();
        has_ray = true;
    }
    const IlluminationReport answered = preview->report(has_ray ? &ray : nullptr);
    session.event_payload.clear();
    return written(encode_illumination_report(answered, session.event_payload));
}

}  // namespace

Status encode_illumination_report(const IlluminationReport& report, Array<u8>& out) noexcept {
    wire::Writer writer(out);
    writer.u32v(kIlluminationReportFormat);
    write_settings(writer, report.settings);
    writer.text(gi::radiance_source_name(report.world_tier))
        .f32v(report.convergence)
        .u32v(report.cards)
        .u32v(report.probes)
        .u32v(report.volumes)
        .u32v(report.lights)
        .u64v(report.rebuilds)
        .u64v(report.applications);
    write_profile(writer, report.profile);
    write_emissive(writer, report);
    writer.u32v(static_cast<u32>(report.objects.size()));
    for (const IlluminationObjectEntry& object : report.objects) {
        writer.u64v(object.node)
            .text(gi::illumination_class_name(object.klass))
            .u8v(object.overridden ? 1U : 0U);
    }
    write_query(writer, report);
    return writer.status();
}

CyResult IlluminationService::open(CyServiceSession* out_session) noexcept {
    void* memory = allocator_->allocate(sizeof(IlluminationSession), alignof(IlluminationSession));
    if (memory == nullptr) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    *out_session =
        reinterpret_cast<CyServiceSession>(new (memory) IlluminationSession(*allocator_));
    return CY_RESULT_OK;
}

void IlluminationService::close(CyServiceSession session) noexcept {
    if (session == nullptr) {
        return;
    }
    IlluminationSession* state = state_of(session);
    state->~IlluminationSession();
    allocator_->deallocate(state, sizeof(IlluminationSession), alignof(IlluminationSession));
}

CyResult IlluminationService::submit(CyServiceSession session,
                                     const CyServiceRequest& request) noexcept {
    IlluminationSession* state = state_of(session);
    if (request.request_id == 0 || request.operation == nullptr) {
        return CY_RESULT_INVALID_ARGUMENT;
    }
    if (state->pending) {
        if (state->busy_request != 0 || request.request_id == state->request) {
            return CY_RESULT_ALREADY_EXISTS;
        }
        state->busy_request = request.request_id;
        return CY_RESULT_OK;
    }
    const usize size = std::strlen(request.operation);
    if (size >= sizeof(state->operation)) {
        return CY_RESULT_INVALID_ARGUMENT;
    }
    std::memcpy(state->operation, request.operation, size + 1);
    state->request_payload.clear();
    if (Status copied = state->request_payload.append({request.payload, request.payload_size});
        !copied) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    state->request = request.request_id;
    state->schema = request.schema_version;
    state->pending = true;
    state->cancelled = false;
    return CY_RESULT_OK;
}

CyResult IlluminationService::cancel(CyServiceSession session, u64 request_id) noexcept {
    IlluminationSession* state = state_of(session);
    if (!state->pending || state->request != request_id) {
        return CY_RESULT_NOT_FOUND;
    }
    state->cancelled = true;
    return CY_RESULT_OK;
}

CyResult IlluminationService::poll(CyServiceSession session, CyServiceEvent& out_event,
                                   bool& out_has_event) noexcept {
    IlluminationSession* state = state_of(session);
    out_has_event = false;
    if (state->busy_request != 0) {
        begin_event(out_event, state->busy_request, CY_SERVICE_EVENT_FAILED);
        state->busy_request = 0;
        if (!wire::encode_failure(state->event_payload, "illumination.busy",
                                  "another illumination request is pending in this session")) {
            return CY_RESULT_OUT_OF_MEMORY;
        }
        out_event.payload = state->event_payload.data();
        out_event.payload_size = state->event_payload.size();
        out_has_event = true;
        return CY_RESULT_OK;
    }
    if (!state->pending) {
        return CY_RESULT_OK;
    }
    begin_event(out_event, state->request, CY_SERVICE_EVENT_COMPLETED);
    state->event_payload.clear();
    bool failed_event = false;
    const std::string_view operation(state->operation);
    CyResult result = CY_RESULT_OK;
    if (state->cancelled) {
        out_event.kind = CY_SERVICE_EVENT_CANCELLED;
    } else if (state->schema != 1) {
        result = failed(*state, failed_event, "illumination.schema.unsupported",
                        "this operation supports schema 1");
    } else if (operation == kCapabilities) {
        result = capabilities(*state, runtime_ != nullptr);
    } else if (operation == kReport) {
        result = report(*state, runtime_, failed_event);
    } else {
        result = failed(*state, failed_event, "operation-unsupported",
                        "this backend does not support the operation");
    }
    if (result != CY_RESULT_OK) {
        return result;
    }
    if (failed_event) {
        out_event.kind = CY_SERVICE_EVENT_FAILED;
    }
    out_event.payload = state->event_payload.data();
    out_event.payload_size = state->event_payload.size();
    out_has_event = true;
    state->pending = false;
    return CY_RESULT_OK;
}

}  // namespace cy::editor
