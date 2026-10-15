// CStrings.swift — names the engine keeps a pointer to. Tasks 3.2, 3.5.
//
// --- THE BUG THIS FILE EXISTS BECAUSE OF ----------------------------------------------------------
//
// `cy_abi.h` states the rule at the component descriptor: "Names must outlive the registration,
// because the engine stores the pointer — a string literal or a static buffer, never a temporary."
// It holds for `register_behaviour` too: `CyBehaviourType_T::name` is a `const char*` the host keeps.
//
// The obvious Swift for it is wrong, and wrong in the way that costs an afternoon:
//
//     type.behaviourName.withCString { name in interface.registerBehaviour(..., name: name, ...) }
//
// `withCString` guarantees the pointer only for the duration of the closure. The registration
// succeeded, `register_behaviour` returned a handle, nothing reported a failure — and
// `find_behaviour("SwiftCounter")` then compared against freed memory and answered null. There is no
// crash and no diagnostic; the behaviour is simply not there.
//
// So every string this module hands the engine to KEEP goes through here, and is never freed. That
// is not a leak in any sense that matters: a registration lives as long as its module image, and
// under the reload model (cy/abi/module.h) an image is never unloaded. Freeing one would be freeing
// something the engine may still read.

/// A NUL-terminated copy of a Swift string, alive for the rest of the process.
final class RetainedCString {
    private let storage: UnsafeMutablePointer<CChar>

    /// Every string this image has handed the engine. The array is what keeps them alive; nothing
    /// ever removes an entry, because nothing ever makes it safe to.
    private nonisolated(unsafe) static var retained: [RetainedCString] = []

    private init(_ value: String) {
        let utf8 = Array(value.utf8CString)
        storage = UnsafeMutablePointer<CChar>.allocate(capacity: utf8.count)
        storage.update(from: utf8, count: utf8.count)
    }

    var pointer: UnsafePointer<CChar> { UnsafePointer(storage) }

    /// A pointer to `value` that the engine may keep.
    static func make(_ value: String) -> UnsafePointer<CChar> {
        let copy = RetainedCString(value)
        retained.append(copy)
        return copy.pointer
    }

    /// How many names this image has handed over. Read by tests, so that "nothing is freed" is a
    /// number rather than a claim.
    static var count: Int { retained.count }
}

/// Objects the engine keeps a pointer to as `user_data`: a behaviour's registration record and a
/// system's. The same rule as the strings above, for the same reason. Nothing makes releasing one
/// safe, so the array that holds them is never emptied.
///
/// HELD HERE, NOT BY AN UNBALANCED `passRetained`. The two say the same thing to ARC, but only this
/// one says it to LeakSanitizer. A +1 that nothing balances is reachable only through the engine's
/// copy of the pointer, and the host frees that copy when it tears down the registry. So every
/// Swift suite under AddressSanitizer reported each record as a direct leak at exit. A static array
/// is a root the leak checker scans, so the records are reachable for as long as they are alive,
/// which is the rest of the process.
enum RetainedObjects {
    private nonisolated(unsafe) static var retained: [AnyObject] = []

    /// An unretained pointer to `object`, which this image now keeps alive for the process.
    static func opaque(_ object: AnyObject) -> UnsafeMutableRawPointer {
        retained.append(object)
        return Unmanaged.passUnretained(object).toOpaque()
    }

    /// How many objects this image has handed over. Read by tests, like `RetainedCString.count`.
    static var count: Int { retained.count }
}
