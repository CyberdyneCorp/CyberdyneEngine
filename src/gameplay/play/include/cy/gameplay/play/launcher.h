#pragma once
// The second runtime process: resolving it, launching it, and talking to it. M11.b tasks 0.5 and
// 3.1, and the `separate-process-play-really-forks` criterion.
//
// ================================================================================================
// WHY THIS FILE EXISTS, AND WHAT WAS WRONG BEFORE IT
// ================================================================================================
//
// `live-editing` (Play modes) gives each mode a RUNTIME LOCATION, and the location is the whole of
// what distinguishes them:
//
//     InEditor         "A runtime world in the editor's hosted runtime process"
//     SeparateProcess  "A second runtime process"
//     RemoteDevice     "A remote runtime"
//
// and `editor-architecture` (Play mode) says what selecting the second one does: *"the game SHALL
// launch as a separate process with the debugger attached, so editor-specific state cannot mask
// bugs"*.
//
// Until this file, `SeparateProcess` in this tree was a WORD. `PlayModeSupport::runtime_launcher`
// was initialised to `true` by a literal, beside a comment that said "this build carries no
// launcher yet", and `mode.cpp` named M11.d as the rung that would write one. `grep -rn
// 'fork\|exec\|posix_spawn\|CreateProcess' src/gameplay/play/` returned four string literals and no
// code. The suite that claimed to drive "a world in each of the three modes" built three
// `PlaySession`s in ONE process and compared them, which is a comparison that cannot fail for the
// reason it claims to test: three copies of the same in-process simulation agree whatever the mode
// argument said.
//
// So the check was not a check, and the mode was not a mode. This file is the mode; `driver.h` is
// the check.
//
// ================================================================================================
// THE ENGINE ALREADY HAD A PROCESS LAUNCHER AND PLAY MODE NEVER USED IT
// ================================================================================================
//
// `cy/core/platform/platform.h` has carried `spawn_process`, `poll_process`, `wait_process`,
// `terminate_process` and `release_process` since M0, implemented by `Sdl3Platform` over SDL's
// process API, and nothing in `src/gameplay/` had ever called one of them. What it did NOT carry
// was a channel — a spawned child with inherited or null streams is started but not addressable,
// and a play mode has to be DRIVEN, not merely begun. `ProcessOptions::piped_standard_streams` and
// the three stream calls beside it were added for exactly this, and this is their first consumer.
//
// ================================================================================================
// WHY AVAILABILITY IS NOW MEASURED RATHER THAN DECLARED
// ================================================================================================
//
// `play_mode_support()` with no argument answers `runtime_launcher = false`, and it is not a
// pessimism: a caller with no `Platform` has no way to start a process, so the honest answer to
// "can this build start and supervise a second runtime process" is no. `play_mode_support(platform)
// ` answers it by RESOLVING THE BINARY — `runtime_host_path()` looks for `cy_play_runtime_host`
// beside the calling executable and asks the filesystem whether it is there. A tree whose host
// binary is deleted, renamed or never built reports the mode unavailable, by name, with the rung it
// is due at, instead of claiming a launcher that is not there.
//
// That is the property the old `= true` destroyed, and it is what makes `availability_of` testable
// in both directions against a fact rather than against a preference.
//
// ================================================================================================
// THE PROTOCOL, AND WHY IT IS ONE LINE PER REQUEST
// ================================================================================================
//
// `live-editing` (Live bridge protocol) requires a VERSIONED MESSAGE PROTOCOL, and requires that
// all three modes be driven through the same live bridge interface — *"Locality SHALL be an
// optimisation of transport, not a different architecture"*. `PlayDriver` in `driver.h` is that
// interface; this is the transport under it for the separate-process case.
//
// One request per line, one reply per line, UTF-8, `\n`-terminated:
//
//     -> hello 1                       <- welcome 1 pid=<n>
//     -> enter                         <- ok
//     -> tick                          <- ok
//     -> pause | resume                <- ok
//     -> step-tick | step-frame        <- ok | refused <reason>
//     -> translation-y <identity>      <- value <ieee-754-single, as 8 hexadecimal digits>
//     -> report                        <- report ticks=<n> stepped_ticks=<n> stepped_frames=<n>
//                                                 restored_exactly=<0|1> entities=<n> bodies=<n>
//     -> stop                          <- ok
//     -> quit                          <- bye
//
// The version is the first thing exchanged and a mismatch is refused BY NUMBER, for the reason
// `play_mode_name` returns a word: a peer built from a different commit must be refused rather than
// read as the closest thing this build understands.
//
// A float crosses as its EXACT BITS rather than as a decimal. The claim the separate-process case
// makes is that the same world driven by the same commands simulates to the same place in another
// process; a decimal round trip would turn that into "to within the precision of printf", which is
// a weaker claim than the one being made and would hide a real divergence of one unit in the last
// place.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/platform/platform.h>
#include <cy/gameplay/play/mode.h>

#include <string_view>

namespace cy::gameplay {

/// The basename of the runtime host binary, without a platform suffix.
///
/// It is the target name in `src/gameplay/play/CMakeLists.txt`, and that CMakeLists.txt puts the
/// binary at the build root — beside the executables that launch it — precisely so that resolving
/// it "beside me" is the same rule in a build tree and in an installed layout.
inline constexpr std::string_view kRuntimeHostBinary = "cy_play_runtime_host";

/// The protocol both sides speak. Bumped when a message changes shape; a peer that answers a
/// different number is refused rather than tolerated.
inline constexpr u32 kPlayProtocolVersion = 1;

/// How long a reply may take before the child is reported unresponsive.
///
/// Generous, because what is being bounded is a HANG rather than a slow machine: a request that
/// takes ten seconds on a loaded continuous-integration runner is fine, and one that never arrives
/// must not turn the suite into a timeout with no message in it.
inline constexpr i64 kReplyDeadlineNanoseconds = 30LL * 1000 * 1000 * 1000;

/// The path of the runtime host binary for this build, written into `out`.
///
/// It is the directory of `platform.executable_path()` plus `kRuntimeHostBinary` plus the
/// platform's executable suffix. The result is NUL-terminated, because it is handed to
/// `Platform::spawn_process` as `argv[0]`.
///
/// Fails when the platform cannot report its own executable. It does NOT fail when the file is
/// absent: `runtime_launcher_available` is the question about the file, and a path that does not
/// exist is still the path a diagnostic should name.
[[nodiscard]] Status runtime_host_path(const Platform& platform, Array<char>& out) noexcept;

/// Whether this build can start and supervise a second runtime process: whether the file
/// `runtime_host_path` names is there and can be executed.
[[nodiscard]] bool runtime_launcher_available(const Platform& platform) noexcept;

/// The support this build actually has, measured against a platform rather than declared.
///
/// This is the overload every host should call. The no-argument `play_mode_support()` in `mode.h`
/// exists for callers that have no platform, and answers `runtime_launcher = false` because a
/// caller with no platform cannot launch anything.
[[nodiscard]] PlayModeSupport play_mode_support(const Platform& platform) noexcept;

/// What a second runtime process is asked to play.
struct RuntimeLaunchRequest {
    /// The `.cyworld` the second process reads. Required: a runtime process with no world would
    /// start and have nothing to simulate, which is a launch that proves nothing.
    std::string_view world_path;
    /// The path the EDITOR opens that world by, which is not the same thing as where the file
    /// happens to sit on disk.
    ///
    /// It is load-bearing rather than cosmetic: `worldfile.h` derives every node's identity from
    /// the document's identity, and the document's identity is `fnv1a_128` over exactly this
    /// string. Two processes that read the same bytes under two different asset paths hold two
    /// different sets of identities, and every `translation-y` across the bridge would refuse —
    /// which is how this was found. Empty means `world_path`, which is right only when the two
    /// coincide.
    std::string_view asset_path;
    /// The binary to launch. Empty means `runtime_host_path()` — the resolved host beside this
    /// executable. Named explicitly only by a test that wants to launch something else and watch
    /// the handshake refuse it.
    std::string_view binary;
    /// How many bodies the child's physics world is sized for. Passed through so that the two
    /// processes simulate the same world rather than two differently-shaped ones.
    u32 body_capacity = 64;
    /// Whether the child is launched stopped, for a debugger to attach to.
    /// `editor-architecture` asks for standalone play to launch *"with the debugger attached"*; the
    /// child honours this by announcing its own process identifier and waiting for `resume-launch`
    /// before it builds anything, which is the part an engine can do without knowing which debugger
    /// the developer uses.
    bool wait_for_debugger = false;
};

/// A second runtime process: launched, supervised, driven and reaped.
///
/// Not thread-safe. The protocol is request/response and a second thread issuing a request between
/// another's request and its reply would read the wrong line.
class RuntimeProcess {
public:
    explicit RuntimeProcess(Allocator& allocator) noexcept;
    ~RuntimeProcess();

    RuntimeProcess(const RuntimeProcess&) = delete;
    RuntimeProcess& operator=(const RuntimeProcess&) = delete;
    RuntimeProcess(RuntimeProcess&&) = delete;
    RuntimeProcess& operator=(RuntimeProcess&&) = delete;

    /// Launch the child and complete the version handshake.
    ///
    /// On any failure nothing is left running: a child that started and then failed its handshake
    /// is terminated and reaped before this returns, because a play mode that half-started is the
    /// residue `PlaySession::enter` is written to avoid at its own end.
    [[nodiscard]] Status launch(Platform& platform, const RuntimeLaunchRequest& request) noexcept;

    /// Send one request and read one reply, both without the terminating newline.
    ///
    /// The reply is written into `reply`. A child that closed its output mid-request is reported as
    /// an error naming the request, rather than as an empty reply.
    [[nodiscard]] Status request(std::string_view command, Array<char>& reply) noexcept;

    /// Ask the child to leave, wait for it, and return its exit code.
    ///
    /// Idempotent: a process already reaped returns the code it exited with.
    [[nodiscard]] Expected<i32, Error> shutdown() noexcept;

    /// Kill the child without asking. For the case a test needs: a runtime that dies must leave the
    /// editor running, and the only honest way to test that is to kill one.
    [[nodiscard]] Status kill() noexcept;

    /// The operating system's identifier for the child, as the CHILD reported it in its handshake.
    /// Zero before a successful launch.
    [[nodiscard]] i64 reported_process_id() const noexcept { return reported_pid_; }
    /// The same identifier, as the OPERATING SYSTEM reports it to this process.
    ///
    /// The two are compared at launch and a disagreement fails it. That comparison is the thing
    /// that cannot be faked in-process: an implementation that answered the protocol from a local
    /// object would have to know a process identifier the operating system agrees with, and there
    /// is none.
    [[nodiscard]] i64 observed_process_id() const noexcept { return observed_pid_; }

    [[nodiscard]] bool running() const noexcept { return handle_ != 0 && !reaped_; }
    /// The path this process was launched from. Empty before a launch.
    [[nodiscard]] std::string_view binary_path() const noexcept;

private:
    /// Read one `\n`-terminated line from the child into `line`, without the newline.
    ///
    /// The child's output is non-blocking (see `Platform::read_process_output`), so this polls: it
    /// reads what is there, and when nothing is, asks the operating system whether the child is
    /// still alive. A child that exited without replying is reported as a dead runtime; a child
    /// that is merely slow is waited for up to `kReplyDeadlineNanoseconds`, after which it is
    /// reported as unresponsive rather than waited for forever — a hung child would otherwise
    /// present as a suite that never finishes, which is the failure hardest to read.
    [[nodiscard]] Status read_line(Array<char>& line) noexcept;

    Allocator* allocator_;
    Platform* platform_ = nullptr;
    ProcessHandle handle_ = 0;
    bool reaped_ = false;
    i32 exit_code_ = 0;
    i64 reported_pid_ = 0;
    i64 observed_pid_ = 0;
    Array<char> path_;
    /// Bytes read from the child that are not yet a whole line. A pipe splits wherever it likes and
    /// discarding a partial line would turn a long reply into a protocol error.
    Array<char> pending_;
};

}  // namespace cy::gameplay
