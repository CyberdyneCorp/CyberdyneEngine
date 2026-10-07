# Tasks

## 1. SwiftPM crashes

- [x] 1.1 Find the cause: every signal-11 crash examined was on the image's Swift 6.4, whose
  default build system is Swift Build; no 6.0.3 job crashed.
- [x] 1.2 `swift_pin_version` in the justfile; `setup-swift` at that version in every Linux x86_64
  job that builds the tree.
- [x] 1.3 `check_workflows.py`: the pin rule, with negative fixtures in `--selftest`.
- [x] 1.4 `cy_swift_module.py`: `--build-system native`, and a bounded, logged retry on a crash
  signal only.
- [x] 1.5 `test_recipes.py`: the classification, the bound and the native build system, each shown
  red by a mutation of the driver.

## 2. Intermittent tests

- [x] 2.1 #80: reproduce the `over budget:` failure of the probe's vfork case at scale 0.25, and run
  the probe at scale 1.
- [x] 2.2 The `survive` race: `cy-runtime-stub` reports `connected` at the Hello; regression test red
  on the old stub.
- [x] 2.3 macOS `fp_policy`: warm libm with the executable; `unit.harness_image_warmup` red on the old
  warm-up with 142 pages unmapped.
- [x] 2.4 #77: attribute the frame's growth to the water passes; recorded as not fixed here.

## 3. Documentation

- [x] 3.1 `docs/guides/swift.md`, `bindings/swift/README.md`, `tests/harness/README.md`.
- [x] 3.2 `tools/roadmap/requirements-coverage.toml`: the swift-scripting "Build and packaging" note.
