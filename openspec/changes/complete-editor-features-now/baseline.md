# Editor feature baseline

Captured on 2026-09-19 from clean `main` commit `13395e7` in the isolated
`feat/editor-features` worktree.

## Reproduction

```sh
cd editor
cargo run -q -p cy-editor-app --bin cyberdyne-editor -- --list-commands
cargo test -p cy-editor-mcp
cargo build -p cy-editor-testhost --bin cy-runtime-stub
cargo test -p cy-editor-app
rg -n '^\s*#\[test\]|^\s*#\[.*test' crates -g '*.rs'
find crates -path '*/tests/*.rs' -type f | sort
cd ..
python3 tools/editor/feature_scope.py --selftest
python3 tools/editor/feature_scope.py
```

## Inventory

| Surface | Baseline |
|---|---|
| Registered commands | 59 projected command lines from `--list-commands` |
| MCP resources | 13 kinds: hierarchy, node, selection, documents, assets, diagnostics, play, operations, sources, build, history, viewport, budget |
| Shell panel implementations | hierarchy, inspector, content browser, console, problems, profiler, viewport |
| Placeholder panel kinds | script graph, animation, and any unrecognised plugin/saved-layout panel |
| Specialized domains registered | 16 |
| Specialized domains technically openable | materials, animation graphs/clips, abilities/effects, gameplay/utility graphs, sequences/cinematics |
| Specialized domains with a complete pin catalogue | materials |
| Engine view modes exposed as commands | 19 |
| Engine view modes named but unavailable | 8 |
| Rust `#[test]` occurrences | 998 |
| Cross-crate integration test source files | 31 |
| Interaction-target cases | palette search, workspace/dock, 100,000-asset virtualization, selection-to-inspector, idle-query cost |

## Verified baseline results

- `cargo test -p cy-editor-mcp`: 14 unit tests, 13 wire integration tests, and one doctest passed.
- `cargo test -p cy-editor-app`: passed after explicitly building its declared
  `cy-runtime-stub` test binary prerequisite.
- `openspec validate complete-editor-features-now --strict`: passed after all planning artifacts
  were created.
- `python3 tools/editor/feature_scope.py --selftest`: rejects representative viewport-transport,
  RHI, Metal, and backend-change paths while accepting an Editor-shell path.

The first combined application invocation exposed a harness-order dependency: Cargo did not place
the `cy-runtime-stub` executable before the crash-survival test despite it being a dev dependency.
Building that binary explicitly made the unchanged application suite pass. This change must not use
that ordering issue to weaken crash coverage.
