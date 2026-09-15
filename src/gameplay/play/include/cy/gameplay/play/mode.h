#pragma once
// The three play modes, their per-mode capabilities, and the refusal a mode that is not available
// SHALL produce. M11.b tasks 0.5 and 3.1.
//
// ================================================================================================
// WHY THIS FILE EXISTS, AND WHY IT IS A TABLE RATHER THAN THREE CLASSES
// ================================================================================================
//
// `live-editing` (Play modes) and `editor-architecture` (Play mode) name the same three modes:
//
//     InEditor         a runtime world in the editor's hosted runtime process
//     SeparateProcess  a second runtime process, with no editor-only state in it
//     RemoteDevice     a runtime on a console, phone, tablet or another machine
//
// and the same specification says what they are NOT: *"Locality SHALL be an optimisation of
// transport, not a different architecture, so that remote play requires no separate
// implementation."* So the three are not three session types and not three world models. They are
// one `PlaySession` over one compiled runtime world, plus a declaration of **where its frames come
// from and what it can be asked to do** — which is a table.
//
// Until M11.b no code in this tree named any of them. `HostingMode{NoRuntime, Embedded, Hosted}` is
// the nearest thing and is deliberately not this axis: `cy-editor-sdk/src/host.rs` documents
// `Hosted` as *"the engine in a separate process **or** on a remote device"*, collapsing exactly
// the two modes the specification separates. A mode named by a specification and by no code is a
// claim nothing can check, which is most of why `editor-architecture` sat at Seed from M5 to M11.
//
// ================================================================================================
// THE ONE RULE THIS FILE IS HERE TO ENFORCE: NO SILENT FALLBACK
// ================================================================================================
//
// `specs/live-editing/` (M11.b) makes it normative: *"Selecting a mode that is not available SHALL
// refuse, naming the mode and the reason. It SHALL NOT fall back to another mode."* A
// `RemoteDevice` request that quietly runs `InEditor` is a green test over a feature that does not
// exist, and that is worse than a red in every respect — the test passes, the feature is absent,
// and nothing in the record says so. `PlaySession::enter` therefore consults `availability_of()`
// before it builds anything, and returns an error whose message names the mode.
//
// ================================================================================================
// WHY AVAILABILITY IS COMPUTED FROM A SUPPORT QUERY RATHER THAN HARD-CODED
// ================================================================================================
//
// `RemoteDevice` is unavailable in every configuration of this tree today, and the reason is a
// concrete missing part rather than a decision: a frame has to cross a machine boundary, and
// `cy_editor_viewport::transport::TransportKind::EncodedStream` is declared on the Rust side,
// `ViewportTransportKind::EncodedStream` on the C++ side, and implemented on neither. The only
// transport above a local surface is a shared texture built on `VK_KHR_external_semaphore_fd`,
// which is same-machine by construction.
//
// Writing `RemoteDevice` off as permanently absent would make the refusal untestable — a constant
// cannot be made to go the other way — so availability is a function of `PlayModeSupport`, which is
// a query about the build. A test flips `frame_encoder` and watches the same mode become available
// and then refuse again, which is what makes the refusal load-bearing rather than asserted (the
// delta specification's second scenario asks for exactly that demonstration).
//
// The day an encoder lands, the one thing that changes is what fills in `PlayModeSupport`.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <string_view>

namespace cy::gameplay {

/// Where a play session's runtime runs.
///
/// `live-editing` is explicit that **no play mode runs in the editor process** — the editor is a
/// separate Rust application — so `InEditor` denotes iteration speed and shared runtime state, not
/// co-location with the editor.
enum class PlayMode : u8 {
    /// A runtime world in the editor's hosted runtime process. Fast iteration.
    InEditor = 0,
    /// A second runtime process, so that editor-only state cannot mask a defect.
    SeparateProcess,
    /// A runtime on another machine: a console, a phone, a tablet.
    RemoteDevice,
};

/// How many modes the engine claims. A fourth added to the enumeration without a row in
/// `play_modes()` fails `describe()` rather than reading as the closest one.
inline constexpr u32 kPlayModeCount = 3;

/// The spelling the protocol carries and the editor shows — `in-editor`, `separate-process`,
/// `remote-device`. Never null.
///
/// A word rather than a number, for the reason `PlayState` is a word on the wire: a fourth mode
/// added on one side and not the other must be refused **by name** rather than falling through a
/// match to the closest integer.
[[nodiscard]] const char* play_mode_name(PlayMode mode) noexcept;

/// The mode a word names, or an error naming the word this build does not know.
[[nodiscard]] Expected<PlayMode, Error> play_mode_of(std::string_view name) noexcept;

/// What a mode can be asked to do.
///
/// `live-editing` requires stepping *"in every mode where the runtime permits"*, and M11.b's delta
/// makes *"where the runtime permits"* answerable by a **query** rather than by trying it: a
/// capability that is absent is declared absent, not silently ignored and not a reason to downgrade
/// the mode.
struct PlayModeCapabilities {
    /// Pause a running session.
    bool pause = false;
    /// Advance exactly one simulation tick while paused.
    bool step_tick = false;
    /// Advance exactly one frame — the ticks one frame of the configured frame rate contains.
    bool step_frame = false;
    /// Accept a compiled live edit against the running world.
    bool live_edit = false;
    /// Answer an inspection query about the running world.
    bool inspect = false;
    /// Whether the runtime's own process is separate from the editor's hosted runtime, so that
    /// editor-only state is absent from it. `live-editing`'s "Standalone behaviour is honest".
    bool isolated_state = false;
    /// The transport a frame reaches the editor by, named rather than implied. Never null.
    const char* frame_transport = "none";
};

/// Whether a mode can run on this build, and when it cannot, why and when it is due.
///
/// `due` is not decoration: M11.b's delta requires that *"a mode declared absent SHALL name the
/// milestone or rung at which it is due, so that 'not yet' is a recorded decision rather than the
/// absence of a check."*
struct PlayModeAvailability {
    bool available = false;
    /// Why, naming the mode. Never null, and non-empty in both directions — an available mode says
    /// what makes it available, so a reader of a refusal and a reader of an acceptance are reading
    /// the same field.
    const char* reason = "";
    /// The rung at which an absent mode is due. Empty when the mode is available.
    const char* due = "";
};

/// What this build can do, which is what decides which modes are available.
///
/// Every field is a fact about the binary rather than a preference, and each names the thing that
/// would set it.
struct PlayModeSupport {
    /// Whether this build can start and supervise a second runtime process.
    ///
    /// MEASURED, not declared: `play_mode_support(platform)` in `cy/gameplay/play/launcher.h`
    /// answers it by resolving `cy_play_runtime_host` beside the calling executable and asking the
    /// filesystem whether it is there. The no-argument `play_mode_support()` answers `false`,
    /// because a caller with no `Platform` cannot start anything.
    ///
    /// It read `true` from a literal until the repair round that built the launcher, which made
    /// every check over `availability_of(SeparateProcess, …)` a check that could not fail.
    bool runtime_launcher = false;
    /// Whether THIS PROCESS is a runtime host something launched.
    ///
    /// Distinct from `runtime_launcher`, and the distinction is the one M11.b got wrong.
    /// `runtime_launcher` is the EDITOR's question — may I offer this mode — and is what
    /// `availability_of` answers. This is the SESSION's question: a `PlaySession` serving
    /// `SeparateProcess` is the runtime world *inside* the second process, and one built in the
    /// process that did the launching would be the in-editor world wearing another mode's name.
    /// That was precisely the fiction the rung shipped: three sessions in one process, compared to
    /// each other, reported as three modes.
    ///
    /// Only `cy_play_runtime_host` sets it. `PlaySession::enter` refuses `SeparateProcess` without
    /// it, naming `ProcessPlayDriver` as the way the editor's side drives the mode.
    bool hosted_runtime_process = false;
    /// Whether this build can encode a frame for a transport that crosses a machine boundary —
    /// `TransportKind::EncodedStream`, declared on both sides of the viewport transport and
    /// implemented on neither. False in every configuration of this tree; see the file header.
    bool frame_encoder = false;
    /// Whether a remote runtime is reachable — an address to connect to. Distinct from the encoder
    /// because a machine with an encoder and nothing to send to is a different refusal.
    bool remote_runtime = false;
};

/// The support this build actually has, read from what is compiled in rather than from a wish.
[[nodiscard]] PlayModeSupport play_mode_support() noexcept;

/// Whether `mode` can run given `support`, with the reason either way.
[[nodiscard]] PlayModeAvailability availability_of(PlayMode mode,
                                                   const PlayModeSupport& support) noexcept;

/// What `mode` can be asked to do. Constant per mode: a capability that varies with the host is a
/// capability of the runtime, not of the mode, and belongs in the runtime's own report.
[[nodiscard]] PlayModeCapabilities capabilities_of(PlayMode mode) noexcept;

/// One row of the enumeration: a mode, its spelling, what it can do, and whether it can run here.
struct PlayModeDescriptor {
    PlayMode mode = PlayMode::InEditor;
    const char* name = "";
    PlayModeCapabilities capabilities;
    PlayModeAvailability availability;
};

/// Every mode the engine claims, filled against `support`.
///
/// `out` receives `kPlayModeCount` rows in enumerator order. Enumerating rather than asking one
/// mode at a time is the shape the delta specification's third scenario needs: *"a mode that is
/// neither driveable through the live bridge nor declared absent with a rung SHALL fail the check,
/// naming the mode"* — a check over a list, not over a mode somebody remembered to ask about.
void describe_play_modes(const PlayModeSupport& support,
                         PlayModeDescriptor (&out)[kPlayModeCount]) noexcept;

}  // namespace cy::gameplay
