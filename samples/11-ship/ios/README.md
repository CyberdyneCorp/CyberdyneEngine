# iOS compute presentation

This application presents the mobile terrain and day/night shader together with a character moved
by `SkinPass` and a spark plume simulated by `VfxGpuPass`. The character draw reads the current
skinning output as its vertex stream. The particle draw reads the VFX particle and liveness buffers
in its vertex shader and renders the fixed capacity, so the CPU never reads a live-particle count.

The iOS presets deliberately set `CY_SHADER_SLANG=OFF`. The graph compiler cooks the plume metadata
at startup, while `shaders/mobile_vfx_msl.h` supplies the native shader produced during the asset
cook. The app hashes the generated Slang and refuses to start if the checked-in shader is stale.

The combined physical-device tier uses a 42% linear drawable scale, three terrain-noise octaves,
and 32 ray-march steps. Skinning has one pass per Metal frame slot, while the VFX parameter buffer
is uploaded once and its persistent simulation state remains ordered on the graphics queue.

To recook after changing `mobile_vfx_effect.cpp`, build and run the host compiler fixture to emit
the dispatch source, compile it to Metal, then refresh the embedded header and hash:

```bash
cmake --build build/dev --target cy_test_integration_vfx_compiler
(cd build/dev && ./cy_test_integration_vfx_compiler '[vfx][compiler]')
cp build/dev/vfx-dispatch.slang samples/11-ship/ios/shaders/mobile_vfx.slang
build/dev/Development/bin/slangc samples/11-ship/ios/shaders/mobile_vfx.slang \
  -target metal -entry cyVfxKernel -stage compute \
  -o samples/11-ship/ios/shaders/mobile_vfx.metal
python3 src/vfx/gpu/shaders/embed_msl.py \
  samples/11-ship/ios/shaders/mobile_vfx_msl.h \
  kMobileVfxMsl=samples/11-ship/ios/shaders/mobile_vfx.metal
```

Update `kMobileVfxSlangHash` and `kMobileVfxSlangBytes` from the copied source. The physical-device
runner is the final check because it requires the combined workload marker from a presented frame.

## RTS stress tier

`CY_IOS_RTS_STRESS=ON` builds the opt-in capacity scene: 500 copies of the 36-vertex model are
processed in one skin dispatch, and 100 `VfxGpuPass` instances retain independent 512-slot
simulations. Use the repository recipe so the runner also checks the exact model and emitter
counts:

```bash
CY_IOS_DEVELOPMENT_TEAM=YOUR_TEAM_ID \
CY_IOS_BUNDLE_IDENTIFIER=com.example.cyberdyne \
  just run-ios-rts-device YOUR_DEVICE_UDID
```

The stress recipe has no minimum FPS threshold because it measures capacity rather than replacing
the normal scene's 55 FPS release gate. It still rejects missing frames, a substituted workload,
CPU particle readback, simulator hardware, and incomplete evidence.
