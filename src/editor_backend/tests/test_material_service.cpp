// SPDX-License-Identifier: MIT
#include <cy/abi/cy_abi.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/editor/material_service.h>
#include <cy/graph/cybergraph.h>
#include <cy/test/test.h>
#if defined(CY_EDITOR_HAS_VFX)
#    include <cy/vfx/authoring.h>
#    include <cy/vfx/asset.h>
#    include <cy/vfx/interfaces.h>
#    include <cy/vfx/renderers.h>
#endif

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

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

#if defined(CY_EDITOR_HAS_VFX)
void append_u32(std::vector<cy::u8>& bytes, cy::u32 value) {
    for (cy::u32 index = 0; index < 4; ++index) {
        bytes.push_back(static_cast<cy::u8>((value >> (index * 8)) & 0xffU));
    }
}

void append_text(std::vector<cy::u8>& bytes, std::string_view value) {
    append_u32(bytes, static_cast<cy::u32>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

std::string vfx_document(std::string_view node_type = "vfx.constant", cy::u32 version = 2) {
    std::vector<cy::u8> bytes;
    append_u32(bytes, version);
    append_text(bytes, "sparks");
    append_u32(bytes, 2);
    for (const char* name : {"cpu", "gpu"}) {
        append_text(bytes, name);
        bytes.push_back(name[0] == 'c' ? 1 : 0);
        append_text(bytes, "Sprite");
        append_u32(bytes, 1);
        bytes.push_back(0);  // Spawn
        const std::string canvas = std::string("cyvfxcanvas 1\nemitter ") + name + "\nnode 1 " +
                                   std::string(node_type) +
                                   "\n# layout 1 12 34\nprop 1 value 3\nnode 2 vfx.spawn_count\n"
                                   "link 1 out 2 value\n";
        append_text(bytes, canvas);
        append_u32(bytes, 0);     // modules
        append_u32(bytes, 0);     // interfaces
        if (version >= 2) {
            append_u32(bytes, 2048);  // capacity
            append_u32(bytes, 1);     // attributes
            append_text(bytes, "position");
            append_text(bytes, "vec3");
            append_u32(bytes, 0);  // minimum
            append_u32(bytes, 0x42c80000U);  // maximum 100
            append_u32(bytes, 0);  // tolerance
            append_text(bytes, "Auto");
        }
    }
    append_u32(bytes, 0);  // parameters
    if (version >= 2) {
        append_u32(bytes, 1);  // channels
        append_text(bytes, "on_death");
        append_u32(bytes, 128);
        append_u32(bytes, 2);
        bytes.push_back(0);
    }
    std::string source = "cyvfxdoc 1\n";
    constexpr char hex[] = "0123456789abcdef";
    for (const cy::u8 byte : bytes) {
        source.push_back(hex[byte >> 4U]);
        source.push_back(hex[byte & 0x0fU]);
    }
    return source;
}

CY_TEST_CASE("editor_backend: VFX document compiles through the engine service") {
    const std::string source = vfx_document();
    auto asset = cy::vfx::read_authoring_document(source, allocator());
    CY_REQUIRE(asset.has_value());
    CY_REQUIRE_EQ(asset->emitters().size(), 2U);
    CY_CHECK_EQ(asset->emitters()[0].capacity(), 2048U);
    CY_CHECK_EQ(asset->emitters()[0].attributes().size(), 1U);
    CY_CHECK_EQ(asset->channels().size(), 1U);
    CY_CHECK_EQ(asset->emitters()[0].stage(cy::vfx::Stage::Spawn)->layout(1)->x, 12.0F);

    cy::abi::Host host(allocator());
    cy::editor::MaterialService service(allocator());
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(api != nullptr);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);
    const CyServiceRequest request{sizeof(CyServiceRequest), 1, 4, "vfx.compile",
                                   reinterpret_cast<const cy::u8*>(source.data()), source.size()};
    const CyServiceEvent event = submit_and_poll(*api, host, session, request);
    CY_REQUIRE_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
    cy::usize cursor = 0;
    CY_REQUIRE_EQ(read_u32(event.payload + cursor), 1U);
    cursor += 4;
    CY_CHECK_NE(read_u64(event.payload + cursor), 0U);
    cursor += 8;
    CY_CHECK_EQ(read_u32(event.payload + cursor), 2U);
    cursor += 4;
    cursor += 4;  // bytes per particle
    CY_REQUIRE_EQ(read_u32(event.payload + cursor), 2U);
    cursor += 4;
    for (const char* name : {"cpu", "gpu"}) {
        CY_CHECK_EQ(read_text(event.payload, event.payload_size, cursor), name);
        CY_CHECK_EQ(event.payload[cursor++], name[0] == 'c' ? 1U : 0U);
        CY_CHECK_EQ(read_u32(event.payload + cursor), 1U);
        cursor += 4;
        cursor += 4 * 2 + 8 + 4;  // size, population, cost, folded constants
        const cy::u32 slots = read_u32(event.payload + cursor);
        cursor += 4;
        CY_CHECK_EQ(slots, 0U);  // Unused declaration has no storage slot.
        for (cy::u32 index = 0; index < slots; ++index) {
            CY_CHECK_EQ(read_text(event.payload, event.payload_size, cursor), "position");
            (void)read_text(event.payload, event.payload_size, cursor);
            cursor += 1 + 4 + 4 + 1;
        }
        CY_REQUIRE_EQ(read_u32(event.payload + cursor), 1U);
        cursor += 4;
        CY_CHECK_FALSE(read_text(event.payload, event.payload_size, cursor).empty());
    }
    CY_CHECK_EQ(cursor, event.payload_size);
    api->service_close(&host, session);
}

CY_TEST_CASE("editor_backend: engine reader upgrades old drafts and refuses corrupt sources") {
    auto old = cy::vfx::read_authoring_document(vfx_document("vfx.constant", 1), allocator());
    CY_REQUIRE(old.has_value());
    CY_REQUIRE_EQ(old->emitters().size(), 2U);
    CY_CHECK_EQ(old->emitters()[0].capacity(), 1024U);
    CY_CHECK(old->channels().empty());

    std::string truncated = vfx_document();
    truncated.resize(truncated.size() - 2);
    CY_CHECK_FALSE(cy::vfx::read_authoring_document(truncated, allocator()).has_value());
    std::string corrupt = vfx_document();
    corrupt.back() = 'z';
    CY_CHECK_FALSE(cy::vfx::read_authoring_document(corrupt, allocator()).has_value());
}

CY_TEST_CASE("editor_backend: VFX compiler diagnostics name the authored node") {
    const std::string source = vfx_document("vfx.unknown");
    cy::abi::Host host(allocator());
    cy::editor::MaterialService service(allocator());
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(api != nullptr);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);
    const CyServiceRequest request{sizeof(CyServiceRequest), 1, 5, "vfx.compile",
                                   reinterpret_cast<const cy::u8*>(source.data()), source.size()};
    const CyServiceEvent event = submit_and_poll(*api, host, session, request);
    CY_REQUIRE_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_FAILED));
    cy::usize cursor = 0;
    CY_REQUIRE_EQ(read_u32(event.payload + cursor), 1U);
    cursor += 4;
    CY_CHECK_EQ(read_text(event.payload, event.payload_size, cursor), "vfx.compile");
    (void)read_text(event.payload, event.payload_size, cursor);
    CY_REQUIRE(read_u32(event.payload + cursor) > 0U);
    cursor += 4;
    cursor += 1;  // severity
    CY_CHECK_FALSE(read_text(event.payload, event.payload_size, cursor).empty());
    (void)read_text(event.payload, event.payload_size, cursor);
    (void)read_text(event.payload, event.payload_size, cursor);
    CY_CHECK_EQ(read_u64(event.payload + cursor), 1U);
    api->service_close(&host, session);
}

CY_TEST_CASE("editor_backend: VFX palette equals the compiler registry") {
    cy::abi::Host host(allocator());
    cy::editor::MaterialService service(allocator());
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(api != nullptr);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);
    const CyServiceRequest request{sizeof(CyServiceRequest), 1, 2, "vfx.catalogue.get", nullptr, 0};
    const CyServiceEvent event = submit_and_poll(*api, host, session, request);
    CY_REQUIRE_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
    CY_REQUIRE(event.payload_size >= 12U);
    CY_CHECK_EQ(read_u32(event.payload), 2U);
    CY_CHECK_EQ(read_u32(event.payload + 4), 2U);

    cy::graph::NodeRegistry registry(allocator());
    CY_REQUIRE(cy::vfx::register_vfx_nodes(registry).has_value());
    cy::vfx::DataInterfaceRegistry interfaces(allocator());
    CY_REQUIRE(cy::vfx::register_builtin_interfaces(interfaces).has_value());
    CY_REQUIRE_EQ(read_u32(event.payload + 8), registry.size());
    cy::usize cursor = 12;
    for (const cy::graph::NodeType& node : registry.types()) {
        CY_REQUIRE(cursor + 12 <= event.payload_size);
        CY_CHECK_EQ(read_u32(event.payload + cursor), node.identity());
        cursor += 4;
        CY_CHECK_EQ(read_u32(event.payload + cursor), node.version());
        cursor += 4;
        CY_CHECK_EQ(read_text(event.payload, event.payload_size, cursor), node.name().text());
        CY_REQUIRE(cursor + 4 <= event.payload_size);
        CY_REQUIRE_EQ(read_u32(event.payload + cursor), node.pins().size());
        cursor += 4;
        for (const cy::graph::PinDesc& pin : node.pins()) {
            CY_REQUIRE(cursor + 5 <= event.payload_size);
            CY_CHECK_EQ(read_u32(event.payload + cursor), pin.identity);
            cursor += 4;
            CY_CHECK_EQ(event.payload[cursor++], static_cast<cy::u8>(pin.direction));
            CY_CHECK_EQ(read_text(event.payload, event.payload_size, cursor), pin.name.text());
            CY_CHECK_EQ(read_text(event.payload, event.payload_size, cursor), pin.type.text());
        }
        CY_REQUIRE(cursor + 4 <= event.payload_size);
        const cy::u32 property_count = read_u32(event.payload + cursor);
        cursor += 4;
        for (cy::u32 property = 0; property < property_count; ++property) {
            CY_REQUIRE(cursor + 5 <= event.payload_size);
            const cy::u32 identity = read_u32(event.payload + cursor);
            cursor += 4;
            const cy::u8 kind = event.payload[cursor++];
            const std::string_view name = read_text(event.payload, event.payload_size, cursor);
            (void)read_text(event.payload, event.payload_size, cursor);  // default
            (void)read_text(event.payload, event.payload_size, cursor);  // tooltip
            (void)read_text(event.payload, event.payload_size, cursor);  // semantic
            (void)read_text(event.payload, event.payload_size, cursor);  // asset kind
            CY_REQUIRE(cursor + 4 <= event.payload_size);
            const cy::u32 choice_count = read_u32(event.payload + cursor);
            cursor += 4;
            if (node.name().text() == "vfx.sample" && name == "interface") {
                CY_CHECK_EQ(identity, 1U);
                CY_CHECK_EQ(kind, 4U);
                CY_REQUIRE_EQ(choice_count, interfaces.size());
                for (const cy::vfx::DataInterface& interface : interfaces.all()) {
                    CY_CHECK_EQ(read_text(event.payload, event.payload_size, cursor),
                                interface.name().text());
                }
            } else {
                for (cy::u32 choice = 0; choice < choice_count; ++choice) {
                    (void)read_text(event.payload, event.payload_size, cursor);
                }
            }
            CY_CHECK_EQ(read_text(event.payload, event.payload_size, cursor), "compile");
            CY_CHECK_EQ(read_text(event.payload, event.payload_size, cursor), "vfx");
            CY_REQUIRE(cursor + 34 <= event.payload_size);
            CY_CHECK_EQ(read_u64(event.payload + cursor), static_cast<cy::u64>(node.required()));
            cursor += 34;  // capabilities, lanes, flags, three optional doubles
        }
        if (node.name().text() == "vfx.sample") {
            CY_CHECK_EQ(property_count, 2U);
        }
    }
    CY_CHECK_EQ(cursor, event.payload_size);
    api->service_close(&host, session);
}

CY_TEST_CASE("editor_backend: VFX renderer and target availability comes from runtime owners") {
    cy::abi::Host host(allocator());
    cy::editor::MaterialService service(allocator());
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(api != nullptr);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);
    const CyServiceRequest request{sizeof(CyServiceRequest), 1, 3,
                                   "vfx.authoring-capabilities.get", nullptr, 0};
    const CyServiceEvent event = submit_and_poll(*api, host, session, request);
    CY_REQUIRE_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
    CY_REQUIRE(event.payload_size >= 8U);
    CY_CHECK_EQ(read_u32(event.payload), 1U);
    CY_REQUIRE_EQ(read_u32(event.payload + 4), cy::vfx::kRendererKindCount);
    cy::usize cursor = 8;
    for (cy::u32 index = 0; index < cy::vfx::kRendererKindCount; ++index) {
        CY_REQUIRE(cursor < event.payload_size);
        CY_CHECK_EQ(event.payload[cursor++], static_cast<cy::u8>(index));
        const auto kind = static_cast<cy::vfx::RendererKind>(index);
        CY_CHECK_EQ(read_text(event.payload, event.payload_size, cursor),
                    std::string_view(cy::vfx::renderer_kind_name(kind)));
        CY_REQUIRE(cursor < event.payload_size);
        const bool available = event.payload[cursor++] != 0;
        const auto owned = cy::vfx::renderer_availability(kind);
        CY_CHECK_EQ(available, owned.available);
        CY_CHECK_EQ(read_text(event.payload, event.payload_size, cursor),
                    std::string_view(owned.reason));
        CY_CHECK_EQ(available, index < 5U);
    }
    CY_REQUIRE(cursor + 4 <= event.payload_size);
    CY_REQUIRE_EQ(read_u32(event.payload + cursor), 2U);
    cursor += 4;
    for (const cy::vfx::SimulationPath path : {cy::vfx::SimulationPath::GpuPreferred,
                                               cy::vfx::SimulationPath::CpuRequired}) {
        CY_REQUIRE(cursor < event.payload_size);
        CY_CHECK_EQ(event.payload[cursor++], static_cast<cy::u8>(path));
        CY_CHECK_EQ(read_text(event.payload, event.payload_size, cursor),
                    std::string_view(cy::vfx::path_name(path)));
        const auto owned = cy::vfx::target_availability(path, nullptr);
        CY_REQUIRE(cursor + 2 <= event.payload_size);
        CY_CHECK_EQ(event.payload[cursor++] != 0, owned.compile_available);
        CY_CHECK_EQ(event.payload[cursor++] != 0, owned.runtime_available);
        CY_CHECK_EQ(read_text(event.payload, event.payload_size, cursor),
                    std::string_view(cy::vfx::fallback_reason_name(owned.reason)));
        CY_CHECK_EQ(read_text(event.payload, event.payload_size, cursor),
                    std::string_view(owned.explanation));
    }
    CY_CHECK_EQ(cursor, event.payload_size);
    api->service_close(&host, session);
}
#endif

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
#if defined(CY_EDITOR_HAS_VFX)
    CY_CHECK_EQ(read_u32(event.payload + 4), 12U);
#else
    CY_CHECK_EQ(read_u32(event.payload + 4), 9U);
#endif

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
    const cy::u32 parameter_id = cy::rendering::parameter_id("live_tint");
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
