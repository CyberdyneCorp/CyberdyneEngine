#pragma once
// The render server's object families, as generational handles. Task 4.1.1.
//
// `rendering-architecture` — "Handle-based render server": `RenderServer` "SHALL own all renderable
// state and expose it through generational handles, with no knowledge of entities, nodes, or
// scripts", and it lists the families. They are all declared here, including the ones M3 does not
// yet store anything behind, because a family that arrives later with a *different* handle spelling
// is a family every caller has to be taught twice.
//
// M1's `cy::Handle<Tag>` exactly: a 32-bit slot index and a 32-bit generation, where freeing a slot
// bumps its generation so a handle held across the free compares unequal to whatever replaced it.
// Every family is a distinct type, so passing a `MeshHandle` where a `MaterialHandle` is expected
// is a compile error rather than a convention.
//
// A null handle is a zero generation, so a zeroed struct — an instance record in a memset buffer, a
// designated initialiser that omits the field — reads as "no resource" rather than "slot 0".

#include <cy/core/values/handle.h>

namespace cy::render {

// The families `rendering-architecture` enumerates, in the order it enumerates them.
CY_HANDLE_TAG(RenderTexture);
CY_HANDLE_TAG(RenderSampler);
CY_HANDLE_TAG(RenderMesh);
CY_HANDLE_TAG(RenderMaterial);
CY_HANDLE_TAG(RenderShader);
CY_HANDLE_TAG(RenderSkeleton);
CY_HANDLE_TAG(RenderEffect);
CY_HANDLE_TAG(RenderLight);
CY_HANDLE_TAG(RenderProbe);
CY_HANDLE_TAG(RenderDecal);
CY_HANDLE_TAG(RenderGiVolume);
CY_HANDLE_TAG(RenderLightmap);
CY_HANDLE_TAG(RenderOccluder);
CY_HANDLE_TAG(RenderCamera);
CY_HANDLE_TAG(RenderView);
CY_HANDLE_TAG(RenderScene);
CY_HANDLE_TAG(RenderInstance);
CY_HANDLE_TAG(RenderCanvas);
CY_HANDLE_TAG(RenderEnvironment);
CY_HANDLE_TAG(RenderPostProcess);

using TextureHandle = Handle<RenderTextureTag>;
using SamplerHandle = Handle<RenderSamplerTag>;
using MeshHandle = Handle<RenderMeshTag>;
using MaterialHandle = Handle<RenderMaterialTag>;
using ShaderHandle = Handle<RenderShaderTag>;
using SkeletonHandle = Handle<RenderSkeletonTag>;
using EffectHandle = Handle<RenderEffectTag>;
using LightHandle = Handle<RenderLightTag>;
using ProbeHandle = Handle<RenderProbeTag>;
using DecalHandle = Handle<RenderDecalTag>;
using GiVolumeHandle = Handle<RenderGiVolumeTag>;
using LightmapHandle = Handle<RenderLightmapTag>;
using OccluderHandle = Handle<RenderOccluderTag>;
using CameraHandle = Handle<RenderCameraTag>;
using ViewHandle = Handle<RenderViewTag>;
using SceneHandle = Handle<RenderSceneTag>;
using InstanceHandle = Handle<RenderInstanceTag>;
using CanvasHandle = Handle<RenderCanvasTag>;
using EnvironmentHandle = Handle<RenderEnvironmentTag>;
using PostProcessHandle = Handle<RenderPostProcessTag>;

}  // namespace cy::render
