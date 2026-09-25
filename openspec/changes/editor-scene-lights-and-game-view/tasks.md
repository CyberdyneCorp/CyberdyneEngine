# Tasks

## 1. Scene authoring

- [x] 1.1 Expose Plane creation in the hierarchy and Scene menu through the existing primitive command; verify a created Plane saves and reopens with its transform.
- [x] 1.2 Add schema-driven camera and directional, point, and spot light creation commands with undo and selection; verify command and save/load regressions.
- [x] 1.3 Add light and camera entries to the scene UI, with editable inspector fields; verify each action uses the same command as MCP.

## 2. Authored rendering

- [x] 2.1 Extract authored light values and composed transforms into hosted frame lighting; verify live intensity and movement changes in renderer tests.
- [x] 2.2 Use an enabled primary scene camera for Game rendering and show an explicit missing-camera result; verify camera save/load and framing regressions.
- [x] 2.3 Draw selectable light handles in Editor and suppress light and transform gizmos in Game; verify captured images contain the expected marks only in Editor.
- [x] 2.4 Draw selectable Camera handles and transformed direction indicators for Cameras, directional lights, and spot lights; verify picking, rotation, and Game suppression.
- [x] 2.5 Keep disabled authored lights selectable without replacing them with preview light, carry `casts_shadow`, and tune Editor exposure so light toggles and directional rotation visibly change the frame; add a Metal regression.
- [x] 2.6 Render an authored directional shadow map through the engine frame and sample it on mesh receivers; verify light rotation, light disabling, and the shadow flag with Metal pixel regressions.
- [x] 2.7 Place a large Plane below the imported tree in the live demo, capture its shadow and the light toggle/rotation outcomes, and document the scene setup.

## 3. View and Play lifecycle

- [x] 3.1 Add distinct Editor and Game view state and visible mode controls, preserve editor camera, and render only the active view; verify switching without Play does not tick physics.
- [x] 3.2 Make Play switch to Game and Stop restore Editor and the authored world; verify physics advances in Play, pauses in Pause, and resets on Stop.
- [x] 3.3a Connect built project Swift behaviours to hosted Play; execute fixed updates, pause them, restore transforms on Stop, report missing modules, and verify execution with a compiled Swift regression.
- [x] 3.3c Answer script-module reload requests in the hosted runtime and apply built Swift generations during Play; expose status and color Swift source in the workspace.
- [ ] 3.3b Connect project audio services to hosted Play where available and verify execution/status regressions.

## 4. Delivery

- [x] 4.1 Update editor workflow documentation and validate OpenSpec; verify command examples and expected view behavior.
- [ ] 4.2 Inspect the live editor with a Plane, textured FBX, light, and camera; capture Editor/Game screenshots, commit and push the PR branch, and verify CI.
