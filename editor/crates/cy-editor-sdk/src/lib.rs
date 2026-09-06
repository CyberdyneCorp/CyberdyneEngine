//! The editor SDK: a safe Rust layer over the Cyberdyne C ABI. Task 2.3.
//!
//! This crate is the **only** place in the editor workspace where `unsafe` appears, and the only
//! place that names a C type. `editor-rust-application` requires both: "`unsafe` code SHALL be
//! confined to narrow, audited interoperation and platform modules, and SHALL NOT appear in panels,
//! view models, services, or domain logic", and "Editor code SHALL use the SDK; it SHALL NOT call
//! the C ABI directly outside the SDK crate."
//!
//! Every other crate carries `#![forbid(unsafe_code)]`, and `cy-editor-app`'s `tests/safety.rs`
//! asserts that they do — a `forbid` cannot be lifted by an inner `allow`, which is the property
//! that makes it worth writing.
//!
//! --- WHAT IS GENERATED AND WHAT IS DECIDED --------------------------------------------------------
//!
//! [`generated`] is written by `tools/gen/rust/sdk_gen.py` from the ABI description that
//! `tools/abi/abi_describe.py` computes — the same single parse that produces the committed ABI
//! baseline and the Swift overlay. It contains the `#[repr(C)]` mirrors, the enums, one typed raw
//! call per table entry, and the layout assertions. It contains no decisions.
//!
//! The decisions are here, in [`host`], [`world`] and [`schema`], and each says which sentence of
//! `cy_abi.h` it implements: that a null `CyWorld` means no world is bound, that a `const char*` the
//! engine returned is UTF-8 borrowed for the engine's lifetime, that a `CyBorrow` must be
//! re-validated against the world's epoch before every read. A generator that invented those would
//! be a second, unchecked copy of the ABI, which is the failure the generated half exists to
//! prevent.
//!
//! --- WHAT DOES NOT CROSS THE BOUNDARY --------------------------------------------------------------
//!
//! No raw pointer reaches an editor caller. [`world::World`] borrows its interface, entities are
//! [`cy_editor_core::ids::RuntimeEntity`] value types, strings are copied into owned `String`s at
//! the boundary, and every failure arrives as a [`cy_editor_core::Problem`] with a reason and,
//! where one exists, a remedy.

pub mod generated;
pub mod host;
pub mod schema;
pub mod value;
pub mod world;

pub use generated::abi;
pub use generated::enums::{InitLevel, Severity, Stage, Status, VarType};
pub use generated::interface::{CallError, Interface};
pub use host::{HostingMode, Runtime, RuntimeLibrary};
pub use world::World;
