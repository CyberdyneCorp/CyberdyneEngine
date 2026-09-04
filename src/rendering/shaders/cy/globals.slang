// The global shader parameter block. Part of the engine's Slang standard library.
//
// `shader-system` — "Global shader parameters": project-wide values readable by any shader, stored
// in one global uniform buffer, with no per-material update needed. The C++ side is
// cy::shader::GlobalParameterBlock, which lays this block out and can *emit* this declaration
// (`emit_slang_declaration`) for a project whose globals are authored rather than fixed.
//
// SET 0, BINDING 0. Set 0 is the per-frame set of the descriptor set convention
// (cy/backends/shader/reflection.h); binding 0 within it is reserved for this block so every shader
// agrees without anybody being asked.

module globals;

public struct CyGlobalsData
{
    public float timeSeconds;
    public float deltaSeconds;
    public float exposureStops;
    public float windStrength;
    public float4 windDirectionAndSpeed;
};

[[vk::binding(0, 0)]]
public ConstantBuffer<CyGlobalsData> cyGlobals;
