// SPDX-License-Identifier: MIT
// Audio.swift — ABI 1.3's `audio_*` entries, as a game writes them. `add-swift-game-api`.
//
// OWNER: implementer C (audio, spawning and time). See openspec/changes/add-swift-game-api/design.md.
//
//     try Audio.play("explosion", at: impact)                       // positional, fire and forget
//     let engine = try Audio.play(Audio.cue("tank.engine"), attachedTo: tank, loop: true)
//     try engine?.stop(fadeOut: 0.5)
//     try Audio.bus("Music").setVolume(0.2, fade: 2)
//
// AUDIO IS PRESENTATION. Every call is allowed in every phase, including a fixed step, because
// nothing in the simulation reads a sound back. Two consequences a game must respect:
//
//   * While a tick is being RESIMULATED (rollback, replay catch-up) `play`, `stop` and
//     `setVolume` succeed and do nothing, so a rolled-back explosion is not heard twice. `play` then
//     returns nil.
//   * A `Voice` is not simulation state. A fixed step must not branch on `isPlaying`; a replay
//     would not see the same answer.
//
// Resolving a cue by name is a lookup in the engine. A behaviour that plays the same sound often
// resolves it once with `Audio.cue(_:)` and keeps the `AudioCue`; handles survive a hot reload.

import CyberdyneABI
import CyberdyneCore

/// An authored cue, resolved once by name.
public struct AudioCue: Hashable, Sendable {
    /// The engine's handle.
    public let raw: CyAudioCue

    /// Wrap a handle the engine handed out.
    public init(raw: CyAudioCue) {
        self.raw = raw
    }
}

/// A bus of the mix graph — "Master", "Music", "SFX" — resolved once by name.
public struct AudioBus: Hashable, Sendable {
    /// The engine's handle.
    public let raw: CyAudioBus

    /// Wrap a handle the engine handed out.
    public init(raw: CyAudioBus) {
        self.raw = raw
    }

    /// Set the bus's linear gain, ramping over `fade` seconds (zero: now). Throws `.outOfRange`
    /// below zero.
    public func setVolume(_ volume: Float, fade: Float = 0) throws {
        try GameServices.engine().audioSetBusVolume(bus: raw, volume: volume, fade: fade)
    }
}

/// One playing instance of a cue.
public struct Voice: Hashable, Sendable {
    /// The engine's handle, generation included: a voice that ended never names a later one.
    public let raw: CyAudioVoice

    /// Wrap a handle the engine handed out.
    public init(raw: CyAudioVoice) {
        self.raw = raw
    }

    /// Whether the voice is still audible. False once it ended, and false with no engine bound.
    public var isPlaying: Bool {
        guard let engine = try? GameServices.engine() else { return false }
        return engine.audioVoicePlaying(voice: raw)
    }

    /// Stop, fading over `fadeOut` seconds (zero: now). Stopping a voice that already ended is
    /// fine: it is idempotent.
    public func stop(fadeOut: Float = 0) throws {
        try GameServices.engine().audioStop(voice: raw, fadeOut: fadeOut)
    }
}

/// How a sound is played. Every property left `nil` means "as authored".
public struct PlayOptions: Equatable, Sendable {
    /// Linear gain. Zero reads as nil — the ABI spells "as authored" as zero — so silence a sound
    /// with its bus, or by not playing it.
    public var volume: Float?
    /// Playback-rate ratio.
    public var pitch: Float?
    /// Loop until stopped.
    public var loop: Bool
    /// The bus to route through; the authored one (or Master) when nil.
    public var bus: AudioBus?
    /// Seconds to fade in over.
    public var fadeIn: Float

    /// Options for one `play`.
    public init(
        volume: Float? = nil, pitch: Float? = nil, loop: Bool = false, bus: AudioBus? = nil,
        fadeIn: Float = 0
    ) {
        self.volume = volume
        self.pitch = pitch
        self.loop = loop
        self.bus = bus
        self.fadeIn = fadeIn
    }
}

/// The audio server's gameplay-facing verbs. Every call throws the engine's refusal as
/// `CyberdyneError.status`.
public enum Audio {
    /// Resolve a cue by its authored name. `.notFound` when there is none.
    public static func cue(_ name: String) throws -> AudioCue {
        let engine = try GameServices.engine()
        var cue: CyAudioCue = 0
        try name.withCString { try engine.audioFindCue(name: $0, into: &cue) }
        return AudioCue(raw: cue)
    }

    /// Resolve a mix bus by its authored name. `.notFound` when there is none.
    public static func bus(_ name: String) throws -> AudioBus {
        let engine = try GameServices.engine()
        var bus: CyAudioBus = 0
        try name.withCString { try engine.audioFindBus(name: $0, into: &bus) }
        return AudioBus(raw: bus)
    }

    /// Play `cue` without a position — music, interface sounds. Nil when nothing started: the tick
    /// is a resimulation, or the mixer had no voice to give.
    @discardableResult
    public static func play(_ cue: AudioCue, options: PlayOptions = PlayOptions()) throws -> Voice?
    {
        try start(request(cue, options))
    }

    /// Play `cue` at a world position.
    @discardableResult
    public static func play(
        _ cue: AudioCue, at position: Vec3, options: PlayOptions = PlayOptions()
    ) throws -> Voice? {
        var play = request(cue, options)
        play.flags |= CY_AUDIO_PLAY_SPATIAL
        play.position = position.tuple
        return try start(play)
    }

    /// Play `cue` following `entity`, `offset` from it in its own frame. `.notFound` when the
    /// entity is not alive.
    @discardableResult
    public static func play(
        _ cue: AudioCue, attachedTo entity: Entity, offset: Vec3 = Vec3(),
        options: PlayOptions = PlayOptions()
    ) throws -> Voice? {
        var play = request(cue, options)
        play.flags |= CY_AUDIO_PLAY_ATTACH
        play.attach_to = entity.bits
        play.position = offset.tuple
        return try start(play)
    }

    /// Play the cue named `name` at a world position: `Audio.play("explosion", at: impact)`.
    @discardableResult
    public static func play(
        _ name: String, at position: Vec3, options: PlayOptions = PlayOptions()
    ) throws -> Voice? {
        try play(cue(name), at: position, options: options)
    }

    /// Play the cue named `name` without a position.
    @discardableResult
    public static func play(_ name: String, options: PlayOptions = PlayOptions()) throws -> Voice? {
        try play(cue(name), options: options)
    }

    static func request(_ cue: AudioCue, _ options: PlayOptions) -> CyAudioPlay {
        var play = CyAudioPlay()
        play.struct_size = UInt32(MemoryLayout<CyAudioPlay>.size)
        play.cue = cue.raw
        play.flags = options.loop ? CY_AUDIO_PLAY_LOOP : 0
        play.bus = options.bus?.raw ?? 0
        play.volume = options.volume ?? 0  // zero is "as authored"
        play.pitch = options.pitch ?? 0
        play.fade_in_seconds = options.fadeIn
        return play
    }

    static func start(_ play: CyAudioPlay) throws -> Voice? {
        var request = play
        var voice: CyAudioVoice = 0
        try GameServices.engine().audioPlay(play: &request, voice: &voice)
        return voice == 0 ? nil : Voice(raw: voice)
    }
}
