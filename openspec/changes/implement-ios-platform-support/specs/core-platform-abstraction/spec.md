## ADDED Requirements

### Requirement: Native iOS port

The engine SHALL provide an iOS platform implementation under `platform/ios/` that implements the
existing platform, display, touch-input, and Metal-surface boundaries without adding iOS conditionals
to shared engine interfaces.

#### Scenario: UIKit owns the frame loop

- **WHEN** an iOS application becomes active
- **THEN** UIKit SHALL drive engine frames from the display refresh callback
- **AND** the runtime SHALL NOT require the desktop host loop

#### Scenario: Metal surface

- **WHEN** the application requests a Metal surface for its single fullscreen window
- **THEN** the display server SHALL return the view's `CAMetalLayer`
- **AND** the existing Metal RHI SHALL create and present a swapchain from it

#### Scenario: Mobile limitations are explicit

- **WHEN** a caller requests subprocesses, dynamic libraries, extra windows, or desktop window
  operations on iOS
- **THEN** the platform SHALL return an explicit unsupported error
- **AND** SHALL NOT silently claim the operation succeeded
