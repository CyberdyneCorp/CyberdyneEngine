// SPDX-License-Identifier: MIT
#pragma once

#include <cy/servers/input/server.h>

namespace cy {

/// Converts UIKit touch callbacks into normalised, timestamped engine input events.
class IosTouchInputSource final {
public:
    ~IosTouchInputSource();
    Status attach(input::InputServer& server, Nanoseconds timestamp) noexcept;
    void detach(Nanoseconds timestamp = 0) noexcept;
    void began(f32 x, f32 y, f32 pressure, Nanoseconds timestamp) noexcept;
    void moved(f32 x, f32 y, f32 pressure, Nanoseconds timestamp) noexcept;
    void ended(f32 x, f32 y, Nanoseconds timestamp) noexcept;
    [[nodiscard]] input::DeviceId device() const noexcept { return device_; }

private:
    void send(input::Control control, f32 value, Nanoseconds timestamp) noexcept;
    input::InputServer* server_ = nullptr;
    input::DeviceId device_{};
};

}  // namespace cy
