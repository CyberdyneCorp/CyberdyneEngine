# Tasks

- [x] Add the `SelectionOutlines` stage to the forward frame, with its refusals, and pass its producer through the assembly.
- [x] Add `HighlightSet`, `OutlineSettings` and the host reference in `src/rendering/selection/highlight.h`.
- [x] Add the mask and composite shaders and `OutlinePass`, with committed SPIR-V and MSL and `shaders/regenerate.py`.
- [x] Add the `SelectionHighlight` component and `gather_highlights`, and the Swift `SelectionHighlight` with `World.highlight`.
- [x] Pin the frame with the stage absent to a committed reference drawn before the change.
- [x] Add `unit.rendering_selection`, `unit.abi_selection` and `render.selection_outlines`, extend `unit.render_forward`, and prove each frame case red by a mutation.
- [x] Add `samples/13-rts-selection` and publish its before and after images under `docs/design/images/`.
- [x] Update the READMEs and the requirements map, and validate this change.
