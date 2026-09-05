// The input server: the accumulation window, routing, and the per-tick resolution. Tasks 4.1.1 to
// 4.1.6.

#include <cy/servers/input/server.h>

#include <cy/core/base/assert.h>
#include <cy/core/memory/allocator.h>

#include <utility>

namespace cy::input {
namespace {

/// `allocate` + `construct_at`, and its inverse. Two functions rather than the pattern written out
/// four times, and local because `core-memory` deliberately offers no `new`-shaped helper — an
/// engine-wide one would be the allocation nobody notices.
template <class T, class... Args>
[[nodiscard]] T* make(Allocator& allocator, Args&&... args) noexcept {
    void* storage = allocator.allocate(sizeof(T), alignof(T));
    if (storage == nullptr) {
        return nullptr;
    }
    return construct_at<T>(storage, std::forward<Args>(args)...);
}

template <class T>
void unmake(Allocator& allocator, T* object) noexcept {
    if (object == nullptr) {
        return;
    }
    object->~T();
    allocator.deallocate(object, sizeof(T), alignof(T));
}

}  // namespace

InputServer::InputServer(Allocator& allocator) noexcept
    : allocator_(&allocator),
      actions_(allocator),
      devices_(allocator),
      events_(allocator),
      text_(allocator),
      contexts_(allocator),
      users_(allocator) {}

InputServer::~InputServer() {
    shutdown();
}

void InputServer::set_backend(const char* name, bool is_null) noexcept {
    backend_name_ = name != nullptr ? name : "null";
    null_backend_ = is_null;
}

Status InputServer::configure(const InputServerConfig& config) noexcept {
    if (initialised_) {
        return fail(ErrorCode::Unavailable,
                    "input: configure() before initialize(); the users and the accumulation window "
                    "are allocated there");
    }
    if (config.users == 0) {
        return fail(ErrorCode::InvalidArgument, "input: a process has at least one input user");
    }
    config_ = config;
    return ok();
}

Status InputServer::initialize() noexcept {
    if (initialised_) {
        return ok();
    }
    // NOTHING HERE ASKS FOR A DEVICE. `input-and-actions` — "Absence of a device backend SHALL NOT
    // prevent the system from initialising": a dedicated server, a replay and a test harness all
    // bring this up and then feed it synthetic events. See the header.
    if (Status reserved = events_.reserve(config_.event_capacity); !reserved) {
        return reserved;
    }
    if (Status reserved = users_.reserve(config_.users); !reserved) {
        return reserved;
    }
    for (u32 index = 0; index < config_.users; ++index) {
        auto* created = make<InputUser>(*allocator_, *allocator_, index);
        if (created == nullptr) {
            return fail(ErrorCode::OutOfMemory, "input: could not allocate an input user");
        }
        if (Status pushed = users_.push_back(created); !pushed) {
            unmake(*allocator_, created);
            return pushed;
        }
    }
    initialised_ = true;
    return ok();
}

void InputServer::shutdown() noexcept {
    for (auto& user : users_) {
        unmake(*allocator_, user);
    }
    users_.clear();
    for (auto& context : contexts_) {
        unmake(*allocator_, context);
    }
    contexts_.clear();
    events_.clear();
    text_.clear();
    initialised_ = false;
}

Expected<ContextHandle, Error> InputServer::register_context(MappingContext&& context) noexcept {
    auto* stored = make<MappingContext>(*allocator_, std::move(context));
    if (stored == nullptr) {
        return fail(ErrorCode::OutOfMemory, "input: could not allocate a mapping context");
    }
    const auto index = static_cast<u32>(contexts_.size());
    if (Status pushed = contexts_.push_back(stored); !pushed) {
        unmake(*allocator_, stored);
        return make_unexpected(pushed.error());
    }
    // Generation 1: `Handle` treats generation 0 as null, so a default-constructed `ContextHandle`
    // is "no context" rather than a reference to the first one registered.
    return ContextHandle::from_slot(index, 1);
}

const MappingContext* InputServer::context(ContextHandle handle) const noexcept {
    if (handle.is_null() || handle.index() >= contexts_.size()) {
        return nullptr;
    }
    return contexts_[handle.index()];
}

Status InputServer::finalize_declarations() noexcept {
    if (!initialised_) {
        return fail(ErrorCode::Unavailable, "input: initialize() before finalize_declarations()");
    }
    for (auto& user : users_) {
        if (Status configured = user->configure(actions_); !configured) {
            return configured;
        }
    }
    return ok();
}

Status InputServer::assign(DeviceId device, u32 user, Nanoseconds timestamp) noexcept {
    if (user >= users_.size()) {
        return fail(ErrorCode::OutOfRange, "input: no such input user");
    }
    const DeviceRecord* record = devices_.find(device);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "input: no such device");
    }
    const DeviceKind kind = record->description.kind;
    if (Status assigned = devices_.assign(device, user, timestamp); !assigned) {
        return assigned;
    }
    users_[user]->note_device_assigned(device, kind);
    return ok();
}

void InputServer::unassign(DeviceId device, Nanoseconds timestamp) noexcept {
    const DeviceRecord* record = devices_.find(device);
    const u32 user = record != nullptr ? record->user : kNoUser;
    devices_.unassign(device, timestamp);
    if (user < users_.size()) {
        users_[user]->note_device_unassigned(device);
    }
}

void InputServer::pump_device_lifecycle() noexcept {
    const Array<DeviceLifecycleEvent>& events = devices_.lifecycle_events();
    for (const auto& event : events) {
        if (event.user >= users_.size()) {
            continue;
        }
        switch (event.kind) {
            case DeviceLifecycle::Assigned:
            case DeviceLifecycle::Reconnected:
                users_[event.user]->note_device_assigned(event.device, event.device_kind);
                break;
            case DeviceLifecycle::Disconnected:
            case DeviceLifecycle::Unassigned:
                users_[event.user]->note_device_unassigned(event.device);
                break;
            default:
                break;
        }
    }
    devices_.clear_lifecycle_events();
}

void InputServer::submit(const DeviceEvent& event) noexcept {
    events_.push(event);
}

Expected<DeviceId, Error> InputServer::create_virtual_device(Name name,
                                                             Nanoseconds timestamp) noexcept {
    DeviceDescription description;
    description.kind = DeviceKind::Virtual;
    description.display_name = name;
    description.hardware_id = name;
    ++virtual_devices_;
    return devices_.connect(description, timestamp);
}

Status InputServer::inject(DeviceId device, Control control, f32 value, Nanoseconds timestamp,
                           EventSource source) noexcept {
    if (!config_.allow_synthetic) {
        // Refused loudly. A shipping build that silently dropped the injection would leave an
        // automation harness reporting green over a game nothing was driving.
        return fail(ErrorCode::PermissionDenied,
                    "input: synthetic and remote input are disabled in this build");
    }
    if (source == EventSource::Physical) {
        return fail(ErrorCode::InvalidArgument,
                    "input: injected input is Synthetic, Remote or Replay — never Physical, which "
                    "is what makes the distinction usable by a diagnostic");
    }
    DeviceEvent event;
    event.timestamp = timestamp;
    event.device = device;
    event.control = control;
    event.value = value;
    event.source = source;
    events_.push(event);
    return ok();
}

void InputServer::submit_text(const TextEvent& event) noexcept {
    (void)text_.push_back(event);
}

Status InputServer::rebuild_user(InputUser& target) noexcept {
    return target.rebuild(actions_, contexts_.data(), static_cast<u32>(contexts_.size()));
}

void InputServer::resolve_tick(u64 tick, Nanoseconds now, f32 delta_seconds) noexcept {
    tick_ = tick;
    pump_device_lifecycle();

    for (auto& user : users_) {
        InputUser& target = *user;
        if (target.dirty()) {
            // A rebuild that fails leaves the previous table in place, which is the safe answer:
            // the player keeps the bindings they had rather than losing input entirely.
            (void)rebuild_user(target);
        }
        target.begin_tick(tick);
    }

    events_.sort();
    last_events_ = static_cast<u32>(events_.size());
    dropped_total_ += events_.take_dropped();

    for (usize index = 0; index < events_.size(); ++index) {
        const DeviceEvent& event = events_[index];
        // The device's control state is advanced first, so that a composite reading four keys sees
        // the world as it was *after* this event and not before it.
        devices_.apply(event);
        bool shared = false;
        const u32 routed = devices_.route(event.device, shared);
        if (shared) {
            for (auto& user : users_) {
                user->observe(event, devices_);
            }
        } else if (routed < users_.size()) {
            users_[routed]->observe(event, devices_);
        }
    }
    events_.clear();

    for (auto& user : users_) {
        user->finish_tick(now, delta_seconds, devices_);
    }

    // Mouse deltas are displacements *within the window* and are consumed by the tick that read
    // them. Leaving them latched would make one flick move the character every tick until the mouse
    // moved again, which is the classic delta-as-level bug — the mirror image of the one
    // `Interpretation` exists for.
    devices_.clear_deltas();
}

}  // namespace cy::input
