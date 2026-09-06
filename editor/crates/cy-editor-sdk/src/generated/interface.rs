// GENERATED FILE — DO NOT EDIT.
//
// Written by tools/gen/rust/sdk_gen.py from src/abi/include/cy/abi/cy_abi.h, through the
// description tools/abi/abi_describe.py produces and tools/abi/abi_gate.py diffs against
// src/abi/abi_baseline.json. Edit the C header or the generator; regenerate with
// `just build-editor --generate`, and `cargo test -p cy-editor-sdk` fails when this file is stale.

//! One method per `CyInterface` entry, generated so that the set cannot fall behind the table.
//!
//! Each method resolves its entry, reports a `Missing` when the runtime's table does not have it,
//! and converts a `CyResult` into a `Status`. That is all it does: no allocation, no string
//! conversion, no null checking of the caller's arguments. The safe API in the crate root is where
//! those decisions live, because they are decisions and this file contains none.

use super::enums::Status;
use super::ffi;

/// Why a call through the table did not produce a value.
///
/// Three cases, and the difference between them matters to a caller. `Missing` is a runtime older
/// than this SDK — the entry is not in its table at all, so nothing can be done but report which
/// one. `Failed` is the engine answering with a reason. `UnknownStatus` is an engine one minor
/// version AHEAD, returning a `CyResult` this build has no name for; it is kept distinct rather
/// than folded into `Failed(Unknown)` because the two call for different actions.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum CallError {
    /// The runtime's table has no entry of this name.
    Missing(&'static str),
    /// The engine reported a failure.
    Failed(Status),
    /// The engine returned a `CyResult` this build does not know.
    UnknownStatus(i32),
}

impl ::std::fmt::Display for CallError {
    fn fmt(&self, f: &mut ::std::fmt::Formatter<'_>) -> ::std::fmt::Result {
        match self {
            CallError::Missing(name) => {
                write!(f, "the runtime's interface table has no `{name}` entry")
            }
            CallError::Failed(status) => write!(f, "{status}"),
            CallError::UnknownStatus(raw) => {
                write!(
                    f,
                    "the engine returned CyResult {raw}, which this build has no name for"
                )
            }
        }
    }
}

impl ::std::error::Error for CallError {}

/// A validated pointer to a runtime's `CyInterface`.
///
/// Constructed only by the crate's `host` module, which is where the header is checked. Holding one
/// is the claim that the table is live and at least as large as `abi::TABLE_SIZE`; every method
/// below relies on that claim and on nothing else.
#[derive(Clone, Copy)]
pub struct Interface {
    table: *const ffi::CyInterface,
}

// SAFETY: the table is immutable for the lifetime of the runtime that published it — `cy_abi.h`
// requires `cy_get_interface` to return a table with static storage duration — so sharing the
// pointer across threads reads the same constant bytes from every one of them. The engine state the
// entries reach is a different question, and one the SDK's session types answer: they are what
// confine calls to the thread that owns the connection.
unsafe impl Send for Interface {}
unsafe impl Sync for Interface {}

impl Interface {
    /// Wrap a table pointer that has already been validated.
    ///
    /// # Safety
    ///
    /// `table` must be non-null, must point at a live `CyInterface` for as long as this value is
    /// used, and its `header.table_size` must be at least `abi::TABLE_SIZE`. `host::Runtime` is the
    /// only caller in this crate and it checks all three.
    #[must_use]
    pub const unsafe fn from_raw(table: *const ffi::CyInterface) -> Self {
        Self { table }
    }

    /// The table this interface wraps, for the crate's own layout and version checks.
    #[must_use]
    pub fn table(&self) -> &ffi::CyInterface {
        // SAFETY: the constructor's contract is that the pointer is non-null and outlives us.
        unsafe { &*self.table }
    }

    /// The header the runtime published: its ABI version and the size of its table.
    #[must_use]
    pub fn header(&self) -> ffi::CyInterfaceHeader {
        self.table().header
    }

    /// Emit a message through the engine's log.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn log(
        &self,
        engine: ffi::CyEngine,
        severity: u32,
        message: *const ::std::ffi::c_char,
    ) -> Result<(), CallError> {
        let entry = self.table().log.ok_or(CallError::Missing("log"))?;
        unsafe { entry(engine, severity, message) };
        Ok(())
    }

    /// The last error message on this thread, or null.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn get_last_error(&self) -> Result<*const ::std::ffi::c_char, CallError> {
        let entry = self
            .table()
            .get_last_error
            .ok_or(CallError::Missing("get_last_error"))?;
        Ok(unsafe { entry() })
    }

    /// The last error code on this thread. Never fails.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn get_last_error_code(&self) -> Result<i32, CallError> {
        let entry = self
            .table()
            .get_last_error_code
            .ok_or(CallError::Missing("get_last_error_code"))?;
        Ok(unsafe { entry() })
    }

    /// Set this thread's last error.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn set_last_error(
        &self,
        result: i32,
        message: *const ::std::ffi::c_char,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .set_last_error
            .ok_or(CallError::Missing("set_last_error"))?;
        unsafe { entry(result, message) };
        Ok(())
    }

    /// A heap-backed string `CyVar`.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn var_make_string(
        &self,
        engine: ffi::CyEngine,
        utf8: *const ::std::ffi::c_char,
        length: u64,
    ) -> Result<ffi::CyVar, CallError> {
        let entry = self
            .table()
            .var_make_string
            .ok_or(CallError::Missing("var_make_string"))?;
        Ok(unsafe { entry(engine, utf8, length) })
    }

    /// A heap-backed byte-buffer `CyVar`.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn var_make_bytes(
        &self,
        engine: ffi::CyEngine,
        data: *const ::std::ffi::c_void,
        size: u64,
    ) -> Result<ffi::CyVar, CallError> {
        let entry = self
            .table()
            .var_make_bytes
            .ok_or(CallError::Missing("var_make_bytes"))?;
        Ok(unsafe { entry(engine, data, size) })
    }

    /// A copy that owns its own heap allocation, if any.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn var_clone(&self, value: *const ffi::CyVar) -> Result<ffi::CyVar, CallError> {
        let entry = self
            .table()
            .var_clone
            .ok_or(CallError::Missing("var_clone"))?;
        Ok(unsafe { entry(value) })
    }

    /// Release a `CyVar`'s heap allocation, if any.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn var_release(&self, value: *mut ffi::CyVar) -> Result<(), CallError> {
        let entry = self
            .table()
            .var_release
            .ok_or(CallError::Missing("var_release"))?;
        unsafe { entry(value) };
        Ok(())
    }

    /// Live heap-backed `CyVar` count, for leak detection.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn var_live_count(&self, engine: ffi::CyEngine) -> Result<u64, CallError> {
        let entry = self
            .table()
            .var_live_count
            .ok_or(CallError::Missing("var_live_count"))?;
        Ok(unsafe { entry(engine) })
    }

    /// The world bound to this engine, or null.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn engine_world(&self, engine: ffi::CyEngine) -> Result<ffi::CyWorld, CallError> {
        let entry = self
            .table()
            .engine_world
            .ok_or(CallError::Missing("engine_world"))?;
        Ok(unsafe { entry(engine) })
    }

    /// Create an entity and return its identifier.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn world_create_entity(
        &self,
        world: ffi::CyWorld,
    ) -> Result<ffi::CyEntity, CallError> {
        let entry = self
            .table()
            .world_create_entity
            .ok_or(CallError::Missing("world_create_entity"))?;
        Ok(unsafe { entry(world) })
    }

    /// Destroy an entity.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn world_destroy_entity(
        &self,
        world: ffi::CyWorld,
        entity: ffi::CyEntity,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .world_destroy_entity
            .ok_or(CallError::Missing("world_destroy_entity"))?;
        let raw = unsafe { entry(world, entity) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Whether an entity identifier is live.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn world_entity_alive(
        &self,
        world: ffi::CyWorld,
        entity: ffi::CyEntity,
    ) -> Result<bool, CallError> {
        let entry = self
            .table()
            .world_entity_alive
            .ok_or(CallError::Missing("world_entity_alive"))?;
        Ok(unsafe { entry(world, entity) })
    }

    /// The world's structural-change counter.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn world_epoch(&self, world: ffi::CyWorld) -> Result<u64, CallError> {
        let entry = self
            .table()
            .world_epoch
            .ok_or(CallError::Missing("world_epoch"))?;
        Ok(unsafe { entry(world) })
    }

    /// Register a component type; 0 on failure.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn world_register_component(
        &self,
        world: ffi::CyWorld,
        descriptor: *const ffi::CyComponentTypeDesc,
    ) -> Result<ffi::CyComponentTypeId, CallError> {
        let entry = self
            .table()
            .world_register_component
            .ok_or(CallError::Missing("world_register_component"))?;
        Ok(unsafe { entry(world, descriptor) })
    }

    /// Look a component type up by name; 0 when absent.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn world_find_component(
        &self,
        world: ffi::CyWorld,
        name: *const ::std::ffi::c_char,
    ) -> Result<ffi::CyComponentTypeId, CallError> {
        let entry = self
            .table()
            .world_find_component
            .ok_or(CallError::Missing("world_find_component"))?;
        Ok(unsafe { entry(world, name) })
    }

    /// Add a component, optionally copying an initial value.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn world_add_component(
        &self,
        world: ffi::CyWorld,
        entity: ffi::CyEntity,
        component: ffi::CyComponentTypeId,
        initial: *const ::std::ffi::c_void,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .world_add_component
            .ok_or(CallError::Missing("world_add_component"))?;
        let raw = unsafe { entry(world, entity, component, initial) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Remove a component from an entity.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn world_remove_component(
        &self,
        world: ffi::CyWorld,
        entity: ffi::CyEntity,
        component: ffi::CyComponentTypeId,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .world_remove_component
            .ok_or(CallError::Missing("world_remove_component"))?;
        let raw = unsafe { entry(world, entity, component) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Whether an entity has a component.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn world_has_component(
        &self,
        world: ffi::CyWorld,
        entity: ffi::CyEntity,
        component: ffi::CyComponentTypeId,
    ) -> Result<bool, CallError> {
        let entry = self
            .table()
            .world_has_component
            .ok_or(CallError::Missing("world_has_component"))?;
        Ok(unsafe { entry(world, entity, component) })
    }

    /// Borrow a component's storage with the world's epoch.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn world_borrow_component(
        &self,
        world: ffi::CyWorld,
        entity: ffi::CyEntity,
        component: ffi::CyComponentTypeId,
    ) -> Result<ffi::CyBorrow, CallError> {
        let entry = self
            .table()
            .world_borrow_component
            .ok_or(CallError::Missing("world_borrow_component"))?;
        Ok(unsafe { entry(world, entity, component) })
    }

    /// Whether a borrow predates the world's current epoch.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn borrow_valid(
        &self,
        world: ffi::CyWorld,
        borrow: ffi::CyBorrow,
    ) -> Result<bool, CallError> {
        let entry = self
            .table()
            .borrow_valid
            .ok_or(CallError::Missing("borrow_valid"))?;
        Ok(unsafe { entry(world, borrow) })
    }

    /// Read one field as a `CyVar`.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn component_get_var(
        &self,
        world: ffi::CyWorld,
        entity: ffi::CyEntity,
        component: ffi::CyComponentTypeId,
        field: u32,
        into: *mut ffi::CyVar,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .component_get_var
            .ok_or(CallError::Missing("component_get_var"))?;
        let raw = unsafe { entry(world, entity, component, field, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Write one field from a `CyVar`.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn component_set_var(
        &self,
        world: ffi::CyWorld,
        entity: ffi::CyEntity,
        component: ffi::CyComponentTypeId,
        field: u32,
        value: *const ffi::CyVar,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .component_set_var
            .ok_or(CallError::Missing("component_set_var"))?;
        let raw = unsafe { entry(world, entity, component, field, value) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Read one `f32` field.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn component_get_f32(
        &self,
        world: ffi::CyWorld,
        entity: ffi::CyEntity,
        component: ffi::CyComponentTypeId,
        field: u32,
        into: *mut f32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .component_get_f32
            .ok_or(CallError::Missing("component_get_f32"))?;
        let raw = unsafe { entry(world, entity, component, field, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Write one `f32` field.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn component_set_f32(
        &self,
        world: ffi::CyWorld,
        entity: ffi::CyEntity,
        component: ffi::CyComponentTypeId,
        field: u32,
        value: f32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .component_set_f32
            .ok_or(CallError::Missing("component_set_f32"))?;
        let raw = unsafe { entry(world, entity, component, field, value) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Read one three-float field.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn component_get_vec3(
        &self,
        world: ffi::CyWorld,
        entity: ffi::CyEntity,
        component: ffi::CyComponentTypeId,
        field: u32,
        into: *mut f32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .component_get_vec3
            .ok_or(CallError::Missing("component_get_vec3"))?;
        let raw = unsafe { entry(world, entity, component, field, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Write one three-float field.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn component_set_vec3(
        &self,
        world: ffi::CyWorld,
        entity: ffi::CyEntity,
        component: ffi::CyComponentTypeId,
        field: u32,
        xyz: *const f32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .component_set_vec3
            .ok_or(CallError::Missing("component_set_vec3"))?;
        let raw = unsafe { entry(world, entity, component, field, xyz) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Register a behaviour type; null on failure.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn register_behaviour(
        &self,
        engine: ffi::CyEngine,
        name: *const ::std::ffi::c_char,
        vtable: *const ffi::CyBehaviourVTable,
    ) -> Result<ffi::CyBehaviourType, CallError> {
        let entry = self
            .table()
            .register_behaviour
            .ok_or(CallError::Missing("register_behaviour"))?;
        Ok(unsafe { entry(engine, name, vtable) })
    }

    /// Look a behaviour type up by name.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn find_behaviour(
        &self,
        engine: ffi::CyEngine,
        name: *const ::std::ffi::c_char,
    ) -> Result<ffi::CyBehaviourType, CallError> {
        let entry = self
            .table()
            .find_behaviour
            .ok_or(CallError::Missing("find_behaviour"))?;
        Ok(unsafe { entry(engine, name) })
    }

    /// The reload generation a behaviour is from.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn behaviour_generation(
        &self,
        behaviour: ffi::CyBehaviourType,
    ) -> Result<u32, CallError> {
        let entry = self
            .table()
            .behaviour_generation
            .ok_or(CallError::Missing("behaviour_generation"))?;
        Ok(unsafe { entry(behaviour) })
    }

    /// How many component types the world knows.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn world_component_count(&self, world: ffi::CyWorld) -> Result<u32, CallError> {
        let entry = self
            .table()
            .world_component_count
            .ok_or(CallError::Missing("world_component_count"))?;
        Ok(unsafe { entry(world) })
    }

    /// Describe a component type: size, alignment, field count.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn world_component_info(
        &self,
        world: ffi::CyWorld,
        component: ffi::CyComponentTypeId,
        into: *mut ffi::CyComponentInfo,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .world_component_info
            .ok_or(CallError::Missing("world_component_info"))?;
        let raw = unsafe { entry(world, component, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Describe one field of a component type.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn world_component_field(
        &self,
        world: ffi::CyWorld,
        component: ffi::CyComponentTypeId,
        field: u32,
        into: *mut ffi::CyFieldDesc,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .world_component_field
            .ok_or(CallError::Missing("world_component_field"))?;
        let raw = unsafe { entry(world, component, field, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// An entity's parent, or the null entity.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn world_parent(
        &self,
        world: ffi::CyWorld,
        entity: ffi::CyEntity,
    ) -> Result<ffi::CyEntity, CallError> {
        let entry = self
            .table()
            .world_parent
            .ok_or(CallError::Missing("world_parent"))?;
        Ok(unsafe { entry(world, entity) })
    }

    /// Reparent an entity.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn world_set_parent(
        &self,
        world: ffi::CyWorld,
        child: ffi::CyEntity,
        parent: ffi::CyEntity,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .world_set_parent
            .ok_or(CallError::Missing("world_set_parent"))?;
        let raw = unsafe { entry(world, child, parent) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// How many children an entity has.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn world_child_count(
        &self,
        world: ffi::CyWorld,
        entity: ffi::CyEntity,
    ) -> Result<u32, CallError> {
        let entry = self
            .table()
            .world_child_count
            .ok_or(CallError::Missing("world_child_count"))?;
        Ok(unsafe { entry(world, entity) })
    }

    /// One child, in the ECS's order rather than the authored order.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn world_child(
        &self,
        world: ffi::CyWorld,
        entity: ffi::CyEntity,
        index: u32,
    ) -> Result<ffi::CyEntity, CallError> {
        let entry = self
            .table()
            .world_child
            .ok_or(CallError::Missing("world_child"))?;
        Ok(unsafe { entry(world, entity, index) })
    }

    /// Enumerate a component's chunks; a null buffer asks for the count.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn world_chunks(
        &self,
        world: ffi::CyWorld,
        component: ffi::CyComponentTypeId,
        into: *mut ffi::CyChunk,
        capacity: u32,
        count: *mut u32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .world_chunks
            .ok_or(CallError::Missing("world_chunks"))?;
        let raw = unsafe { entry(world, component, into, capacity, count) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }
}
