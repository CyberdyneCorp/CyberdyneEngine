# `tools/project/fixtures/`

Task 4.4. Each fixture is a project the graph validation **must** reject, plus the ones it must
accept. `../selftest.py` runs them and asserts the outcome *and* that the diagnostic names both ends
of the collision — a message that named neither would satisfy "the build fails" and fail the
requirement, which asks for the build to fail naming both modules.

A fixture that stops failing means the check stopped firing. Fix the check; the fixture is the
evidence, not the problem.

| Fixture | Checked by | Must |
|---|---|---|
| `cycle/` | `graph.py` | fail: three modules depend on one another in a ring |
| `undeclared-dependency/` | `graph.py` | fail: a module includes a header owned by a module it does not declare |
| `missing-module/` | `graph.py` | fail: a declared dependency names a module the project does not contain |
| `layer-violation/` | `graph.py` | fail: a core module depends on a scene module |
| `private-leak/` | `graph.py` | fail: a public header reaches a privately declared dependency |
| `editor-in-shipping/` | `graph.py` | fail: a shipping target reaches an editor module |
| `unknown-key/` | `schema.py` | fail: a mistyped manifest key is reported, not ignored |
| `incompatible-plugin/` | `graph.py` | fail: a plugin's engine API range excludes this engine |
| `malformed-json/` | `schema.py` | fail: a manifest that is not JSON, reported with the line it stopped at |
| `valid/` | all of the above | pass, and render a header |
| `target-graph-violation/` | `cmake/project.cmake` | fail configure: a layer 0 target reaches a layer 4 target through a link `cy_add_module()` never saw |
| `target-graph-legal/` | `cmake/project.cmake` | pass configure: the same two targets, wired downward |

`valid/` is also the worked example the README quotes, so it cannot rot into an example that does not
work: every run of `selftest.py` validates it, renders it twice, and checks that the `android`
platform override reaches the generated header.

The two `target-graph-*` fixtures are configured, never built: the check under test is a
configure-time one and the sources exist only because CMake requires a target to have some. The rest
are never even configured — they are read by `project.py`.

These fixtures are *inside* the tree the repository's own gates scan, and unlike
`tools/layercheck/fixtures/` they are not excluded from it — so they are kept clean rather than
exempted. The sources are formatted, none of them includes an engine header, and `support.cmake`
declares no target of its own: it pulls `cy_compile_options` out of `cmake/compilers.cmake`, so
there is no bare `add_library()` here for `layercheck --check targets` to find. A fixture's
*wrongness* is in its manifest and in how its targets are wired, never in a file that a source gate
reads.
