// SPDX-License-Identifier: MIT
#include <cy/platform/ios_touch_input_source.h>

#include <cy/core/values/name.h>

namespace cy {

IosTouchInputSource::~IosTouchInputSource() {
    detach();
}

Status IosTouchInputSource::attach(input::InputServer& server, Nanoseconds timestamp) noexcept {
    if (server_ != nullptr) {
        return ok();
    }
    input::DeviceDescription description;
    description.kind = input::DeviceKind::Touch;
    description.hardware_id = Name::intern("ios/main-touchscreen");
    description.display_name = Name::intern("iOS Touchscreen");
    auto connected = server.devices().connect(description, timestamp);
    if (!connected) {
        return make_unexpected(connected.error());
    }
    server_ = &server;
    device_ = *connected;
    return ok();
}

void IosTouchInputSource::detach(Nanoseconds timestamp) noexcept {
    if (server_ != nullptr) {
        server_->devices().disconnect(device_, timestamp);
    }
    server_ = nullptr;
    device_ = {};
}

void IosTouchInputSource::send(input::Control control, f32 value, Nanoseconds timestamp) noexcept {
    if (server_ == nullptr) {
        return;
    }
    input::DeviceEvent event;
    event.timestamp = timestamp;
    event.device = device_;
    event.control = control;
    event.value = value;
    server_->submit(event);
}

void IosTouchInputSource::began(f32 x, f32 y, f32 pressure, Nanoseconds timestamp) noexcept {
    send(input::touch_control(input::TouchControl::PositionX), x, timestamp);
    send(input::touch_control(input::TouchControl::PositionY), y, timestamp);
    send(input::touch_control(input::TouchControl::Pressure), pressure, timestamp);
    send(input::touch_control(input::TouchControl::Primary), 1.0F, timestamp);
}
void IosTouchInputSource::moved(f32 x, f32 y, f32 pressure, Nanoseconds timestamp) noexcept {
    send(input::touch_control(input::TouchControl::PositionX), x, timestamp);
    send(input::touch_control(input::TouchControl::PositionY), y, timestamp);
    send(input::touch_control(input::TouchControl::Pressure), pressure, timestamp);
}
void IosTouchInputSource::ended(f32 x, f32 y, Nanoseconds timestamp) noexcept {
    send(input::touch_control(input::TouchControl::PositionX), x, timestamp);
    send(input::touch_control(input::TouchControl::PositionY), y, timestamp);
    send(input::touch_control(input::TouchControl::Pressure), 0.0F, timestamp);
    send(input::touch_control(input::TouchControl::Primary), 0.0F, timestamp);
}

}  // namespace cy
