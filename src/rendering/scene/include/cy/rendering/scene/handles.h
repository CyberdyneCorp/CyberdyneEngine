#pragma once
// The renderer's handle families, in one place, because two of them are shared. Task 4.1.1.
//
// `rendering-architecture` — "Handle-based render server" lists the object families the renderer
// owns and requires that every one of them be addressed through a generational handle. The tags
// themselves live here, below the server, for one structural reason: the GPU scene stores a mesh
// reference and a material reference in every instance, and the GPU scene is *lower* than the
// server that owns those meshes. Declaring `MeshHandle` in the server and having the GPU scene
// reach up for it would be a cycle; declaring it twice would give the engine two types that mean
// the same thing and compare unequal.
//
// A tag is a phantom type — see <cy/core/values/handle.h>. It has no definition, costs nothing, and
// is the whole reason `RenderMeshHandle` and `RenderMaterialHandle` are different types rather than
// two spellings of `u64`.
//
// WHY THE FAMILIES THAT M3 DOES NOT IMPLEMENT ARE STILL DECLARED. `rendering-architecture` names
// twenty families and M3 builds pools for six of them. The remaining tags are declared, unused, and
// deliberately so: a family that arrives at M7 with its handle type invented on the spot is a
// family whose handle is stored as a `u32` somewhere in between. The declarations cost nothing and
// they fix the vocabulary now.

#include <cy/core/base/types.h>
#include <cy/core/values/handle.h>

namespace cy::rendering {

// --- The families with storage at M3 -------------------------------------------------------------

CY_HANDLE_TAG(RenderTexture);
CY_HANDLE_TAG(RenderSampler);
CY_HANDLE_TAG(RenderMesh);
CY_HANDLE_TAG(RenderMaterial);
/// A compiled material *program*, which any number of material instances share.
CY_HANDLE_TAG(RenderMaterialProgram);
CY_HANDLE_TAG(RenderScene);
CY_HANDLE_TAG(RenderView);
CY_HANDLE_TAG(RenderInstance);
/// A publisher into the GPU scene. See gpu_scene.h — this is the identity a producer holds.
CY_HANDLE_TAG(RenderProducer);

// --- The families whose storage arrives later ----------------------------------------------------

CY_HANDLE_TAG(RenderShader);
CY_HANDLE_TAG(RenderSkeleton);
CY_HANDLE_TAG(RenderEffect);
CY_HANDLE_TAG(RenderLight);
CY_HANDLE_TAG(RenderReflectionProbe);
CY_HANDLE_TAG(RenderDecal);
CY_HANDLE_TAG(RenderGiVolume);
CY_HANDLE_TAG(RenderLightmap);
CY_HANDLE_TAG(RenderOccluder);
CY_HANDLE_TAG(RenderCamera);
CY_HANDLE_TAG(RenderCanvas);
CY_HANDLE_TAG(RenderEnvironment);
CY_HANDLE_TAG(RenderPostProcess);
/// A chain of mesh LODs with their screen-coverage thresholds.
CY_HANDLE_TAG(RenderLodChain);

using TextureHandle = Handle<RenderTextureTag>;
using SamplerHandle = Handle<RenderSamplerTag>;
using MeshHandle = Handle<RenderMeshTag>;
using MaterialHandle = Handle<RenderMaterialTag>;
using MaterialProgramHandle = Handle<RenderMaterialProgramTag>;
using SceneHandle = Handle<RenderSceneTag>;
using ViewHandle = Handle<RenderViewTag>;
using InstanceHandle = Handle<RenderInstanceTag>;
using ProducerHandle = Handle<RenderProducerTag>;

using ShaderHandle = Handle<RenderShaderTag>;
using SkeletonHandle = Handle<RenderSkeletonTag>;
using EffectHandle = Handle<RenderEffectTag>;
using LightHandle = Handle<RenderLightTag>;
using ReflectionProbeHandle = Handle<RenderReflectionProbeTag>;
using DecalHandle = Handle<RenderDecalTag>;
using GiVolumeHandle = Handle<RenderGiVolumeTag>;
using LightmapHandle = Handle<RenderLightmapTag>;
using OccluderHandle = Handle<RenderOccluderTag>;
using CameraHandle = Handle<RenderCameraTag>;
using CanvasHandle = Handle<RenderCanvasTag>;
using EnvironmentHandle = Handle<RenderEnvironmentTag>;
using PostProcessHandle = Handle<RenderPostProcessTag>;
using LodChainHandle = Handle<RenderLodChainTag>;

}  // namespace cy::rendering
