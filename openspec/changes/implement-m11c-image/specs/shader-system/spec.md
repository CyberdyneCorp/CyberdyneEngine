## MODIFIED Requirements

### Requirement: Compilation pipeline
Shader compilation SHALL be an **offline** step producing cooked artefacts:

1. Slang source → Slang compiler → SPIR-V per entry point and permutation
2. SPIR-V → validation and optimisation
3. SPIR-V → reflection (descriptor bindings, push constants, vertex inputs, specialization
   constants, workgroup size)
4. Per backend: the **same Slang program compiled again for that backend's target** — SPIR-V
   retained (Vulkan), MSL (Metal), DXIL (D3D12)
5. Package into a **shader library** artefact keyed by content hash

Runtime shader compilation SHALL exist only in development builds, for hot reload.

Step 4 SHALL recompile the authored program rather than translate the SPIR-V. The specification
previously named SPIRV-Cross for the Metal leg; the toolchain the engine already integrates emits
all three targets from one front end, and putting a second compiler between the source and one
artefact would make the agreement below a claim about the translator rather than about the shader.

Two artefacts of one entry point produced for two targets SHALL declare the same **interface**: the
same entry point name, the same stage, the same workgroup size, and the same parameters in the same
order with the same kinds and element counts. They SHALL NOT be required to place those parameters
at the same binding indices — Metal gives buffers, textures and samplers separate index spaces and
D3D12 gives them separate register classes, so identical placement is not a property the targets
have.

An artefact SHALL be in the form its target names, and a front end SHALL refuse to return one that
is not. Existence and non-emptiness are not evidence that a target was emitted: the cheapest way to
appear to deliver a second target is to write the first target's bytes under the second target's
name.

#### Scenario: Shipping build compiles no shaders from source
- **WHEN** a game ships
- **THEN** it SHALL contain compiled backend-native shader artefacts and no Slang compiler

#### Scenario: Compile error surfaces with source location
- **WHEN** a shader fails to compile
- **THEN** the error SHALL carry the Slang source file, line, and column, and appear in the
  editor's shader editor and the build log

#### Scenario: One graph, two targets, one interface
- **WHEN** an entry point is compiled for two of the three targets
- **THEN** both artefacts SHALL declare the same entry point name, stage, workgroup size and
  parameter list, and the comparison SHALL report every difference naming both targets

#### Scenario: A target that cannot be emitted is reported, not assumed
- **WHEN** the toolchain for a target is absent from the build — DXIL without the downstream
  compiler Slang loads for it
- **THEN** the build SHALL report that target as not emitted, having asked by compiling for it,
  rather than reporting the build option that was set

#### Scenario: An artefact in the wrong form is not an artefact
- **WHEN** what a front end produces for a target is not in that target's form
- **THEN** the compilation SHALL fail with a diagnostic naming the target, rather than returning
  bytes that pass a length check
