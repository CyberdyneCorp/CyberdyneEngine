// GENERATED FILE — DO NOT EDIT.
//
// Written by tools/gen/rust/sdk_gen.py from src/abi/include/cy/abi/cy_abi.h, through the
// description tools/abi/abi_describe.py produces and tools/abi/abi_gate.py diffs against
// src/abi/abi_baseline.json. Edit the C header or the generator; regenerate with
// `just build-editor --generate`, and `cargo test -p cy-editor-sdk` fails when this file is stale.

//! The ABI this SDK was generated against.
//!
//! `MAJOR` and `MINOR` are what the SDK asks `cy_get_interface` for. The engine's rule, stated in
//! `cy_abi.h`, is that a request for a minor at or below the engine's own succeeds and returns the
//! engine's table — so an SDK generated against 1.1 works against any 1.x engine with
//! x >= 1, and is refused by an older one rather than reading a short table.
//!
//! `TABLE_SIZE` is the byte size of `CyInterface` as this SDK declares it. The engine reports its
//! own in `CyInterfaceHeader::table_size`, and a table SMALLER than this one is a runtime the SDK
//! must refuse: the entries past its end are not there. A LARGER one is fine and expected — that is
//! what an append looks like from the old side of it.

/// The ABI major version. A change here is a break, and the SDK will not talk to a different one.
pub const MAJOR: u32 = 1;

/// The ABI minor version. Appends increment it; the SDK works against this or any later minor.
pub const MINOR: u32 = 1;

/// The ABI patch version, which carries no compatibility meaning and is reported for diagnostics.
pub const PATCH: u32 = 0;

/// `size_of::<CyInterface>()` as this SDK declares it. Asserted against the compiler in `layout`.
pub const TABLE_SIZE: u32 = 320;

/// How many function-pointer entries `CyInterface` has, not counting its header.
pub const TABLE_ENTRY_COUNT: usize = 38;
