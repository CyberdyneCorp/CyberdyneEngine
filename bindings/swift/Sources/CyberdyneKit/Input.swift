// SPDX-License-Identifier: MIT
// Input.swift — ABI 1.3's `input_*` entries, as a game writes them. `add-swift-game-api`.
//
// OWNER: implementer A (input and camera). See openspec/changes/add-swift-game-api/design.md.
//
//     if try Input.action("select").justPressed { … }        // F or U
//     let pointer = try Input.pointer()                         // U only: device state
//     if try Input.modifiers().contains(.shift) { … }          // U only
//
// WHICH PHASE MAY READ WHAT. Action state is what the input server resolved for the tick, so a
// fixed step (`onFixedUpdate`) reads it and replays identically. The pointer and the modifier keys
// are the devices as they are now: a fixed step that read them would not replay, so the engine
// refuses them there and these throw `CyberdyneError.status(.permissionDenied, …)`. The RTS shape
// is: read the pointer in `onUpdate`, turn a click into an order the game records, act on the
// order in `onFixedUpdate`.
//
// A name lookup per call is a hash and a short scan in the engine; a behaviour that reads the same
// action every tick resolves it once with `Input.find(action:)` and keeps the `InputAction`, which
// stays valid across a hot reload.

import CyberdyneABI
import CyberdyneCore

/// A declared action, resolved once by name. Valid for the life of the process.
public struct InputAction: Hashable, Sendable {
    /// The engine's dense action index.
    public let raw: CyInputAction

    /// Wrap an index the engine handed out.
    public init(raw: CyInputAction) {
        self.raw = raw
    }

    /// This action's state for `user`, as resolved for the current tick.
    public func state(user: UInt32 = 0) throws -> ActionState {
        var state = ActionState.emptyRaw()
        try GameServices.engine().inputActionState(user: user, action: raw, into: &state)
        return ActionState(state)
    }
}

/// One action's state for one input user, as the input server resolved it for a tick.
public struct ActionState: Equatable, Sendable {
    /// The CY_INPUT_ACTION_* flags.
    public var flags: UInt32
    /// The value: `x` for a button or one axis, `x y` for two axes, `x y z` for three.
    public var value: Vec3
    /// How many times the action went down during the tick.
    public var pressCount: Int
    /// How many times it came up during the tick. A press and a release inside one tick are 1 and 1.
    public var releaseCount: Int
    /// The tick this state was resolved for.
    public var tick: UInt64

    /// From the ABI's spelling.
    public init(_ raw: CyInputActionState) {
        flags = raw.flags
        value = Vec3(raw.value)
        pressCount = Int(raw.press_count)
        releaseCount = Int(raw.release_count)
        tick = raw.tick
    }

    /// Actuated at the end of the tick.
    public var pressed: Bool { flags & CY_INPUT_ACTION_PRESSED != 0 }
    /// Went down at least once during the tick.
    public var justPressed: Bool { flags & CY_INPUT_ACTION_JUST_PRESSED != 0 }
    /// Came up at least once during the tick.
    public var justReleased: Bool { flags & CY_INPUT_ACTION_JUST_RELEASED != 0 }
    /// The action's trigger fired during the tick.
    public var triggered: Bool { flags & CY_INPUT_ACTION_TRIGGERED != 0 }
    /// The value came from injection or replay rather than a device.
    public var synthetic: Bool { flags & CY_INPUT_ACTION_SYNTHETIC != 0 }
    /// A one-axis action's value.
    public var scalar: Float { value.x }
    /// A two-axis action's value.
    public var axis2: Vec2 { Vec2(x: value.x, y: value.y) }

    /// A zeroed `CyInputActionState` that asks for this build's size.
    static func emptyRaw() -> CyInputActionState {
        var raw = CyInputActionState()
        raw.struct_size = UInt32(MemoryLayout<CyInputActionState>.size)
        return raw
    }
}

/// Pointer buttons, as CY_INPUT_BUTTON_* bits.
public struct PointerButtons: OptionSet, Hashable, Sendable {
    /// The CY_INPUT_BUTTON_* bits, as the engine reports them.
    public let rawValue: UInt32
    /// Buttons from raw CY_INPUT_BUTTON_* bits.
    public init(rawValue: UInt32) { self.rawValue = rawValue }

    /// The primary button.
    public static let left = PointerButtons(rawValue: CY_INPUT_BUTTON_LEFT)
    /// The secondary button.
    public static let right = PointerButtons(rawValue: CY_INPUT_BUTTON_RIGHT)
    /// The middle button or wheel press.
    public static let middle = PointerButtons(rawValue: CY_INPUT_BUTTON_MIDDLE)
    /// The first extra button, usually "back".
    public static let extra1 = PointerButtons(rawValue: CY_INPUT_BUTTON_EXTRA1)
    /// The second extra button, usually "forward".
    public static let extra2 = PointerButtons(rawValue: CY_INPUT_BUTTON_EXTRA2)
}

/// Modifier keys, as CY_INPUT_MOD_* bits.
public struct Modifiers: OptionSet, Hashable, Sendable {
    /// The CY_INPUT_MOD_* bits, as the engine reports them.
    public let rawValue: UInt32
    /// Modifiers from raw CY_INPUT_MOD_* bits.
    public init(rawValue: UInt32) { self.rawValue = rawValue }

    /// Either Shift key.
    public static let shift = Modifiers(rawValue: CY_INPUT_MOD_SHIFT)
    /// Either Control key.
    public static let ctrl = Modifiers(rawValue: CY_INPUT_MOD_CTRL)
    /// Either Alt (Option) key.
    public static let alt = Modifiers(rawValue: CY_INPUT_MOD_ALT)
    /// Either Super key: Command on macOS, Windows elsewhere.
    public static let `super` = Modifiers(rawValue: CY_INPUT_MOD_SUPER)
}

/// A user's pointer during one frame update: where it is, what it holds, and what changed since
/// the previous frame update.
public struct Pointer: Equatable, Sendable {
    /// The CY_INPUT_POINTER_* flags.
    public var flags: UInt32
    /// Buttons held now.
    public var buttons: PointerButtons
    /// Buttons that went down since the previous frame update.
    public var pressed: PointerButtons
    /// Buttons that came up since the previous frame update.
    public var released: PointerButtons
    /// Window pixels, origin top-left, +y down — the space `Camera.ray(through:)` takes.
    public var position: Vec2
    /// Pixels moved since the previous frame update.
    public var delta: Vec2
    /// Wheel notches since the previous frame update; `x` is horizontal.
    public var wheel: Vec2

    /// From the ABI's spelling.
    public init(_ raw: CyInputPointer) {
        flags = raw.flags
        buttons = PointerButtons(rawValue: raw.buttons)
        pressed = PointerButtons(rawValue: raw.buttons_pressed)
        released = PointerButtons(rawValue: raw.buttons_released)
        position = Vec2(raw.position)
        delta = Vec2(raw.delta)
        wheel = Vec2(raw.wheel)
    }

    /// The user has a pointing device. When false every other field is zero.
    public var isPresent: Bool { flags & CY_INPUT_POINTER_PRESENT != 0 }
    /// The pointer is inside the window's client area.
    public var isInWindow: Bool { flags & CY_INPUT_POINTER_IN_WINDOW != 0 }
    /// An interface layer holds pointer focus; a world click should usually be ignored.
    public var isOverUI: Bool { flags & CY_INPUT_POINTER_OVER_UI != 0 }
}

/// A registered mapping context, resolved once by name.
public struct InputContext: Hashable, Sendable {
    /// The engine's context handle.
    public let raw: CyInputContext

    /// Wrap a handle the engine handed out.
    public init(raw: CyInputContext) {
        self.raw = raw
    }
}

/// The input server's gameplay-facing verbs. Every call throws the engine's refusal as
/// `CyberdyneError.status`.
public enum Input {
    /// Resolve an action by its declared name. `NOT_FOUND` when nothing is declared under it.
    public static func find(action name: String) throws -> InputAction {
        let engine = try GameServices.engine()
        var action = CyInputAction.max  // CY_INPUT_ACTION_INVALID; the cast macro does not import
        try name.withCString { try engine.inputFindAction(name: $0, into: &action) }
        return InputAction(raw: action)
    }

    /// The named action's state for `user`, as resolved for the current tick. Callable in a fixed
    /// step: it is simulation state.
    public static func action(_ name: String, user: UInt32 = 0) throws -> ActionState {
        let engine = try GameServices.engine()
        var state = ActionState.emptyRaw()
        try name.withCString {
            try engine.inputActionStateByName(user: user, name: $0, into: &state)
        }
        return ActionState(state)
    }

    /// `user`'s pointer. Frame update only: a fixed step is refused.
    public static func pointer(user: UInt32 = 0) throws -> Pointer {
        var pointer = CyInputPointer()
        pointer.struct_size = UInt32(MemoryLayout<CyInputPointer>.size)
        try GameServices.engine().inputPointer(user: user, into: &pointer)
        return Pointer(pointer)
    }

    /// The modifier keys `user` holds now. Frame update only: a fixed step is refused.
    public static func modifiers(user: UInt32 = 0) throws -> Modifiers {
        var modifiers: UInt32 = 0
        try GameServices.engine().inputModifiers(user: user, into: &modifiers)
        return Modifiers(rawValue: modifiers)
    }

    /// Resolve a mapping context by its registered name. `NOT_FOUND` when none is.
    public static func context(_ name: String) throws -> InputContext {
        let engine = try GameServices.engine()
        var context: CyInputContext = 0  // CY_INPUT_CONTEXT_NULL
        try name.withCString { try engine.inputFindContext(name: $0, into: &context) }
        return InputContext(raw: context)
    }

    /// Push `context` onto `user`'s stack; the higher priority wins. Effective from the next tick.
    /// `ALREADY_EXISTS` when it is already there.
    public static func push(_ context: InputContext, priority: Int32 = 0, user: UInt32 = 0) throws {
        try GameServices.engine().inputPushContext(
            user: user, context: context.raw, priority: priority)
    }

    /// Remove `context` from `user`'s stack wherever it sits. `NOT_FOUND` when it is not there.
    public static func pop(_ context: InputContext, user: UInt32 = 0) throws {
        try GameServices.engine().inputPopContext(user: user, context: context.raw)
    }
}
