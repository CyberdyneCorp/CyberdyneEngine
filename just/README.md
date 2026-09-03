# `just/`

One file per recipe category, `import`ed by the root `justfile`. Recipe names are flat and
hyphenated — `<category>-<verb>` — so that bare `just` lists **every** recipe with its description,
which `just` modules would not do. See `design.md` §10.

| File | Category | Covers |
|---|---|---|
| `env.just` | Environment | Diagnosis, toolchain setup, first-time bootstrap |
| `build.just` | Build | Engine, editor, tools, shaders; per profile and per platform |
| `run.just` | Run | Samples, runtime host, headless runtime, editor |
| `test.just` | Test | Unit, integration, smoke, benchmarks |
| `quality.just` | Quality | Format, lint, layering, spec validation |
| `generate.just` | Generate | ABI bindings, overlays, reflection data, their currency check, and the generator test suite |
| `content.just` | Content | Import, cook, package, patch |
| `diagnose.just` | Diagnose | Trace inspection, crash artefacts, log collection |
| `roadmap.just` | Roadmap | Capability status, milestone exit criteria |
| `maintenance.just` | Maintenance | Clean, caches, dependency update |
| `release.just` | Release | Version, changelog, artefacts, publication |

Import, rather than one large file, gives every category a single owner.

**Recipes a merge gate must call.** These exist and pass today; nothing else runs them, because
`.github/` does not exist yet (task 2.4.x).

| Recipe | Checks | Cost |
|---|---|---|
| `quality-layers` | Layer order: declared links, source-level includes, the `platform/` SDL rule, and no bare target. Runs the negative fixtures, so enforcement that stopped firing is caught. | under a second |
| `generate-check` | `cy_features.h` and `cy_modules.h` are current and regeneration is reproducible. Needs a configured build; honours `CY_BUILD_DIR`. | seconds |
| `generate-test` | The generators, the feature options and the module graph: undeclared dependencies, cycles, layer violations, exclusion. | about 25 s |
| `maintenance-deps-check` | `THIRD_PARTY.md` matches `deps/manifest.toml`. | instant |
| `maintenance-deps-test` | A disabled feature fetches, builds and links nothing. Configures twice. | minutes on a cold cache — periodic, not per-commit |

**Rules that apply to every recipe here**
- It orchestrates; it does not build. Incremental logic belongs to CMake, Cargo or the Slang
  toolchain, and a recipe that tracks timestamps itself is a defect.
- It forwards the exit status of what it invokes, and never swallows it.
- It begins `cd "{{root}}"` if it touches the tree. `just` 1.21 runs a recipe in the directory of
  the file that *defines* it, so a recipe in this directory starts here, not at the repository root.
- It carries a one-line description, because those descriptions are the workflow's documentation.
- A destructive recipe names what it will remove and confirms, unless invoked non-interactively.

**Overrides** available to every recipe, reported through `_report-override` when active:
`CY_PROFILE` (the default profile), `CY_BUILD_DIR` (a build tree other than the preset's
`build/<profile>/`), `CY_JOBS` (build parallelism).

**Governed by**: `developer-workflow-and-just`.
