# `tools/gen/reflect/` — the reflection generator

Run through `tools/gen/reflect_gen.py`, and through `just generate-headers`, `just generate-check`
and `just quality-identity`. See `tools/gen/README.md` for the overview and the dependency, and
`src/core/reflect/README.md` for what the output is registered against.

| Module | Concern |
|---|---|
| `annotations.py` | The grammar of a `CY_REFLECT_*` argument list. |
| `attrspec.py` | `core-type-system`'s attribute table, module-declared schemas, and validation. |
| `model.py` | What a parse produced, with no libclang in it. |
| `parse.py` | libclang: the frontend, the pruned descent, the include digests. |
| `manifest.py` | The committed identity manifest: assignment, tombstones, the gate. |
| `emit.py` | The generated C++. |
| `cache.py` | The content-keyed incremental cache. |
| `cli.py` | The command line, and the four things it does. |

The split is not decoration. `model.py` deliberately carries no frontend type, which is what lets
the manifest, the emitter and the cache be reasoned about — and mostly tested — without libclang
present, and what would make replacing the frontend a change to `parse.py` alone.

## Declaring an attribute of your own

A module writes a TOML schema and names it in CMake with `--attributes`. The generator emits one
struct per attribute, with one member per parameter, into that module's generated header, and a
constexpr instance per use. See `src/core/reflect/reflect_attributes.toml` for a worked example and
`cy::reflect::find_custom<T>()` for how a consumer reads one.

## Tests

`src/core/reflect/tests/test_generator.py`. It lives beside the module it generates for rather than
here, because what it actually asserts is the *identity* behaviour of that module — and because
`ctest` should run it.
