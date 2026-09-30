# Make the CI test legs pass on every hosted runner

## Why

The four `test` jobs of the `ci` workflow (linux-x86_64, linux-arm64, macos-arm64, windows-x86_64)
failed on main in every run since they first ran. Some failures were product defects that only a
hosted runner exposed (a Windows path, a Windows text-mode pipe, a paravirtual Metal GPU, a Darwin
backtrace taken on the alternate signal stack, fused multiply-adds on arm64); some were tests that
assumed a device, a scheduler or a build step the runner does not have. Two requirements change in
what they promise on a platform: the remote mount now serves on Windows, and a macOS crash artefact
now carries the faulting thread's stack.

## What changes

- **Remote file serving on Windows.** `cy/core/assets/remote` gains a WinSock half behind the same
  calls as the POSIX one; `remote_serving_available()` is true on Windows.
- **macOS crash backtraces.** The fault handler walks the interrupted thread's frame-pointer chain
  from the signal context instead of calling `backtrace()`, which returns nothing on Darwin from the
  alternate signal stack.
- **Metal on a device that cannot make argument encoders** (the hosted runner's "Apple Paravirtual
  device") refuses a descriptor set as `Unsupported` instead of terminating the process with an
  uncaught NSException; the Metal device suites skip, saying why, on a device on the compatibility
  path.
- **Windows defects:** the crash directory is created under an absolute drive path; `File::read_at`
  leaves the file cursor where it was; `cy_play_runtime_host` writes its protocol in binary mode;
  test executables find Slang's DLLs; the cook test passes its scratch path in generic form.
- **Water exactness on arm64:** `cy_water` and its tests compile with FP contraction off, as `cy_pcg`
  already does, so the ring vertices are bit-identical to `evaluate_displacement()` on every target.
- **Tests that assumed the runner:** the PCG device suite skips loudly without a device; the two
  concurrency teardown cases wait for their threads to be running; `smoke.ship` reports its ELF-only
  symbol act as not evaluated on Mach-O and PE; `smoke.shader_targets` expects two targets on macOS,
  where Slang fetches no DXC, and runs as three shards that each fit the smoke budget;
  `smoke.authoring` finds the editor where `just build-editor` puts it; the static-light identity
  case of the lightmap bake bakes at 16 samples.
- **CI:** the test and agent jobs build the editor before the suites that drive it, so no test pays
  for a cold cargo build inside its CTest timeout.
