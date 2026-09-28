# Design

## Shared graph authoring

Use `cy_editor_interface::specialised::GraphCanvas` for both domains. The engine service publishes versioned catalogues, including stable identities, typed pins, properties, capabilities, and stage metadata. The editor stores node instances and layout separately; graph validation and compilation remain in the engine. A catalogue addition must reach the palette without editing a Rust node list.

## VFX asset and compilation path

The authored hierarchy is system → emitters → spawn/update/event/render stage graphs. Modules are separately saved graph assets referenced by stages. Each emitter declares its CPU or GPU target, renderer, user parameters, and data-interface bindings. Editor commands produce document transactions, and the same commands back MCP. Saved-document commands now cover emitter removal, capacity, particle attribute declarations, and event channel bounds with undo/redo; wire tests check valid edits and atomic refusal of unknown emitters and invalid capacity. The panel preserves other unsaved stages when an emitter is removed. The service compiles through `cy::vfx-compiler`, returning `CompileReport`, attribute layout, generated source on demand, and node-located diagnostics. Save writes a reopenable authoring document and canonical cook input. Runtime parameter updates use the live instance path; graph edits schedule compilation.

The editable draft is a versioned `.cyvfxdoc` project source. `vfx.document.save` journals its prior and new contents in the active scene document so undo/redo restores the file; `vfx.document.read` reopens it through the same command registry. The engine service will produce canonical cook input from this document after stage validation and compilation are connected. The draft format is not itself a cooked effect.

Reusable modules have a separate versioned `.cyvfxmodule` source contract: compatible stage, typed host inputs, dependency names, and shared-canvas graph. The editor can encode and reopen the asset, and the engine has an independent reader. `vfx.module.save`/`vfx.module.read` persist it with undo/redo through the shared command registry. System drafts keep module names rather than copying nodes. Version 3 of the binary `.cyvfxdoc` payload maps each name to an explicit project-relative `.cyvfxmodule` path; version 1 and 2 drafts still reopen. The project layer now loads the mapped sources for compile and preview, and the engine validates typed inputs, stage compatibility, missing assets, and dependency cycles before composing their graphs into emitter stages. Composed graph digests contribute to the cook key. The panel creates and opens modules on the shared canvas, edits their stage, typed host inputs and dependencies, and saves them through `vfx.module.save`. Attaching a saved module to an emitter records its explicit project path through `vfx.document.save`. Both saves participate in scene undo/redo. Opening or creating another module refuses to replace unsaved edits; the panel explicitly offers discard to reload the saved source or close an unsaved new module. Automatic compile signatures include saved module content while ignoring canvas layout. Saved module node insertion, connection, disconnection, removal, and property edits now use engine-catalogue commands shared with MCP and save through project undo history. The panel's unsaved canvas edits still need individual transactions, and full MCP parity remains task 2.6. The service must not infer a module file from an unqualified name or silently omit one.

Saved module stage changes now use `vfx.module.stage.set` through that command registry, with MCP
undo/redo and invalid-stage refusal coverage. A new module stages edits until its first Save
chooses a project path; typed desktop-command parity remains open.
The desktop's compatible-stage, host-input, and dependency controls now send their corresponding
typed commands for the committed path of the open module. The editable Save/Open path field does
not retarget those controls. A new unsaved module still edits its local draft, and saved edits
refresh the open canvas from the project source before the next frame. Each typed command is an
individual undo step; remaining canvas and system controls still need typed desktop parity.
Palette insertion into a clean saved system stage or module now invokes `vfx.node.add` or
`vfx.module.node.add`, sharing the MCP path and one undo entry. A canvas with unsaved gestures
keeps palette insertion local until Save, preserving those pending edits. A clean saved canvas
previews a node drag locally and sends one typed `vfx.node.move` or `vfx.module.node.move` command
on release; the frame journal skips its whole-document save so the move is one undo step. A new
draft without a saved project path keeps movement local until its first Save. Edits already pending
on a saved canvas follow frame journaling. Editing a property on a clean saved
canvas now invokes `vfx.node.property.set` or `vfx.module.node.property.set`; property changes on
an unsaved canvas remain local until Save. Compatible pin connections on a clean saved canvas
invoke `vfx.node.connect` or `vfx.module.node.connect` after the shared canvas validates them;
dirty drafts continue to connect locally until Save.
The VFX panel's selected-node removal button invokes `vfx.node.remove` or
`vfx.module.node.remove` on a clean saved graph, removing attached wires within one undoable edit.
Unsaved drafts remove locally until Save.
The selected node's wire list offers disconnection through `vfx.node.disconnect` or
`vfx.module.node.disconnect` for clean saved graphs; the shared canvas checks that the exact wire
exists before queuing a command. Dirty drafts disconnect locally until Save.
The saved system's parameter, event-channel, particle-attribute, and emitter-capacity controls now
send their matching typed commands too. Attaching a saved module uses `vfx.module.attach` against
the committed system path. Unsaved systems still stage metadata locally until their first Save.
The panel keeps the engine preview's exposed-parameter update queue when a saved parameter value
changes. Other hierarchy and canvas gestures remain to be moved to typed commands.
Saved emitter renderer/target changes, emitter removal, and data-interface binding changes now
use `vfx.emitter.configure`, `vfx.emitter.remove`, and `vfx.interface.bind/unbind` respectively.
The editor refreshes their saved source and stage selection after each command. Saved emitter
creation now uses `vfx.emitter.add` too and selects its Spawn stage after the command succeeds;
new systems still add the emitter locally. Remaining canvas gestures use frame-level document
saves until their individual typed desktop actions are connected.

After a system or module has its first saved project path, the desktop journals each changed UI
frame through `vfx.document.save` or `vfx.module.save` before applying other frame intents. The
saved source and open shared canvas reload together on undo/redo. An unchanged frame adds no
transaction, and an explicit Save in the same frame suppresses the automatic duplicate. New
drafts still need the initial Save to select a path. The panel and MCP now share undoable saved
document semantics. A project source changed by MCP refreshes the open desktop canvas before the
next frame can journal another edit. Matching each desktop gesture to its individual typed MCP
command remains part of task 2.6.

## Preview and inspection

An isolated engine preview instance drives play, pause, restart, scrub, and time scale. The viewport displays the runtime result. Debug snapshots expose bounded per-emitter counts, budget state, event traffic, and one-particle attribute readback. Unsupported renderer kinds or target capabilities return named refusals.

## Scene instance and gameplay parameters

Issue #15 also requires system and emitter user parameters in the Inspector, persistent overrides
on individual scene effect instances, and Swift gameplay access. Version 4 `.cyvfxdoc` now records
emitter-local typed defaults; the Engine reader retains their scope, compiler resolves local names
before system names, and runtime parameter updates address the emitter and effect handle. The
editor panel and MCP commands edit these declarations through document history. Preview updates
use `emitter:name` to reach a local parameter on the Engine instance. The scene now stores
`cy::vfx::Effect` asset references and typed override fields per entity; MCP commands create and
edit them through scene transactions, and the generated Inspector sees their schema. The Engine
`SceneEffects` adapter loads two entities as independent instances and applies their saved values.
The editor runtime host connects that adapter to saved scene loads, live updates, simulation, and
sprite publication. ABI 1.4 appends typed live-effect set/get and generates matching Swift and Rust
wrappers. A Swift behaviour uses it on its Play entity; a native scene test checks that another
instance and emitter keep their values. The scene's built-in `Effect` template has not been
registered as an ECS component; the explicit scene command creates the authoring component.

Extend the editable VFX document with emitter-local typed declarations while preserving system
parameters shared across emitters. Give emitter-local declarations stable, unambiguous identities
through authoring, compilation, cooking, preview, and runtime lookup. A scene effect component
references the cooked system and stores validated exposed-parameter overrides; the Inspector and
MCP edit it through the same scene transaction. Runtime scene loading creates one Engine VFX
instance per enabled effect entity, applies its overrides, and keeps its transform bound to the
entity. Saving and reopening the scene must retain both the asset reference and overrides.

Expose effect-instance parameter set and get through append-only `CyInterface` entries, then
regenerate the Swift overlay rather than hand-writing a separate bridge. A Swift test module must
set and read one instance while a second instance retains its value. Keep system parameter names
compatible with existing cooked assets and reject unknown, non-exposed, wrong-type, or ambiguous
emitter-local requests with named diagnostics. A parameter update must use the Engine instance's
parameter words and must not invoke the VFX compiler.

## Vertex material stage

The existing material graph gains an explicit vertex stage and typed outputs for world-position offset, custom interpolants, and displacement. Stage-aware nodes use the same canvas and backend catalogue. Compilation reports variants by geometry source. Unsupported paths, including virtual geometry where offset evaluation is unavailable, fail in both editor validation and cook. The same vertex expression drives the main, shadow, and previous-frame positions so motion vectors follow displacement.
The `material.vertex_output` graph node now accepts scalar normal displacement as well as a
float3 world-space offset. Graph lowering combines them as `offset + normal * displacement` in
the existing typed vertex root; tests confirm the normal reaches visible and shadow vertex programs.
The text front end accepts the same scalar displacement assignment and graph/text cook identities
match when both offset and displacement are present.
The IR now retains a sorted array of named `float` through `float4` vertex interpolant roots. The
name, type, and source content enter the module digest and version 3 encoding. Optimisation and
visible derivation preserve them, while shadow derivation drops them. The generated vertex
evaluator returns offset plus typed interpolants; the hosted material shader assigns these to
varyings and binds them into the fragment context. The `material.vertex_interpolant` catalogue
node supplies a named output, and a typed `material.attribute` reads it on the surface. Text uses
`vertex_interpolant tint = color0;` with `attribute tint : float3;`. Both front ends produce one
cook identity. Compilation rejects mismatched reads and any vertex output that reads an
interpolant. Slang vertex and fragment probes compile, and shader assembly has source assertions;
a hosted device image comparison and authored-scene integration remain open. Previous-frame
evaluation remains open.

The material IR first carries a typed `float3` world-space vertex offset root beside its surface
and opacity roots. Graph and text authoring lower to that same root; its content enters the module
digest and versioned encoding. Optimisation and shadow derivation retain it so later vertex shader
emission reads one expression across visible, shadow, and previous-frame positions. Each compiled
variant carries a separate generated vertex function from this root, using the same SSA expression
writer as the surface function. The vertex function is compiled against the engine Slang library as
a vertex-stage probe before frame integration.
Cooked material bundle version 2 carries each variant's vertex source and digest beside its
fragment source. Opaque shadow variants retain the vertex source despite having no fragment work;
the reader continues to accept older version 1 bundles, and the compiler version forces recooking.
The hosted Metal material program now places that generated vertex function in its shader unit and
adds its world-space result before projection. Visible and shadow vertex entry points call the same
displaced-position helper and bind the same material parameter block. A shader-unit regression
checks both calls; the Metal image case compares the constant GPU offset, including its shadow,
against moving the same mesh on the CPU. Motion vectors and a time-varying CPU reference remain
before task 3.3 is complete.
The compiler now walks the vertex offset expression and reports `vertex-stage-unsupported` if a
texture sample or custom Slang node reaches it. Editor validation and material cooking consume the
same compiler diagnostic and refuse the asset, while surface-only texture sampling stays valid.
The material build producer accepts an explicit comma-separated `geometry` option and passes
its source set to the compiler; a virtual-geometry assignment with a vertex offset fails without
an artefact. Saved worlds classify a `.cyvg` mesh reference as the Engine's cooked CYVG geometry
path, so editor requests and project cooking both carry `VirtualGeometry` for that assignment.
The `cy_material cook` front end now accepts a repeated material-to-geometry assignment, passes
each source set to that producer, and refuses assignments for materials missing from the cook
inputs. `cy_material cook --world` now reads saved `.cyworld` assets through the engine reader,
discovers live mesh material references (including imported slots), and passes `StaticMesh` or
`VirtualGeometry` according to the referenced mesh asset. The cook lowers canonical `.cygraph`
files through the registered engine graph nodes. It also discovers terrain-layer assignments.
The editor backend now accepts a versioned material request envelope listing named geometry
sources and passes those paths to the same compiler options used by cooking. Validate and Compile
return the compiler's `vertex-geometry-unsupported` code for a vertex graph assigned to
`VirtualGeometry`; the desktop sends the active scene's `StaticMesh`, `VirtualGeometry`, and
`Terrain` assignments, including imported material slots. Compile results return those named
sources to the editor for the variant report. Other geometry paths need their renderer-specific
scene bindings before saved worlds can assign them.
Author requests carrying assigned geometry now run the same compiler check before returning a
canonical graph. The desktop Save action and `material.graph.save` MCP command both derive the
active scene's geometry assignments. The save and generated Inspector property sync join one
document transaction, so one undo restores both project files and scene fields. Assignments from
other geometry sources are reported when their scene bindings exist.
Live graph Preview sends the same assigned geometry envelope before applying its unsaved canvas
to the authored scene. The Engine refuses an unsupported vertex path before the preview runtime
receives a replacement graph; a supported assignment still previews without saving an asset.

The sine sway example first needs numeric sine in the material vocabulary. `Sin` is appended to the material IR and graph operation enums, preserving existing operation identities. The text front end and engine-owned node palette both lower it to the same typed IR operation; the emitter writes Slang `sin` and constant folding uses the same radian operation. This arithmetic addition is shared by surface and future vertex expressions and does not itself enable vertex outputs.

Material catalogue schema 3 publishes a stage mask per node from the engine vocabulary: surface only for closures, texture samples, custom Slang, and the surface output; shared for typed numeric inputs and math. The editor reads older schemas with an unrestricted mask and filters its surface palette using the engine value. Vertex-only nodes and the vertex canvas will use the same mask rather than a second Rust vocabulary.

Named engine catalogue nodes for object position, camera-relative world position, normal, and UV0
now lower to fixed typed material attributes. The hosted viewport supplies both positions to the
vertex function and carries them to fragment evaluation; visible and shadow entry points use the
same binding. A typed time node now reads monotonic elapsed seconds from the first-light hosted renderer's
per-frame uniform for sine-driven offset expressions, without changing the material program.
The engine material IR now includes a typed three-dimensional value-noise operation, exposed by
both the graph palette and `noise(position)` text syntax. Slang compiles it in a vertex expression;
the node rejects non-float3 coordinates. A typed procedural wind operation samples three
decorrelated noise channels at an animated coordinate and returns a float3 vector; both graph and
text front ends lower to the same IR operation. This is not an `environment-fields` sample; a
separate typed field node reads the engine-owned wind. The typed RGB vertex-colour node reads
`color0` from an added RGBA mesh stream in the first-light compiled-material preview, including
shadow vertex evaluation;
the generated sample colours faces by normal axis, while uncoloured vertices default to white.
The material cook's saved-world discovery now matches the editor's terrain-layer assignment:
live layers under `TerrainAuthoring` add a `Terrain` variant, including when a material is shared
with a static mesh. CYVG mesh references and terrain layers now supply their named compiler paths
through saved-world discovery. The authored scene frame now
compiles vertex offsets and interpolants for its static meshes, including visible, depth, and
shadow passes. The authored scene binds the weather-owned `wind` field. The first-light material
preview now uses the same `WindFieldPreview` provider when a field graph is active. Its renderer
uploads the Engine image into the global descriptor table's storage-buffer slot and keeps the
origin in f64 until each camera's relative offset is written to the material field block. Camera
motion outside the preview region republishes the weather image. The shader assembler supplies
the mesh's camera-relative position to both vertex and fragment contexts. Other field names
receive an explicit refusal. The procedural wind node remains a separate operation. A native
material-mesh image comparison is encoded but has not run on this sandbox's unavailable Metal
device.

The field path must use `environment::build_deterministic_field_image` for the weather-owned
`environment::fields::kWind` declaration, retain the image origin, and pass coordinates made local
with `environment::image_local` before GPU sampling. The generated material prelude now imports
`cy.field`, selects the IR field's typed components from the sampler's float4 result, and carries
one bindless slot and camera-to-image offset per field. The Engine frame declares `cy.field`'s set 0
binding 3 and writes caller-supplied field-image buffers into its bounded table every frame. The
authored frame now runs one deterministic clear-weather tick around the viewed scene, binds its
weather-owned wind image, and uploads the image origin through a separate per-frame material field
buffer. This keeps camera motion out of the material's authored parameter layout. The editor
refreshes that image when the viewport camera leaves the preview region and rejects unbound field
names during preview. A headless regression compares several image positions
and vertical cells against the WeatherSystem's `FieldStore`; native pixel evidence remains open.
The GPU buffer has no world origin in its words: `FieldGpuImage::origin_x` and `origin_z` are kept
separately so large world coordinates can be subtracted in f64 before sampling. A material shader
therefore needs each bound field's origin relative to the current camera, plus the vertex's
camera-relative position; it cannot treat a material parameter as the field value or use one
unqualified position for fields with different origins. The same field table and coordinate
transform must be used at current and previous frame times for motion vectors. The host must set
`CyMaterialContext.fieldPosition` to the vertex's camera-relative position and each binding's
`cameraToImage.xz` to camera world position minus the image origin; its y component supplies the
camera's absolute world height, because the field sampler interprets y as absolute metres.

### Authored scene pipeline integration (open)

`AuthoredFrame` currently asks `graph_diffuse_colour` for one constant diffuse value per graph.
`FrameRecorder::draw_layer` binds one fixed pipeline before walking a layer, so it cannot select
a compiled material variant for an individual `DrawItem`. `FramePipelines` has three descriptor
sets; the generated material prelude expects a per-material parameter binding. The scene path
must retain the engine-compiled program and its parameter layout instead of reducing it to a
colour. Its material slot and geometry source select the variant for each draw, and the frame
recorder binds the matching vertex/fragment program and parameters under a compatible pipeline
layout. Pipeline states are prepared when the graph changes, before recording a frame.
The frame recorder now has a per-draw prepared-pipeline selector and can bind a material's fourth
descriptor set. A selected depth or shadow program can request the normal and UV streams in
addition to position. Null-backend command-log tests verify both paths. The scene shader assembler
now compiles a sine-sway vertex expression to MSL for visible, depth, and shadow entries using the
frame's actual camera-relative transforms, engine time, and material parameter binding. Depth
reevaluates the expression at the previous time sample for velocity. The compiler test runs
without a Metal device. `FramePipelines` can now create caller-owned geometry variants from those
vertex modules with the standard pass attachments, depth rules, and three vertex streams; a null
backend regression covers all four geometry passes. The authored frame now compiles saved or
previewed `.cygraph` vertex roots through the engine material compiler to MSL or SPIR-V, retains
depth, opaque, and shadow pipelines by material slot, and selects them per draw. Parameter defaults
use the compiled layout in a fourth descriptor set. A null-RHI scene test verifies actual draw
selection, graph edit rebuild, and restoration of the standard pipeline. The visible variant now
compiles a graph surface fragment beside the vertex program. `cy.frame` shares its forward lighting
function with the standard fragment, so compiled surfaces retain the frame's light, irradiance-probe
and AO treatment. The visible vertex passes typed interpolants and object position to that fragment;
shadow and depth use the same offset without surface varyings. The frame pipeline accepts a
caller-owned fragment only for opaque and transparent geometry, keeping fixed depth and shadow
outputs. The authored mesh has no vertex-colour or second UV stream, so those graph attributes are
refused at shader assembly. MSL and SPIR-V vertex/fragment compilation, graph lowering, and null-RHI
draw selection are tested. Native image comparison remains open. The authored frame now supplies
elapsed and delta time from its frame
clock, retains previous object placements by stable entity ID and previous camera origin, and
binds those rows beside each graph material's parameters. The depth vertex evaluates the same
offset with previous time and placement, and the frame retains temporal history across unchanged
scene topology. A graph edit, new mesh, or topology change cuts history. The null-RHI regression
inspects the actual bound previous-transform buffer across object and camera motion and a graph
edit. The native authored-frame test now encodes a visible and shadow comparison against a
CPU-translated version of the same mesh and material; that comparison has not run on this sandbox
because it has no Metal device. Motion vectors still need comparison with a CPU reference.
The local macOS sandbox currently selects the null RHI when Metal is requested. The authored-frame
pixel tests now check the actual selected backend before making image assertions, so its all-black
null framebuffer cannot be mistaken for a failed Metal rendering result. A device-backed run is
still required for the issue's visual acceptance criterion.
An MCP wire test submits an editable vertex canvas, accepts the engine authoring response, reads
the saved source, and checks undo and redo of both the canonical graph and canvas files. Scene and
preview-mesh vertex pixels still need device-backed verification. A separate wire test previews
the same vertex canvas without writing either asset file or adding an undo transaction.

The depth prepass, visible pass, and shadow pass must resolve the same draw to the same vertex
expression. Depth also evaluates the expression with previous-frame time and transform for
velocity, while its current clip position uses the current expression. The previous frame's
inputs must be retained per view, with the first frame producing no history-dependent motion.
Typed interpolants belong to the visible program's varying layout and surface context; they are
not needed in the shadow or depth fragment programs. A compiled variant that cannot satisfy a
pass or geometry source fails validation before a draw, with the compiler's diagnostic carried
back to the editor. A scene device test will compare visible pixels, shadow placement, and
velocity against CPU-displaced geometry using the same time samples, then repeat after a graph
edit and scene save/reopen.

## Verification

Use compiler-registry parity, service, save/reopen/cook, transaction/MCP, and live preview tests. Image tests compare VFX and displaced material output to committed references; displaced shadows and motion vectors are compared with CPU-displaced geometry. The authored scene render accepts an optional explicit frame time so a two-frame sine graph can be compared with the same mesh translated on the CPU at the corresponding times; ordinary editor frames still use elapsed time. For each acceptance criterion, record a mutation that makes its ledger criterion fail.
