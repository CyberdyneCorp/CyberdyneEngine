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
    public static let minor: UInt32 = 5
    public static let patch: UInt32 = 0

    /// `sizeof(CyInterface)` as this overlay was generated. The engine may export a larger table —
    /// that is what append-only growth looks like from here — and may never export a smaller one.
    public static let interfaceTableSize: UInt32 = 768

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
        "world_component_count",
        "world_component_info",
        "world_component_field",
        "world_parent",
        "world_set_parent",
        "world_child_count",
        "world_child",
        "world_chunks",
        "service_open",
        "service_close",
        "service_submit",
        "service_cancel",
        "service_poll",
        "time_get",
        "input_find_action",
        "input_action_state",
        "input_action_state_by_name",
        "input_pointer",
        "input_modifiers",
        "input_find_context",
        "input_push_context",
        "input_pop_context",
        "camera_active",
        "camera_view",
        "camera_screen_to_ray",
        "camera_world_to_screen",
        "camera_set_target",
        "camera_set_pose",
        "camera_clear_pose",
        "physics_raycast",
        "physics_raycast_all",
        "physics_shape_cast",
        "physics_overlap",
        "nav_find_path",
        "nav_request_path",
        "nav_poll_path",
        "nav_cancel_path",
        "nav_agent_configure",
        "nav_agent_move_to",
        "nav_agent_stop",
        "nav_agent_state",
        "audio_find_cue",
        "audio_play",
        "audio_stop",
        "audio_voice_playing",
        "audio_find_bus",
        "audio_set_bus_volume",
        "spawn_resolve",
        "spawn_instantiate",
        "spawn_instantiate_many",
        "spawn_destroy",
        "vfx_effect_parameter_set",
        "vfx_effect_parameter_get",
        "register_system",
        "node_find",
        "physics_apply_force",
        "physics_apply_impulse",
        "physics_apply_torque",
        "physics_set_velocity",
        "physics_get_velocity",
        "character_create",
        "character_destroy",
        "character_move",
        "character_state",
    ]
}
