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

}  // namespace cy::abi::game
