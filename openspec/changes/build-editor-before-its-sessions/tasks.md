# Tasks

## 1. The fixture

- [x] 1.1 `samples/editor_fixture.cmake`: `smoke.editor_build` as the `cy_editor` fixture's setup, with
  `CY_EDITOR_BUILD_TIMEOUT`, and `cy_sample_requires_editor()`.
- [x] 1.2 The six editor-driving entries require `cy_editor` and drop `--build`.
- [x] 1.3 `samples/08a-authoring/authoring.py` locates the editor through `just _editor-binary`.

## 2. Regression tests

- [x] 2.1 `integration.editor_fixture`: red on a tree configured from `main` before this change (five
  entries without the fixture, four passing `--build`), green after.
- [x] 2.2 `tools/ci/test_recipes.py` "the authoring driver finds the editor the recipe built": red
  on the old driver (eight mismatches), green after.

## 3. Documentation

- [x] 3.1 `tests/editor/README.md`, the sample CMakeLists comments, and finding 6 of
  `docs/roadmap/capability-matrix.md`.
- [x] 3.2 `tools/roadmap/requirements-coverage.toml`: the taxonomy requirement's evidence.
