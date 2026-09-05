// Diagnostics.swift — logging and error surfacing from Swift game code. Task 3.8.
//
// `swift-scripting`: "Engine errors raised from Swift SHALL carry a Swift stack trace", and "when a
// Swift behaviour traps ... the engine SHALL report the error with the Swift stack trace in the
// editor log and SHALL disable that behaviour rather than terminating the process, in development
// builds".
//
// THE HALF THAT WORKS TODAY AND THE HALF THAT CANNOT. A THROWN Swift error is a value: the bridge
// catches it, logs it with the callback and behaviour that produced it, and disables the instance
// — that is implemented, in BehaviourBridge.swift. A Swift TRAP (a force-unwrap of nil, an array
// bound, an arithmetic overflow) is `Builtin.int_trap`; there is no catch for it in any Swift on any
// platform, and a module that traps takes the engine's process down with it. So the honest
// statement is: this package does not trap, and every failure it can produce is a thrown error. The
// scenario's "disable rather than terminate" holds for those; for a trap in *game* code it does not,
// and no arrangement of this file would make it.

import CyberdyneABI
import CyberdyneCore

/// The severities the engine's diagnostic system carries.
///
/// THESE THREE, AND EXACTLY THESE NUMBERS, BECAUSE THEY ARE THE WIRE FORMAT. `cy_abi.h` says of the
/// `log` entry: "`severity` is `cy::DiagnosticSeverity`'s value", and that enum
/// (`src/core/base/include/cy/core/base/diagnostic_sink.h`) has three enumerators — Info 0,
/// Warning 1, Error 2. The host's thunk clamps anything above Error TO Error, silently, which is
/// what makes a wrong number here invisible: it does not fail, it relabels.
///
/// A SIX-LEVEL VERSION OF THIS ENUM SHIPPED IN THIS FILE AND WAS WRONG. `trace`/`debug`/`info`/
/// `warning`/`error`/`fatal` numbered 0-5 put `.info` on the wire as 2, and every `Log.info` from a
/// behaviour arrived in the engine's log as `[error]` — visible in the reload suite's output as
/// `[error] abi: SwiftCounter restored ammo, health, label`, a line that is neither an error nor
/// wrong-looking enough for anybody to read twice. `integration.swift_reload`'s "a behaviour's
/// Log.info arrives as Info" case is the regression test.
///
/// A SECOND COPY, LIKE `SystemStage`, AND FOR THE SAME REASON: there is no `CySeverity` in
/// `cy_abi.h`, so the generated overlay cannot produce this and nothing checks it at compile time.
/// The check is the C++ case named above, which installs a diagnostic sink and reads the severity
/// the engine actually received. Appending a `CySeverity` enum to the ABI would let the generator
/// own this, and that is the fix rather than more care here.
public enum Severity: UInt32, Sendable, CaseIterable {
    case info = 0
    case warning = 1
    case error = 2
}

/// Logging into the engine's diagnostic stream.
///
/// Not `print`: a module's stdout is not the engine's log, is not captured by the editor, and is
/// block-buffered through a pipe — which the hot-reload spike paid for once, when a crash lost the
/// buffer and made a failure look as though it had happened earlier than it did.
public enum Log {
    // NO `trace` OR `debug`. The engine's sink has three levels; four names collapsing onto two
    // would be an API promising a fidelity the receiver does not have, which is the drift this
    // package argues against everywhere else. When `cy::DiagnosticSeverity` grows a level, this
    // grows with it — and the case in `integration.swift_reload` is what will notice.
    public static func info(_ message: @autoclosure () -> String) { write(.info, message()) }
    public static func warning(_ message: @autoclosure () -> String) { write(.warning, message()) }
    public static func error(_ message: @autoclosure () -> String) { write(.error, message()) }

    /// Where a message goes when the engine is not bound — before `cy_module_entry`, in a unit test
    /// of this package, after `cy_module_shutdown`. Collected rather than dropped so a test can
    /// assert on what a behaviour said.
    public nonisolated(unsafe) static var fallback: (Severity, String) -> Void = { _, _ in }

    static func write(_ severity: Severity, _ message: String) {
        guard let engine = Runtime.engine else {
            fallback(severity, message)
            return
        }
        message.withCString { text in
            engine.log(severity: severity.rawValue, message: text)
        }
    }
}

extension CyberdyneError {
    /// The engine's last error for this thread, as a Swift error. Used where an ABI entry reports
    /// failure by a sentinel return value — a null handle, an invalid component id — rather than by
    /// a `CyResult`, which is the only case the generated `check` does not already cover.
    static func fromLastError(_ fallback: Status = .unknown) -> CyberdyneError {
        guard let interface = Runtime.interface else { return .invalidHandle }
        let code = Status(rawValue: Int32(interface.getLastErrorCode().rawValue)) ?? fallback
        var message = ""
        if let text = interface.getLastError() {
            message = String(cString: text)
        }
        return .status(code == .ok ? fallback : code, message: message)
    }
}
