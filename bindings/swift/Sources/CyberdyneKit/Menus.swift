// SPDX-License-Identifier: MIT
// Menus.swift — ABI 1.17: what a game's menus need beyond the toolkit, as a game writes it.
//
//     // Controls: rebind Jump on the keyboard, through the input system's own flow.
//     let jump = try Input.find(action: "Game.Jump")
//     try jump.rebind(scheme: .keyboardMouse, conflict: .swap)
//     …                                                   // every frame while the dialog is open:
//     let state = try Rebinding.poll()
//     if state.status == .applied { try UserData.write("bindings.cyinputprofile",
//                                                      text: try InputProfile.save()) }
//     let shown = try jump.binding(scheme: .keyboardMouse)   // "keyboard/f"
//
//     // Graphics: the display's resolutions, a quality preset and the upscaler.
//     var video = try Video.settings
//     video.quality = .high
//     video.upscale = .balanced
//     try Video.apply(video)
//
//     // At start: the player's choices back.
//     if let text = try UserData.readText("bindings.cyinputprofile") { try InputProfile.load(text) }
//
// Every call is presentation and refused in `onFixedUpdate` with `.permissionDenied`; a host that
// bound no display or no user-data directory answers `.unavailable`.

import CyberdyneABI
import CyberdyneCore

/// Copy a fixed-size C character array the importer gave as a tuple into a `String`.
func string<T>(fromTuple tuple: T) -> String {
    withUnsafeBytes(of: tuple) { bytes in
        let characters = bytes.prefix { $0 != 0 }
        return String(decoding: characters, as: UTF8.self)
    }
}

/// Where a rebind is, as `input_rebind_poll` answered.
public struct RebindState: Equatable, Sendable {
    /// Idle, listening, applied, refused for a conflict, or cancelled.
    public var status: RebindStatus
    /// The action being rebound.
    public var action: InputAction
    /// Its scheme.
    public var scheme: InputScheme
    /// Its slot.
    public var slot: UInt32
    /// The control the player pressed, as a path: "keyboard/f", "gamepad/north".
    public var control: String
    /// The action that held that control, when one did.
    public var conflict: String

    /// From the ABI's spelling.
    public init(_ raw: CyInputRebind) {
        status = RebindStatus(rawValue: raw.status) ?? .idle
        action = InputAction(raw: raw.action)
        scheme = InputScheme(rawValue: raw.scheme) ?? .keyboardMouse
        slot = raw.slot
        control = string(fromTuple: raw.control)
        conflict = string(fromTuple: raw.conflict)
    }
}

extension InputAction {
    /// Begin rebinding this action's `slot`-th control on `scheme` for `user`. The next control
    /// the player presses on that scheme becomes the action's, under `conflict`; Escape (or a
    /// gamepad's Back) cancels. Poll `Rebinding.poll(user:)` for where it is.
    public func rebind(
        scheme: InputScheme, slot: UInt32 = 0, conflict: InputConflict = .swap, user: UInt32 = 0
    ) throws {
        try GameServices.engine().inputRebindBegin(
            user: user, action: raw, scheme: scheme.rawValue, slot: slot,
            conflict: conflict.rawValue)
    }

    /// The control this action's `slot`-th binding on `scheme` has now — the player's or the
    /// authored one — as a path; nil when it has no such binding.
    public func binding(scheme: InputScheme, slot: UInt32 = 0, user: UInt32 = 0) throws -> String? {
        let engine = try GameServices.engine()
        var buffer = [CChar](repeating: 0, count: 64)
        var size: UInt32 = 0
        do {
            try buffer.withUnsafeMutableBufferPointer {
                try engine.inputBinding(
                    user: user, action: raw, scheme: scheme.rawValue, slot: slot,
                    buffer: $0.baseAddress, capacity: UInt32($0.count), size: &size)
            }
        } catch CyberdyneError.status(.notFound, _) {
            return nil
        }
        return String(
            decoding: buffer.prefix(Int(size)).map { UInt8(bitPattern: $0) }, as: UTF8.self)
    }
}

/// The rebind a user has open.
public enum Rebinding {
    /// Where `user`'s rebind is.
    public static func poll(user: UInt32 = 0) throws -> RebindState {
        var raw = CyInputRebind()
        raw.struct_size = UInt32(MemoryLayout<CyInputRebind>.size)
        try GameServices.engine().inputRebindPoll(user: user, into: &raw)
        return RebindState(raw)
    }

    /// Cancel it; nothing happens when none is listening.
    public static func cancel(user: UInt32 = 0) throws {
        try GameServices.engine().inputRebindCancel(user: user)
    }
}

/// A user's rebinding profile: every control the player changed, over the authored bindings.
public enum InputProfile {
    /// The profile as text, for `UserData.write`. Empty when the player changed nothing.
    public static func save(user: UInt32 = 0) throws -> String {
        let engine = try GameServices.engine()
        var size: UInt32 = 0
        try engine.inputProfileSave(user: user, buffer: nil, capacity: 0, size: &size)
        var buffer = [CChar](repeating: 0, count: Int(size) + 1)
        try buffer.withUnsafeMutableBufferPointer {
            try engine.inputProfileSave(
                user: user, buffer: $0.baseAddress, capacity: UInt32($0.count), size: &size)
        }
        return String(
            decoding: buffer.prefix(Int(size)).map { UInt8(bitPattern: $0) }, as: UTF8.self)
    }

    /// Put back a profile `save` wrote. Throws `.invalidArgument` naming a line that does not
    /// read, having changed nothing.
    public static func load(_ text: String, user: UInt32 = 0) throws {
        let engine = try GameServices.engine()
        let bytes = Array(text.utf8)
        try bytes.withUnsafeBufferPointer { buffer in
            try buffer.withMemoryRebound(to: CChar.self) {
                try engine.inputProfileLoad(
                    user: user, text: $0.baseAddress, size: UInt64(bytes.count))
            }
        }
    }

    /// Every authored binding again.
    public static func reset(user: UInt32 = 0) throws {
        try GameServices.engine().inputProfileReset(user: user)
    }
}

/// The display settings a settings screen changes.
public struct VideoSettings: Equatable, Sendable {
    /// The output resolution's width: one of `Video.modes`.
    public var width: UInt32
    /// The output resolution's height.
    public var height: UInt32
    /// The quality preset.
    public var quality: VideoQuality
    /// The temporal upscaler's mode.
    public var upscale: VideoUpscale
    /// Whether the window covers the display.
    public var fullscreen: Bool
    /// Whether presentation waits for the display's refresh.
    public var vsync: Bool

    /// Every field.
    public init(
        width: UInt32, height: UInt32, quality: VideoQuality = .high,
        upscale: VideoUpscale = .native, fullscreen: Bool = false, vsync: Bool = true
    ) {
        self.width = width
        self.height = height
        self.quality = quality
        self.upscale = upscale
        self.fullscreen = fullscreen
        self.vsync = vsync
    }

    /// From the ABI's spelling.
    public init(_ raw: CyVideoSettings) {
        width = raw.width
        height = raw.height
        quality = VideoQuality(rawValue: raw.quality) ?? .high
        upscale = VideoUpscale(rawValue: raw.upscale) ?? .native
        fullscreen = raw.flags & CY_VIDEO_FULLSCREEN != 0
        vsync = raw.flags & CY_VIDEO_VSYNC != 0
    }

    /// In the ABI's spelling.
    public var raw: CyVideoSettings {
        var out = CyVideoSettings()
        out.struct_size = UInt32(MemoryLayout<CyVideoSettings>.size)
        out.width = width
        out.height = height
        out.quality = quality.rawValue
        out.upscale = upscale.rawValue
        out.flags = (fullscreen ? CY_VIDEO_FULLSCREEN : 0) | (vsync ? CY_VIDEO_VSYNC : 0)
        return out
    }
}

/// The display, as ABI 1.17's `video_*` reach it.
public enum Video {
    /// The settings in effect.
    public static var settings: VideoSettings {
        get throws {
            var raw = CyVideoSettings()
            raw.struct_size = UInt32(MemoryLayout<CyVideoSettings>.size)
            try GameServices.engine().videoGetSettings(into: &raw)
            return VideoSettings(raw)
        }
    }

    /// Apply settings from the next frame. Throws `.notFound` for a resolution the display does
    /// not offer.
    public static func apply(_ settings: VideoSettings) throws {
        var raw = settings.raw
        try GameServices.engine().videoSetSettings(settings: &raw)
    }

    /// The resolutions the display offers, as width and height.
    public static var modes: [(width: UInt32, height: UInt32)] {
        get throws {
            let engine = try GameServices.engine()
            var count: UInt32 = 0
            try engine.videoModes(modes: nil, capacity: 0, count: &count)
            var modes = [CyVideoMode](repeating: CyVideoMode(), count: Int(count))
            try modes.withUnsafeMutableBufferPointer {
                try engine.videoModes(modes: $0.baseAddress, capacity: count, count: &count)
            }
            return modes.prefix(Int(count)).map { (width: $0.width, height: $0.height) }
        }
    }
}

/// The player's files, in the game's user-data directory.
public enum UserData {
    /// Write a file whole. `name` is one plain file name.
    public static func write(_ name: String, bytes: [UInt8]) throws {
        let engine = try GameServices.engine()
        try name.withCString { name in
            try bytes.withUnsafeBytes {
                try engine.userDataWrite(name: name, bytes: $0.baseAddress, size: UInt64($0.count))
            }
        }
    }

    /// Write a file of text.
    public static func write(_ name: String, text: String) throws {
        try write(name, bytes: Array(text.utf8))
    }

    /// A file's bytes, or nil when there is none.
    public static func read(_ name: String) throws -> [UInt8]? {
        let engine = try GameServices.engine()
        return try name.withCString { name -> [UInt8]? in
            var size: UInt64 = 0
            do {
                try engine.userDataRead(name: name, buffer: nil, capacity: 0, size: &size)
            } catch CyberdyneError.status(.notFound, _) {
                return nil
            }
            var bytes = [UInt8](repeating: 0, count: Int(size))
            try bytes.withUnsafeMutableBytes {
                try engine.userDataRead(
                    name: name, buffer: $0.baseAddress, capacity: size, size: &size)
            }
            return bytes
        }
    }

    /// A file's text, or nil when there is none.
    public static func readText(_ name: String) throws -> String? {
        try read(name).map { String(decoding: $0, as: UTF8.self) }
    }
}

extension Widget {
    /// The tooltip the widget shows when the pointer rests on it; empty removes it.
    public func setTooltip(_ text: String) throws {
        let engine = try GameServices.engine()
        try text.withCString { try engine.uiWidgetSetTooltip(widget: raw, text: $0) }
    }
}
