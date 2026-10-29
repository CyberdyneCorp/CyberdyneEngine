// SPDX-License-Identifier: MIT
// Settings.swift — the settings screen's model: what it shows, what Apply hands the engine, and
// the player's file it is kept in.
//
// Every value goes to the ENGINE through its own entry: the resolution, the quality preset, the
// upscaler and fullscreen through ABI 1.17's `Video.apply`, which the host's renderer acts on; the
// global illumination quality through ABI 1.9's `Illumination.apply`; and each audio bus's volume
// through the document itself — the sliders are bound (`bind=volume.music`, two-way) to the
// host's buses, so a slider moved is the bus moved, with no game code between. What the game keeps
// is the file: `settings.cfg`, one `key=value` a line, written on Apply and read at start.

import CyberdyneKit

struct GameSettings: Equatable {
    var width: UInt32 = 1280
    var height: UInt32 = 720
    var quality: VideoQuality = .high
    var upscale: VideoUpscale = .native
    var fullscreen = false
    var gi: GIQuality = .high
    /// Bus name to linear volume.
    var volumes: [String: Float] = ["Master": 1, "Music": 1, "SFX": 1, "Voice": 1]

    static let buses = ["Master", "Music", "SFX", "Voice"]
    static let busWidgets = ["master", "music", "sfx", "voice"]

    /// The text the player's file holds.
    var text: String {
        var lines = [
            "width=\(width)", "height=\(height)", "quality=\(quality.rawValue)",
            "upscale=\(upscale.rawValue)", "fullscreen=\(fullscreen ? 1 : 0)", "gi=\(gi.rawValue)",
        ]
        for bus in Self.buses {
            lines.append("volume.\(bus)=\(volumes[bus] ?? 1)")
        }
        return lines.joined(separator: "\n") + "\n"
    }

    /// The settings a file says, over the defaults; a line this build does not know is skipped.
    init(text: String) {
        for line in text.split(separator: "\n") {
            let parts = line.split(separator: "=", maxSplits: 1).map(String.init)
            guard parts.count == 2 else { continue }
            apply(key: parts[0], value: parts[1])
        }
    }

    init() {}

    private mutating func apply(key: String, value: String) {
        switch key {
        case "width": width = UInt32(value) ?? width
        case "height": height = UInt32(value) ?? height
        case "quality": quality = UInt32(value).flatMap(VideoQuality.init(rawValue:)) ?? quality
        case "upscale": upscale = UInt32(value).flatMap(VideoUpscale.init(rawValue:)) ?? upscale
        case "fullscreen": fullscreen = value == "1"
        case "gi": gi = UInt32(value).flatMap(GIQuality.init(rawValue:)) ?? gi
        default:
            if key.hasPrefix("volume."), let volume = Float(value) {
                volumes[String(key.dropFirst("volume.".count))] = min(max(volume, 0), 1)
            }
        }
    }

    /// Hand every value to the engine.
    func applyToEngine() throws {
        var video = try Video.settings
        video.width = width
        video.height = height
        video.quality = quality
        video.upscale = upscale
        video.fullscreen = fullscreen
        try Video.apply(video)
        var illumination = try Illumination.settings
        illumination.quality = gi
        try Illumination.apply(illumination)
        for bus in Self.buses {
            try Audio.bus(bus).setVolume(volumes[bus] ?? 1)
        }
    }
}

/// The settings screen's widgets, and the model read from and written to them.
struct SettingsScreen {
    let document: UIDocument
    /// The display's resolutions, in the order the drop-down lists them.
    private(set) var modes: [(width: UInt32, height: UInt32)] = []

    init(document: UIDocument) {
        self.document = document
    }

    /// The resolution drop-down lists what the display offers, not what the document was made
    /// with.
    mutating func listModes() throws {
        modes = try Video.modes
        try document["resolution"].setItems(modes.map { "\($0.width)x\($0.height)" })
    }

    /// Show `settings` in the widgets.
    func show(_ settings: GameSettings) throws {
        let mode = modes.firstIndex { $0.width == settings.width && $0.height == settings.height }
        try document["resolution"].select(mode ?? 0)
        try document["quality"].select(Int(settings.quality.rawValue))
        try document["upscaler"].select(Int(settings.upscale.rawValue))
        try document["gi-quality"].select(Int(settings.gi.rawValue))
        try document["fullscreen"].setValue(settings.fullscreen ? 1 : 0)
    }

    /// What the widgets say now.
    func read(over base: GameSettings) throws -> GameSettings {
        var settings = base
        let index = try document["resolution"].selected
        if index >= 0 && index < modes.count {
            settings.width = modes[index].width
            settings.height = modes[index].height
        }
        settings.quality = VideoQuality(rawValue: UInt32(try document["quality"].selected)) ?? .high
        settings.upscale =
            VideoUpscale(rawValue: UInt32(try document["upscaler"].selected)) ?? .native
        settings.gi = GIQuality(rawValue: UInt32(try document["gi-quality"].selected)) ?? .high
        settings.fullscreen = try document["fullscreen"].value >= 0.5
        for (bus, widget) in zip(GameSettings.buses, GameSettings.busWidgets) {
            settings.volumes[bus] = try document["volume-\(widget)"].value
        }
        return settings
    }
}
