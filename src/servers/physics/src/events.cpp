// The contact event buffer. Task 4.2.4.

#include <cy/servers/physics/events.h>

namespace cy::physics {

const char* contact_phase_name(ContactPhase value) noexcept {
    switch (value) {
        case ContactPhase::Enter:
            return "enter";
        case ContactPhase::Stay:
            return "stay";
        case ContactPhase::Exit:
            return "exit";
    }
    return "unknown";
}

bool EventBuffer::push(ContactEvent event) noexcept {
    // The pair order is the interface's, not the broad phase's — see events.h. Swapping here rather
    // than at every producer is what makes the rule true of both backends: a backend cannot forget
    // a rule it never had the chance to apply.
    if (event.b.bits() < event.a.bits()) {
        const BodyHandle body = event.a;
        event.a = event.b;
        event.b = body;
        const UserData data = event.user_data_a;
        event.user_data_a = event.user_data_b;
        event.user_data_b = data;
        for (u32 index = 0; index < event.point_count; ++index) {
            event.points[index].normal = -event.points[index].normal;
        }
    }
    // Never grows: an allocation inside the step is what the reservation exists to prevent, and a
    // dropped event with a count beside it is a diagnosable configuration problem, where a
    // mid-solve allocation is a frame-time spike nobody attributes to physics.
    if (events_.size() >= capacity_) {
        ++dropped_;
        return false;
    }
    return events_.push_back(event).has_value();
}

}  // namespace cy::physics
