#pragma once
// The camera system's three object families, as generational handles. Task 4.3.1.
//
// `camera-system` — "A camera is not a scene object": a camera "SHALL be addressable as a
// lightweight handle and data, and SHALL NOT require an ECS entity, a node, or a base class to
// exist", and "There SHALL NOT be a camera base class intended for subclassing to produce camera
// behaviours."
//
// That requirement is the reason this file exists before any other in the module. A handle is what
// makes a debug camera, a reflection capture and an editor viewport cost the same as a gameplay
// camera — none of them needs a world — and `cy::Handle<Tag>`'s generation counter is what makes a
// rig destroyed mid-frame answer "no" to the next lookup rather than resolve to whatever replaced
// it.
//
// THREE FAMILIES, WHICH ARE THREE OF THE FOUR SEPARATED CONCEPTS:
//
//   CameraDefinition   the authored asset: a rig composition compiled to a program. Shared.
//   CameraRig          a runtime instance of a definition, holding its own state. Per camera.
//   CameraStack        one local player's ordered contributions and the blend between them.
//
// The fourth concept, the **evaluated camera**, deliberately has no handle: it is the *result* of
// evaluating a rig or resolving a stack for one frame, it is a value, and giving it an identity
// would invite something to keep one across a frame boundary and read a pose that no longer holds.
// The fifth thing in the vocabulary, the **render view**, is `cy::render`'s and is created through
// `RenderServer::create_view()` — see view.h.

#include <cy/core/values/handle.h>

namespace cy::camera {

CY_HANDLE_TAG(CameraDefinition);
CY_HANDLE_TAG(CameraRig);
CY_HANDLE_TAG(CameraStack);

using DefinitionHandle = Handle<CameraDefinitionTag>;
using RigHandle = Handle<CameraRigTag>;
using StackHandle = Handle<CameraStackTag>;

}  // namespace cy::camera
