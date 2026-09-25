# Proposal

## Why

An empty project can be opened and FBX assets can be imported, but the live editor viewport draws every authored node as a box. The engine frame recorder also captured black on Metal, so the editor cannot show the mesh and transform the user is authoring.

## What Changes

- Correct the Metal frame shader bindings and require visible pixels in the Metal render suite.
- Render project mesh references from the authored world through `FrameAssembly` and `FrameRecorder` in the editor host.
- Preserve full translation, rotation, and nonuniform scale when building render instances.
- Use mesh bounds for selection and camera framing, and report missing or unsupported assets.
- Keep the existing external FBX import, scene transactions, transform gizmos, viewport transport, and play session connected to the rendered world.
- Show engine-rendered transform changes during a gizmo drag, and refine the viewport controls so numeric rows align, the header uses the product mark alone, and the orientation widget lets more of the scene show through.

## Capabilities

### Modified Capabilities

- `editor-viewport-and-gizmos`: An empty authored world and imported mesh instances must appear in the live viewport with their edited transforms and matching picking.
- `rendering-architecture`: The editor host must submit authored mesh instances to the standard frame path on Metal and Vulkan.

## Impact

The frame shader assets and Metal render tests, editor engine host, world to mesh resolution, viewport chrome, runtime mirror, viewport documentation, and live authoring smoke coverage change. The editor command and world file formats remain compatible.
