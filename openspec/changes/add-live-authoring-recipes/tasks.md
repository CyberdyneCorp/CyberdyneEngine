# Tasks

## 1. An authorable empty project

- [x] 1.1 Built-in templates write `types.cytypes`, name the project after its directory, and declare
  `worlds`.
- [x] 1.2 A world file that declares no types is given the project's type manifest on open.
- [x] 1.3 `cyberdyne-editor --new-project <dir> [--template <name>]`.
- [x] 1.4 Regression tests: `a_new_project_can_be_authored.rs` (fails without 1.1 and 1.2) and the
  flag's unit test.

## 2. Recipes

- [x] 2.1 `just content-new-project <dir> [--template <name>]`.
- [x] 2.2 `just run-engine --project <dir> [--world <path>]`, with no time limit.
- [x] 2.3 `just run-editor-live --project <dir> [--world <path>]`, stopping the engine on exit.
- [x] 2.4 `_engine-session` resolution held by `tools/ci/test_recipes.py`.

## 3. Build and documentation

- [x] 3.1 Remove the unused `allocator_` fields in `src/weather/` that Apple clang 21 rejects.
- [x] 3.2 README: create an empty project and author it with the recipes.
- [x] 3.3 editor/README: workflow list, and the stale "arrives at task 5.2" hosting note.
