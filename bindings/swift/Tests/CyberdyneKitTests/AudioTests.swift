// SPDX-License-Identifier: MIT
// AudioTests.swift — the `Audio` facade through `FakeEngine`. `add-swift-game-api`.
//
// OWNER: implementer C. Each case checks that a facade call reaches its entry with the right
// arguments, converts the answer, and turns a refusal into `CyberdyneError.status`.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

/// What the fake entries saw.
private enum Seen {
    nonisolated(unsafe) static var play = CyAudioPlay()
    nonisolated(unsafe) static var stopped: (CyAudioVoice, Float) = (0, -1)
    nonisolated(unsafe) static var bus: (CyAudioBus, Float, Float) = (0, -1, -1)
    nonisolated(unsafe) static var answerVoice: CyAudioVoice = 0x2_0000_0007
}

private let kExplosion: CyAudioCue = 0x1_0000_0003
private let kMusic: CyAudioBus = 0x1_0000_0001

final class AudioTests: XCTestCase {
    override func setUp() {
        super.setUp()
        Seen.play = CyAudioPlay()
        Seen.answerVoice = 0x2_0000_0007
        FakeEngine.install { table in
            table.audio_find_cue = { _, name, cue in
                guard let name, String(cString: name) == "explosion" else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "audio_find_cue: no cue")
                }
                cue?.pointee = kExplosion
                return CY_RESULT_OK
            }
            table.audio_find_bus = { _, name, bus in
                guard let name, String(cString: name) == "Music" else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "audio_find_bus: no bus")
                }
                bus?.pointee = kMusic
                return CY_RESULT_OK
            }
            table.audio_play = { _, play, voice in
                Seen.play = play?.pointee ?? CyAudioPlay()
                voice?.pointee = Seen.answerVoice
                return CY_RESULT_OK
            }
            table.audio_stop = { _, voice, fade in
                Seen.stopped = (voice, fade)
                return CY_RESULT_OK
            }
            table.audio_voice_playing = { _, voice in voice == 0x2_0000_0007 }
            table.audio_set_bus_volume = { _, bus, volume, fade in
                guard volume >= 0 else {
                    return FakeEngine.fail(CY_RESULT_OUT_OF_RANGE, "below zero")
                }
                Seen.bus = (bus, volume, fade)
                return CY_RESULT_OK
            }
        }
    }

    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    func testPlayByNameAtAPositionSendsASpatialRequest() throws {
        let voice = try Audio.play("explosion", at: Vec3(x: 1, y: 2, z: 3))
        XCTAssertEqual(voice, Voice(raw: 0x2_0000_0007))
        XCTAssertEqual(Seen.play.struct_size, UInt32(MemoryLayout<CyAudioPlay>.size))
        XCTAssertEqual(Seen.play.cue, kExplosion)
        XCTAssertEqual(Seen.play.flags, CY_AUDIO_PLAY_SPATIAL)
        XCTAssertEqual(Seen.play.position.0, 1)
        XCTAssertEqual(Seen.play.position.2, 3)
        // Nothing asked for: zero, which the engine reads as "as authored".
        XCTAssertEqual(Seen.play.volume, 0)
        XCTAssertEqual(Seen.play.pitch, 0)
        XCTAssertEqual(Seen.play.bus, 0)
    }

    func testOptionsReachTheRequest() throws {
        let music = try Audio.bus("Music")
        try Audio.play(
            Audio.cue("explosion"),
            options: PlayOptions(volume: 0.5, pitch: 1.25, loop: true, bus: music, fadeIn: 2))
        XCTAssertEqual(Seen.play.flags, CY_AUDIO_PLAY_LOOP)
        XCTAssertEqual(Seen.play.volume, 0.5)
        XCTAssertEqual(Seen.play.pitch, 1.25)
        XCTAssertEqual(Seen.play.bus, kMusic)
        XCTAssertEqual(Seen.play.fade_in_seconds, 2)
    }

    func testPlayAttachedFollowsTheEntityWithTheOffset() throws {
        let cue = try Audio.cue("explosion")
        try Audio.play(cue, attachedTo: Entity(bits: 42), offset: Vec3(x: 0, y: 3, z: 0))
        XCTAssertEqual(Seen.play.flags, CY_AUDIO_PLAY_ATTACH)
        XCTAssertEqual(Seen.play.attach_to, 42)
        XCTAssertEqual(Seen.play.position.1, 3)
    }

    func testANullVoiceIsNil() throws {
        Seen.answerVoice = 0  // resimulating, or no free voice
        XCTAssertNil(try Audio.play("explosion"))
    }

    func testAVoiceReportsPlayingAndStopsWithItsFade() throws {
        let voice = try XCTUnwrap(try Audio.play("explosion"))
        XCTAssertTrue(voice.isPlaying)
        XCTAssertFalse(Voice(raw: 9).isPlaying)
        try voice.stop(fadeOut: 0.5)
        XCTAssertEqual(Seen.stopped.0, voice.raw)
        XCTAssertEqual(Seen.stopped.1, 0.5)
    }

    func testABusVolumeReachesItsEntryAndARefusalThrows() throws {
        let music = try Audio.bus("Music")
        try music.setVolume(0.25, fade: 1.5)
        XCTAssertEqual(Seen.bus.0, kMusic)
        XCTAssertEqual(Seen.bus.1, 0.25)
        XCTAssertEqual(Seen.bus.2, 1.5)
        XCTAssertThrowsError(try music.setVolume(-1)) { error in
            XCTAssertEqual(error as? CyberdyneError, .status(.outOfRange, message: "below zero"))
        }
    }

    func testAnUnknownCueThrowsNotFound() {
        XCTAssertThrowsError(try Audio.play("nope", at: Vec3())) { error in
            XCTAssertEqual(
                error as? CyberdyneError, .status(.notFound, message: "audio_find_cue: no cue"))
        }
    }

    func testWithNoEngineAVoiceIsNotPlaying() {
        FakeEngine.uninstall()
        XCTAssertFalse(Voice(raw: 0x2_0000_0007).isPlaying)
        XCTAssertThrowsError(try Audio.cue("explosion"))
    }
}
