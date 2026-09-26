// SPDX-License-Identifier: MIT
#include <cy/abi/cy_abi.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/editor/material_service.h>
#include <cy/test/test.h>

#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Scripting);
}

std::string read_source(std::ifstream& input) {
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (length <= 0) {
        return {};
    }
    std::string source(static_cast<std::size_t>(length), '\0');
    input.seekg(0, std::ios::beg);
    return input.read(source.data(), static_cast<std::streamsize>(source.size())) ? source
                                                                                  : std::string{};
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

cy::f32 read_f32(const cy::u8* bytes) noexcept {
    const cy::u32 bits = read_u32(bytes);
    cy::f32 value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
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

void append_u32(std::vector<cy::u8>& bytes, cy::u32 value) {
    for (cy::u32 index = 0; index < 4; ++index) {
        bytes.push_back(static_cast<cy::u8>((value >> (index * 8)) & 0xffU));
    }
}

void append_f32(std::vector<cy::u8>& bytes, cy::f32 value) {
    cy::u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u32(bytes, bits);
}

void append_text(std::vector<cy::u8>& bytes, std::string_view value) {
    append_u32(bytes, static_cast<cy::u32>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
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

}  // namespace

struct VfxPreviewSnapshot {
    cy::u64 cook_key = 0;
    bool playing = false;
    cy::f32 time = 0.0F;
    cy::f32 scale = 0.0F;
    cy::u32 live = 0;
    cy::u32 spawned = 0;
    cy::u32 fallbacks = 0;
    cy::u32 emitter_live[2] = {};
    cy::u32 pool_shortfall = 0;
    cy::u32 events_raised = 0;
    bool has_sample = false;
    cy::u32 sample_emitter = 0;
    cy::u32 sample_attribute_count = 0;
    bool sampled_position = false;
};

VfxPreviewSnapshot preview_snapshot(const CyServiceEvent& event) {
    CY_REQUIRE_EQ(event.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_COMPLETED));
    CY_REQUIRE(event.payload_size >= 65U);
    const cy::u8* bytes = event.payload;
    CY_REQUIRE_EQ(read_u32(bytes), 2U);
    VfxPreviewSnapshot snapshot;
    snapshot.cook_key = read_u64(bytes + 4);
    snapshot.playing = bytes[12] != 0;
    snapshot.time = read_f32(bytes + 13);
    snapshot.scale = read_f32(bytes + 17);
    snapshot.live = read_u32(bytes + 21);
    snapshot.spawned = read_u32(bytes + 25);
    snapshot.fallbacks = read_u32(bytes + 33);
    CY_CHECK_GT(read_u64(bytes + 45), 0U);  // finite pool budget
    CY_REQUIRE_EQ(read_u32(bytes + 61), 2U);
    cy::usize cursor = 65;
    for (cy::u32 index = 0; index < 2; ++index) {
        CY_CHECK_EQ(read_text(bytes, event.payload_size, cursor),
                    index == 0 ? "CpuEmitter" : "GpuEmitter");
        CY_REQUIRE(cursor + 4 <= event.payload_size);
        snapshot.emitter_live[index] = read_u32(bytes + cursor);
        cursor += 4;
    }
    CY_REQUIRE(cursor + 21 <= event.payload_size);
    snapshot.pool_shortfall = read_u32(bytes + cursor);
    cursor += 8;  // pool shortfall and reduced requests
    snapshot.events_raised = read_u32(bytes + cursor);
    cursor += 12;  // raised, delivered, deferred
    snapshot.has_sample = bytes[cursor++] != 0;
    if (snapshot.has_sample) {
        CY_REQUIRE(cursor + 12 <= event.payload_size);
        snapshot.sample_emitter = read_u32(bytes + cursor);
        cursor += 8;  // emitter and particle slot
        snapshot.sample_attribute_count = read_u32(bytes + cursor);
        cursor += 4;
        CY_REQUIRE(snapshot.sample_attribute_count <= 32U);
        for (cy::u32 index = 0; index < snapshot.sample_attribute_count; ++index) {
            const std::string_view name = read_text(bytes, event.payload_size, cursor);
            CY_REQUIRE(cursor < event.payload_size);
            const cy::u8 components = bytes[cursor++];
            CY_REQUIRE(components >= 1);
            CY_REQUIRE(components <= 4);
            CY_REQUIRE(cursor + (static_cast<cy::usize>(components) * 4U) <= event.payload_size);
            if (name == "position") {
                snapshot.sampled_position = true;
                CY_CHECK_EQ(components, 3U);
            }
            cursor += static_cast<cy::usize>(components) * 4U;
        }
    }
    CY_CHECK_EQ(cursor, event.payload_size);
    return snapshot;
}

CY_TEST_CASE("editor_backend: VFX preview controls and live parameters use the engine world") {
    const std::string path =
        std::string(CY_SOURCE_DIR) +
        "/samples/05b-editor-window/project/effects/issue15_two_emitters.cyvfxdoc";
    std::ifstream input(path);
    CY_REQUIRE(input.good());
    const std::string source = read_source(input);
    CY_REQUIRE_FALSE(source.empty());
    cy::abi::Host host(allocator());
    cy::editor::MaterialService service(allocator());
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(api != nullptr);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);
    CY_CHECK_EQ(service.vfx_preview_world(session), nullptr);
    cy::u64 request_id = 1;
    const auto call = [&](const char* operation, const std::vector<cy::u8>& payload) {
        const CyServiceRequest request{
            sizeof(CyServiceRequest), 1, request_id++, operation, payload.data(), payload.size()};
        return submit_and_poll(*api, host, session, request);
    };

    const CyServiceEvent empty = call("vfx.preview.state", {});
    CY_CHECK_EQ(empty.kind, static_cast<cy::u32>(CY_SERVICE_EVENT_FAILED));
    const std::vector<cy::u8> authored(source.begin(), source.end());
    const VfxPreviewSnapshot loaded = preview_snapshot(call("vfx.preview.load", authored));
    CY_REQUIRE(service.vfx_preview_world(session) != nullptr);
    CY_CHECK_NE(loaded.cook_key, 0U);
    CY_CHECK_FALSE(loaded.playing);
    CY_CHECK_EQ(loaded.live, 0U);

    const VfxPreviewSnapshot playing = preview_snapshot(call("vfx.preview.control", {0}));
    CY_CHECK(playing.playing);
    std::vector<cy::u8> frame;
    append_f32(frame, 1.0F / 30.0F);
    const VfxPreviewSnapshot first = preview_snapshot(call("vfx.preview.step", frame));
    CY_CHECK_EQ(first.cook_key, loaded.cook_key);
    CY_CHECK_EQ(first.spawned, 4U);
    CY_CHECK_EQ(first.fallbacks, 1U);
    CY_CHECK_EQ(first.emitter_live[0], 2U);
    CY_CHECK_EQ(first.emitter_live[1], 2U);
    CY_CHECK(first.has_sample);
    CY_CHECK_EQ(first.sample_emitter, 0U);
    CY_CHECK_GT(first.sample_attribute_count, 0U);
    CY_CHECK(first.sampled_position);
    CY_CHECK_EQ(first.pool_shortfall, 0U);

    std::vector<cy::u8> parameter;
    append_text(parameter, "speed");
    parameter.push_back(1);
    append_f32(parameter, 4.0F);
    const VfxPreviewSnapshot updated =
        preview_snapshot(call("vfx.preview.parameter.update", parameter));
    CY_CHECK_EQ(updated.cook_key, first.cook_key);
    const VfxPreviewSnapshot second = preview_snapshot(call("vfx.preview.step", frame));
    CY_CHECK_EQ(second.spawned, 8U);
    CY_CHECK_EQ(second.emitter_live[0], 6U);
    CY_CHECK_EQ(second.emitter_live[1], 6U);

    const VfxPreviewSnapshot paused = preview_snapshot(call("vfx.preview.control", {1}));
    CY_CHECK_FALSE(paused.playing);
    const VfxPreviewSnapshot held = preview_snapshot(call("vfx.preview.step", frame));
    CY_CHECK_EQ(held.time, paused.time);
    CY_CHECK_EQ(held.live, paused.live);

    std::vector<cy::u8> scale{4};
    append_f32(scale, 2.0F);
    const VfxPreviewSnapshot faster = preview_snapshot(call("vfx.preview.control", scale));
    CY_CHECK_EQ(faster.scale, 2.0F);
    const VfxPreviewSnapshot restarted = preview_snapshot(call("vfx.preview.control", {2}));
    CY_CHECK_EQ(restarted.time, 0.0F);
    CY_CHECK_EQ(restarted.live, 0U);
    std::vector<cy::u8> seek{3};
    append_f32(seek, 0.1F);
    const VfxPreviewSnapshot scrubbed = preview_snapshot(call("vfx.preview.control", seek));
    CY_CHECK_EQ(scrubbed.time, 0.1F);
    CY_CHECK_GT(scrubbed.live, 0U);
    CY_CHECK_FALSE(scrubbed.playing);
    api->service_close(&host, session);
}
