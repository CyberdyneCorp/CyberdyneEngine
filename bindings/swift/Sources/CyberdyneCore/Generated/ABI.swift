// GENERATED FILE — DO NOT EDIT.
//
// Written by tools/gen/swift/overlay_gen.py from src/abi/include/cy/abi/cy_abi.h, through the
// description tools/abi/abi_describe.py produces and tools/abi/abi_gate.py diffs against
// src/abi/abi_baseline.json. Edit the C header or the generator; regenerate with
// `just generate-swift`, and `just generate-swift --check` fails when this file is stale.

/// The ABI version this overlay was generated against.
///
/// A module built on this overlay is compiled against exactly this table. The check that matters at
/// run time is the module's own, in `CyberdyneKit`'s module entry point: the engine's
/// `header.table_size` must be at least the size recorded here, and `abiMajor` must be equal. That
/// is `native-abi`'s "Older engine, newer module", and refusing there is how it is reported without
/// aborting engine startup.
public enum ABI {
    public static let major: UInt32 = 1
    public static let minor: UInt32 = 0
    public static let patch: UInt32 = 0

    /// `sizeof(CyInterface)` as this overlay was generated. The engine may export a larger table —
    /// that is what append-only growth looks like from here — and may never export a smaller one.
    public static let interfaceTableSize: UInt32 = 256

    /// The entries this overlay knows, in the table's order. Written down so that a diagnostic can
    /// say *which* entry a mismatched table stops at rather than only that the sizes differ.
    public static let entryNames: [String] = [
        "log",
        "get_last_error",
        "get_last_error_code",
        "set_last_error",
        "var_make_string",
        "var_make_bytes",
        "var_clone",
        "var_release",
        "var_live_count",
        "engine_world",
        "world_create_entity",
        "world_destroy_entity",
        "world_entity_alive",
        "world_epoch",
        "world_register_component",
        "world_find_component",
        "world_add_component",
        "world_remove_component",
        "world_has_component",
        "world_borrow_component",
        "borrow_valid",
        "component_get_var",
        "component_set_var",
        "component_get_f32",
        "component_set_f32",
        "component_get_vec3",
        "component_set_vec3",
        "register_behaviour",
        "find_behaviour",
        "behaviour_generation",
    ]
}
