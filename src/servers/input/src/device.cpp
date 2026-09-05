// Device lifecycle, ownership and routing. Task 4.1.1.

#include <cy/servers/input/device.h>

#include <cy/core/base/assert.h>

namespace cy::input {

const char* disconnect_policy_name(DisconnectPolicy policy) noexcept {
    switch (policy) {
        case DisconnectPolicy::HoldAndAwait:
            return "HoldAndAwait";
        case DisconnectPolicy::ReassignToAnother:
            return "ReassignToAnother";
        case DisconnectPolicy::Pause:
            return "Pause";
    }
    return "HoldAndAwait";
}

const char* keyboard_mouse_policy_name(KeyboardMousePolicy policy) noexcept {
    switch (policy) {
        case KeyboardMousePolicy::Exclusive:
            return "Exclusive";
        case KeyboardMousePolicy::Shared:
            return "Shared";
        case KeyboardMousePolicy::Split:
            return "Split";
    }
    return "Exclusive";
}

const char* device_lifecycle_name(DeviceLifecycle event) noexcept {
    switch (event) {
        case DeviceLifecycle::Connected:
            return "Connected";
        case DeviceLifecycle::Disconnected:
            return "Disconnected";
        case DeviceLifecycle::Reconnected:
            return "Reconnected";
        case DeviceLifecycle::CapabilityChanged:
            return "CapabilityChanged";
        case DeviceLifecycle::BatteryLow:
            return "BatteryLow";
        case DeviceLifecycle::Assigned:
            return "Assigned";
        case DeviceLifecycle::Unassigned:
            return "Unassigned";
        case DeviceLifecycle::Count:
            break;
    }
    return "Connected";
}

DeviceRegistry::DeviceRegistry(Allocator& allocator) noexcept : lifecycle_(allocator) {}

i32 DeviceRegistry::index_of(DeviceId device) const noexcept {
    if (device.is_null()) {
        return -1;
    }
    const u32 slot = device.index();
    if (slot >= count_ || generations_[slot] != device.generation()) {
        return -1;
    }
    return static_cast<i32>(slot);
}

DeviceRecord* DeviceRegistry::find(DeviceId device) noexcept {
    const i32 slot = index_of(device);
    return slot < 0 ? nullptr : &records_[slot];
}

const DeviceRecord* DeviceRegistry::find(DeviceId device) const noexcept {
    const i32 slot = index_of(device);
    return slot < 0 ? nullptr : &records_[slot];
}

void DeviceRegistry::emit(DeviceLifecycle kind, const DeviceRecord& record, u32 user,
                          Nanoseconds timestamp, f32 battery) noexcept {
    DeviceLifecycleEvent event;
    event.kind = kind;
    event.device = record.id;
    event.device_kind = record.description.kind;
    event.user = user;
    event.timestamp = timestamp;
    event.battery = battery;
    // A lifecycle event that could not be recorded is dropped rather than aborting a disconnection:
    // the device state is already correct, and the only loss is the notification.
    (void)lifecycle_.push_back(event);
}

void DeviceRegistry::remember(Name hardware_id, u32 user) noexcept {
    if (hardware_id.is_empty()) {
        return;
    }
    for (u32 index = 0; index < remembered_count_; ++index) {
        if (remembered_[index].hardware_id == hardware_id) {
            remembered_[index].user = user;
            return;
        }
    }
    if (remembered_count_ == kMaxRemembered) {
        // Forget the oldest. See the header: bounded forgetting beats unbounded growth in a
        // registry that lives for the process.
        for (u32 index = 1; index < kMaxRemembered; ++index) {
            remembered_[index - 1] = remembered_[index];
        }
        --remembered_count_;
    }
    remembered_[remembered_count_++] = Remembered{hardware_id, user};
}

u32 DeviceRegistry::recall(Name hardware_id) const noexcept {
    if (hardware_id.is_empty()) {
        return kNoUser;
    }
    for (u32 index = 0; index < remembered_count_; ++index) {
        if (remembered_[index].hardware_id == hardware_id) {
            return remembered_[index].user;
        }
    }
    return kNoUser;
}

Expected<DeviceId, Error> DeviceRegistry::connect(const DeviceDescription& description,
                                                  Nanoseconds timestamp) noexcept {
    // A disconnected slot is reused, with its generation already bumped, so a stale `DeviceId` from
    // the previous occupant resolves to nothing.
    u32 slot = count_;
    for (u32 index = 0; index < count_; ++index) {
        if (!records_[index].connected) {
            slot = index;
            break;
        }
    }
    if (slot == count_) {
        if (count_ == kMaxDevices) {
            return fail(ErrorCode::OutOfRange, "input: no room for another device");
        }
        generations_[slot] = 0;
        ++count_;
    }

    DeviceRecord& record = records_[slot];
    record = DeviceRecord{};
    ++generations_[slot];
    record.id = DeviceId::from_slot(slot, generations_[slot]);
    record.description = description;
    record.connected = true;
    record.user = kNoUser;

    emit(DeviceLifecycle::Connected, record, kNoUser, timestamp, description.battery);

    // Reconnection. The pairing is restored under every policy except `Pause`, which exists
    // precisely so that a game can require an explicit acknowledgement before play resumes.
    const u32 remembered = recall(description.hardware_id);
    if (remembered != kNoUser && disconnect_policy_ != DisconnectPolicy::Pause) {
        record.user = remembered;
        emit(DeviceLifecycle::Reconnected, record, remembered, timestamp, description.battery);
    }
    return record.id;
}

void DeviceRegistry::reassign_after_disconnect(u32 user, DeviceKind kind,
                                               Nanoseconds timestamp) noexcept {
    for (u32 index = 0; index < count_; ++index) {
        DeviceRecord& candidate = records_[index];
        if (candidate.connected && candidate.user == kNoUser &&
            candidate.description.kind == kind) {
            candidate.user = user;
            remember(candidate.description.hardware_id, user);
            emit(DeviceLifecycle::Assigned, candidate, user, timestamp, -1.0F);
            return;
        }
    }
}

void DeviceRegistry::disconnect(DeviceId device, Nanoseconds timestamp) noexcept {
    DeviceRecord* record = find(device);
    if (record == nullptr || !record->connected) {
        return;
    }
    const u32 user = record->user;
    const DeviceKind kind = record->description.kind;
    // The pairing is remembered *before* the record is cleared, which is what makes a flat battery
    // a pause rather than a player swap when the controller comes back.
    remember(record->description.hardware_id, user);
    record->connected = false;
    record->user = kNoUser;
    for (f32& control : record->controls) {
        control = 0.0F;
    }
    emit(DeviceLifecycle::Disconnected, *record, user, timestamp, -1.0F);
    // The slot's generation is bumped here rather than at the next connect, so a stale id is dead
    // the moment the device is gone.
    ++generations_[device.index()];

    if (user != kNoUser && disconnect_policy_ == DisconnectPolicy::ReassignToAnother) {
        reassign_after_disconnect(user, kind, timestamp);
    }
}

Status DeviceRegistry::set_capabilities(DeviceId device, u16 capabilities,
                                        Nanoseconds timestamp) noexcept {
    DeviceRecord* record = find(device);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "input: no such device");
    }
    if (record->description.capabilities == capabilities) {
        return ok();
    }
    record->description.capabilities = capabilities;
    emit(DeviceLifecycle::CapabilityChanged, *record, record->user, timestamp, -1.0F);
    return ok();
}

Status DeviceRegistry::set_battery(DeviceId device, f32 battery, Nanoseconds timestamp) noexcept {
    DeviceRecord* record = find(device);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "input: no such device");
    }
    const f32 previous = record->description.battery;
    record->description.battery = battery;
    // On the crossing, once. A report every frame while the battery sits at 0.1 would be a
    // notification the interface has to de-duplicate, which is the wrong place for the logic.
    const bool crossed = previous > low_battery_ && battery <= low_battery_;
    if (crossed) {
        emit(DeviceLifecycle::BatteryLow, *record, record->user, timestamp, battery);
    }
    return ok();
}

Status DeviceRegistry::assign(DeviceId device, u32 user, Nanoseconds timestamp) noexcept {
    DeviceRecord* record = find(device);
    if (record == nullptr || !record->connected) {
        return fail(ErrorCode::NotFound, "input: no such device");
    }
    if (record->user != kNoUser && record->user != user) {
        return fail(ErrorCode::AlreadyExists,
                    "input: that device already belongs to another user; sharing a keyboard or a "
                    "mouse is KeyboardMousePolicy::Shared, not two owners of one record");
    }
    record->user = user;
    remember(record->description.hardware_id, user);
    emit(DeviceLifecycle::Assigned, *record, user, timestamp, -1.0F);
    return ok();
}

void DeviceRegistry::unassign(DeviceId device, Nanoseconds timestamp) noexcept {
    DeviceRecord* record = find(device);
    if (record == nullptr || record->user == kNoUser) {
        return;
    }
    const u32 user = record->user;
    record->user = kNoUser;
    // The remembered pairing is deliberately *kept*: an explicit unassignment is a change of
    // assignment, not a statement that the device never belonged to anyone. A caller that wants the
    // pairing forgotten assigns the device elsewhere, which overwrites it.
    emit(DeviceLifecycle::Unassigned, *record, user, timestamp, -1.0F);
}

u32 DeviceRegistry::devices_of(u32 user, DeviceId* out, u32 capacity) const noexcept {
    u32 found = 0;
    for (u32 index = 0; index < count_; ++index) {
        if (records_[index].connected && records_[index].user == user) {
            if (found < capacity) {
                out[found] = records_[index].id;
            }
            ++found;
        }
    }
    return found;
}

f32 DeviceRegistry::control_value(DeviceId device, Control control) const noexcept {
    const DeviceRecord* record = find(device);
    if (record == nullptr || control.code >= kMaxControlCode) {
        return 0.0F;
    }
    if (record->description.kind != control.kind) {
        return 0.0F;
    }
    return record->controls[control.code];
}

void DeviceRegistry::apply(const DeviceEvent& event) noexcept {
    DeviceRecord* record = find(event.device);
    if (record == nullptr || event.control.code >= kMaxControlCode) {
        return;
    }
    // A delta accumulates within the window and is cleared by the resolver at the end of the tick;
    // everything else is a level. Distinguishing them here rather than at the binding is what lets
    // two bindings read one mouse delta and both see the whole motion.
    const bool is_delta = event.control.kind == DeviceKind::Mouse &&
                          (event.control.code == static_cast<u16>(MouseControl::MoveX) ||
                           event.control.code == static_cast<u16>(MouseControl::MoveY) ||
                           event.control.code == static_cast<u16>(MouseControl::Wheel) ||
                           event.control.code == static_cast<u16>(MouseControl::WheelX));
    if (is_delta) {
        record->controls[event.control.code] += event.value;
    } else {
        record->controls[event.control.code] = event.value;
    }
}

void DeviceRegistry::clear_deltas() noexcept {
    for (u32 index = 0; index < count_; ++index) {
        DeviceRecord& record = records_[index];
        if (record.description.kind != DeviceKind::Mouse) {
            continue;
        }
        record.controls[static_cast<u16>(MouseControl::MoveX)] = 0.0F;
        record.controls[static_cast<u16>(MouseControl::MoveY)] = 0.0F;
        record.controls[static_cast<u16>(MouseControl::Wheel)] = 0.0F;
        record.controls[static_cast<u16>(MouseControl::WheelX)] = 0.0F;
    }
}

void DeviceRegistry::set_keyboard_mouse_policy(KeyboardMousePolicy policy, u32 keyboard_user,
                                               u32 mouse_user) noexcept {
    keyboard_mouse_policy_ = policy;
    keyboard_user_ = keyboard_user;
    mouse_user_ = mouse_user;
}

u32 DeviceRegistry::route(DeviceId device, bool& shared) const noexcept {
    shared = false;
    const DeviceRecord* record = find(device);
    if (record == nullptr || !record->connected) {
        return kNoUser;
    }
    const DeviceKind kind = record->description.kind;
    if (kind != DeviceKind::Keyboard && kind != DeviceKind::Mouse) {
        return record->user;
    }
    switch (keyboard_mouse_policy_) {
        case KeyboardMousePolicy::Exclusive:
            // An explicit assignment still wins: a game that assigned the keyboard to player two
            // meant it. The policy decides only the *unassigned* case, which is the assumption the
            // requirement forbids making implicitly.
            return record->user != kNoUser ? record->user : keyboard_user_;
        case KeyboardMousePolicy::Shared:
            shared = true;
            return kNoUser;
        case KeyboardMousePolicy::Split:
            return kind == DeviceKind::Keyboard ? keyboard_user_ : mouse_user_;
    }
    return record->user;
}

}  // namespace cy::input
