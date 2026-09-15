#pragma once
// ONE command vocabulary, two localities. M11.b tasks 0.5 and 3.1.
//
// ================================================================================================
// THE SENTENCE THIS FILE EXISTS TO MAKE CHECKABLE
// ================================================================================================
//
// `live-editing` (Play modes): *"All three SHALL be driven through the **same live bridge
// interface**. Locality SHALL be an optimisation of transport, not a different architecture, so
// that remote play requires no separate implementation."*
//
// A sentence like that is satisfied by a design and refuted by a diff, and until this file the tree
// had neither: there was one implementation, it was in-process, and `PlayMode` was a field on its
// configuration. Three `PlaySession`s built in one process and compared to each other agree for
// reasons that have nothing to do with the mode — so the suite that compared them could not have
// failed if `SeparateProcess` had been deleted outright.
//
// `PlayDriver` is the interface the sentence names. It has exactly the vocabulary
// `editor-architecture` requires of play mode — enter, tick, pause, resume, step one tick, step one
// frame, stop — plus the two reads an editor needs while a session runs, and NOTHING about where
// the runtime is. Two implementations:
//
//     LocalPlayDriver    calls a `PlaySession` in this process.   InEditor.
//     ProcessPlayDriver  sends the same calls, one line each, to a `RuntimeProcess` this process
//                        launched and supervises.                 SeparateProcess.
//
// Both are driven by the same sequence of calls in `test_editor_play.cpp`, and the simulated result
// is compared BETWEEN THE TWO PROCESSES. That comparison is the check: it fails if the child does
// not run, if it runs a different world, if it runs a different number of ticks, or if the two
// diverge by one unit in the last place — and it cannot be satisfied by a second `PlaySession` in
// the parent, because `RuntimeProcess::launch` refuses unless the operating system agrees that the
// process which answered is the process it spawned.
//
// ================================================================================================
// WHY THE DRIVER OWNS NO WORLD
// ================================================================================================
//
// `LocalPlayDriver` holds a `PlaySession&` it did not create, and `ProcessPlayDriver` holds a
// `RuntimeProcess&` it did not launch. A driver that built its own would have to decide how the
// authored world reaches it, and the two localities answer that differently — the local one shares
// the editor's `ser::World`, the separate one is given a path to read. Making the driver own it
// would push that difference into the interface, which is the "different architecture" the
// requirement forbids.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/gameplay/play/launcher.h>
#include <cy/gameplay/play/mode.h>
#include <cy/gameplay/play/session.h>

namespace cy::gameplay {

/// What a driver can be asked about a session in flight. The subset of `PlayReport` that survives
/// a process boundary, which is why it is a type of its own: a field only one locality could answer
/// would be a field the interface cannot carry.
struct PlayObservation {
    u64 ticks = 0;
    u64 stepped_ticks = 0;
    u64 stepped_frames = 0;
    u32 entities = 0;
    u32 bodies = 0;
    /// Whether the authoring document's bytes at `stop()` were identical to its bytes at `enter()`.
    /// Meaningful only after `stop()`.
    bool restored_exactly = false;
};

/// The live bridge interface: what the editor asks of a play session, wherever it is running.
class PlayDriver {
public:
    PlayDriver() = default;
    virtual ~PlayDriver() = default;

    PlayDriver(const PlayDriver&) = delete;
    PlayDriver& operator=(const PlayDriver&) = delete;
    PlayDriver(PlayDriver&&) = delete;
    PlayDriver& operator=(PlayDriver&&) = delete;

    /// Where this driver's runtime runs. The one place locality is visible.
    [[nodiscard]] virtual PlayMode mode() const noexcept = 0;

    [[nodiscard]] virtual Status enter() noexcept = 0;
    [[nodiscard]] virtual Status tick() noexcept = 0;
    [[nodiscard]] virtual Status pause() noexcept = 0;
    [[nodiscard]] virtual Status resume() noexcept = 0;
    [[nodiscard]] virtual Status step_tick() noexcept = 0;
    [[nodiscard]] virtual Status step_frame() noexcept = 0;
    [[nodiscard]] virtual Status stop() noexcept = 0;

    /// The height of one simulated node, read out of the running world by the authored identity the
    /// editor knows it by.
    ///
    /// One scalar rather than a whole transform, because it is the comparison the round-trip case
    /// makes and a wider read would invite a comparison "to within printing precision". It crosses
    /// a process boundary as its exact bits; see `launcher.h`.
    [[nodiscard]] virtual Expected<f32, Error> translation_y(u64 identity) noexcept = 0;

    [[nodiscard]] virtual Expected<PlayObservation, Error> observe() noexcept = 0;
};

/// A session in this process. `InEditor`: the editor's hosted runtime process is this one.
class LocalPlayDriver final : public PlayDriver {
public:
    /// `session` and `authored` must outlive this driver. `configuration.mode` is taken as given
    /// and is NOT overridden: a caller that asks for a mode this driver cannot serve is refused by
    /// `enter`, not silently corrected, which is the no-silent-fallback rule of `mode.h`.
    LocalPlayDriver(PlaySession& session, const PlayConfiguration& configuration,
                    scene::serialization::World& authored) noexcept;

    [[nodiscard]] PlayMode mode() const noexcept override { return configuration_.mode; }

    [[nodiscard]] Status enter() noexcept override;
    [[nodiscard]] Status tick() noexcept override;
    [[nodiscard]] Status pause() noexcept override;
    [[nodiscard]] Status resume() noexcept override;
    [[nodiscard]] Status step_tick() noexcept override;
    [[nodiscard]] Status step_frame() noexcept override;
    [[nodiscard]] Status stop() noexcept override;
    [[nodiscard]] Expected<f32, Error> translation_y(u64 identity) noexcept override;
    [[nodiscard]] Expected<PlayObservation, Error> observe() noexcept override;

private:
    PlaySession* session_;
    PlayConfiguration configuration_;
    scene::serialization::World* authored_;
};

/// A session in a process this one launched. `SeparateProcess`.
///
/// Every call is one line out and one line back over the child's standard streams. Nothing here
/// simulates: if the child is not running, every call fails, which is the property that makes the
/// comparison in `test_editor_play.cpp` load-bearing.
class ProcessPlayDriver final : public PlayDriver {
public:
    /// `process` must already have launched and must outlive this driver.
    explicit ProcessPlayDriver(RuntimeProcess& process, Allocator& allocator) noexcept;

    [[nodiscard]] PlayMode mode() const noexcept override { return PlayMode::SeparateProcess; }

    [[nodiscard]] Status enter() noexcept override;
    [[nodiscard]] Status tick() noexcept override;
    [[nodiscard]] Status pause() noexcept override;
    [[nodiscard]] Status resume() noexcept override;
    [[nodiscard]] Status step_tick() noexcept override;
    [[nodiscard]] Status step_frame() noexcept override;
    [[nodiscard]] Status stop() noexcept override;
    [[nodiscard]] Expected<f32, Error> translation_y(u64 identity) noexcept override;
    [[nodiscard]] Expected<PlayObservation, Error> observe() noexcept override;

    /// The process behind this driver, for a caller that needs to prove it is one.
    [[nodiscard]] RuntimeProcess& process() noexcept { return *process_; }

private:
    /// Send one command and require the reply to be exactly `ok`. A child that answered anything
    /// else — a refusal, a protocol error, a reply to a different request — fails, carrying what it
    /// said, rather than being read as success.
    [[nodiscard]] Status command(std::string_view request) noexcept;

    RuntimeProcess* process_;
    Allocator* allocator_;
};

}  // namespace cy::gameplay
