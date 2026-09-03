# `example-null` — the module template

A module that does nothing. It exists so that the module system has a subject: discovery, the
`CY_MODULE_EXAMPLE_NULL` option, registration ordering and exclusion are all demonstrated here, and
a contributor adding a module copies this directory.

```
modules/example-null/
├── module.json                             the manifest — authoritative
├── CMakeLists.txt                          the target, declared with cy_add_module()
├── include/cy/modules/example_null.h       the public header: what dependents may use
└── src/example_null.cpp                    the implementation
```

## Adding a module

1. Copy this directory to `modules/<your-name>/`.
2. Edit `module.json`. Every key is required and unknown keys are rejected; the name must match the
   directory name.
3. Rename the target in `CMakeLists.txt` to `cy_module_<your_name>` with hyphens replaced by
   underscores — `cmake/modules.cmake` derives that name from the module's and fails if the module
   does not declare it.
4. Configure. `CY_MODULE_<YOUR_NAME>` exists, `cy_modules.h` records it, and the module initialises
   at its registration level.

Nothing else registers the module: there is no list to add it to. The manifest is the declaration
and the discovery finds it.

## What this module demonstrates

| Property | Where it shows |
|---|---|
| Discovery from a manifest, not a hard-coded list | no reference to this directory anywhere else |
| The generated option and header agree | `#error` in `src/example_null.cpp` when they do not |
| The generated table is well-formed | `CY_MODULE_TABLE` expanded and counted at compile time |
| Exclusion, not runtime stubbing | `-DCY_MODULE_EXAMPLE_NULL=OFF` compiles none of this |
| Layer and registration level are distinct | `"layer": "core"` and `"registration_level": "Core"` are two fields, and a module may set them independently |

**Governed by**: `project-and-plugins`, `engine-architecture` (module system).
