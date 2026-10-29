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
//
// STREAMED TRACKS (ABI 1.18, issue #87). Music and ambience are not cues: they stream, decoded ahead
// into a bounded ring, so a ten-minute track costs a ring and a decoder. The host registers cooked
// tracks by name; a game plays and crossfades them:
//
//     let theme = try Audio.stream("frontier.theme", options: StreamOptions(fadeIn: 2))
//     let battle = try Audio.crossfade(from: theme, to: Audio.track("battle"), seconds: 3)
//     try battle?.seek(to: 30)
//     try Audio.bus("Music").setVolume(0.5, fade: 1)   // a stream on the bus obeys it

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

// --- Streamed tracks (ABI 1.18) ------------------------------------------------------------------

/// A cooked track the host registered — music, an ambience bed — resolved once by name.
public struct AudioTrack: Hashable, Sendable {
    /// The engine's handle.
    public let raw: CyAudioTrack

    /// Wrap a handle the engine handed out.
    public init(raw: CyAudioTrack) {
        self.raw = raw
    }
}

/// Where a stream is, as the engine last mixed it.
public struct StreamState: Equatable, Sendable {
    /// Where the listener is in the track, in seconds.
    public var position: Double
    /// One pass of the track, in seconds.
    public var duration: Double
    /// The gain after the stream's fades.
    public var gain: Float
    /// Mixer blocks its ring ran dry in, each played as silence.
    public var underruns: UInt32
    /// Sounding, or about to be once its ring is primed.
    public var isPlaying: Bool
    /// The track loops.
    public var isLooping: Bool
}

/// One playing stream of a track.
public struct AudioStream: Hashable, Sendable {
    /// The engine's handle: an ended stream's never names a later one.
    public let raw: CyAudioStream

    /// Wrap a handle the engine handed out.
    public init(raw: CyAudioStream) {
        self.raw = raw
    }

    /// Fade out over `fadeOut` seconds (zero: now) and stop. Idempotent.
    public func stop(fadeOut: Float = 0) throws {
        try GameServices.engine().audioStreamStop(stream: raw, fadeOut: fadeOut)
    }

    /// Jump to `seconds` into the track. `.notFound` once the stream has ended, `.outOfRange` past
    /// the track's end.
    public func seek(to seconds: Double) throws {
        try GameServices.engine().audioStreamSeek(stream: raw, seconds: seconds)
    }

    /// Ramp the stream's linear gain over `fade` seconds.
    public func setVolume(_ volume: Float, fade: Float = 0) throws {
        try GameServices.engine().audioStreamSetVolume(stream: raw, volume: volume, fade: fade)
    }

    /// The stream's state, or nil once it has ended.
    public func state() throws -> StreamState? {
        var state = CyAudioStreamState()
        state.struct_size = UInt32(MemoryLayout<CyAudioStreamState>.size)
        do {
            try GameServices.engine().audioStreamState(stream: raw, into: &state)
        } catch CyberdyneError.status(let status, _) where status == .notFound {
            return nil
        }
        return StreamState(
            position: state.position_seconds, duration: state.duration_seconds, gain: state.gain,
            underruns: state.underruns,
            isPlaying: (state.flags & CY_AUDIO_STREAM_PLAYING) != 0,
            isLooping: (state.flags & CY_AUDIO_STREAM_LOOPING) != 0)
    }
}

/// How a track is streamed. Every property left at its default means "as authored".
public struct StreamOptions: Equatable, Sendable {
    /// Linear gain; nil is the authored level.
    public var volume: Float?
    /// The bus to route through; Master when nil.
    public var bus: AudioBus?
    /// Seconds to fade in over.
    public var fadeIn: Float
    /// Where in the track to begin, in seconds.
    public var start: Double
    /// Ignore the loudness gain the importer measured, and play the track at its own level.
    public var rawLevel: Bool

    /// Options for one stream.
    public init(
        volume: Float? = nil, bus: AudioBus? = nil, fadeIn: Float = 0, start: Double = 0,
        rawLevel: Bool = false
    ) {
        self.volume = volume
        self.bus = bus
        self.fadeIn = fadeIn
        self.start = start
        self.rawLevel = rawLevel
    }
}

extension Audio {
    /// Resolve a streamed track by the name the host registered it under. `.notFound` when there
    /// is none, `.unavailable` when the host has no stream player.
    public static func track(_ name: String) throws -> AudioTrack {
        let engine = try GameServices.engine()
        var track: CyAudioTrack = 0
        try name.withCString { try engine.audioFindTrack(name: $0, into: &track) }
        return AudioTrack(raw: track)
    }

    /// Stream `track`. Nil when nothing started because the tick is a resimulation.
    @discardableResult
    public static func stream(
        _ track: AudioTrack, options: StreamOptions = StreamOptions()
    ) throws
        -> AudioStream?
    {
        try start(streamRequest(track, options, from: nil))
    }

    /// Stream the track named `name`.
    @discardableResult
    public static func stream(
        _ name: String, options: StreamOptions = StreamOptions()
    ) throws
        -> AudioStream?
    {
        try stream(track(name), options: options)
    }

    /// Start `track` fading in over `seconds` while `from` fades out over the same seconds, both
    /// equal-power and starting on the same sample; `from` stops itself at the end. A nil or ended
    /// `from` makes this a plain fade-in.
    @discardableResult
    public static func crossfade(
        from: AudioStream?, to track: AudioTrack, seconds: Float,
        options: StreamOptions = StreamOptions()
    ) throws -> AudioStream? {
        var faded = options
        faded.fadeIn = seconds
        return try start(streamRequest(track, faded, from: from))
    }

    static func streamRequest(
        _ track: AudioTrack, _ options: StreamOptions, from: AudioStream?
    )
        -> CyAudioStreamPlay
    {
        var play = CyAudioStreamPlay()
        play.struct_size = UInt32(MemoryLayout<CyAudioStreamPlay>.size)
        play.track = track.raw
        play.flags = options.rawLevel ? CY_AUDIO_STREAM_RAW_LEVEL : 0
        play.bus = options.bus?.raw ?? 0
        play.crossfade_from = from?.raw ?? 0
        play.volume = options.volume ?? 0  // zero is "as authored"
        play.fade_seconds = options.fadeIn
        play.start_seconds = options.start
        return play
    }

    static func start(_ play: CyAudioStreamPlay) throws -> AudioStream? {
        var request = play
        var stream: CyAudioStream = 0
        try GameServices.engine().audioStreamPlay(play: &request, stream: &stream)
        return stream == 0 ? nil : AudioStream(raw: stream)
    }
}
