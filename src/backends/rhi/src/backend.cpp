// The backend registry and the fallback chain. Task 2.1.3.
//
// Small and fixed-size on purpose: there are four backends on the whole roadmap (null, Vulkan,
// Metal, D3D12) and a test may substitute one. A registry that allocated would be a registry that
// can fail during start-up, which is the one moment a fallback chain must not.

#include <cy/backends/rhi/backend.h>

#include <cy/core/base/assert.h>

#include <cstring>
#include <mutex>

namespace cy::rhi {
namespace {

constexpr u32 kMaxBackends = 8;
/// Devices alive at once. Each one remembers which factory made it, because `destroy_device` must
/// call that factory's destructor and a Device has no field to carry it in — deliberately, so that
/// the interface stays the interface rather than acquiring a registry pointer.
constexpr u32 kMaxLiveDevices = 16;

struct LiveDevice {
    Device* device = nullptr;
    DeviceDestructor destroy = nullptr;
};

struct Registry {
    std::mutex mutex;
    BackendRegistration entries[kMaxBackends] = {};
    u32 count = 0;
    LiveDevice live[kMaxLiveDevices] = {};
};

Registry& registry() noexcept {
    static Registry instance;
    return instance;
}

bool same_name(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    return std::strcmp(a, b) == 0;
}

bool available(const BackendRegistration& entry) noexcept {
    return entry.is_available == nullptr || entry.is_available();
}

/// Why a requested backend is absent from the table. The three real backends are build options,
/// so a request for one this build left out is answered with the option that would put it in —
/// "asked for metal, ran vulkan" is then a configuration answer rather than a mystery.
const char* not_registered_reason(const char* requested) noexcept {
    struct Known {
        const char* name;
        const char* reason;
    };
    static constexpr Known kKnown[] = {
        {"vulkan",
         "the requested backend 'vulkan' is not in this build: it is compiled only when "
         "CY_RENDERER_VULKAN is on"},
        {"metal",
         "the requested backend 'metal' is not in this build: it is compiled only on Apple "
         "platforms and only when CY_RENDERER_METAL is on"},
        {"d3d12",
         "the requested backend 'd3d12' is not in this build: it is compiled only on Windows "
         "and only when CY_RENDERER_D3D12 is on"},
    };
    for (const Known& known : kKnown) {
        if (same_name(known.name, requested)) {
            return known.reason;
        }
    }
    return "the requested backend is not registered in this build";
}

}  // namespace

Status register_backend(const BackendRegistration& registration) noexcept {
    if (registration.name == nullptr || registration.name[0] == '\0' ||
        registration.create == nullptr || registration.destroy == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "a backend registration needs a name, a factory and a destructor");
    }

    Registry& table = registry();
    const std::lock_guard<std::mutex> lock(table.mutex);

    for (u32 index = 0; index < table.count; ++index) {
        if (same_name(table.entries[index].name, registration.name)) {
            table.entries[index] = registration;
            return ok();
        }
    }
    if (table.count == kMaxBackends) {
        return fail(ErrorCode::OutOfRange, "the RHI backend table is full");
    }
    table.entries[table.count] = registration;
    ++table.count;
    return ok();
}

Span<const BackendRegistration> registered_backends() noexcept {
    Registry& table = registry();
    const std::lock_guard<std::mutex> lock(table.mutex);
    return {table.entries, table.count};
}

const BackendRegistration* find_backend(const char* name) noexcept {
    Registry& table = registry();
    const std::lock_guard<std::mutex> lock(table.mutex);
    for (u32 index = 0; index < table.count; ++index) {
        if (same_name(table.entries[index].name, name)) {
            return &table.entries[index];
        }
    }
    return nullptr;
}

namespace {

/// The backends `create_device` will try, in order, resolved under the registry's lock so that a
/// backend registered concurrently either is or is not in the table for the whole decision.
struct Candidates {
    BackendRegistration entries[kMaxBackends] = {};
    u32 count = 0;
    /// Whether entries[0] is the backend the caller named, which is never silently replaced.
    bool first_is_requested = false;
    const char* reason = "";

    void add(const BackendRegistration& entry) noexcept {
        if (count < kMaxBackends) {
            entries[count++] = entry;
        }
    }
};

/// The chain: what was asked for, then every available non-null backend in registration order, then
/// the null backend. A request that is registered and available is the whole chain.
Candidates resolve_candidates(Registry& table, const char* requested) noexcept {
    Candidates out;
    const std::lock_guard<std::mutex> lock(table.mutex);
    if (requested[0] != '\0') {
        const BackendRegistration* named = nullptr;
        for (u32 index = 0; index < table.count && named == nullptr; ++index) {
            if (same_name(table.entries[index].name, requested)) {
                named = &table.entries[index];
            }
        }
        if (named != nullptr && available(*named)) {
            out.add(*named);
            out.first_is_requested = true;
            return out;
        }
        out.reason = named != nullptr
                         ? "the requested backend is registered but reports itself unavailable"
                         : not_registered_reason(requested);
    }
    for (u32 index = 0; index < table.count; ++index) {
        if (!same_name(table.entries[index].name, kNullBackendName) &&
            available(table.entries[index])) {
            out.add(table.entries[index]);
        }
    }
    for (u32 index = 0; index < table.count; ++index) {
        if (same_name(table.entries[index].name, kNullBackendName)) {
            out.add(table.entries[index]);
        }
    }
    return out;
}

}  // namespace

Expected<Device*, Error> create_device(Allocator& allocator, const char* requested,
                                       const DeviceDescription& desc,
                                       BackendSelection& selection) noexcept {
    Registry& table = registry();

    selection = BackendSelection{};
    selection.requested = requested != nullptr ? requested : "";

    Candidates candidates = resolve_candidates(table, selection.requested);
    if (candidates.count == 0) {
        return fail(ErrorCode::Unavailable,
                    "no RHI backend is registered, not even the null one — link cy::rhi-null");
    }

    // A backend that is AVAILABLE can still fail to create a device: `is_available` answers without
    // creating one, and a Vulkan loader with no driver behind it — every hosted CI runner — loads
    // fine and then refuses `vkCreateInstance`. Chosen by default, such a backend is skipped for
    // the next one, ending at the null backend that cannot fail this way. Chosen BY NAME it is not:
    // the caller asked for that backend, and quietly running another one would answer a question
    // about Vulkan with the null device's answer.
    const BackendRegistration* chosen = nullptr;
    Expected<Device*, Error> device =
        fail(ErrorCode::Unavailable, "no RHI backend could create a device");
    const char* reason = candidates.reason;
    for (u32 index = 0; index < candidates.count; ++index) {
        chosen = &candidates.entries[index];
        device = chosen->create(allocator, desc);
        if (device || (index == 0 && candidates.first_is_requested)) {
            break;
        }
        if (reason[0] == '\0') {
            reason =
                "an available backend failed to create a device, so the next one in the "
                "chain was used";
        }
    }
    if (!device) {
        return device;
    }

    selection.selected = chosen->name;
    selection.kind = chosen->kind;
    selection.fell_back = !same_name(chosen->name, selection.requested);
    selection.reason = selection.fell_back ? reason : "";

    const std::lock_guard<std::mutex> lock(table.mutex);
    for (LiveDevice& slot : table.live) {
        if (slot.device == nullptr) {
            slot.device = device.value();
            slot.destroy = chosen->destroy;
            return device;
        }
    }

    // Nowhere to record how to destroy it, so destroying it later would leak or guess. Refuse now,
    // while the caller still has a diagnostic rather than a leak six frames from here.
    chosen->destroy(allocator, device.value());
    return fail(ErrorCode::OutOfRange, "more RHI devices are alive than the registry can track");
}

void destroy_device(Allocator& allocator, Device* device) noexcept {
    if (device == nullptr) {
        return;
    }
    Registry& table = registry();
    DeviceDestructor destructor = nullptr;
    {
        const std::lock_guard<std::mutex> lock(table.mutex);
        for (LiveDevice& slot : table.live) {
            if (slot.device == device) {
                destructor = slot.destroy;
                slot = LiveDevice{};
                break;
            }
        }
    }
    CY_ASSERT_MSG(destructor != nullptr,
                  "destroy_device() on a device this registry did not create");
    if (destructor != nullptr) {
        destructor(allocator, device);
    }
}

}  // namespace cy::rhi
