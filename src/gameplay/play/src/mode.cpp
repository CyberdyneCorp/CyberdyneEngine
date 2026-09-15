// The play-mode table. See cy/gameplay/play/mode.h for why it is a table and for the one rule it
// enforces: a mode that is not available refuses by name and never falls back.

#include <cy/gameplay/play/mode.h>

namespace cy::gameplay {
namespace {

/// The spellings, in enumerator order. Words rather than numbers, for the reason the header gives.
constexpr std::string_view kNames[kPlayModeCount] = {
    "in-editor",
    "separate-process",
    "remote-device",
};

/// Every refusal and acceptance string is a literal with static storage, because `cy::Error` holds
/// a `const char*` and a message assembled into a buffer would dangle the moment the frame it was
/// built in returned.
constexpr const char* kInEditorAvailable =
    "in-editor: the hosted runtime process simulates the compiled runtime world directly";
constexpr const char* kSeparateProcessAvailable =
    "separate-process: a second runtime process, launched and supervised by the host";
constexpr const char* kSeparateProcessNoLauncher =
    "separate-process: this build cannot launch a second runtime process";
constexpr const char* kRemoteDeviceAvailable =
    "remote-device: an encoded frame stream to a runtime on another machine";
constexpr const char* kRemoteDeviceNoEncoder =
    "remote-device: no frame encoder in this build — the viewport transport declares "
    "EncodedStream on both sides and implements it on neither, and the only transport above a "
    "local surface is a same-machine shared texture";
constexpr const char* kRemoteDeviceNoRuntime =
    "remote-device: no remote runtime is reachable — this build has a frame encoder and no "
    "address to send to";

/// The rung at which `RemoteDevice` is due. M11.b task 5.2 owns `EncodedStream`; if that task does
/// not land in this rung, this string is what has to be re-pointed, and it is one string.
constexpr const char* kRemoteDeviceDue = "M11.b task 5.2 (EncodedStream) — see design.md §1.2";
/// What `SeparateProcess` needs, when it is missing. It is no longer a RUNG: the launcher is built
/// (`cy/gameplay/play/launcher.h`) and the mode is unavailable only when the host binary it
/// launches cannot be found beside the calling executable, which is an installation to repair
/// rather than work to schedule.
constexpr const char* kSeparateProcessDue =
    "build cy_play_runtime_host and install it beside this executable";

}  // namespace

const char* play_mode_name(PlayMode mode) noexcept {
    const auto index = static_cast<u32>(mode);
    if (index >= kPlayModeCount) {
        return "unknown";
    }
    return kNames[index].data();
}

Expected<PlayMode, Error> play_mode_of(std::string_view name) noexcept {
    for (u32 index = 0; index < kPlayModeCount; ++index) {
        if (kNames[index] == name) {
            return static_cast<PlayMode>(index);
        }
    }
    // Named rather than defaulted. A word this build does not know is a peer built against a
    // different mode set, and answering it with the closest mode is the silent fallback the
    // specification forbids.
    return fail(ErrorCode::InvalidArgument,
                "play mode: the requested mode is not one this build knows — it is one of "
                "in-editor, separate-process or remote-device");
}

PlayModeSupport play_mode_support() noexcept {
    PlayModeSupport support;
    // FALSE, AND THE FALSE IS THE POINT. A caller with no `Platform` cannot start a process, so
    // "can this build start and supervise a second runtime process" has one honest answer here and
    // it is no.
    //
    // This field read `true` from M11.b until the repair round that built the launcher, beside a
    // comment that said "this build carries no launcher yet". A literal `true` made
    // `availability_of(SeparateProcess, …)` a constant, which made every check over it a check that
    // could not fail, which is how a mode with no implementation at all sat behind a green suite.
    //
    // `play_mode_support(platform)` in `cy/gameplay/play/launcher.h` is the overload a host calls:
    // it MEASURES the answer by resolving `cy_play_runtime_host` beside the calling executable and
    // asking the filesystem whether it is there.
    support.runtime_launcher = false;
    // Both false in every configuration of this tree. See the header: `EncodedStream` is declared
    // on both sides of the viewport transport and implemented on neither.
    support.frame_encoder = false;
    support.remote_runtime = false;
    return support;
}

PlayModeAvailability availability_of(PlayMode mode, const PlayModeSupport& support) noexcept {
    PlayModeAvailability availability;
    switch (mode) {
        case PlayMode::InEditor:
            // The hosted runtime is where a `PlaySession` already runs, so this mode is the one
            // the tree has had since M8.a without being able to name it.
            availability.available = true;
            availability.reason = kInEditorAvailable;
            return availability;

        case PlayMode::SeparateProcess:
            if (!support.runtime_launcher) {
                availability.reason = kSeparateProcessNoLauncher;
                availability.due = kSeparateProcessDue;
                return availability;
            }
            availability.available = true;
            availability.reason = kSeparateProcessAvailable;
            return availability;

        case PlayMode::RemoteDevice:
            // Two refusals rather than one, because a build with an encoder and no address is a
            // different problem from a build with neither, and a reader of the message should not
            // have to guess which.
            if (!support.frame_encoder) {
                availability.reason = kRemoteDeviceNoEncoder;
                availability.due = kRemoteDeviceDue;
                return availability;
            }
            if (!support.remote_runtime) {
                availability.reason = kRemoteDeviceNoRuntime;
                availability.due = kRemoteDeviceDue;
                return availability;
            }
            availability.available = true;
            availability.reason = kRemoteDeviceAvailable;
            return availability;
    }
    availability.reason = "play mode: not one this build knows";
    availability.due = kRemoteDeviceDue;
    return availability;
}

PlayModeCapabilities capabilities_of(PlayMode mode) noexcept {
    PlayModeCapabilities capabilities;
    // Common to all three, and that commonality IS the architecture claim: one world model, one
    // session, one live bridge. A capability that differed between modes for any reason other than
    // transport would be the refutation the spike budgeted for.
    capabilities.pause = true;
    capabilities.step_tick = true;
    capabilities.step_frame = true;
    capabilities.live_edit = true;
    capabilities.inspect = true;

    switch (mode) {
        case PlayMode::InEditor:
            capabilities.isolated_state = false;
            capabilities.frame_transport = "shared-texture";
            return capabilities;
        case PlayMode::SeparateProcess:
            // The point of the mode: editor-only state is absent from the runtime, so
            // editor-specific behaviour cannot mask a defect.
            capabilities.isolated_state = true;
            capabilities.frame_transport = "shared-texture";
            return capabilities;
        case PlayMode::RemoteDevice:
            capabilities.isolated_state = true;
            capabilities.frame_transport = "encoded-stream";
            // A remote runtime's frames arrive encoded and late, and a single-FRAME step over a
            // stream whose frames are not individually addressable is a request the transport
            // cannot honour. The tick step still can: a tick is a message, not a picture. This is
            // the one place a capability differs by mode, and it differs by TRANSPORT, which is
            // what `live-editing` says locality is allowed to be.
            capabilities.step_frame = false;
            return capabilities;
    }
    return capabilities;
}

void describe_play_modes(const PlayModeSupport& support,
                         PlayModeDescriptor (&out)[kPlayModeCount]) noexcept {
    for (u32 index = 0; index < kPlayModeCount; ++index) {
        const auto mode = static_cast<PlayMode>(index);
        out[index].mode = mode;
        out[index].name = play_mode_name(mode);
        out[index].capabilities = capabilities_of(mode);
        out[index].availability = availability_of(mode, support);
    }
}

}  // namespace cy::gameplay
