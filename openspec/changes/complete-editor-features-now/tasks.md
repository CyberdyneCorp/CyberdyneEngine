# Tasks

## 1. Scope and baseline

- [x] 1.1 Record the implementable-versus-dependent Editor feature matrix in the change README and verify every unchecked M11.b Editor task is mapped to this change, an owning active change, or a named unavailable producer.
- [x] 1.2 Capture the current command, panel, workspace, MCP resource, and interaction-test inventories and verify the baseline is reproducible on the clean worktree.
- [x] 1.3 Add contract tests preventing this branch from editing viewport transport, RHI, or native Metal ownership areas and verify the changed-file check rejects a fixture path from each area.

## 2. Document lifecycle and workspace restoration

- [x] 2.1 Add service-level guarded-close outcomes for clean, save, discard, cancel, and save failure; verify dirty content cannot be removed without an explicit outcome.
- [x] 2.2 Coordinate document close and active-document selection atomically in `Editor`; verify closing the active, inactive, last, and missing document cases.
- [x] 2.3 Add document-tab presentation state and intents without authoritative document copies; verify activation changes no dirty state or history.
- [x] 2.4 Render document tabs with active and dirty cues and Save/Discard/Cancel close flow; add shell regression tests for click, keyboard focus, cancellation, and save failure.
- [x] 2.5 Persist versioned per-user open-document and per-document view state; verify restart restoration, missing assets, and unknown newer fields are handled without changing project content.
- [x] 2.6 Add an attributed Undo History view model and panel over the active document; verify actor, agent intent, cursor, undo/redo availability, and truncation are presented from authoritative history.

## 3. Settings and source-control panels

- [x] 3.1 Wire `SettingsService` into `Editor` and add a settings view model for category search, defaults, project overrides, platform overrides, and user preferences; verify view-model refresh is revision-driven.
- [x] 3.2 Register settings commands for typed writes and resets and verify project changes are canonical while user preferences never enter the project diff.
- [x] 3.3 Add the Settings panel and navigable settings search results; verify modified values, scope, validation failures, reset, compact density, and keyboard operation.
- [x] 3.4 Wire `SourceControlService` and the existing source-control view model into `Editor` using observable background refresh; verify idle frames never invoke provider processes.
- [x] 3.5 Register provider-neutral refresh, history, checkout, revert, submit, lock, and unlock commands with correct effect classes; verify unsupported capabilities retain provider refusals.
- [x] 3.6 Add the Source Control panel with status, history, capability-aware actions, progress, and failures; verify Git, Perforce recording doubles, null provider, and real temporary Git repository cases.

## 4. Hierarchy authoring

- [x] 4.1 Extend hierarchy intents for replace, additive, subtractive, and visible-range selection; verify selection order and mixed-value inspector behavior headlessly.
- [x] 4.2 Register transactional node rename and reparent commands; verify exact undo/redo, stable identity, invalid names, missing nodes, and cycle refusal.
- [x] 4.3 Add inline rename with commit, cancel, and coalescing; verify focus loss cannot create an accidental transaction.
- [x] 4.4 Add identity-based drag/drop reparenting and keyboard equivalents; verify filtered rows never convert visible indexes into document identities.
- [x] 4.5 Add hierarchy creation-from-template affordances over registered commands and verify no unsupported visibility or lock control is rendered.

## 5. Swift Workspace foundation

- [x] 5.1 Add `SourceWorkspaceService` with project-relative discovery, file fingerprints, external-change detection, and observable revisions; verify traversal refusal, Swift filtering, and deterministic ordering.
- [x] 5.2 Add source-buffer view models with dirty state, cursor/selection, base revision, diagnostics, and explicit reload/keep/merge conflict state; verify in-progress text is not project state.
- [x] 5.3 Extend source save commands with expected fingerprints and structured conflict results; verify human, MCP, and external edits cannot silently overwrite one another.
- [x] 5.4 Add the Swift Workspace file tree and tabbed source editor with syntax-aware presentation and keyboard editing; verify opening and editing do not write until Save.
- [x] 5.5 Connect existing Swift build and module-reload operations with structured progress and diagnostics; verify build failure preserves buffers and reload reports preserved and dropped state.

## 6. Shared JSON wire and SourceKit-LSP

- [x] 6.1 Extract the dependency-free JSON value/parser/writer into `cy-editor-json`; verify all MCP malformed-input, Unicode, depth, integer, and byte-round-trip tests remain green.
- [x] 6.2 Add a `cy-editor-sourcekit` process client behind Editor-owned request/response types; verify initialize, shutdown, crash, timeout, and unavailable-toolchain behavior against a recording process.
- [x] 6.3 Implement versioned document synchronization and diagnostics; verify stale publications are discarded after a newer buffer revision.
- [x] 6.4 Implement completion, hover, workspace symbols, rename, definition, and reference requests where advertised; verify missing server capabilities disable actions with an explanation.
- [x] 6.5 Connect SourceKit results to the Swift Workspace and diagnostic navigation; verify selecting a diagnostic opens the exact file, line, and column.
- [x] 6.6 Exercise the client against the installed `sourcekit-lsp` and a temporary Swift package; verify editing and building remain available when the process is then terminated.

## 7. Interactive desktop MCP

- [x] 7.1 Split MCP transport decoding/encoding from command execution while preserving the `AgentTransport` boundary; verify the existing wire suite remains byte-compatible.
- [x] 7.2 Add a bounded `AgentConnectionService` request queue and per-request response path; verify backpressure, deadlines, budgets, and disconnect cleanup without a window.
- [x] 7.3 Drain agent intents through the UI-owned `Editor` and shared registry; verify interleaved human and agent transactions retain order and human conflicts supersede agent work explicitly.
- [x] 7.4 Add pending desktop confirmation for irreversible and external effects plus time-bounded grants; verify reversible edits do not prompt and revocation cancels uncommitted work.
- [x] 7.5 Add agent status UI showing identity, intent, scope, budget, current operation, pause, and revoke; verify keyboard access and non-colour state cues.
- [x] 7.6 Run MCP alongside the desktop window while retaining `--mcp --headless`; verify both modes expose identical tools/resources and the window stays responsive under a saturated client.
- [x] 7.7 Emit agent connection, invocation, refusal, transaction, render request, and cost records into Editor diagnostics; verify privacy classification and session attribution.

## 8. Semantic diff and merge

- [x] 8.1 Complete identity-keyed semantic comparison for every currently supported document operation and verify before/after revision cases for entities, components, fields, names, parents, and asset references.
- [x] 8.2 Implement three-way merge classification into automatic changes and typed conflicts; verify independent changes merge and same-target divergent changes never choose silently.
- [x] 8.3 Add merge resolution commands that accept local, incoming, or a validated replacement and commit one attributed transaction; verify undo restores the pre-merge document exactly.
- [x] 8.4 Add semantic Diff and Merge view models and panels with source-control revision inputs; verify panels never parse serialized files or mutate documents directly.

## 9. Asset-browser local operations

- [x] 9.1 Add deterministic folder navigation and combined name/type filtering to the asset view model; verify large listings remain virtualized and idle refresh is revision-driven.
- [x] 9.2 Register asset rename and move commands that move metadata together and preserve identity-based references; verify collisions, traversal, undo eligibility, and external-change failures.
- [x] 9.3 Add drag/drop intents for scene placement and inspector assignment through existing registered commands; verify one transaction per drop and identical MCP availability.
- [x] 9.4 Add import-settings presentation and command-driven edits for formats currently supported by the importer; verify unsupported settings and formats refuse by name.

## 10. Integration, quality, and documentation

- [x] 10.1 Run rustfmt, clippy with warnings denied, all Editor tests, OpenSpec strict validation, and relevant engine integration gates; compare any failure against clean `main` before attribution.
- [x] 10.2 Measure changed Rust functions manually and with available complexity tooling, split functions above the project targets, and record the lack of Rust support where numeric measurement is unavailable.
- [x] 10.3 Exercise dark/light themes, compact/comfortable density, narrow windows, keyboard-only flows, and screen-reader labels for every new panel; capture matching screenshots where rendering is available.
- [x] 10.4 Update `editor/README.md`, the visual-language documentation, command help, and roadmap evidence for the behavior actually delivered; keep renderer/backend dependencies explicitly open.
- [x] 10.5 Re-audit every item from task 1.1 against code, tests, runtime behavior, and documentation, and leave no feature classified as locally implementable without a completed task or an evidence-backed dependency.
- [x] 10.6 Map all 133 requirements in the nine Editor capability rows to executable evidence or an explicit M11.e deferral, and verify the coverage audit rejects missing, stale, or renamed evidence.

## 11. Engine-usable local integration

- [x] 11.1 Add `--project <directory>` before workspace restoration and document opening; verify a declared project is selected and an arbitrary directory is refused with the missing manifest named.
- [x] 11.2 Make Play and Pause update viewport state only after an attached runtime accepts the request; verify a disconnected editor remains in Editing and reports an actionable notification.
- [ ] 11.3 After the Metal backend change merges, rebase this branch and run the editor with `cy_editor_window_runtime` on macOS; verify the first IOSurface frame, Play/Pause/Stop, runtime loss, document survival, and restart in one connected acceptance run.
