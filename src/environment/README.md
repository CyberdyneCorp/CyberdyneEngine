# `src/environment/` — CyberField

The shared environment substrate: sparse, tiled, world-scale data addressed by world position, with
**exactly one producer per field**. M10 section 1, and `environment-fields` reaching Working.

This is the milestone's substrate. Six other M10 rows — terrain, water, foliage, weather and wind,
atmosphere and sky, and procedural content generation — write into it or read from it, and it was
built first and alone so that it is not a substrate designed around its first two producers.

## What is here

| file | what it carries |
|---|---|
| `include/cy/environment/field.h` | `FieldId`, `FieldDeclaration`, the declaration's own rules, `ProducerToken`, `FieldRegistry` and the refusal that names both producers |
| `include/cy/environment/store.h` | `TileAddress`, `FieldStore`, the two sampling paths, `FieldWriter`'s staged writes, `FieldReader`'s firewall, the change queue |
| `include/cy/environment/gpu.h` | `FieldGpuImage` — the field's GPU memory image — and `sample_field_image()`, the shader's algorithm run on the CPU so it can be compared |
| `include/cy/environment/streaming.h` | `FieldStreaming`: cells bound through `world::CellEventQueue`, tiles paged through `residency::ResidencyServer` |

## The five decisions a reader should know before changing anything

**1. One producer per field is both refused and unspellable.** `FieldRegistry::claim()` fails the
second claim and names the incumbent and the challenger in the error's own message. That is the
diagnostic. The guarantee is `ProducerToken`: it is issued once, it is move-only, and it is the only
thing `FieldStore` accepts a write through — so a second writer cannot be written even by a caller
that ignored the error.

**2. There are two sampling paths and the FIELD decides which one a reader gets.** `sample()` walks
the levels from finest to coarsest; `sample_deterministic()` reads one declared level and returns the
declared default where it has no data. A gameplay-visible field is sampled deterministically for
everyone, the renderer included, because "terrain material, foliage placement and audio observe the
same value" is false the moment one of them is reading the finest resident level and the others are
not. Dispatching on the *reader's* class would have broken exactly the fields where it matters.

**3. A gameplay-visible field is constrained at declaration, not checked at every sample.** It may
not be GPU-produced, and its `gameplay_level` must be one declared `resident_everywhere`. Both are
refusals from `validate_declaration()`. A runtime check would have reported the defect after it had
already produced two different answers on two machines.

**4. The GPU side duplicates the CPU side on purpose.** `sample_field_image()` reads the encoding,
the range, the interpolation and the layer rule out of the image's own header and decodes the stored
bytes with its own arithmetic. It calls neither `decode_value()` nor `combine_layers()`. Two
implementations that agree are evidence; one implementation called twice is not.

**5. Nothing here is a parallel streaming or residency mechanism.** `FieldStreaming` is a registered
consumer of `world::CellEventQueue` and a client of `residency::ResidencyServer`. It owns no budget,
no eviction rule and no importance model, and it does not register the residency subsystem — the
budget belongs to whoever owns the world.

## Designed for producers that do not exist yet

Every axis a producer might vary is a field of `FieldDeclaration` rather than a case in this module's
code: the quantity's shape (one to four components, or an integer category), its storage encoding,
its vertical extent (1 for planar, more for volumetric — one sampling path with a degenerate case,
not two), which resolutions exist, how a baked base and a runtime delta combine, what the field means
for the simulation, and whether it recovers toward a declared potential.

A project declaring a `radiation` field writes a declaration and changes no engine code.
`tests/test_declaration.cpp` holds that as a case.

## What is NOT here, and what M10's later rows owe

- **No shader.** `gpu.h` fixes and documents the buffer layout and tests the algorithm on the CPU; no
  `.slang` module accompanies it and no agreement against a real device is claimed. Writing
  `cy/field.slang` against this layout, binding the buffer through the GPU scene, and measuring the
  agreement on a device belongs to the renderer-facing row that first samples a field in a shader.
  **`environment-fields`' "CPU and GPU access" requirement is therefore half-discharged here**, and
  the half that is missing is the device.
- **No wind model.** The wind field's transient sources — a shape, a strength, a lifetime, a budget,
  and the lowest-priority source dropped deterministically — belong to `weather-and-wind`, which the
  specification names as the wind field's producer. What this module provides is the substrate the
  contributions layer into.
- **No persistence.** A field declares itself `persistent`; writing its runtime changes into the
  world persistence overlay is `save-and-persistence`'s encoding and is not written here.
- **No cooked tile format.** `FieldStreaming::set_loader()` is the seam a cooked source plugs into; a
  field with no loader materialises its declared default.

## Suites

    ctest --test-dir build/<label> -R "environment" --output-on-failure

`environment` is the unit suite (declaration, the refusal, the store, determinism); `environment_gpu`
and `environment_streaming` are integration suites, because a measurement over thousands of positions
and a test that drives a real residency server are both over the unit budget by design.
