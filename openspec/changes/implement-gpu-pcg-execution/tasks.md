# Tasks

## 1. Contract and CPU reference

- [x] 1.1 Add the canonical candidate record, integer generation function, and digest to `cy::pcg`.
- [x] 1.2 Test non-empty populations, seed sensitivity, and stable digest reproduction.

## 2. Portable GPU adapter

- [x] 2.1 Add one Slang source and checked-in SPIR-V/MSL outputs.
- [x] 2.2 Add `cy::pcg-gpu` without adding an RHI dependency to `cy::pcg`.
- [x] 2.3 Dispatch through the RHI and compare every GPU record against the CPU reference.

## 3. Evidence

- [x] 3.1 Add an Apple Metal integration test whose absence cannot pass as agreement.
- [x] 3.2 Run on real Apple hardware and record device, accepted count, and digest.
- [x] 3.3 Document the NVIDIA/Vulkan command and explicitly retain the two-vendor closure gap.
