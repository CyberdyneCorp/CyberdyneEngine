# Tasks

- [x] Add percentage-closer soft shadows to `cy/shadow.slang` and their C++ twin and shape derivation in `src/rendering/lighting/soft_shadows.h`.
- [x] Add the contact trace, its host reference and `ContactShadowPass` in `src/rendering/contact_shadows/`, with committed SPIR-V and MSL.
- [x] Add the `ContactShadows` stage to the forward frame and pass its producer and target through the assembly.
- [x] Apply both in `cy/frame.slang` behind appended flags and regenerate the frame's committed SPIR-V and MSL.
- [x] Pin the frame with the setting off to a committed reference rendered by the pre-change frame shader.
- [x] Add `integration.render_soft_shadows`, `integration.rendering_contact_shadows` and `render.soft_shadows`, and prove each frame case red by a mutation.
- [x] Add the setting to the beauty shot and `just capture-soft-shadows`, and publish the before/after images.
- [x] Update the READMEs and the requirements map, and validate this change.
