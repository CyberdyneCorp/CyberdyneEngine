// The convention probe: the smallest shader that can answer section 5's questions on a device.
//
// COMPILED OFFLINE AND CHECKED IN, exactly as src/backends/shader/tests/fixtures/probe.slang is, and
// for the same reason: `CY_SHADER_SLANG` is off by default, and a suite that needed a shader compiler
// would not run on the machines that most need to run it. `embed_spirv.py` beside this file is how
// the header is regenerated; the invocation is at the bottom of this comment.
//
// ================================================================================================
// WHY THE MATRIX IS FOUR EXPLICIT ROWS AND NOT A float4x4
// ================================================================================================
//
// A `float4x4` in a push-constant block has a MATRIX LAYOUT — row-major or column-major — that is a
// property of the compiler's flags rather than of this file, and `mul(M, v)` versus `mul(v, M)`
// silently transposes the result. That is an excellent way to spend an afternoon on a test whose
// whole purpose is to catch a transposition.
//
// So the block carries four rows and the shader takes four dot products. The C++ side writes them
// out of `cy::Mat4` explicitly, and there is no layout convention anywhere in between for the two
// sides to disagree about. It costs four lines and removes the only ambiguity in the file.
//
// ================================================================================================
// THE POSITION IS CAMERA-RELATIVE, AND THERE IS NOWHERE TO PUT A WORLD ONE
// ================================================================================================
//
// design.md §3: "Positions reach the GPU relative to the camera." The vertex input is named for what
// it is, and the matrix is relative-to-clip rather than world-to-clip — the same arrangement
// `cy/view.slang` uses. A test that fed world positions through a world-to-clip matrix would pass at
// the origin and fail a million units out, which is exactly the failure task 5.3 exists to catch.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   slangc conventions.slang -target spirv -profile spirv_1_5 \
//          -entry conventionVertex -stage vertex   -o conventions_vertex.spv
//   slangc conventions.slang -target spirv -profile spirv_1_5 \
//          -entry conventionFragment -stage fragment -o conventions_fragment.spv
//   python3 embed_spirv.py conventions_spirv.h \
//          kConventionVertexSpirv=conventions_vertex.spv \
//          kConventionFragmentSpirv=conventions_fragment.spv
//   clang-format -i conventions_spirv.h

struct ConventionPush
{
    // Rows of the camera-relative-to-clip matrix. See the header comment.
    float4 row0;
    float4 row1;
    float4 row2;
    float4 row3;
    float4 color;
};

[[vk::push_constant]]
ConstantBuffer<ConventionPush> push;

struct VertexInput
{
    [[vk::location(0)]] float3 positionRelativeToCamera : POSITION;
};

struct VertexOutput
{
    float4 clip : SV_Position;
};

[shader("vertex")]
VertexOutput conventionVertex(VertexInput input)
{
    let point = float4(input.positionRelativeToCamera, 1.0);
    VertexOutput output;
    output.clip = float4(dot(push.row0, point), dot(push.row1, point), dot(push.row2, point),
                         dot(push.row3, point));
    return output;
}

[shader("fragment")]
float4 conventionFragment() : SV_Target
{
    return push.color;
}
