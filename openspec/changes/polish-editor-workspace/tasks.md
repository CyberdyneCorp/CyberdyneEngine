# Tasks

## 1. Baseline

- [x] 1.1 Confirm the proposed shell-polish scope with the user before implementation.
- [x] 1.2 Build and capture the current editor with reproducible content; record invocation, size, and theme.

## 2. Shell polish

- [x] 2.1 Flatten resting menus and toolbar controls; verify hover, disabled, and keyboard-focus states in both themes.
- [x] 2.2 Add authoritative active-transform feedback; test toolbar and shortcut changes with a regression test.
- [x] 2.3 Add selected-tab underlines and refine hierarchy selection; verify selection remains legible in monochrome and errors stay visible.
- [x] 2.4 Replace implementation counters with content summaries, preserving diagnostic access; verify summaries derive from current filtered rows and selection; retain counters on hover.
- [x] 2.5 Refine search and empty-state typography and guidance; verify compact and comfortable density with empty and no-result panels; populated-scene visual review follows Metal integration.

## 3. Review

- [x] 3.1 Run relevant shell/interface tests, rustfmt, and clippy; compare any suspected unrelated failures with main before attributing them.
- [x] 3.2 Capture matching before/after editor views, inspect theme and density variants, and update the visual-language documentation with the result. A sample world verified browser item and hierarchy selection counts. Narrow-window and rendered-scene visual review remain follow-up checks; no renderer is available on macOS yet.
- [x] 3.3 Validate the OpenSpec change and review modified Rust functions for maintainable control flow; record the complexity tool's lack of Rust support.
