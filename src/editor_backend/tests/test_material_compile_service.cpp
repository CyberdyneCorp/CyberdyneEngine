// SPDX-License-Identifier: MIT
#include <cy/abi/cy_abi.h>
#include <cy/abi/host.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/editor/material_service.h>
#include <cy/test/test.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Scripting);
}

cy::Array<cy::u8> source_graph() {
    cy::Array<cy::u8> bytes(allocator());
    FILE* file =
        std::fopen(CY_SOURCE_DIR "/content/beauty/materials/weathered_stone.cygraph", "rb");
    CY_REQUIRE(file != nullptr);
    CY_REQUIRE(std::fseek(file, 0, SEEK_END) == 0);
    const long size = std::ftell(file);
    CY_REQUIRE(size > 0);
    CY_REQUIRE(std::fseek(file, 0, SEEK_SET) == 0);
    CY_REQUIRE(bytes.resize(static_cast<cy::usize>(size)));
    CY_REQUIRE(std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size());
    (void)std::fclose(file);
    return bytes;
}

cy::Array<cy::u8> source_canvas() {
    cy::Array<cy::u8> bytes(allocator());
    FILE* file =
        std::fopen(CY_SOURCE_DIR "/content/beauty/materials/weathered_stone.cymatcanvas", "rb");
    CY_REQUIRE(file != nullptr);
    CY_REQUIRE(std::fseek(file, 0, SEEK_END) == 0);
    const long size = std::ftell(file);
    CY_REQUIRE(size > 0);
    CY_REQUIRE(std::fseek(file, 0, SEEK_SET) == 0);
    CY_REQUIRE(bytes.resize(static_cast<cy::usize>(size)));
    CY_REQUIRE(std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size());
    (void)std::fclose(file);
    return bytes;
}

cy::u64 read_u64(const cy::u8* bytes) noexcept {
    cy::u64 value = 0;
    for (cy::usize byte = 0; byte < 8; ++byte) {
        value |= static_cast<cy::u64>(bytes[byte]) << (byte * 8);
    }
    return value;
}

cy::u32 read_u32(const cy::u8* bytes) noexcept {
    cy::u32 value = 0;
    for (cy::usize byte = 0; byte < 4; ++byte) {
        value |= static_cast<cy::u32>(bytes[byte]) << (byte * 8);
    }
    return value;
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

class PublicationRuntime final : public cy::editor::MaterialPreviewRuntime {
public:
    cy::Status publish(
        cy::u64 artefact,
        const cy::rendering::material::CompiledMaterial& material) noexcept override {
        published = artefact;
        programs = static_cast<cy::u32>(material.programs().size());
        return cy::ok();
    }
    cy::Status create(cy::u64) noexcept override { return cy::ok(); }
    cy::Status reload(cy::u64, cy::u64,
                      cy::Span<const cy::editor::MaterialPreviewTarget>) noexcept override {
        return cy::ok();
    }
    cy::Status update(cy::u64, cy::u64,
                      const cy::editor::MaterialParameterUpdate&) noexcept override {
        return cy::ok();
    }
    cy::Status destroy(cy::u64) noexcept override { return cy::ok(); }

    cy::u64 published = 0;
    cy::u32 programs = 0;
};

class AuthoringRuntime final : public cy::editor::MaterialAuthoringRuntime {
public:
    cy::Status preview(std::string_view reference,
                       std::string_view canonical_graph) noexcept override {
        asset = reference;
        graph = canonical_graph;
        ++updates;
        return cy::ok();
    }
    std::string asset;
    std::string graph;
    int updates = 0;
};

void append_text(std::vector<cy::u8>& payload, std::string_view text) {
    const auto size = static_cast<cy::u32>(text.size());
    for (cy::u32 byte = 0; byte < 4; ++byte) {
        payload.push_back(static_cast<cy::u8>((size >> (byte * 8)) & 0xff));
    }
    payload.insert(payload.end(), text.begin(), text.end());
}

}  // namespace

CY_TEST_CASE("editor_backend: material compile returns stable artefact and dependency identities") {
    cy::abi::Host host(allocator());
    PublicationRuntime preview_runtime;
    cy::editor::MaterialService service(allocator(), &preview_runtime);
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);
    const cy::Array<cy::u8> graph = source_graph();

    cy::u64 identities[2] = {};
    for (cy::u64 request_id = 1; request_id <= 2; ++request_id) {
        const CyServiceRequest request{sizeof(CyServiceRequest),
                                       1,
                                       request_id,
                                       "material.compile",
                                       graph.data(),
                                       graph.size()};
        const CyServiceEvent event = submit_and_poll(*api, host, session, request);
        CY_REQUIRE_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
        CY_REQUIRE(event.payload_size >= 25U);
        identities[request_id - 1] = read_u64(event.payload + 5);
        CY_CHECK(read_u64(event.payload + 13) != 0);
    }
    CY_CHECK_EQ(identities[0], identities[1]);
    CY_CHECK_EQ(preview_runtime.published, identities[1]);
    CY_CHECK_GT(preview_runtime.programs, 0U);
    api->service_close(&host, session);
}

CY_TEST_CASE("editor_backend: the visible editor canvas crosses the service compiler boundary") {
    cy::abi::Host host(allocator());
    cy::editor::MaterialService service(allocator());
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);
    cy::Array<cy::u8> canvas = source_canvas();
    constexpr std::string_view dependency = "\nprop 2 texture 0123456789abcdef0123456789abcdef\n";
    CY_REQUIRE(
        canvas.append({reinterpret_cast<const cy::u8*>(dependency.data()), dependency.size()}));

    const CyServiceRequest request{
        sizeof(CyServiceRequest), 1, 41, "material.compile", canvas.data(), canvas.size()};
    const CyServiceEvent event = submit_and_poll(*api, host, session, request);
    CY_REQUIRE_EQ(event.request_id, 41U);
    CY_REQUIRE_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
    CY_REQUIRE(event.payload_size >= 25U);
    CY_CHECK(read_u64(event.payload + 5) != 0);
    CY_CHECK(read_u64(event.payload + 13) != 0);
    CY_REQUIRE(event.payload_size >= 65U);
    CY_CHECK_EQ(read_u32(event.payload + 25), 1U);
    CY_CHECK_EQ(read_u32(event.payload + 29), 32U);
    CY_CHECK(std::memcmp(event.payload + 33, "0123456789abcdef0123456789abcdef", 32) == 0);
    api->service_close(&host, session);
}

CY_TEST_CASE("editor_backend: author returns a canonical graph only for a valid canvas") {
    cy::abi::Host host(allocator());
    cy::editor::MaterialService service(allocator());
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);
    const cy::Array<cy::u8> canvas = source_canvas();
    CyServiceRequest request{
        sizeof(CyServiceRequest), 1, 50, "material.author", canvas.data(), canvas.size()};
    CyServiceEvent event = submit_and_poll(*api, host, session, request);
    CY_REQUIRE_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
    CY_REQUIRE(event.payload_size >= 9U);
    CY_CHECK_EQ(read_u32(event.payload), 2U);
    CY_CHECK_EQ(event.payload[4], 1U);
    const cy::u32 size = read_u32(event.payload + 5);
    CY_REQUIRE_EQ(event.payload_size, 9U + size);
    CY_REQUIRE(size > 8);
    CY_CHECK_EQ(std::memcmp(event.payload + 9, "cygraph 1", 9), 0);

    constexpr std::string_view invalid =
        "cymatcanvas 1\nmaterial broken\nnode 1 material.output\nlink 99 out 1 surface\n";
    request = {sizeof(CyServiceRequest),
               1,
               51,
               "material.author",
               reinterpret_cast<const cy::u8*>(invalid.data()),
               invalid.size()};
    event = submit_and_poll(*api, host, session, request);
    CY_CHECK_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_FAILED));
    api->service_close(&host, session);
}

CY_TEST_CASE("editor_backend: vertex canvas reaches engine authoring and compilation") {
    constexpr std::string_view canvas =
        "cymatcanvas 1\nmaterial wind_sway\n"
        "node 1 material.attribute\nprop 1 symbol position\nprop 1 type float3\n"
        "node 2 material.attribute\nprop 2 symbol time\nprop 2 type float\n"
        "node 3 material.sin\nnode 4 material.multiply\n"
        "node 5 material.vertex_output\n"
        "link 2 out 3 value\nlink 1 out 4 a\nlink 3 out 4 b\nlink 4 out 5 offset\n";
    cy::abi::Host host(allocator());
    cy::editor::MaterialService service(allocator());
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);

    const CyServiceRequest author{sizeof(CyServiceRequest),
                                  1,
                                  52,
                                  "material.author",
                                  reinterpret_cast<const cy::u8*>(canvas.data()),
                                  canvas.size()};
    const CyServiceEvent authored = submit_and_poll(*api, host, session, author);
    CY_REQUIRE_EQ(authored.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
    CY_REQUIRE(authored.payload_size > 9U);
    const cy::u32 source_size = read_u32(authored.payload + 5);
    CY_REQUIRE_EQ(authored.payload_size, 9U + source_size);
    const std::string_view source(reinterpret_cast<const char*>(authored.payload + 9), source_size);
    CY_CHECK(source.find("material.vertex_output") != std::string_view::npos);

    const CyServiceRequest compile{sizeof(CyServiceRequest),
                                   1,
                                   53,
                                   "material.compile",
                                   reinterpret_cast<const cy::u8*>(canvas.data()),
                                   canvas.size()};
    const CyServiceEvent compiled = submit_and_poll(*api, host, session, compile);
    CY_REQUIRE_EQ(compiled.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
    CY_REQUIRE(compiled.payload_size >= 25U);
    CY_CHECK_NE(read_u64(compiled.payload + 5), 0U);
    api->service_close(&host, session);
}

CY_TEST_CASE("editor_backend: live graph preview validates before updating the authored scene") {
    cy::abi::Host host(allocator());
    AuthoringRuntime runtime;
    cy::editor::MaterialService service(allocator(), nullptr, &runtime);
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);
    const auto canvas = source_canvas();
    std::vector<cy::u8> payload;
    append_text(payload, "materials/live.cygraph");
    append_text(payload, {reinterpret_cast<const char*>(canvas.data()), canvas.size()});
    CyServiceRequest request{
        sizeof(CyServiceRequest), 1, 61, "material.preview.set", payload.data(), payload.size()};
    auto event = submit_and_poll(*api, host, session, request);
    CY_REQUIRE_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
    CY_CHECK_EQ(runtime.asset, "materials/live.cygraph");
    CY_CHECK(runtime.graph.starts_with("cygraph 1"));
    CY_CHECK_EQ(runtime.updates, 1);

    payload.clear();
    append_text(payload, "materials/live.cygraph");
    append_text(payload,
                "cymatcanvas 1\nmaterial broken\nnode 1 material.output\nlink 99 out 1 surface\n");
    request = {
        sizeof(CyServiceRequest), 1, 62, "material.preview.set", payload.data(), payload.size()};
    event = submit_and_poll(*api, host, session, request);
    CY_CHECK_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_FAILED));
    CY_CHECK_EQ(runtime.updates, 1);
    api->service_close(&host, session);
}

CY_TEST_CASE("editor_backend: material preview vertical slice crosses the public ABI") {
    cy::abi::Host host(allocator());
    PublicationRuntime preview_runtime;
    cy::editor::MaterialService service(allocator(), &preview_runtime);
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);
    const cy::Array<cy::u8> graph = source_graph();

    CyServiceRequest request{sizeof(CyServiceRequest), 1, 1, "material.catalogue.get", nullptr, 0};
    CyServiceEvent event = submit_and_poll(*api, host, session, request);
    CY_REQUIRE_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
    CY_REQUIRE(event.payload_size > 12U);

    request = {sizeof(CyServiceRequest), 1, 2, "material.validate", graph.data(), graph.size()};
    event = submit_and_poll(*api, host, session, request);
    CY_REQUIRE_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));

    request = {sizeof(CyServiceRequest), 1, 3, "material.compile", graph.data(), graph.size()};
    event = submit_and_poll(*api, host, session, request);
    CY_REQUIRE_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
    CY_REQUIRE(event.payload_size >= 25U);
    const cy::u64 artefact = read_u64(event.payload + 5);

    request = {sizeof(CyServiceRequest), 1, 4, "preview.create", nullptr, 0};
    event = submit_and_poll(*api, host, session, request);
    CY_REQUIRE_EQ(event.payload_size, 8U);
    cy::u8 preview[8];
    std::memcpy(preview, event.payload, sizeof(preview));

    cy::u8 reload[40] = {};
    std::memcpy(reload, preview, sizeof(preview));
    std::memcpy(reload + sizeof(preview), &artefact, sizeof(artefact));
    const cy::u32 target_count = 1;
    std::memcpy(reload + 16, &target_count, sizeof(target_count));
    const cy::u32 material_slot = 0;
    std::memcpy(reload + 36, &material_slot, sizeof(material_slot));
    request = {sizeof(CyServiceRequest), 1, 5, "preview.reload", reload, sizeof(reload)};
    event = submit_and_poll(*api, host, session, request);
    CY_REQUIRE_EQ(event.payload_size, sizeof(reload));
    CY_CHECK_EQ(read_u64(event.payload), artefact);
    CY_CHECK_EQ(read_u64(event.payload + 8), artefact);

    cy::u8 parameter[22] = {};
    std::memcpy(parameter, preview, sizeof(preview));
    std::memcpy(parameter + 8, &artefact, sizeof(artefact));
    const cy::u32 parameter_id = cy::rendering::parameter_id("live_tint");
    std::memcpy(parameter + 16, &parameter_id, sizeof(parameter_id));
    parameter[20] = 1;
    parameter[21] = 1;
    request = {sizeof(CyServiceRequest),   1,         6,
               "preview.parameter.update", parameter, sizeof(parameter)};
    event = submit_and_poll(*api, host, session, request);
    CY_REQUIRE_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));

    cy::u8 mismatched[29] = {};
    std::memcpy(mismatched, preview, sizeof(preview));
    std::memcpy(mismatched + 8, &artefact, sizeof(artefact));
    std::memcpy(mismatched + 16, &parameter_id, sizeof(parameter_id));
    mismatched[20] = 3;
    request = {sizeof(CyServiceRequest),   1,          7,
               "preview.parameter.update", mismatched, sizeof(mismatched)};
    event = submit_and_poll(*api, host, session, request);
    CY_CHECK_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_FAILED));

    request = {sizeof(CyServiceRequest), 1, 8, "preview.destroy", preview, sizeof(preview)};
    event = submit_and_poll(*api, host, session, request);
    CY_CHECK_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
    api->service_close(&host, session);
}
