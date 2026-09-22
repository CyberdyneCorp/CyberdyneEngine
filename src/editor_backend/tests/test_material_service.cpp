#include <cy/abi/cy_abi.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/editor/material_service.h>
#include <cy/graph/cybergraph.h>
#include <cy/test/test.h>

#include <cstring>
#include <string_view>

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Scripting);
}

cy::u32 read_u32(const cy::u8* bytes) noexcept {
    return static_cast<cy::u32>(bytes[0]) | (static_cast<cy::u32>(bytes[1]) << 8U) |
           (static_cast<cy::u32>(bytes[2]) << 16U) | (static_cast<cy::u32>(bytes[3]) << 24U);
}

cy::u64 read_u64(const cy::u8* bytes) noexcept {
    cy::u64 value = 0;
    for (cy::usize byte = 0; byte < 8; ++byte) {
        value |= static_cast<cy::u64>(bytes[byte]) << (byte * 8);
    }
    return value;
}

std::string_view read_text(const cy::u8* bytes, cy::usize size, cy::usize& cursor) {
    CY_REQUIRE(cursor + 4 <= size);
    const cy::u32 length = read_u32(bytes + cursor);
    cursor += 4;
    CY_REQUIRE(cursor + length <= size);
    const std::string_view text(reinterpret_cast<const char*>(bytes + cursor), length);
    cursor += length;
    return text;
}

CyServiceEvent submit_and_poll(const CyInterface& api, cy::abi::Host& host,
                               CyServiceSession session, const CyServiceRequest& request) {
    CY_REQUIRE_EQ(api.service_submit(&host, session, &request), CY_RESULT_OK);
    CyServiceEvent event{};
    bool present = false;
    CY_REQUIRE_EQ(api.service_poll(&host, session, &event, &present), CY_RESULT_OK);
    CY_REQUIRE(present);
    return event;
}

class PreviewRuntime final : public cy::editor::MaterialPreviewRuntime {
public:
    cy::Status publish(cy::u64 artefact,
                       const cy::rendering::material::CompiledMaterial&) noexcept override {
        published = artefact;
        return cy::ok();
    }

    cy::Status create(cy::u64 preview) noexcept override {
        created = preview;
        return cy::ok();
    }

    cy::Status reload(cy::u64 preview, cy::u64 artefact,
                      cy::Span<const cy::editor::MaterialPreviewTarget> targets) noexcept override {
        if (reject_reload) {
            return cy::fail(cy::ErrorCode::Unavailable,
                            "the viewport refused to install the material program");
        }
        reloaded_preview = preview;
        reloaded_artefact = artefact;
        reloaded_targets = static_cast<cy::u32>(targets.size());
        reloaded_slot = targets.empty() ? 0 : targets[0].material_slot;
        return cy::ok();
    }

    cy::Status update(cy::u64 preview, cy::u64 artefact,
                      const cy::editor::MaterialParameterUpdate& parameter) noexcept override {
        updated_preview = preview;
        updated_artefact = artefact;
        updated_parameter = parameter.identity;
        updated_kind = parameter.kind;
        return cy::ok();
    }

    cy::Status destroy(cy::u64 preview) noexcept override {
        destroyed = preview;
        return cy::ok();
    }

    cy::u64 published = 0;
    cy::u64 created = 0;
    cy::u64 reloaded_preview = 0;
    cy::u64 reloaded_artefact = 0;
    cy::u64 updated_preview = 0;
    cy::u64 updated_artefact = 0;
    cy::u64 destroyed = 0;
    cy::u32 reloaded_targets = 0;
    cy::u32 reloaded_slot = 0;
    cy::u32 updated_parameter = 0;
    cy::u8 updated_kind = 0;
    bool reject_reload = false;
};

}  // namespace

CY_TEST_CASE("editor_backend: catalogue crosses the ABI service unchanged") {
    cy::abi::Host host(allocator());
    cy::editor::MaterialService service(allocator());
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(api != nullptr);

    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);
    const CyServiceRequest request{sizeof(CyServiceRequest), 1,       1,
                                   "material.catalogue.get", nullptr, 0};
    CY_REQUIRE_EQ(api->service_submit(&host, session, &request), CY_RESULT_OK);
    CyServiceEvent event{};
    bool present = false;
    CY_REQUIRE_EQ(api->service_poll(&host, session, &event, &present), CY_RESULT_OK);
    CY_REQUIRE(present);
    CY_CHECK_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
    CY_REQUIRE(event.payload_size >= 12U);
    CY_CHECK_EQ(read_u32(event.payload), 2U);
    CY_CHECK_EQ(read_u32(event.payload + 4), 3U);
    CY_CHECK_EQ(read_u32(event.payload + 8), 25U);
    api->service_close(&host, session);
}

CY_TEST_CASE("editor_backend: cancellation wins before publication") {
    cy::abi::Host host(allocator());
    cy::editor::MaterialService service(allocator());
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);
    const CyServiceRequest request{sizeof(CyServiceRequest), 1, 9, "material.compile", nullptr, 0};
    CY_REQUIRE_EQ(api->service_submit(&host, session, &request), CY_RESULT_OK);
    CY_REQUIRE_EQ(api->service_cancel(&host, session, 9), CY_RESULT_OK);
    CyServiceEvent event{};
    bool present = false;
    CY_REQUIRE_EQ(api->service_poll(&host, session, &event, &present), CY_RESULT_OK);
    CY_REQUIRE(present);
    CY_CHECK_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_CANCELLED));
    CY_CHECK_EQ(event.request_id, 9U);
    api->service_close(&host, session);
}

CY_TEST_CASE("editor_backend: zero is never a request identity") {
    cy::abi::Host host(allocator());
    cy::editor::MaterialService service(allocator());
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);
    const CyServiceRequest request{sizeof(CyServiceRequest), 1, 0, "capabilities.get", nullptr, 0};
    CY_CHECK_EQ(api->service_submit(&host, session, &request), CY_RESULT_INVALID_ARGUMENT);
    api->service_close(&host, session);
}

CY_TEST_CASE("editor_backend: capabilities are discoverable and schema mismatches are structured") {
    cy::abi::Host host(allocator());
    cy::editor::MaterialService service(allocator());
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);

    const CyServiceRequest capabilities{sizeof(CyServiceRequest), 1,       1,
                                        "capabilities.get",       nullptr, 0};
    CyServiceEvent event = submit_and_poll(*api, host, session, capabilities);
    CY_REQUIRE_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
    CY_REQUIRE(event.payload_size >= 8U);
    CY_CHECK_EQ(read_u32(event.payload), 1U);
    CY_CHECK_EQ(read_u32(event.payload + 4), 8U);

    const CyServiceRequest too_new{sizeof(CyServiceRequest), 2, 2, "capabilities.get", nullptr, 0};
    event = submit_and_poll(*api, host, session, too_new);
    CY_CHECK_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_FAILED));
    CY_REQUIRE(event.payload_size >= 8U);
    CY_CHECK_EQ(read_u32(event.payload), 1U);
    api->service_close(&host, session);
}

CY_TEST_CASE("editor_backend: material diagnostics carry stable node and pin locations") {
    cy::abi::Host host(allocator());
    cy::editor::MaterialService service(allocator());
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);

    constexpr std::string_view canvas =
        "cymatcanvas 1\n"
        "material broken_link\n"
        "node 1 material.diffuse\n"
        "node 2 material.texture_sample\n"
        "link 1 out 2 uv\n";
    const CyServiceRequest request{
        sizeof(CyServiceRequest),
        1,
        17,
        "material.validate",
        reinterpret_cast<const cy::u8*>(canvas.data()),
        canvas.size(),
    };
    const CyServiceEvent event = submit_and_poll(*api, host, session, request);
    CY_REQUIRE_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_FAILED));

    cy::usize cursor = 0;
    CY_REQUIRE(event.payload_size >= 8U);
    CY_CHECK_EQ(read_u32(event.payload + cursor), 2U);
    cursor += 4;
    CY_CHECK_EQ(read_u32(event.payload + cursor), 1U);
    cursor += 4;
    CY_CHECK_EQ(event.payload[cursor++], static_cast<cy::u8>(cy::graph::Severity::Error));
    CY_CHECK_EQ(read_text(event.payload, event.payload_size, cursor), "graph.link.type-mismatch");
    (void)read_text(event.payload, event.payload_size, cursor);  // message
    CY_CHECK_EQ(read_text(event.payload, event.payload_size, cursor), "closure");
    CY_CHECK_EQ(read_text(event.payload, event.payload_size, cursor), "broken_link");
    CY_CHECK_EQ(read_u64(event.payload + cursor), 2U);
    cursor += 8;
    CY_CHECK_EQ(read_u32(event.payload + cursor), 5U);  // material.texture_sample
    cursor += 4;
    CY_CHECK_EQ(read_u32(event.payload + cursor), 1U);  // uv
    cursor += 4;
    CY_CHECK_EQ(read_text(event.payload, event.payload_size, cursor), "uv");
    CY_CHECK_EQ(read_u32(event.payload + cursor), 1U);
    cursor += 4;
    CY_CHECK_EQ(read_text(event.payload, event.payload_size, cursor), "broken_link");
    CY_CHECK_EQ(read_u64(event.payload + cursor), 1U);
    cursor += 8;
    CY_CHECK_EQ(read_u32(event.payload + cursor), 16U);  // material.diffuse
    cursor += 4;
    CY_CHECK_EQ(read_u32(event.payload + cursor), 3U);  // out
    api->service_close(&host, session);
}

CY_TEST_CASE("editor_backend: preview handles are generational and reload is acknowledged") {
    cy::abi::Host host(allocator());
    PreviewRuntime preview_runtime;
    cy::editor::MaterialService service(allocator(), &preview_runtime);
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);

    const CyServiceRequest create{sizeof(CyServiceRequest), 1, 1, "preview.create", nullptr, 0};
    CY_REQUIRE_EQ(api->service_submit(&host, session, &create), CY_RESULT_OK);
    CyServiceEvent event{};
    bool present = false;
    CY_REQUIRE_EQ(api->service_poll(&host, session, &event, &present), CY_RESULT_OK);
    CY_REQUIRE(present);
    CY_REQUIRE_EQ(event.payload_size, 8U);
    cy::u8 handle[8];
    std::memcpy(handle, event.payload, sizeof(handle));
    CY_CHECK_EQ(preview_runtime.created, read_u64(handle));

    cy::u8 reload_payload[40] = {};
    std::memcpy(reload_payload, handle, 8);
    const cy::u64 artefact = 0xAABB'CCDD'1122'3344ULL;
    std::memcpy(reload_payload + 8, &artefact, 8);
    const cy::u32 target_count = 1;
    std::memcpy(reload_payload + 16, &target_count, 4);
    const cy::u32 material_slot = 2;
    std::memcpy(reload_payload + 36, &material_slot, 4);
    const CyServiceRequest reload{sizeof(CyServiceRequest), 1, 2, "preview.reload", reload_payload,
                                  sizeof(reload_payload)};
    CY_REQUIRE_EQ(api->service_submit(&host, session, &reload), CY_RESULT_OK);
    CY_REQUIRE_EQ(api->service_poll(&host, session, &event, &present), CY_RESULT_OK);
    CY_REQUIRE_EQ(event.payload_size, sizeof(reload_payload));
    cy::u64 requested = 0;
    cy::u64 applied = 0;
    std::memcpy(&requested, event.payload, 8);
    std::memcpy(&applied, event.payload + 8, 8);
    CY_CHECK_EQ(requested, artefact);
    CY_CHECK_EQ(applied, artefact);
    CY_CHECK_EQ(preview_runtime.reloaded_preview, read_u64(handle));
    CY_CHECK_EQ(preview_runtime.reloaded_artefact, artefact);
    CY_CHECK_EQ(preview_runtime.reloaded_targets, 1U);
    CY_CHECK_EQ(preview_runtime.reloaded_slot, material_slot);

    const CyServiceRequest destroy{sizeof(CyServiceRequest), 1, 3, "preview.destroy", handle, 8};
    CY_REQUIRE_EQ(api->service_submit(&host, session, &destroy), CY_RESULT_OK);
    CY_REQUIRE_EQ(api->service_poll(&host, session, &event, &present), CY_RESULT_OK);
    CY_REQUIRE_EQ(api->service_submit(&host, session, &destroy), CY_RESULT_OK);
    CY_REQUIRE_EQ(api->service_poll(&host, session, &event, &present), CY_RESULT_OK);
    CY_CHECK_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
    CY_CHECK_EQ(preview_runtime.destroyed, read_u64(handle));

    const CyServiceRequest stale{sizeof(CyServiceRequest),   1,      4,
                                 "preview.parameter.update", handle, 8};
    CY_REQUIRE_EQ(api->service_submit(&host, session, &stale), CY_RESULT_OK);
    CY_REQUIRE_EQ(api->service_poll(&host, session, &event, &present), CY_RESULT_OK);
    CY_CHECK_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_FAILED));
    api->service_close(&host, session);
}

CY_TEST_CASE("editor_backend: a host without a renderer cannot claim a preview") {
    cy::abi::Host host(allocator());
    cy::editor::MaterialService service(allocator());
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);

    const CyServiceRequest create{sizeof(CyServiceRequest), 1, 1, "preview.create", nullptr, 0};
    const CyServiceEvent event = submit_and_poll(*api, host, session, create);
    CY_CHECK_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_FAILED));
    cy::usize cursor = 4;
    CY_CHECK_EQ(read_text(event.payload, event.payload_size, cursor),
                "preview-runtime-unavailable");
    api->service_close(&host, session);
}

CY_TEST_CASE("editor_backend: a renderer rejection is not acknowledged or made current") {
    cy::abi::Host host(allocator());
    PreviewRuntime preview_runtime;
    cy::editor::MaterialService service(allocator(), &preview_runtime);
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);

    const CyServiceRequest create{sizeof(CyServiceRequest), 1, 1, "preview.create", nullptr, 0};
    CyServiceEvent event = submit_and_poll(*api, host, session, create);
    CY_REQUIRE_EQ(event.payload_size, 8U);
    cy::u8 preview[8] = {};
    std::memcpy(preview, event.payload, sizeof(preview));

    const auto reload = [&](cy::u64 request, cy::u64 artefact) {
        cy::u8 payload[40] = {};
        std::memcpy(payload, preview, sizeof(preview));
        std::memcpy(payload + 8, &artefact, sizeof(artefact));
        const cy::u32 target_count = 1;
        std::memcpy(payload + 16, &target_count, sizeof(target_count));
        const CyServiceRequest request_value{sizeof(CyServiceRequest), 1,       request,
                                             "preview.reload",         payload, sizeof(payload)};
        return submit_and_poll(*api, host, session, request_value);
    };

    constexpr cy::u64 accepted = 0x1111;
    event = reload(2, accepted);
    CY_REQUIRE_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
    preview_runtime.reject_reload = true;
    event = reload(3, 0x2222);
    CY_REQUIRE_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_FAILED));
    CY_CHECK_EQ(preview_runtime.reloaded_artefact, accepted);

    cy::u8 parameter[22] = {};
    std::memcpy(parameter, preview, sizeof(preview));
    std::memcpy(parameter + 8, &accepted, sizeof(accepted));
    const cy::u32 parameter_id = 4;
    std::memcpy(parameter + 16, &parameter_id, sizeof(parameter_id));
    parameter[20] = 1;
    parameter[21] = 1;
    const CyServiceRequest update{sizeof(CyServiceRequest),   1,         4,
                                  "preview.parameter.update", parameter, sizeof(parameter)};
    event = submit_and_poll(*api, host, session, update);
    CY_CHECK_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
    CY_CHECK_EQ(preview_runtime.updated_artefact, accepted);
    CY_CHECK_EQ(preview_runtime.updated_parameter, parameter_id);
    CY_CHECK_EQ(preview_runtime.updated_kind, 1U);
    api->service_close(&host, session);
}
