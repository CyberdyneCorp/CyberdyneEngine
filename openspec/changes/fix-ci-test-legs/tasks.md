# Tasks

## 1. Product defects
- [x] 1.1 Create the Windows crash directory under an absolute drive path (`_mkdir("C:")` is not
      EEXIST). Regressions: `diagnostics.crash`, `diagnostics.test_reproduction`,
      `integration.stub_platform`, `smoke.empty_sample` on windows-x86_64.
- [x] 1.2 Keep the file cursor across `File::read_at` on Windows. Regression: `integration.assets_io`.
- [x] 1.3 Write `cy_play_runtime_host`'s replies in binary mode on Windows. Regression:
      `integration.editor_play`.
- [x] 1.4 Serve and fetch the remote mount over WinSock. Regression: `integration.assets_remote`.
- [x] 1.5 Walk the faulting thread's frames from the signal context on macOS. Regression:
      `diagnostics.crash` on macos-arm64.
- [x] 1.6 Refuse a Metal descriptor set on a device that cannot make argument encoders instead of
      terminating. Regression: `integration.rhi_metal`'s "a descriptor set is allocated or refused,
      and never ends the process".
- [x] 1.7 Compile `cy_water` and its tests with FP contraction off. Regression: `unit.water` built
      with `-mfma` on x86-64, and on linux-arm64.
- [x] 1.8 Find the editor where `just build-editor` puts it in `samples/08a-authoring`. Regression:
      `smoke.authoring`.

## 2. Tests that assumed the runner
- [x] 2.1 Skip the PCG device suite loudly without a device, and the Metal device suites on the
      compatibility path.
- [x] 2.2 Wait for the reader and writer threads to run in the virtual geometry and virtual texturing
      teardown cases.
- [x] 2.3 Report `smoke.ship`'s symbol act as not evaluated on Mach-O and PE (`symbols.binary_format`,
      covered by `unit.build_symbols`).
- [x] 2.4 Expect two shader targets on macOS, where Slang fetches no DXC.
- [x] 2.5 Put Slang's DLL directory on the Windows test PATH.
- [x] 2.6 Pass the cook test's scratch path in generic form.
- [x] 2.7 Shard `smoke.shader_targets` into three cases (`shadertool::Options::shard/shards`) that fit
      the smoke budget on a hosted runner, together covering every entry point.
- [x] 2.8 Bake the static-light identity case of `integration.render_lightmap_bake` at 16 samples.

## 3. CI
- [x] 3.1 Build the editor in the test and agent jobs before the suites that drive it.
