# `tools/layercheck/`

The source-level half of the layer enforcement. Tasks 1.3.2, 1.3.3 and 1.3.4.

The layer order is enforced twice, because neither check sees what the other does:

| Where | Sees | Catches |
|---|---|---|
| `cmake/module.cmake` | every link a target declares | a `cy_add_module()` dependency pointing upward, at configure time |
| `layercheck.py` | every `#include` a file writes | an upward include through a path CMake was never told about, and an SDL header outside `platform/` |

A translation unit can include a header from a layer above it without any target declaring the link —
a shared include directory, a header that happens to be reachable, a target that links a third
target transitively. CMake never sees that, and it is what fires in practice.

```
just quality-layers            # both, and the fixtures
python3 tools/layercheck/layercheck.py                 # the tree
python3 tools/layercheck/layercheck.py --check sdl     # one check
python3 tools/layercheck/selftest.py                   # the fixtures alone
```

## The checks

- **includes** — a file may include a header at its own layer or below, never above. A file's layer
  comes from its directory (`src/core/` is 0, `platform/` is 3, and so on); an include's layer comes
  from the leading component of the include path, with `cy/` and `src/` stripped so that
  `"core/expected.h"`, `"src/core/expected.h"` and `<cy/core/expected.h>` all resolve alike. The
  match is textual on purpose: it is right for a header that has not been written yet, and it does
  not depend on how the include directories happen to be set up.
- **sdl** — no SDL header appears outside `platform/`. `design.md` §4: SDL3 sits beneath the
  engine-owned `Platform` and `DisplayServer` interfaces, and a native backend at M11 validates the
  abstraction against a second implementation. The layer rule alone does not cover this — `platform/`
  is layer 3, so `src/runtime/` including SDL is *downward* and legal by layer, and wrong.
- **targets** — no bare `add_library` or `add_executable` in the engine tree. Every engine target is
  declared through `cy_add_module()`, which records its layer; a bare target has no layer, so nothing
  constrains what it links. `cmake/` is out of scope: it is the build system itself, and it declares
  `cy_add_module()`, the `cy_compile_options` interface, and the shims wrapping third-party targets.
  Inside the scope there is one exemption table, in `layercheck.py`, and each row carries its reason.

## Cost

Under 100 ms on the M0 tree; excluded directories are pruned rather than filtered, so a build tree
beside the sources costs nothing to skip. It is meant to run on every pull request and it does.

## Adding a layer or a directory

Three places, and they must agree: `LAYER_OF_NAME` and `DIRECTORY_LAYERS` in `layercheck.py`, and
`CY_LAYER_NAMES` / `CY_LAYER_INDICES` / `CY_LAYER_INDEX_<name>` in `cmake/module.cmake`. The
authority is the table in `engine-architecture` ("Layered architecture") and `design.md` §1.
`cmake/modules.cmake` reads the CMake vocabulary rather than repeating it.

**Governed by**: `engine-architecture` (Layered architecture), `project-and-plugins` (Architectural
layering is enforced), `build-system-and-platforms` (Platform porting surface).
