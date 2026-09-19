# `platform/stub/` — the porting surface's own proof

Two desktop backends — `desktop-sdl3/` and `linux-native/` — disagree about which library opens a
window and **agree about everything a desktop takes for granted**: a command line, a mouse, a
resizable window, several writable directories, a process table, a dynamic loader, many cores. A
porting surface exercised only by desktops is a desktop interface, and no number of desktop backends
discovers that.

This one shares **no desktop assumption at all**:

| It has | It does not have |
|---|---|
| A monotonic clock and a wall clock | A command line — `argument_count()` is zero |
| Somewhere to write text | An environment, an executable path, a dynamic loader |
| **One** writable directory, which all three user directories answer | A process table — every subprocess call is `Unsupported` |
| **One** window, which is the whole display and is not resizable | A second window, a title bar, a position, a mode to change |
| One screen, with no reported refresh rate | A pointer, a keyboard, a gamepad — there is no input source here at all |
| A fixed 512 MiB memory budget | A crash handler, `Feature`s — `has_feature()` answers **false** to every one |

It is **not a mock and not a test double**. It is a real implementation of both interfaces, it builds
and links into the engine like any other port, and `integration.stub_platform` starts `Runtime` on it
and runs frames until the simulation has ticked — driving the loop with a plain `for`, because
`core-platform-abstraction` requires the main loop to be the platform's and not the engine's. What
makes it a stub is the smallness of what it offers, not the honesty of what it does.

```
just build-engine --platform stub     the port's dependency closure and its suite, and nothing else
just run-sample 00-empty --platform stub --frames 60
```

The restricted build is the point of the first: a port's closure that links and a suite that runs
prove the layers above need nothing the port does not have. Building the whole tree instead would
prove only that the desktop backends still compile.

**What it has already found**, in the run that first started the runtime on it:

- `Runtime::enter_display()`'s "a platform that cannot answer is not a startup failure" path had
  never been exercised. This is the only implementation in the tree whose `create_surface()` returns
  no handle for any API, and the warning it produces is the intended behaviour rather than a defect.
- Eight frames produce **no** simulation tick, because the fixed step is derived from the platform's
  own monotonic clock and eight frames take microseconds. Correct, and not what the first version of
  the test assumed — so the case now runs until a tick happens, which makes it a proof that the
  stub's clock drives the simulation rather than a count of calls.

**Governed by**: `core-platform-abstraction` (the porting surface), `build-system-and-platforms`.
