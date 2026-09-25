# material-compiler Spec Delta

## ADDED Requirements

### Requirement: Material editor service
The material compiler SHALL publish its authoring catalogue, graph validation, compilation,
lowering-stage inspection, node preview compilation, compiled artefact metadata, cost reports,
parameter layout, and dependency information through `editor-backend-services`.

The service SHALL invoke the same lowering, optimisation, shader pipeline, and cook identity logic as
offline cooking. It SHALL NOT maintain an editor-only catalogue, compiler, or preview shader path.

#### Scenario: Catalogue is compiler-owned
- **WHEN** a material node or pin is added to the compiler's registered catalogue
- **THEN** the editor-service catalogue SHALL expose it without a corresponding Rust source edit

#### Scenario: Service and cook agree
- **WHEN** the same graph and target profile are compiled through the editor service and offline cook
- **THEN** their derivation identities, diagnostics, generated programs, and dependency sets SHALL agree

#### Scenario: Constant Diffuse graph in an authored scene
- **WHEN** an authored mesh references a canonical graph whose surface is an opaque constant-color Diffuse closure
- **THEN** the authored scene renderer SHALL use that graph color on the mesh
- **AND** a graph outside this supported authored-frame shape SHALL produce an explicit unsupported-material error

### Requirement: Material service capabilities
Material catalogue and compile results SHALL report node and feature support per target profile,
including required device capabilities, unsupported closures or operations, variant limits, and
declared degradations.

#### Scenario: Target limitation is consistent
- **WHEN** a material uses the generic layered evaluator against a target that disallows it
- **THEN** capability discovery and compilation SHALL report the same stable diagnostic code and responsible node
