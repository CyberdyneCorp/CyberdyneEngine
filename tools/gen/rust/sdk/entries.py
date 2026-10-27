"""What the description cannot say about a table entry, said once and checked. Task 2.3.

`tools/abi/abi_describe.py` records each entry's name and its signature with **parameter names
stripped** — deliberately, because a parameter name is not part of a C ABI and a baseline that
diffed on one would fail a review that renamed `entity` to `target`. A generated Rust API whose
parameters read `a0`, `a1`, `a2` is one nobody will call, and one whose doc comment cannot name what
it takes is one no agent can call either.

So the names live here, hand-written, beside the one other thing a C declaration does not carry:
whether an entry returning `CyResult` is reporting a **failure** (the wrapper yields
`Result<_, Status>`) or answering a **question** (`get_last_error_code` returns a status and never
fails, so a mechanical "returns CyResult therefore fallible" rule would make asking what went wrong
itself a fallible call).

THIS TABLE CANNOT DRIFT SILENTLY, WHICH IS THE ONLY REASON IT IS ACCEPTABLE. `validate()` refuses a
description containing an entry with no record here, refuses a record naming an entry that is not in
the description, and refuses a record whose parameter-name count differs from the signature's arity.
Appending an entry to `CyInterface` therefore stops generation until somebody names its parameters —
the same shape as the ABI gate itself: the change is legal, and it is not silent.

WHY THIS IS A SECOND COPY OF `tools/gen/swift/overlay/entries.py` RATHER THAN A SHARED ONE. The two
tables answer the same question for different languages and they are validated against the same
description, so neither can drift away from the ABI without failing generation. Sharing them would
couple two generators through a module that belongs to neither, and would put Swift's argument
labels — which are part of Swift's API and not of Rust's — into the Rust generator's vocabulary.
The duplication is checked; the coupling would not be.
"""

from __future__ import annotations

from dataclasses import dataclass


class EntryError(Exception):
    """The entry table and the ABI description disagree. Names every disagreement."""


@dataclass(frozen=True)
class Entry:
    """One table entry's Rust face.

    `result` is what the generated wrapper does with the C return value:
      `value`    — hand it back unchanged, in its FFI spelling. The safe layer decides what a null
                   pointer means, because C cannot say and a generator that trusted the header's
                   prose would be wrong on the day the prose was.
      `fallible` — it is a `CyResult`; the wrapper yields `Result<(), Status>`, `Ok` for
                   `CY_RESULT_OK` and the status otherwise.
    """

    parameters: tuple[str, ...]
    result: str = "value"
    #: One line for the generated doc comment. Written for a reader who has the SDK and not the C
    #: header — the same audience `editor-agent-interface` writes command descriptions for.
    doc: str = ""


# In the table's order, which is the order `CyInterface` declares and the order the gate protects.
# The list is not itself the contract — the description is — but keeping it in order makes a
# reviewer's diff of an append read as an append.
ENTRIES: dict[str, Entry] = {
    # diagnostics
    "log": Entry(("engine", "severity", "message"), doc="Emit a message through the engine's log."),
    "get_last_error": Entry((), doc="The last error message on this thread, or null."),
    "get_last_error_code": Entry((), doc="The last error code on this thread. Never fails."),
    "set_last_error": Entry(("result", "message"), doc="Set this thread's last error."),
    # values
    "var_make_string": Entry(("engine", "utf8", "length"), doc="A heap-backed string `CyVar`."),
    "var_make_bytes": Entry(("engine", "data", "size"), doc="A heap-backed byte-buffer `CyVar`."),
    "var_clone": Entry(("value",), doc="A copy that owns its own heap allocation, if any."),
    "var_release": Entry(("value",), doc="Release a `CyVar`'s heap allocation, if any."),
    "var_live_count": Entry(("engine",), doc="Live heap-backed `CyVar` count, for leak detection."),
    # the world
    "engine_world": Entry(("engine",), doc="The world bound to this engine, or null."),
    "world_create_entity": Entry(("world",), doc="Create an entity and return its identifier."),
    "world_destroy_entity": Entry(("world", "entity"), result="fallible",
                                  doc="Destroy an entity."),
    "world_entity_alive": Entry(("world", "entity"), doc="Whether an entity identifier is live."),
    "world_epoch": Entry(("world",), doc="The world's structural-change counter."),
    # components
    "world_register_component": Entry(("world", "descriptor"),
                                      doc="Register a component type; 0 on failure."),
    "world_find_component": Entry(("world", "name"),
                                  doc="Look a component type up by name; 0 when absent."),
    "world_add_component": Entry(("world", "entity", "component", "initial"), result="fallible",
                                 doc="Add a component, optionally copying an initial value."),
    "world_remove_component": Entry(("world", "entity", "component"), result="fallible",
                                    doc="Remove a component from an entity."),
    "world_has_component": Entry(("world", "entity", "component"),
                                 doc="Whether an entity has a component."),
    "world_borrow_component": Entry(("world", "entity", "component"),
                                    doc="Borrow a component's storage with the world's epoch."),
    "borrow_valid": Entry(("world", "borrow"),
                          doc="Whether a borrow predates the world's current epoch."),
    "component_get_var": Entry(("world", "entity", "component", "field", "into"),
                               result="fallible", doc="Read one field as a `CyVar`."),
    "component_set_var": Entry(("world", "entity", "component", "field", "value"),
                               result="fallible", doc="Write one field from a `CyVar`."),
    "component_get_f32": Entry(("world", "entity", "component", "field", "into"),
                               result="fallible", doc="Read one `f32` field."),
    "component_set_f32": Entry(("world", "entity", "component", "field", "value"),
                               result="fallible", doc="Write one `f32` field."),
    "component_get_vec3": Entry(("world", "entity", "component", "field", "into"),
                                result="fallible", doc="Read one three-float field."),
    "component_set_vec3": Entry(("world", "entity", "component", "field", "xyz"),
                                result="fallible", doc="Write one three-float field."),
    # behaviours
    "register_behaviour": Entry(("engine", "name", "vtable"),
                                doc="Register a behaviour type; null on failure."),
    "find_behaviour": Entry(("engine", "name"), doc="Look a behaviour type up by name."),
    "behaviour_generation": Entry(("behaviour",), doc="The reload generation a behaviour is from."),
    # 1.1: describing a world the caller did not build
    "world_component_count": Entry(("world",), doc="How many component types the world knows."),
    "world_component_info": Entry(("world", "component", "into"), result="fallible",
                                  doc="Describe a component type: size, alignment, field count."),
    "world_component_field": Entry(("world", "component", "field", "into"), result="fallible",
                                   doc="Describe one field of a component type."),
    # 1.1: the hierarchy
    "world_parent": Entry(("world", "entity"), doc="An entity's parent, or the null entity."),
    "world_set_parent": Entry(("world", "child", "parent"), result="fallible",
                              doc="Reparent an entity."),
    "world_child_count": Entry(("world", "entity"), doc="How many children an entity has."),
    "world_child": Entry(("world", "entity", "index"),
                         doc="One child, in the ECS's order rather than the authored order."),
    # 1.1: chunks
    "world_chunks": Entry(("world", "component", "into", "capacity", "count"), result="fallible",
                          doc="Enumerate a component's chunks; a null buffer asks for the count."),
    # 1.2: asynchronous editor backend services
    "service_open": Entry(("engine", "into"), result="fallible",
                          doc="Open an isolated editor-service session."),
    "service_close": Entry(("engine", "session"), doc="Close an editor-service session."),
    "service_submit": Entry(("engine", "session", "request"), result="fallible",
                            doc="Submit one versioned asynchronous editor-service request."),
    "service_cancel": Entry(("engine", "session", "request_id"), result="fallible",
                            doc="Cooperatively cancel an editor-service request."),
    "service_poll": Entry(("engine", "session", "event", "has_event"), result="fallible",
                          doc="Poll one editor-service event without blocking."),
    # 1.3: live scene VFX instances
    "vfx_effect_parameter_set": Entry(
        ("engine", "entity", "emitter", "parameter", "value"), result="fallible",
        doc="Set one exposed parameter on a playing scene effect entity."),
    "vfx_effect_parameter_get": Entry(
        ("engine", "entity", "emitter", "parameter", "into"), result="fallible",
        doc="Read one exposed parameter from a playing scene effect entity."),
    # 1.3: the game services (`add-swift-game-api`)
    "time_get": Entry(("engine", "into"), result="fallible",
                       doc="Read the engine clock and the current update phase."),
    "input_find_action": Entry(("engine", "name", "into"), result="fallible",
                                doc="Resolve an input action by its declared name."),
    "input_action_state": Entry(("engine", "user", "action", "into"), result="fallible",
                                 doc="Read one action's resolved state for an input user."),
    "input_action_state_by_name": Entry(("engine", "user", "name", "into"), result="fallible",
                                         doc="Read one action's resolved state by the action's name."),
    "input_pointer": Entry(("engine", "user", "into"), result="fallible",
                            doc="Read an input user's pointer in window pixels (frame update only)."),
    "input_modifiers": Entry(("engine", "user", "into"), result="fallible",
                              doc="Read the modifier keys an input user holds (frame update only)."),
    "input_find_context": Entry(("engine", "name", "into"), result="fallible",
                                 doc="Resolve a mapping context by its registered name."),
    "input_push_context": Entry(("engine", "user", "context", "priority"), result="fallible",
                                 doc="Push a mapping context onto an input user's stack."),
    "input_pop_context": Entry(("engine", "user", "context"), result="fallible",
                                doc="Remove a mapping context from an input user's stack."),
    "camera_active": Entry(("engine", "into"), result="fallible",
                            doc="The camera of the primary view."),
    "camera_view": Entry(("engine", "camera", "into"), result="fallible",
                          doc="A camera's evaluated pose, projection and viewport."),
    "camera_screen_to_ray": Entry(("engine", "camera", "screen", "into"), result="fallible",
                                   doc="The world ray under a window-pixel point."),
    "camera_world_to_screen": Entry(("engine", "camera", "points", "count", "into"),
                                    result="fallible",
                                    doc="Project world points to window pixels."),
    "camera_set_target": Entry(("engine", "camera", "target"), result="fallible",
                                doc="Point a camera's rig at a focus."),
    "camera_set_pose": Entry(("engine", "camera", "pose"), result="fallible",
                              doc="Override a camera's rig with an explicit pose."),
    "camera_clear_pose": Entry(("engine", "camera"), result="fallible",
                                doc="Hand a camera back to its rig."),
    "physics_raycast": Entry(("engine", "ray", "filter", "into", "has_hit"), result="fallible",
                              doc="The nearest physics hit along a ray."),
    "physics_raycast_all": Entry(("engine", "ray", "filter", "into", "capacity", "count"),
                                 result="fallible",
                                 doc="Every physics hit along a ray, nearest first."),
    "physics_shape_cast": Entry(("engine", "shape", "start", "direction", "max_distance", "filter",
                                 "into", "has_hit"),
                                result="fallible",
                                doc="Sweep a shape and report its first hit."),
    "physics_overlap": Entry(("engine", "shape", "pose", "filter", "into", "capacity", "count"),
                             result="fallible",
                             doc="Every entity whose body overlaps a shape."),
    "nav_find_path": Entry(("engine", "request", "into", "capacity", "result"), result="fallible",
                            doc="Find a navigation path now."),
    "nav_request_path": Entry(("engine", "request", "into"), result="fallible",
                               doc="Queue a deterministic asynchronous path search."),
    "nav_poll_path": Entry(("engine", "query", "into", "capacity", "result"), result="fallible",
                            doc="Poll a queued path search."),
    "nav_cancel_path": Entry(("engine", "query"), result="fallible",
                              doc="Cancel a queued path search."),
    "nav_agent_configure": Entry(("engine", "entity", "params"), result="fallible",
                                  doc="Make an entity a crowd agent, or update its parameters."),
    "nav_agent_move_to": Entry(("engine", "entity", "target"), result="fallible",
                                doc="Send a crowd agent to a target point."),
    "nav_agent_stop": Entry(("engine", "entity"), result="fallible",
                             doc="Stop a crowd agent where it is."),
    "nav_agent_state": Entry(("engine", "entity", "into"), result="fallible",
                              doc="A crowd agent's status, motion and target."),
    "audio_find_cue": Entry(("engine", "name", "into"), result="fallible",
                             doc="Resolve an audio cue by its authored name."),
    "audio_play": Entry(("engine", "play", "voice"), result="fallible",
                         doc="Start an audio voice."),
    "audio_stop": Entry(("engine", "voice", "fade_out"), result="fallible",
                         doc="Stop an audio voice, optionally fading out."),
    "audio_voice_playing": Entry(("engine", "voice"),
                                  doc="Whether an audio voice is still audible."),
    "audio_find_bus": Entry(("engine", "name", "into"), result="fallible",
                             doc="Resolve a mix bus by its authored name."),
    "audio_set_bus_volume": Entry(("engine", "bus", "volume", "fade"), result="fallible",
                                   doc="Set a mix bus's linear gain."),
    "spawn_resolve": Entry(("engine", "asset", "into"), result="fallible",
                            doc="Resolve a prefab or scene asset by its content path."),
    "spawn_instantiate": Entry(("engine", "prefab", "params", "root"), result="fallible",
                                doc="Instantiate a prefab and return its root entity."),
    "spawn_instantiate_many": Entry(("engine", "prefab", "parent", "poses", "count", "roots"),
                                    result="fallible",
                                    doc="Instantiate a prefab once per pose, in one batch."),
    "spawn_destroy": Entry(("engine", "root"), result="fallible",
                            doc="Destroy an entity and its whole subtree."),
    # 1.5: scheduled systems, the scene tree, rigid bodies and characters
    "register_system": Entry(("engine", "desc"), result="fallible",
                              doc="Schedule a module system into a stage by its access list."),
    "node_find": Entry(("engine", "from", "path", "into"), result="fallible",
                       doc="Resolve a node path, relative to a node or absolute."),
    "physics_apply_force": Entry(("engine", "entity", "force"), result="fallible",
                                 doc="Add a force at a body's centre of mass for the next step."),
    "physics_apply_impulse": Entry(("engine", "entity", "impulse", "point"), result="fallible",
                                   doc="Apply an impulse to a body, at a point or its centre."),
    "physics_apply_torque": Entry(("engine", "entity", "torque"), result="fallible",
                                  doc="Add a torque to a body for the next step."),
    "physics_set_velocity": Entry(("engine", "entity", "linear", "angular"), result="fallible",
                                  doc="Replace a body's linear and or angular velocity."),
    "physics_get_velocity": Entry(("engine", "entity", "linear", "angular"), result="fallible",
                                  doc="Read a body's linear and angular velocity."),
    "character_create": Entry(("engine", "entity", "desc"), result="fallible",
                              doc="Give an entity a capsule character controller."),
    "character_destroy": Entry(("engine", "entity"), result="fallible",
                               doc="Remove an entity's character controller and its body."),
    "character_move": Entry(("engine", "entity", "input"), result="fallible",
                            doc="Move a character by one fixed step."),
    "character_state": Entry(("engine", "entity", "into"), result="fallible",
                             doc="Read what a character's last move produced."),
    # 1.6: the runtime interface
    "ui_root": Entry(("engine", "into"), result="fallible",
                     doc="The screen's root interface element."),
    "ui_create": Entry(("engine", "parent", "desc", "into"), result="fallible",
                       doc="Create an interface element as the last child of a parent."),
    "ui_destroy": Entry(("engine", "element"), result="fallible",
                        doc="Destroy an interface element and its subtree."),
    "ui_set_layout": Entry(("engine", "element", "layout"), result="fallible",
                           doc="Replace an interface element's layout input."),
    "ui_set_style": Entry(("engine", "element", "style"), result="fallible",
                          doc="Replace what an interface element draws."),
    "ui_set_text": Entry(("engine", "element", "text", "colour", "pixel_scale"),
                         result="fallible", doc="Set a label's or a button's text."),
    "ui_set_image": Entry(("engine", "element", "page", "uv"), result="fallible",
                          doc="Set an image element's atlas page and rectangle."),
    "ui_set_progress": Entry(("engine", "element", "value"), result="fallible",
                             doc="Set a progress bar's fraction."),
    "ui_set_visibility": Entry(("engine", "element", "visibility"), result="fallible",
                               doc="Show, hide or collapse an interface element."),
    "ui_set_opacity": Entry(("engine", "element", "opacity"), result="fallible",
                            doc="Set an interface element's opacity."),
    "ui_element_rect": Entry(("engine", "element", "into"), result="fallible",
                             doc="Read where the last layout put an interface element."),
    "ui_hit_test": Entry(("engine", "position", "into"), result="fallible",
                         doc="The module's interface element under a window point."),
    "ui_focus": Entry(("engine", "into"), result="fallible",
                      doc="The module's interface element with keyboard focus."),
    "ui_set_focus": Entry(("engine", "element"), result="fallible",
                          doc="Move keyboard focus to a button, or clear it."),
    # 1.7: animation
    "animation_attach": Entry(("engine", "entity", "desc"), result="fallible",
                              doc="Give an entity an animator over a registered rig."),
    "animation_detach": Entry(("engine", "entity"), result="fallible",
                              doc="Remove an entity's animator."),
    "animation_play": Entry(("engine", "entity", "state", "crossfade"), result="fallible",
                            doc="Crossfade an animator to a state, or cut with zero seconds."),
    "animation_stop": Entry(("engine", "entity", "blend"), result="fallible",
                            doc="Blend an animator back to its program's entry state."),
    "animation_set_float": Entry(("engine", "entity", "parameter", "value"), result="fallible",
                                 doc="Set an animation program parameter."),
    "animation_set_bool": Entry(("engine", "entity", "parameter", "value"), result="fallible",
                                doc="Set an animation condition parameter to one or zero."),
    "animation_fire_trigger": Entry(("engine", "entity", "parameter"), result="fallible",
                                    doc="Raise an animation parameter for exactly one tick."),
    "animation_get_float": Entry(("engine", "entity", "parameter", "into"), result="fallible",
                                 doc="Read an animation program parameter."),
    "animation_state": Entry(("engine", "entity", "into"), result="fallible",
                             doc="Where an animator's state machine is."),
    "animation_events": Entry(("engine", "into", "capacity", "count"), result="fallible",
                              doc="The animation events of the ticks since the previous frame."),
    "animation_root_motion": Entry(("engine", "entity", "into"), result="fallible",
                                   doc="An animator's last root motion delta and running total."),
    "animation_take_root_motion": Entry(("engine", "entity", "into"), result="fallible",
                                        doc="Take an accumulating animator's root motion."),
    "animation_set_root_motion": Entry(("engine", "entity", "mode"), result="fallible",
                                       doc="Choose where an animator's root motion goes."),
    "animation_joint_pose": Entry(("engine", "entity", "joint", "into"), result="fallible",
                                  doc="A skeleton joint's world pose from the evaluated pose."),
    # 1.8: deterministic math and the lockstep path
    "component_get_fixed": Entry(("world", "entity", "component", "field", "into"),
                                 result="fallible", doc="Read a fixed-point field's raw value."),
    "component_set_fixed": Entry(("world", "entity", "component", "field", "value"),
                                 result="fallible", doc="Write a fixed-point field's raw value."),
    "detmath_kernel_version": Entry((), doc="The deterministic math kernel's version."),
    "detmath_sqrt": Entry(("x",), doc="The fixed-point square root, correctly rounded."),
    "detmath_sin": Entry(("angle",), doc="The fixed-point sine of a binary angle."),
    "detmath_cos": Entry(("angle",), doc="The fixed-point cosine of a binary angle."),
    "detmath_tan": Entry(("angle",), doc="The fixed-point tangent of a binary angle."),
    "detmath_atan": Entry(("x",), doc="The binary angle whose tangent is `x`."),
    "detmath_atan2": Entry(("y", "x"), doc="The binary angle of the vector (x, y)."),
    "detmath_asin": Entry(("x",), doc="The binary angle whose sine is `x`."),
    "detmath_acos": Entry(("x",), doc="The binary angle whose cosine is `x`."),
    "detmath_exp2": Entry(("x",), doc="Two to the fixed-point power `x`."),
    "detmath_log2": Entry(("x",), doc="The base-two logarithm of a fixed-point value."),
    "detmath_exp": Entry(("x",), doc="e to the fixed-point power `x`."),
    "detmath_log": Entry(("x",), doc="The natural logarithm of a fixed-point value."),
    "detmath_pow": Entry(("x", "y"), doc="`x` to the power `y`, for positive `x`."),
    "detmath_evaluate": Entry(("function", "x", "y", "into", "count"), result="fallible",
                              doc="A deterministic math function over a span of raw inputs."),
    "lockstep_enlist": Entry(("engine", "desc", "into"), result="fallible",
                             doc="Enlist a unit in the lockstep session before its first tick."),
    "lockstep_order": Entry(("engine", "order"), result="fallible",
                            doc="Record an order for the lockstep session's next tick."),
    "lockstep_unit": Entry(("engine", "unit", "into"), result="fallible",
                           doc="A lockstep unit's authoritative state."),
    "lockstep_status": Entry(("engine", "into"), result="fallible",
                             doc="The lockstep session's tick, log, hash and digest."),
    # 1.9: global illumination
    "gi_get_settings": Entry(("engine", "into"), result="fallible",
                             doc="The running world's global illumination settings."),
    "gi_set_settings": Entry(("engine", "settings"), result="fallible",
                             doc="Apply global illumination settings from the next frame."),
    "gi_volume_create": Entry(("engine", "desc", "into"), result="fallible",
                              doc="Place a GI volume."),
    "gi_volume_update": Entry(("engine", "volume", "desc"), result="fallible",
                              doc="Replace a GI volume's description."),
    "gi_volume_destroy": Entry(("engine", "volume"), result="fallible",
                               doc="Remove a GI volume."),
    "gi_object_set_class": Entry(("engine", "entity", "klass"), result="fallible",
                                 doc="Set an object's illumination class, or clear it."),
    "gi_object_class": Entry(("engine", "entity", "into"), result="fallible",
                             doc="The illumination class an object has now."),
    "gi_emission_set": Entry(("engine", "entity", "material", "mode"), result="fallible",
                             doc="What GI makes of an object's or a material's emission."),
    "gi_sample_irradiance": Entry(("engine", "position", "normal", "into"), result="fallible",
                                  doc="The indirect light arriving at a point."),
    "gi_status": Entry(("engine", "into"), result="fallible",
                       doc="What the global illumination did on the last frame."),
    # 1.10: fog of war
    "fog_set_vision": Entry(("engine", "entity", "vision"), result="fallible",
                            doc="Give an entity a vision: its team's sight to a reveal radius."),
    "fog_get_vision": Entry(("engine", "entity", "into"), result="fallible",
                            doc="The vision an entity was given."),
    "fog_cell": Entry(("engine", "team", "position", "into"), result="fallible",
                      doc="How a team knows the fog of war cell holding a position."),
    "fog_sight": Entry(("engine", "team", "entity", "into"), result="fallible",
                       doc="How a team sees an entity through the fog of war."),
    "fog_status": Entry(("engine", "into"), result="fallible",
                        doc="The fog of war grid and its last update."),
    # 1.11: lights
    "light_spawn": Entry(("engine", "desc", "into"), result="fallible",
                         doc="A light a game makes: a muzzle flash, a spark, a torch."),
    "light_update": Entry(("engine", "light", "desc"), result="fallible",
                          doc="Replace a live light's description, keeping its age."),
    "light_destroy": Entry(("engine", "light"), result="fallible",
                           doc="End a light now."),
    "light_alive": Entry(("engine", "light", "into"), result="fallible",
                         doc="Whether a light is alive."),
    "light_status": Entry(("engine", "into"), result="fallible",
                          doc="The light pool: live, capacity, spawned, expired, refused."),
    # 1.12: the widget toolkit
    "ui_load_document": Entry(("engine", "parent", "desc", "into"), result="fallible",
                              doc="Make a UI document's widgets under a parent."),
    "ui_load_stylesheet": Entry(("engine", "text", "size"), result="fallible",
                                doc="Replace the widget toolkit's style sheet."),
    "ui_find": Entry(("engine", "scope", "id", "into"), result="fallible",
                     doc="The first widget under a scope with an id."),
    "ui_create_widget": Entry(("engine", "parent", "desc", "into"), result="fallible",
                              doc="Make a widget under a parent."),
    "ui_widget_set_text": Entry(("engine", "widget", "text"), result="fallible",
                                doc="A widget's caption or text."),
    "ui_widget_text": Entry(("engine", "widget", "buffer", "capacity", "size"),
                            result="fallible", doc="A widget's text, into a buffer."),
    "ui_widget_set_value": Entry(("engine", "widget", "value"), result="fallible",
                                 doc="A slider's, progress bar's or toggle's value."),
    "ui_widget_value": Entry(("engine", "widget", "into"), result="fallible",
                             doc="A widget's value."),
    "ui_widget_set_selected": Entry(("engine", "widget", "index"), result="fallible",
                                    doc="A widget's selected item."),
    "ui_widget_selected": Entry(("engine", "widget", "into"), result="fallible",
                                doc="A widget's selected item."),
    "ui_widget_set_items": Entry(("engine", "widget", "items", "count"), result="fallible",
                                 doc="A radio group's, drop-down's or list's items."),
    "ui_widget_set_enabled": Entry(("engine", "widget", "enabled"), result="fallible",
                                   doc="Whether a widget takes input."),
    "ui_widget_set_classes": Entry(("engine", "widget", "classes"), result="fallible",
                                   doc="A widget's style classes."),
    "ui_widget_open": Entry(("engine", "widget", "open"), result="fallible",
                            doc="Open or close a drop-down or a dialog."),
    "ui_widget_animate": Entry(("engine", "widget", "animation"), result="fallible",
                               doc="Animate one of a widget's paint properties."),
    "ui_navigate": Entry(("engine", "action", "consumed"), result="fallible",
                         doc="Deliver a semantic interface action."),
    # 1.13: the first-person camera and the weapon view
    "first_person_configure": Entry(("engine", "camera", "desc"), result="fallible",
                                    doc="Give a camera first-person behaviours, or new settings."),
    "first_person_input": Entry(("engine", "camera", "input"), result="fallible",
                                doc="A first-person camera's input for this frame."),
    "first_person_kick": Entry(("engine", "camera", "pitch", "yaw"), result="fallible",
                               doc="A shot's kick into the recoil spring."),
    "first_person_reset": Entry(("engine", "camera", "yaw", "pitch"), result="fallible",
                                doc="A cut: the look set and every smoothed state at rest."),
    "first_person_state": Entry(("engine", "camera", "into"), result="fallible",
                                doc="What a first-person camera's last frame produced."),
    "first_person_release": Entry(("engine", "camera"), result="fallible",
                                  doc="Take a camera's first-person behaviours away."),
    "viewmodel_configure": Entry(("engine", "camera", "desc"), result="fallible",
                                 doc="Set a camera's weapon view: its layers, lens and shadows."),
    "viewmodel_get": Entry(("engine", "camera", "into"), result="fallible",
                           doc="A camera's weapon view as it was set."),
    # 1.14: foot placement and aim
    "animation_aim_at": Entry(("engine", "entity", "target", "point"), result="fallible",
                              doc="Point a character's aim at a place in the world."),
    "animation_foot_placement": Entry(("engine", "entity", "index", "into"), result="fallible",
                                      doc="What a character's foot placement stood on."),
    # 1.15: effects and decals
    "effect_find": Entry(("engine", "name", "into"), result="fallible",
                         doc="The effect the host registered under a name."),
    "effect_play": Entry(("engine", "effect", "play"), result="fallible",
                         doc="Play an effect once, fire-and-forget: a muzzle burst, sparks."),
    "decal_find_material": Entry(("engine", "name", "into"), result="fallible",
                                 doc="The decal material the host registered under a name."),
    "decal_spawn": Entry(("engine", "desc", "into"), result="fallible",
                         doc="A decal projected onto the world: an impact's mark."),
    "decal_status": Entry(("engine", "into"), result="fallible",
                          doc="The decal budget: live, capacity, spawned, evicted."),
}

RESULT_KINDS = frozenset({"value", "fallible"})


def signature(entry: dict) -> tuple[str, list[str]]:
    """Split `void(*)(CyEngine, uint32_t, const char*)` into its return type and its parameters."""
    text = entry["type"]
    marker = "(*)("
    index = text.find(marker)
    if index < 0 or not text.endswith(")"):
        raise EntryError(f"entry {entry['name']!r}: {text!r} is not a function-pointer signature")
    returns = text[:index].strip()
    inside = text[index + len(marker) : -1].strip()
    parameters = [item.strip() for item in inside.split(",")] if inside else []
    return returns, parameters


def function_entries(description: dict) -> list[dict]:
    """The table's function-pointer entries: everything but the `CyInterfaceHeader` at index 0."""
    return [entry for entry in description["table"]["entries"] if "(*)(" in entry["type"]]


def validate(description: dict) -> None:
    """Refuse a description this table does not exactly cover. Reports every problem, not the first.

    Reporting all of them matters when the ABI grows by several entries at once, which is what
    `native-abi`'s append-only rule makes the normal shape of a change: being told about one missing
    name, fixing it, and being told about the next is three round trips for one edit.
    """
    problems: list[str] = []
    described = {entry["name"]: entry for entry in function_entries(description)}

    for name, entry in described.items():
        record = ENTRIES.get(name)
        if record is None:
            problems.append(
                f"  {name}: in CyInterface, absent here. Add an Entry naming its parameters; the "
                f"signature is {entry['type']}")
            continue
        _, parameters = signature(entry)
        if len(record.parameters) != len(parameters):
            problems.append(
                f"  {name}: {len(record.parameters)} name(s) for {len(parameters)} parameter(s) "
                f"({entry['type']})")
        if record.result not in RESULT_KINDS:
            problems.append(f"  {name}: result kind {record.result!r} is not one of "
                            f"{sorted(RESULT_KINDS)}")
        if record.result == "fallible" and signature(entry)[0] != "CyResult":
            problems.append(f"  {name}: marked fallible but returns {signature(entry)[0]}")
        if not record.doc:
            problems.append(f"  {name}: no doc line. The SDK is read by callers who do not have "
                            f"cy_abi.h in front of them")

    for name in ENTRIES:
        if name not in described:
            problems.append(f"  {name}: named here, absent from CyInterface. Remove the Entry — the "
                            f"ABI is append-only, so this means it was never added or the parser "
                            f"stopped recognising it")

    if problems:
        raise EntryError(
            "tools/gen/rust/sdk/entries.py does not cover the ABI description:\n"
            + "\n".join(problems))
