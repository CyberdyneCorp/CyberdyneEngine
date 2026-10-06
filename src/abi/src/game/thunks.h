// SPDX-License-Identifier: MIT
// The thunks behind ABI 1.3's game-service entries. `add-swift-game-api`. PRIVATE to cy_abi.
//
// `src/abi/src/interface.cpp` puts these in the table, in the order cy/abi/cy_abi.h declares the
// entries; each group is defined in its own file so the three implementers of the change edit
// disjoint files (design.md, "File ownership"):
//
//   time_thunks.cpp        C       audio_thunks.cpp       C       spawn_thunks.cpp       C
//   input_thunks.cpp       A       camera_thunks.cpp      A
//   physics_thunks.cpp     B       navigation_thunks.cpp  B
//
// Every thunk follows cy/abi/game/services.h's six steps before it reaches a backend, and is total:
// no input, however malformed, crashes or throws. Their signatures are the table's, exactly.

#pragma once

#include <cy/abi/cy_abi.h>

namespace cy::abi::game {

// --- time (C) -------------------------------------------------------------------------------
CyResult time_get(CyEngine engine, CyTime* out_time);

// --- input (A) ------------------------------------------------------------------------------
CyResult input_find_action(CyEngine engine, const char* name, CyInputAction* out_action);
CyResult input_action_state(CyEngine engine, uint32_t user, CyInputAction action,
                            CyInputActionState* out_state);
CyResult input_action_state_by_name(CyEngine engine, uint32_t user, const char* name,
                                    CyInputActionState* out_state);
CyResult input_pointer(CyEngine engine, uint32_t user, CyInputPointer* out_pointer);
CyResult input_modifiers(CyEngine engine, uint32_t user, uint32_t* out_modifiers);
CyResult input_find_context(CyEngine engine, const char* name, CyInputContext* out_context);
CyResult input_push_context(CyEngine engine, uint32_t user, CyInputContext context,
                            int32_t priority);
CyResult input_pop_context(CyEngine engine, uint32_t user, CyInputContext context);

// --- camera (A) -----------------------------------------------------------------------------
CyResult camera_active(CyEngine engine, CyCamera* out_camera);
CyResult camera_view(CyEngine engine, CyCamera camera, CyCameraView* out_view);
CyResult camera_screen_to_ray(CyEngine engine, CyCamera camera, const float* screen_xy,
                              CyRay* out_ray);
CyResult camera_world_to_screen(CyEngine engine, CyCamera camera, const float* points_xyz,
                                uint32_t count, CyScreenPoint* out_points);
CyResult camera_set_target(CyEngine engine, CyCamera camera, const CyCameraTarget* target);
CyResult camera_set_pose(CyEngine engine, CyCamera camera, const CyPose* pose);
CyResult camera_clear_pose(CyEngine engine, CyCamera camera);

// --- physics queries (B) --------------------------------------------------------------------
CyResult physics_raycast(CyEngine engine, const CyRay* ray, const CyQueryFilter* filter,
                         CyPhysicsHit* out_hit, bool* out_has_hit);
CyResult physics_raycast_all(CyEngine engine, const CyRay* ray, const CyQueryFilter* filter,
                             CyPhysicsHit* out_hits, uint32_t capacity, uint32_t* out_count);
CyResult physics_shape_cast(CyEngine engine, const CyShape* shape, const CyPose* start,
                            const float* direction, float max_distance, const CyQueryFilter* filter,
                            CyPhysicsHit* out_hit, bool* out_has_hit);
CyResult physics_overlap(CyEngine engine, const CyShape* shape, const CyPose* pose,
                         const CyQueryFilter* filter, CyEntity* out_entities, uint32_t capacity,
                         uint32_t* out_count);

// --- navigation (B) -------------------------------------------------------------------------
CyResult nav_find_path(CyEngine engine, const CyNavPathRequest* request, float* out_points_xyz,
                       uint32_t capacity, CyNavPathResult* out_result);
CyResult nav_request_path(CyEngine engine, const CyNavPathRequest* request, CyNavQuery* out_query);
CyResult nav_poll_path(CyEngine engine, CyNavQuery query, float* out_points_xyz, uint32_t capacity,
                       CyNavPathResult* out_result);
CyResult nav_cancel_path(CyEngine engine, CyNavQuery query);
CyResult nav_agent_configure(CyEngine engine, CyEntity entity, const CyNavAgentParams* params);
CyResult nav_agent_move_to(CyEngine engine, CyEntity entity, const float* target_xyz);
CyResult nav_agent_stop(CyEngine engine, CyEntity entity);
CyResult nav_agent_state(CyEngine engine, CyEntity entity, CyNavAgentState* out_state);

// --- audio (C) ------------------------------------------------------------------------------
CyResult audio_find_cue(CyEngine engine, const char* name, CyAudioCue* out_cue);
CyResult audio_play(CyEngine engine, const CyAudioPlay* play, CyAudioVoice* out_voice);
CyResult audio_stop(CyEngine engine, CyAudioVoice voice, float fade_out_seconds);
bool audio_voice_playing(CyEngine engine, CyAudioVoice voice);
CyResult audio_find_bus(CyEngine engine, const char* name, CyAudioBus* out_bus);
CyResult audio_set_bus_volume(CyEngine engine, CyAudioBus bus, float volume, float fade_seconds);

// --- spawning (C) ---------------------------------------------------------------------------
CyResult spawn_resolve(CyEngine engine, const char* asset, CyPrefab* out_prefab);
CyResult spawn_instantiate(CyEngine engine, CyPrefab prefab, const CySpawnParams* params,
                           CyEntity* out_root);
CyResult spawn_instantiate_many(CyEngine engine, CyPrefab prefab, CyEntity parent,
                                const CyPose* poses, uint32_t count, CyEntity* out_roots);
CyResult spawn_destroy(CyEngine engine, CyEntity root);

// --- 1.5: scheduled systems, the scene tree, bodies and characters (`add-swift-m12-gaps`) ----
CyResult register_system(CyEngine engine, const CySystemDesc* desc);
CyResult node_find(CyEngine engine, CyEntity from, const char* path, CyEntity* out_entity);
CyResult physics_apply_force(CyEngine engine, CyEntity entity, const float* force_xyz);
CyResult physics_apply_impulse(CyEngine engine, CyEntity entity, const float* impulse_xyz,
                               const float* point_xyz);
CyResult physics_apply_torque(CyEngine engine, CyEntity entity, const float* torque_xyz);
CyResult physics_set_velocity(CyEngine engine, CyEntity entity, const float* linear_xyz,
                              const float* angular_xyz);
CyResult physics_get_velocity(CyEngine engine, CyEntity entity, float* out_linear_xyz,
                              float* out_angular_xyz);
CyResult character_create(CyEngine engine, CyEntity entity, const CyCharacterDesc* desc);
CyResult character_destroy(CyEngine engine, CyEntity entity);
CyResult character_move(CyEngine engine, CyEntity entity, const CyCharacterInput* input);
CyResult character_state(CyEngine engine, CyEntity entity, CyCharacterState* out_state);

// --- 1.6: the runtime interface ------------------------------------------------------------------
CyResult ui_root(CyEngine engine, CyUiElement* out_root);
CyResult ui_create(CyEngine engine, CyUiElement parent, const CyUiElementDesc* desc,
                   CyUiElement* out_element);
CyResult ui_destroy(CyEngine engine, CyUiElement element);
CyResult ui_set_layout(CyEngine engine, CyUiElement element, const CyUiLayout* layout);
CyResult ui_set_style(CyEngine engine, CyUiElement element, const CyUiStyle* style);
CyResult ui_set_text(CyEngine engine, CyUiElement element, const char* utf8, uint32_t colour,
                     uint32_t pixel_scale);
CyResult ui_set_image(CyEngine engine, CyUiElement element, uint32_t atlas_page,
                      const float* uv_xywh);
CyResult ui_set_progress(CyEngine engine, CyUiElement element, float value);
CyResult ui_set_visibility(CyEngine engine, CyUiElement element, uint32_t visibility);
CyResult ui_set_opacity(CyEngine engine, CyUiElement element, float opacity);
CyResult ui_element_rect(CyEngine engine, CyUiElement element, float* out_rect_xywh);
CyResult ui_hit_test(CyEngine engine, const float* position_xy, CyUiElement* out_element);
CyResult ui_focus(CyEngine engine, CyUiElement* out_element);
CyResult ui_set_focus(CyEngine engine, CyUiElement element);

// --- 1.7: animation ------------------------------------------------------------------------------
CyResult animation_attach(CyEngine engine, CyEntity entity, const CyAnimatorDesc* desc);
CyResult animation_detach(CyEngine engine, CyEntity entity);
CyResult animation_play(CyEngine engine, CyEntity entity, const char* state,
                        float crossfade_seconds);
CyResult animation_stop(CyEngine engine, CyEntity entity, float blend_seconds);
CyResult animation_set_float(CyEngine engine, CyEntity entity, const char* parameter, float value);
CyResult animation_set_bool(CyEngine engine, CyEntity entity, const char* parameter, bool value);
CyResult animation_fire_trigger(CyEngine engine, CyEntity entity, const char* parameter);
CyResult animation_get_float(CyEngine engine, CyEntity entity, const char* parameter,
                             float* out_value);
CyResult animation_state(CyEngine engine, CyEntity entity, CyAnimatorState* out_state);
CyResult animation_events(CyEngine engine, CyAnimationEvent* out_events, uint32_t capacity,
                          uint32_t* out_count);
CyResult animation_root_motion(CyEngine engine, CyEntity entity, CyRootMotion* out_motion);
CyResult animation_take_root_motion(CyEngine engine, CyEntity entity, CyRootMotion* out_motion);
CyResult animation_set_root_motion(CyEngine engine, CyEntity entity, uint32_t mode);
CyResult animation_joint_pose(CyEngine engine, CyEntity entity, const char* joint,
                              CyPose* out_pose);

}  // namespace cy::abi::game
