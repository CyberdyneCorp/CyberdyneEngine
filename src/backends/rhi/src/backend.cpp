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

Expected<Device*, Error> create_device(Allocator& allocator, const char* requested,
                                       const DeviceDescription& desc,
                                       BackendSelection& selection) noexcept {
    Registry& table = registry();

    selection = BackendSelection{};
    selection.requested = requested != nullptr ? requested : "";

    // The chain, in order: what was asked for, then the first available non-null backend, then the
    // null backend. Resolved under the lock so that a backend registered concurrently either is or
    // is not in the table for the whole decision, rather than for part of it.
    const BackendRegistration* chosen = nullptr;
    const char* reason = "";
    {
        const std::lock_guard<std::mutex> lock(table.mutex);

        if (selection.requested[0] != '\0') {
            for (u32 index = 0; index < table.count; ++index) {
                if (!same_name(table.entries[index].name, selection.requested)) {
                    continue;
                }
                if (available(table.entries[index])) {
                    chosen = &table.entries[index];
                } else {
                    reason = "the requested backend is registered but reports itself unavailable";
                }
                break;
            }
            if (chosen == nullptr && reason[0] == '\0') {
                reason = "the requested backend is not registered in this build";
            }
        }

        if (chosen == nullptr) {
            for (u32 index = 0; index < table.count; ++index) {
                if (!same_name(table.entries[index].name, kNullBackendName) &&
                    available(table.entries[index])) {
                    chosen = &table.entries[index];
                    break;
                }
            }
        }
        if (chosen == nullptr) {
            for (u32 index = 0; index < table.count; ++index) {
                if (same_name(table.entries[index].name, kNullBackendName)) {
                    chosen = &table.entries[index];
                    break;
                }
            }
        }
    }

    if (chosen == nullptr) {
        return fail(ErrorCode::Unavailable,
                    "no RHI backend is registered, not even the null one — link cy::rhi-null");
    }

    Expected<Device*, Error> device = chosen->create(allocator, desc);
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
