# `src/rendering/material/` — layer 4

The BRDF, the material model and its parameter storage, the standard material, the fallbacks, and
material validation.

**Governed by**: `rendering-materials-and-shading`. Arrived at M3, tasks 4.2.3–4.2.5.

## Why the BRDF exists as C++ and not only as Slang

The specification is unusually concrete here, and says why:

> The BRDF is specified concretely — exact terms and their sources — because "PBR" alone is not a
> specification and mismatched terms produce subtly wrong lighting that is very hard to debug later.

A shading term that can only be evaluated on a GPU is a term whose energy conservation is checked by
looking at a screenshot. Every term in `brdf.h` is a free function over cosines, so `material`'s
suite checks the properties that actually matter — that GGX is normalised, that Fresnel is `f0`
head-on and `f90` at grazing, that a metal has no diffuse, that rough metals get their energy back —
in arithmetic, with no device.

That paid for itself immediately. **The stable GGX form was transcribed without its `NoH`**
(`k = α / (1 − NoH² + α²)` rather than `α / (1 − NoH² + (NoH·α)²)`), which integrates to 0.80 at
α = 0.5 and 0.60 at α = 0.81: every rough highlight was quietly losing energy that multi-scatter
compensation could not distinguish from the loss it exists to correct. `material_ibl` now integrates
`∫D·NoH·dω` over the roughness range and asserts it is one.

## The three parts

| | |
|---|---|
| `brdf.h` | the core BRDF (`D`, `V`, `F`, three diffuse models), multi-scatter compensation, the DFG table, octahedral environment maps, L2 irradiance |
| `material.h` | parameters and their compile-time identifiers, `MaterialProgram`, and the **GPU material table** |
| `standard.h` | the standard material's slot set, its defaults, and the three fallback materials |
| `validation.h` | what cooking reports and what it refuses |

## Parameter storage: a table, not a descriptor set per material

`rendering-materials-and-shading` requires that a GPU-generated draw reach its parameters with no
per-object binding:

> **WHEN** a GPU-generated draw shades a pixel **THEN** it SHALL index the material table using the
> instance's material identifier, with no per-object descriptor binding

So a material's parameters are **bytes at an index**: `index * kMaterialBlockBytes`, a
multiplication with nothing to look up first. Every block is the same size, a parameter is addressed
by a compile-time hash of its name, and a change marks one interval that the frame uploads in one
transfer. A material *instance* is a slot plus a program — allocating one copies 256 bytes and
compiles nothing.

## Validation is a report, not a boolean

The four things the specification asks to be reported are not the same severity, and one answer
would force the wrong behaviour on two of them. A parameter nobody reads is a note; a subsurface
material with an additive blend mode has no defined shading and must not cook. So every finding
carries its own `fatal` flag and `MaterialReport::fatal()` is what a cooker branches on.

`validate_material()` returning `ok()` means validation **ran**. A cooker that could not tell "could
not validate" from "validated and it is wrong" would treat a full disk and a broken material the
same way.

## What is deliberately not here

* **The material compiler** — M7's, and design.md §5 is explicit that M3 does not build it. What is
  here is the *shape* a compiled program has, so an authored graph lowers into it at M7 and nothing
  downstream changes.
* **Anything that touches a device.** The table is bytes; whoever owns a device uploads them. That
  is what lets the DFG bake — the one piece of the image-based lighting chain that would otherwise
  need a GPU — run in continuous integration.
