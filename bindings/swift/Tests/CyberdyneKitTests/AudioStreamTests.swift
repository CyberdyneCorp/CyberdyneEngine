// SPDX-License-Identifier: MIT
// AudioStreamTests.swift — ABI 1.18's streamed tracks through the `Audio` facade and `FakeEngine`.
// Issue #87.
//
// Each case checks that a facade call reaches its entry with the right arguments, converts the
// answer, and turns a refusal into `CyberdyneError.status` — or, for an ended stream's state, nil.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

/// What the fake entries saw.
private enum SeenStream {
    nonisolated(unsafe) static var play = CyAudioStreamPlay()
    nonisolated(unsafe) static var stopped: (CyAudioStream, Float) = (0, -1)
    nonisolated(unsafe) static var sought: (CyAudioStream, Double) = (0, -1)
    nonisolated(unsafe) static var volume: (CyAudioStream, Float, Float) = (0, -1, -1)
}

private let kTheme: CyAudioTrack = 1
private let kBattle: CyAudioTrack = 2
private let kLive: CyAudioStream = 40
private let kNext: CyAudioStream = 41

final class AudioStreamTests: XCTestCase {
    override func setUp() {
        super.setUp()
        SeenStream.play = CyAudioStreamPlay()
        FakeEngine.install { table in
            table.audio_find_track = { _, name, track in
                guard let name else { return CY_RESULT_INVALID_ARGUMENT }
                switch String(cString: name) {
                case "theme": track?.pointee = kTheme
                case "battle": track?.pointee = kBattle
                default: return FakeEngine.fail(CY_RESULT_NOT_FOUND, "audio_find_track: no track")
                }
                return CY_RESULT_OK
            }
            table.audio_stream_play = { _, play, stream in
                SeenStream.play = play?.pointee ?? CyAudioStreamPlay()
                stream?.pointee = SeenStream.play.crossfade_from == 0 ? kLive : kNext
                return CY_RESULT_OK
            }
            table.audio_stream_stop = { _, stream, fade in
                SeenStream.stopped = (stream, fade)
                return CY_RESULT_OK
            }
            table.audio_stream_seek = { _, stream, seconds in
                SeenStream.sought = (stream, seconds)
                return CY_RESULT_OK
            }
            table.audio_stream_set_volume = { _, stream, volume, fade in
                SeenStream.volume = (stream, volume, fade)
                return CY_RESULT_OK
            }
            table.audio_stream_state = { _, stream, state in
                guard stream == kLive else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "audio_stream_state: ended")
                }
                state?.pointee.flags = CY_AUDIO_STREAM_PLAYING | CY_AUDIO_STREAM_LOOPING
                state?.pointee.position_seconds = 12.5
                state?.pointee.duration_seconds = 32
                state?.pointee.gain = 0.75
                state?.pointee.underruns = 0
                return CY_RESULT_OK
            }
        }
    }

    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    func testStreamByNameSendsAWholeRequestWithAuthoredDefaults() throws {
        let stream = try Audio.stream("theme", options: StreamOptions(fadeIn: 2, start: 4))
        XCTAssertEqual(stream, AudioStream(raw: kLive))
        XCTAssertEqual(SeenStream.play.struct_size, UInt32(MemoryLayout<CyAudioStreamPlay>.size))
        XCTAssertEqual(SeenStream.play.track, kTheme)
        XCTAssertEqual(SeenStream.play.fade_seconds, 2)
        XCTAssertEqual(SeenStream.play.start_seconds, 4)
        XCTAssertEqual(SeenStream.play.volume, 0)  // zero is "as authored"
        XCTAssertEqual(SeenStream.play.bus, 0)
        XCTAssertEqual(SeenStream.play.crossfade_from, 0)
        XCTAssertEqual(SeenStream.play.flags, 0)
    }

    func testCrossfadeNamesTheOutgoingStreamAndUsesOneLength() throws {
        let theme = try XCTUnwrap(Audio.stream("theme"))
        let battle = try Audio.crossfade(
            from: theme, to: Audio.track("battle"), seconds: 3,
            options: StreamOptions(volume: 0.5, rawLevel: true))
        XCTAssertEqual(battle, AudioStream(raw: kNext))
        XCTAssertEqual(SeenStream.play.track, kBattle)
        XCTAssertEqual(SeenStream.play.crossfade_from, kLive)
        XCTAssertEqual(SeenStream.play.fade_seconds, 3)
        XCTAssertEqual(SeenStream.play.volume, 0.5)
        XCTAssertEqual(SeenStream.play.flags, CY_AUDIO_STREAM_RAW_LEVEL)
    }

    func testStopSeekAndVolumeReachTheirEntries() throws {
        let stream = AudioStream(raw: kLive)
        try stream.stop(fadeOut: 1.5)
        XCTAssertEqual(SeenStream.stopped.0, kLive)
        XCTAssertEqual(SeenStream.stopped.1, 1.5)
        try stream.seek(to: 30)
        XCTAssertEqual(SeenStream.sought.1, 30)
        try stream.setVolume(0.25, fade: 1)
        XCTAssertEqual(SeenStream.volume.1, 0.25)
        XCTAssertEqual(SeenStream.volume.2, 1)
    }

    func testStateConvertsAndAnEndedStreamIsNil() throws {
        let state = try XCTUnwrap(AudioStream(raw: kLive).state())
        XCTAssertEqual(state.position, 12.5)
        XCTAssertEqual(state.duration, 32)
        XCTAssertEqual(state.gain, 0.75)
        XCTAssertTrue(state.isPlaying)
        XCTAssertTrue(state.isLooping)
        XCTAssertNil(try AudioStream(raw: 99).state())
    }

    func testAnUnknownTrackThrowsNotFound() {
        XCTAssertThrowsError(try Audio.track("nope")) { error in
            guard case CyberdyneError.status(let status, _) = error else {
                return XCTFail("expected a status, got \(error)")
            }
            XCTAssertEqual(status, .notFound)
        }
    }
}
