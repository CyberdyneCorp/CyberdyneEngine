# Add the first-person weapon view

## Why

`camera-system` asks that "WHEN a first-person camera renders the world and a separate weapon view
THEN both SHALL be render views derived from one evaluated camera", and its requirements for aim,
recoil and accessibility describe the first-person camera that weapon view belongs to. The tree had
the vocabulary and none of the behaviour: `RenderViewRequest::view_key` and `override_lens` could
describe a weapon view, and nothing drew one, so a weapon held at arm's length went through any
wall the player walked up to. The requirements map recorded "Aim is three things" as
`exempt:m11e` — "Nothing derives weapon aim from offsets or recoil" — and "Shake, recoil, and the
impulse bus" with "Recoil as a spring with recovery integrated with the aim layer is not
implemented".

## What changes

- **The weapon view is drawn into the world's frame.** A new module, `cy::rendering-viewmodel`
  (`src/rendering/viewmodel/`): the viewmodel's own projection — its field of view and near plane —
  with its depth squeezed into the slice of the reversed-Z range nearest the eye
  (`z' = (1 - s) + s·z`), so every weapon fragment is in front of every world surface past
  `world_near / (1 - s)` and the weapon still orders itself. One depth buffer, one prepass, one light
  loop, one temporal resolve.
- **The assembly culls it on its own.** `AssemblyView::viewmodel` (appended): the weapon view's
  layers, frustum and shadow policy. The world view no longer draws those layers; the weapon view's
  survivors are marked `render::kInstanceViewmodel` and carry the policy on the two flags the shadow
  and forward passes already read. A device-culled frame refuses a weapon view.
- **The frame's shaders.** `CyFrameData` appends the viewmodel's rows, last frame's and a policy
  word; `cy/frame.slang` appends four vertex entries (`cyViewmodelDepthVertex`,
  `cyViewmodelForwardVertex` and their skinned forms) and finds a weapon surface's light lists
  through the world's projection of its position (`frameClusterPixel`). `PipelineSetup::viewmodel`
  creates the variants; the recorder binds them for marked draws. The committed SPIR-V and MSL of
  the frame and of the particles and strips that import it are regenerated.
- **Lighting, shadows and GI from where it is.** The weapon is lit at its world position: lights,
  the shadow map (received by default, cast on request), fog, the irradiance volume. Contact shadows
  are off for it by default.
- **Motion.** `ViewmodelFrame` keeps last frame's lens; the previous rows are that lens over
  `previous_camera_relative_view` (split out of `previous_camera_relative_to_clip`), so a carried
  weapon has no motion and a raised one moves as its lens does.
- **The first-person camera.** `cy::camera::FirstPersonCamera` (`servers/camera/first_person.h`):
  mouse look with the sensitivity applied once, head bob by distance walked, recoil as a damped
  spring stepped in closed form, aiming down the sights blending both lenses, motion reduction, and
  the three aims. `camera::produce_weapon_view` and `ViewPurpose::Viewmodel`.
- **ABI 1.13.** `first_person_configure`, `_input`, `_kick`, `_reset`, `_state`, `_release`,
  `viewmodel_configure`, `viewmodel_get`, over `cy::game-backend-first-person`; CyberdyneKit's
  `FirstPersonCamera` and `WeaponView`.

## Impact

Off, the frame is the frame it was: no weapon view is no second cull, no mark, no viewmodel
pipeline bound — held byte for byte to the reference a build from before wrote. The view block grows
from 688 to 832 bytes, appended.
