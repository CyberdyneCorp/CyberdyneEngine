# Design

## One resolver for both run recipes

`run-engine` and `run-editor-live` must agree on four things the two processes use to find each
other: the project's absolute path, the world's project-relative path (its identity crosses the live
protocol), the viewport socket and the control socket. `_engine-session` resolves all four once:

- `--project` is required and resolves against the directory `just` was invoked from
  (`CY_CALLER_DIR`, set by the public recipe, since every recipe `cd`s to the repository root).
- `--world` defaults to the first `worlds/*.cyworld` by name.
- The sockets are `/tmp/cy-<cksum of the project path>-{viewport,control}.sock`, so two projects can
  be open at once and the same project always gets the same pair. `/tmp`, not `$TMPDIR`, because a
  Unix socket path is limited to about a hundred bytes and macOS's per-user directory spends half.

`tools/ci/test_recipes.py` holds those four properties and the refusals.

## Engine defaults

`run-engine` passes `--seconds 0` (no time limit) and `--no-validation` unless `--validation` is
given. The host reads the **first** occurrence of a flag, so forwarded arguments go before the
defaults and can override them.

`run-editor-live` removes stale sockets, starts the host in the background with its output in
`<build>/engine-live.log`, waits against a 60-second deadline for both sockets (reporting the log if
the host exits first), then runs `just run-editor` with `--host`, `--open` and
`CY_VIEWPORT_SOCKET`. An `EXIT` trap stops the host however the editor ends.

## Where the template's schema comes from

The editor is a client of the engine and must not link it, so it cannot ask the registry for the
schema. The template embeds the committed manifest with `include_str!`, the same file
`a_person_opens_a_world_and_saves_it.rs` already embeds, and that
`cy_test_unit_scene_serialization` regenerates and compares. One file, gated from the engine side.

The loader rule changes in one place: a world that declares **no** types falls back to the project
manifest, like a missing world. A world that declares any types keeps reading only its own, so no
type can be declared twice.
