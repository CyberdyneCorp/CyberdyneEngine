# Create an empty project and author it against the engine, with `just`

## Why

There was no supported way to start from nothing and author a scene against the engine:

- **No recipe started the engine for editing.** `just run-editor-runtime` starts the Rust
  `cy-runtime-stub`, which holds no world. The engine host, `cy_editor_window_runtime`, was started
  by hand with eight flags copied from the README — a documented procedure with no recipe, which
  `developer-workflow-and-just` calls a gap.
- **The engine host shut itself down after three minutes.** `--seconds` defaults to 180, which suits
  the artefact drivers that use it and ends an authoring session mid-edit.
- **Nothing created a project.** `cy_editor_services::Template` declared an `empty` template that
  only a unit test called; the README told people to copy the M5.5 sample, which is not empty.
- **The `empty` template was not authorable.** It wrote no `types.cytypes` and a world containing
  only `cyworld 1`. A world file that exists is read with its own schema only, so the schema was
  empty and `scene.create-primitive` wrote an entity with a `MeshRenderer` and **no `Transform`** —
  nothing the gizmo or `scene.translate` could move.
- **Two processes had to be paired by hand** — the same project, the same world path, two socket
  paths — and a mismatch produced an editor with no viewport and no error.

## What Changes

- `cyberdyne-editor --new-project <dir> [--template <name>]` creates a project from a built-in
  template and exits.
- Every built-in template carries the engine's type manifest (the committed
  `samples/05b-editor-window/project/types.cytypes`, which the engine's unit suite regenerates and
  compares byte for byte), names the project after its directory, and declares `worlds`.
- A world file that declares no types is given the project's manifest when opened, the same as a
  world that does not exist yet.
- New recipes:
  - `just content-new-project <dir> [--template <name>]`
  - `just run-engine --project <dir> [--world <path>]` — the engine host, with no time limit.
  - `just run-editor-live --project <dir> [--world <path>]` — the engine and an editor attached to
    it; closing the editor stops the engine.
- The README's macOS editor section is rewritten around these recipes.
- Two unused `allocator_` fields in `src/weather/` are removed: Apple clang 21 rejects them under
  `-Werror`, which stopped `just build-engine` — the first step of both new run recipes.

## Non-goals

- **The engine host's renderer is unchanged.** It still draws every node as a unit box, at most 64,
  through `samples/03-first-light`'s renderer. Replacing it with `FrameAssembly` and real meshes is a
  separate change.
- No New Project window in the editor; the recipe and the flag are the entry points.
- Headless `--script` sessions still pump once before exiting, so a scripted edit may not reach an
  attached engine. That is not part of this change.

## Impact

- `editor/crates/cy-editor-services` (templates, document loading), `editor/crates/cy-editor-app`
  (the flag), `just/run.just`, `just/content.just`, `just/build.just`, `tools/ci/test_recipes.py`,
  `README.md`, `editor/README.md`, `src/weather/`.
