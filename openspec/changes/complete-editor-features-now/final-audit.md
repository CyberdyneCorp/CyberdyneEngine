# Final Editor feature audit

Audited on 2026-09-19 against the scope ledger in `README.md`, the implementation, the focused
regression tests, the desktop command projection, and the clean-worktree verification gates.

## Delivered surface

The change completes 43 functional task items across nine product capability areas. The desktop
command projection grows from the 59-command baseline to 81 commands. Every command remains the
same typed operation exposed to menus, the palette, scripts, and MCP; panels do not implement a
second mutation path.

| Capability | Implementation evidence | Regression evidence |
|---|---|---|
| Documents, restoration, and history | `documents.rs`, `workspace_store.rs`, document tabs, History panel | guarded close, atomic active-document selection, restart restoration, attributed history |
| Settings | `SettingsService`, settings view model and panel, typed setting commands | scope/default/override/preference, reset, validation, revision-driven refresh |
| Source control | provider-neutral service commands, view model, and panel | null/Git/Perforce doubles, temporary real Git repository, idle-process check |
| Hierarchy authoring | identity-based selection, rename, reparent, and template creation | exact undo, cycles/missing identities, filtered range selection, keyboard equivalents |
| Swift Workspace | source workspace service, buffers, tabs/editor, build and reload presentation | fingerprints/conflicts, no write before Save, failed-build preservation, reload report |
| SourceKit-LSP | `cy-editor-sourcekit`, versioned synchronization, language actions and navigation | recording process plus opt-in live installed-toolchain package exercise |
| Desktop MCP | bounded UI-owned connection queue, confirmations/grants, session controls and audit | desktop/headless parity, saturation, deadlines, rollback, cancellation, privacy attribution |
| Semantic diff and merge | identity-keyed comparison, three-way classification, one-transaction resolution, panels | independent/divergent edits, typed replacement, exact undo, no panel-side parsing/mutation |
| Asset browser | folder/filter navigation, move/rename, drops, import settings | virtualization, collision/traversal/stale checks, sidecars, one transaction per drop |

## Complexity review

The repository cognitive-complexity skill reports no analyzable functions because its supported
language set does not include Rust. As a Rust-native numeric cross-check, Clippy's
`cognitive_complexity` lint was run over every workspace target with a temporary threshold of 12.
The only changed production function above 12 was semantic comparison's `diff_node` at 13, which
is below the backend/service target of 15 and is already separated from operation construction and
merge resolution. New production UI functions remained at or below 12. A few scenario-style test
functions scored 13–15; they are linear assertion fixtures rather than shipped control flow and
were retained for readable end-to-end narratives. Manual review found no changed production
function above its applicable project target.

## Verification record

- `cargo fmt --all -- --check`: passed.
- `cargo clippy --workspace --all-targets -- -D warnings`: passed.
- `cargo test --workspace`: passed; the installed-SourceKit case is deliberately opt-in and was
  exercised separately with `CY_RUN_SOURCEKIT_LIVE=1` against `/usr/bin/sourcekit-lsp`.
- `openspec validate complete-editor-features-now --strict`: passed.
- `python3 tools/editor/feature_scope.py`: passed, confirming no viewport transport, RHI, native
  Metal, renderer, or backend ownership paths entered this change.
- The Editor requirement audit passed with 133 of 133 requirements mapped across all nine rows: 91
  map to executable tests, proven criteria, or gates, and 42 are explicit M11.e deferrals with named
  re-entry tests. The implemented and tested process boundary, tick-boundary edit application,
  policy classification, state preservation, and refusal behavior advance `editor-architecture`
  and `live-editing` from Seed to Working; the deferrals keep both below Complete.
- The 56-case theme/density/width accessibility matrix and pointer-free keyboard check passed for
  all seven new panels; the headless harness has no raster screenshot path, as recorded in the
  change README.
- The SDK generator self-test passed 17/17 cases. The canonical `just build-editor-check` wrapper
  now runs on macOS Bash 3.2 and passed rustfmt, Clippy with warnings denied, the full workspace test
  suite, and doctests. The physical native-window smoke command also passed through
  `just run-editor --smoke` and closed after its bounded three-frame draw.
- The editor SDK now loads the ABI fixture through the native Windows wide-character loader as well
  as through the Unix loader. The generated-inspector integration suite therefore exercises the
  real interface table, reflected catalogue, edit transaction, and engine-side value write on the
  Windows editor leg instead of stopping at an unsupported-loader error.

## Dependencies intentionally left open

Nothing classified as locally implementable in the scope ledger remains unimplemented. Open rows
all require an unavailable producer or an already-owned workstream: renderer images and diagnostic
views, encoded remote-device transport, general cook/package/deploy and device install services,
native debugger/profiler transports, stable offline artefact schemas, domain-specific canonical
authoring vocabularies/lowerings, and the remaining engine ABI/lifecycle callbacks. The UI does not
render placeholder controls for those dependencies.
