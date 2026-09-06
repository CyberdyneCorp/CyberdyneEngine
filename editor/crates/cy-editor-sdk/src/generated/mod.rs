// GENERATED FILE — DO NOT EDIT.
//
// Written by tools/gen/rust/sdk_gen.py from src/abi/include/cy/abi/cy_abi.h, through the
// description tools/abi/abi_describe.py produces and tools/abi/abi_gate.py diffs against
// src/abi/abi_baseline.json. Edit the C header or the generator; regenerate with
// `just build-editor --generate`, and `cargo test -p cy-editor-sdk` fails when this file is stale.

//! Everything in this directory is produced from the C ABI description. The safe API that sits on
//! top of it is hand-written in the crate root's other modules, because the decisions it makes —
//! what a null handle means, which lifetime a borrowed string has, when a borrow must be
//! re-validated — are not in the C declarations and a generator that invented them would be a
//! second, unchecked copy of the ABI.

pub mod abi;
pub mod enums;
pub mod ffi;
pub mod interface;

#[cfg(test)]
mod layout;
