# Design: native iOS platform support

## Evidence before implementation

The external spike at `~/cyberdyne-spikes/m11e-mobile-spike/` produced a signed arm64 UIKit/Metal
application with Xcode 27 and installed it on a physical iPhone 16 running iOS 27. The same CMake
toolchain configured SDL 3.4.16 for UIKit and Metal, then the engine stopped at its hard-coded
platform gate in `cmake/modules.cmake`. The SDK and SDL dependency are therefore available; the
missing work is Cyberdyne's platform selection and application target.

## Platform boundary

All iOS-specific code lives under `platform/ios/` and `samples/11-ship/ios/`. Shared core, server,
scene, and RHI interfaces remain unchanged. The port implements the existing `Platform` and
`DisplayServer` contracts and forwards touch input through `InputServer`.

iOS owns the application loop. UIKit calls the sample's frame callback from `CADisplayLink`; the
runtime does not acquire a desktop `main()` loop. The display owns one fullscreen `UIWindow` and a
root `UIView` backed by `CAMetalLayer`. `create_surface(GraphicsApi::Metal)` returns that layer to
the existing Metal RHI.

Unsupported desktop facilities fail explicitly: subprocesses and dynamic library loading return
`ErrorCode::Unsupported`; window movement, resizing, title bars, and additional windows are not
pretended. The three writable directory calls resolve inside the application sandbox.

## Build and signing

The committed toolchain selects `CMAKE_SYSTEM_NAME=iOS`, the requested SDK, arm64 for devices, and
an explicit deployment target. Signing identities, team identifiers, and bundle identifiers stay
outside the toolchain and are supplied to the sample target through cache variables.

The ordinary cross-compile target does not require signing and is suitable for CI. The device app
requires signing only when it is built or installed.

The sample is a deliberately bounded mobile workload rather than a copy of the desktop M10
artefact: a full-screen procedural terrain shader renders an orbiting view while the sun completes
a day/night cycle. Its overlay and `CY_IOS_FPS` records report presentation rate. This proves the
UIKit → platform surface → Metal RHI path and provides a repeatable hardware workload without
claiming that the complete desktop world assembly has been ported to the constrained renderer.

## Verification

Device-free tests verify platform identity, supported features, explicit refusals, one-window
semantics, touch translation, and Metal surface shape. The device run must additionally prove that
the signed app installs, launches, creates a real Apple GPU device, acquires a drawable, submits a
clear/draw, presents it, and emits the success marker. A missing marker is a failure.

`just run-ios-simulator` is the repeatable lifecycle regression: it builds through the committed
simulator toolchain, installs the bundle, launches it, verifies the same platform, native Metal,
and continuous-presentation markers required on hardware, verifies that the UIKit process remains
alive, and captures the presented frame. Simulator FPS is never recorded as hardware evidence.

`just run-ios-device` is the physical evidence path. It queries CoreDevice and refuses simulated or
disconnected targets, installs the signed bundle, attaches to the application console for a bounded
measurement window, requires the platform contract and native Metal readiness markers plus at
least three FPS samples, and captures the physical display. The raw console remains under the build
tree; the screenshot and generated evidence summary are reviewable source artefacts.
