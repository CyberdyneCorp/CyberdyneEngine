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

    /// Open an isolated editor-service session.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn service_open(
        &self,
        engine: ffi::CyEngine,
        into: *mut ffi::CyServiceSession,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .service_open
            .ok_or(CallError::Missing("service_open"))?;
        let raw = unsafe { entry(engine, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Close an editor-service session.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn service_close(
        &self,
        engine: ffi::CyEngine,
        session: ffi::CyServiceSession,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .service_close
            .ok_or(CallError::Missing("service_close"))?;
        unsafe { entry(engine, session) };
        Ok(())
    }

    /// Submit one versioned asynchronous editor-service request.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn service_submit(
        &self,
        engine: ffi::CyEngine,
        session: ffi::CyServiceSession,
        request: *const ffi::CyServiceRequest,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .service_submit
            .ok_or(CallError::Missing("service_submit"))?;
        let raw = unsafe { entry(engine, session, request) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Cooperatively cancel an editor-service request.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn service_cancel(
        &self,
        engine: ffi::CyEngine,
        session: ffi::CyServiceSession,
        request_id: u64,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .service_cancel
            .ok_or(CallError::Missing("service_cancel"))?;
        let raw = unsafe { entry(engine, session, request_id) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Poll one editor-service event without blocking.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn service_poll(
        &self,
        engine: ffi::CyEngine,
        session: ffi::CyServiceSession,
        event: *mut ffi::CyServiceEvent,
        has_event: *mut bool,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .service_poll
            .ok_or(CallError::Missing("service_poll"))?;
        let raw = unsafe { entry(engine, session, event, has_event) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Read the engine clock and the current update phase.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn time_get(
        &self,
        engine: ffi::CyEngine,
        into: *mut ffi::CyTime,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .time_get
            .ok_or(CallError::Missing("time_get"))?;
        let raw = unsafe { entry(engine, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Resolve an input action by its declared name.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn input_find_action(
        &self,
        engine: ffi::CyEngine,
        name: *const ::std::ffi::c_char,
        into: *mut ffi::CyInputAction,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .input_find_action
            .ok_or(CallError::Missing("input_find_action"))?;
        let raw = unsafe { entry(engine, name, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Read one action's resolved state for an input user.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn input_action_state(
        &self,
        engine: ffi::CyEngine,
        user: u32,
        action: ffi::CyInputAction,
        into: *mut ffi::CyInputActionState,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .input_action_state
            .ok_or(CallError::Missing("input_action_state"))?;
        let raw = unsafe { entry(engine, user, action, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Read one action's resolved state by the action's name.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn input_action_state_by_name(
        &self,
        engine: ffi::CyEngine,
        user: u32,
        name: *const ::std::ffi::c_char,
        into: *mut ffi::CyInputActionState,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .input_action_state_by_name
            .ok_or(CallError::Missing("input_action_state_by_name"))?;
        let raw = unsafe { entry(engine, user, name, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Read an input user's pointer in window pixels (frame update only).
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn input_pointer(
        &self,
        engine: ffi::CyEngine,
        user: u32,
        into: *mut ffi::CyInputPointer,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .input_pointer
            .ok_or(CallError::Missing("input_pointer"))?;
        let raw = unsafe { entry(engine, user, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Read the modifier keys an input user holds (frame update only).
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn input_modifiers(
        &self,
        engine: ffi::CyEngine,
        user: u32,
        into: *mut u32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .input_modifiers
            .ok_or(CallError::Missing("input_modifiers"))?;
        let raw = unsafe { entry(engine, user, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Resolve a mapping context by its registered name.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn input_find_context(
        &self,
        engine: ffi::CyEngine,
        name: *const ::std::ffi::c_char,
        into: *mut ffi::CyInputContext,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .input_find_context
            .ok_or(CallError::Missing("input_find_context"))?;
        let raw = unsafe { entry(engine, name, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Push a mapping context onto an input user's stack.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn input_push_context(
        &self,
        engine: ffi::CyEngine,
        user: u32,
        context: ffi::CyInputContext,
        priority: i32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .input_push_context
            .ok_or(CallError::Missing("input_push_context"))?;
        let raw = unsafe { entry(engine, user, context, priority) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Remove a mapping context from an input user's stack.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn input_pop_context(
        &self,
        engine: ffi::CyEngine,
        user: u32,
        context: ffi::CyInputContext,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .input_pop_context
            .ok_or(CallError::Missing("input_pop_context"))?;
        let raw = unsafe { entry(engine, user, context) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// The camera of the primary view.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn camera_active(
        &self,
        engine: ffi::CyEngine,
        into: *mut ffi::CyCamera,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .camera_active
            .ok_or(CallError::Missing("camera_active"))?;
        let raw = unsafe { entry(engine, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// A camera's evaluated pose, projection and viewport.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn camera_view(
        &self,
        engine: ffi::CyEngine,
        camera: ffi::CyCamera,
        into: *mut ffi::CyCameraView,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .camera_view
            .ok_or(CallError::Missing("camera_view"))?;
        let raw = unsafe { entry(engine, camera, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// The world ray under a window-pixel point.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn camera_screen_to_ray(
        &self,
        engine: ffi::CyEngine,
        camera: ffi::CyCamera,
        screen: *const f32,
        into: *mut ffi::CyRay,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .camera_screen_to_ray
            .ok_or(CallError::Missing("camera_screen_to_ray"))?;
        let raw = unsafe { entry(engine, camera, screen, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Project world points to window pixels.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn camera_world_to_screen(
        &self,
        engine: ffi::CyEngine,
        camera: ffi::CyCamera,
        points: *const f32,
        count: u32,
        into: *mut ffi::CyScreenPoint,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .camera_world_to_screen
            .ok_or(CallError::Missing("camera_world_to_screen"))?;
        let raw = unsafe { entry(engine, camera, points, count, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Point a camera's rig at a focus.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn camera_set_target(
        &self,
        engine: ffi::CyEngine,
        camera: ffi::CyCamera,
        target: *const ffi::CyCameraTarget,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .camera_set_target
            .ok_or(CallError::Missing("camera_set_target"))?;
        let raw = unsafe { entry(engine, camera, target) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Override a camera's rig with an explicit pose.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn camera_set_pose(
        &self,
        engine: ffi::CyEngine,
        camera: ffi::CyCamera,
        pose: *const ffi::CyPose,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .camera_set_pose
            .ok_or(CallError::Missing("camera_set_pose"))?;
        let raw = unsafe { entry(engine, camera, pose) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Hand a camera back to its rig.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn camera_clear_pose(
        &self,
        engine: ffi::CyEngine,
        camera: ffi::CyCamera,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .camera_clear_pose
            .ok_or(CallError::Missing("camera_clear_pose"))?;
        let raw = unsafe { entry(engine, camera) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// The nearest physics hit along a ray.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn physics_raycast(
        &self,
        engine: ffi::CyEngine,
        ray: *const ffi::CyRay,
        filter: *const ffi::CyQueryFilter,
        into: *mut ffi::CyPhysicsHit,
        has_hit: *mut bool,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .physics_raycast
            .ok_or(CallError::Missing("physics_raycast"))?;
        let raw = unsafe { entry(engine, ray, filter, into, has_hit) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Every physics hit along a ray, nearest first.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn physics_raycast_all(
        &self,
        engine: ffi::CyEngine,
        ray: *const ffi::CyRay,
        filter: *const ffi::CyQueryFilter,
        into: *mut ffi::CyPhysicsHit,
        capacity: u32,
        count: *mut u32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .physics_raycast_all
            .ok_or(CallError::Missing("physics_raycast_all"))?;
        let raw = unsafe { entry(engine, ray, filter, into, capacity, count) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Sweep a shape and report its first hit.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    #[allow(clippy::too_many_arguments, reason = "mirrors the C ABI entry")]
    pub unsafe fn physics_shape_cast(
        &self,
        engine: ffi::CyEngine,
        shape: *const ffi::CyShape,
        start: *const ffi::CyPose,
        direction: *const f32,
        max_distance: f32,
        filter: *const ffi::CyQueryFilter,
        into: *mut ffi::CyPhysicsHit,
        has_hit: *mut bool,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .physics_shape_cast
            .ok_or(CallError::Missing("physics_shape_cast"))?;
        let raw = unsafe {
            entry(
                engine,
                shape,
                start,
                direction,
                max_distance,
                filter,
                into,
                has_hit,
            )
        };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Every entity whose body overlaps a shape.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    #[allow(clippy::too_many_arguments, reason = "mirrors the C ABI entry")]
    pub unsafe fn physics_overlap(
        &self,
        engine: ffi::CyEngine,
        shape: *const ffi::CyShape,
        pose: *const ffi::CyPose,
        filter: *const ffi::CyQueryFilter,
        into: *mut ffi::CyEntity,
        capacity: u32,
        count: *mut u32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .physics_overlap
            .ok_or(CallError::Missing("physics_overlap"))?;
        let raw = unsafe { entry(engine, shape, pose, filter, into, capacity, count) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Find a navigation path now.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn nav_find_path(
        &self,
        engine: ffi::CyEngine,
        request: *const ffi::CyNavPathRequest,
        into: *mut f32,
        capacity: u32,
        result: *mut ffi::CyNavPathResult,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .nav_find_path
            .ok_or(CallError::Missing("nav_find_path"))?;
        let raw = unsafe { entry(engine, request, into, capacity, result) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Queue a deterministic asynchronous path search.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn nav_request_path(
        &self,
        engine: ffi::CyEngine,
        request: *const ffi::CyNavPathRequest,
        into: *mut ffi::CyNavQuery,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .nav_request_path
            .ok_or(CallError::Missing("nav_request_path"))?;
        let raw = unsafe { entry(engine, request, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Poll a queued path search.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn nav_poll_path(
        &self,
        engine: ffi::CyEngine,
        query: ffi::CyNavQuery,
        into: *mut f32,
        capacity: u32,
        result: *mut ffi::CyNavPathResult,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .nav_poll_path
            .ok_or(CallError::Missing("nav_poll_path"))?;
        let raw = unsafe { entry(engine, query, into, capacity, result) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Cancel a queued path search.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn nav_cancel_path(
        &self,
        engine: ffi::CyEngine,
        query: ffi::CyNavQuery,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .nav_cancel_path
            .ok_or(CallError::Missing("nav_cancel_path"))?;
        let raw = unsafe { entry(engine, query) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Make an entity a crowd agent, or update its parameters.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn nav_agent_configure(
        &self,
        engine: ffi::CyEngine,
        entity: ffi::CyEntity,
        params: *const ffi::CyNavAgentParams,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .nav_agent_configure
            .ok_or(CallError::Missing("nav_agent_configure"))?;
        let raw = unsafe { entry(engine, entity, params) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Send a crowd agent to a target point.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn nav_agent_move_to(
        &self,
        engine: ffi::CyEngine,
        entity: ffi::CyEntity,
        target: *const f32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .nav_agent_move_to
            .ok_or(CallError::Missing("nav_agent_move_to"))?;
        let raw = unsafe { entry(engine, entity, target) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Stop a crowd agent where it is.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn nav_agent_stop(
        &self,
        engine: ffi::CyEngine,
        entity: ffi::CyEntity,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .nav_agent_stop
            .ok_or(CallError::Missing("nav_agent_stop"))?;
        let raw = unsafe { entry(engine, entity) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// A crowd agent's status, motion and target.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn nav_agent_state(
        &self,
        engine: ffi::CyEngine,
        entity: ffi::CyEntity,
        into: *mut ffi::CyNavAgentState,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .nav_agent_state
            .ok_or(CallError::Missing("nav_agent_state"))?;
        let raw = unsafe { entry(engine, entity, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Resolve an audio cue by its authored name.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn audio_find_cue(
        &self,
        engine: ffi::CyEngine,
        name: *const ::std::ffi::c_char,
        into: *mut ffi::CyAudioCue,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .audio_find_cue
            .ok_or(CallError::Missing("audio_find_cue"))?;
        let raw = unsafe { entry(engine, name, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Start an audio voice.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn audio_play(
        &self,
        engine: ffi::CyEngine,
        play: *const ffi::CyAudioPlay,
        voice: *mut ffi::CyAudioVoice,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .audio_play
            .ok_or(CallError::Missing("audio_play"))?;
        let raw = unsafe { entry(engine, play, voice) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Stop an audio voice, optionally fading out.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn audio_stop(
        &self,
        engine: ffi::CyEngine,
        voice: ffi::CyAudioVoice,
        fade_out: f32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .audio_stop
            .ok_or(CallError::Missing("audio_stop"))?;
        let raw = unsafe { entry(engine, voice, fade_out) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Whether an audio voice is still audible.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn audio_voice_playing(
        &self,
        engine: ffi::CyEngine,
        voice: ffi::CyAudioVoice,
    ) -> Result<bool, CallError> {
        let entry = self
            .table()
            .audio_voice_playing
            .ok_or(CallError::Missing("audio_voice_playing"))?;
        Ok(unsafe { entry(engine, voice) })
    }

    /// Resolve a mix bus by its authored name.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn audio_find_bus(
        &self,
        engine: ffi::CyEngine,
        name: *const ::std::ffi::c_char,
        into: *mut ffi::CyAudioBus,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .audio_find_bus
            .ok_or(CallError::Missing("audio_find_bus"))?;
        let raw = unsafe { entry(engine, name, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Set a mix bus's linear gain.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn audio_set_bus_volume(
        &self,
        engine: ffi::CyEngine,
        bus: ffi::CyAudioBus,
        volume: f32,
        fade: f32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .audio_set_bus_volume
            .ok_or(CallError::Missing("audio_set_bus_volume"))?;
        let raw = unsafe { entry(engine, bus, volume, fade) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Resolve a prefab or scene asset by its content path.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn spawn_resolve(
        &self,
        engine: ffi::CyEngine,
        asset: *const ::std::ffi::c_char,
        into: *mut ffi::CyPrefab,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .spawn_resolve
            .ok_or(CallError::Missing("spawn_resolve"))?;
        let raw = unsafe { entry(engine, asset, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Instantiate a prefab and return its root entity.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn spawn_instantiate(
        &self,
        engine: ffi::CyEngine,
        prefab: ffi::CyPrefab,
        params: *const ffi::CySpawnParams,
        root: *mut ffi::CyEntity,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .spawn_instantiate
            .ok_or(CallError::Missing("spawn_instantiate"))?;
        let raw = unsafe { entry(engine, prefab, params, root) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Instantiate a prefab once per pose, in one batch.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn spawn_instantiate_many(
        &self,
        engine: ffi::CyEngine,
        prefab: ffi::CyPrefab,
        parent: ffi::CyEntity,
        poses: *const ffi::CyPose,
        count: u32,
        roots: *mut ffi::CyEntity,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .spawn_instantiate_many
            .ok_or(CallError::Missing("spawn_instantiate_many"))?;
        let raw = unsafe { entry(engine, prefab, parent, poses, count, roots) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Destroy an entity and its whole subtree.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn spawn_destroy(
        &self,
        engine: ffi::CyEngine,
        root: ffi::CyEntity,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .spawn_destroy
            .ok_or(CallError::Missing("spawn_destroy"))?;
        let raw = unsafe { entry(engine, root) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Set one exposed parameter on a playing scene effect entity.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn vfx_effect_parameter_set(
        &self,
        engine: ffi::CyEngine,
        entity: ffi::CyEntity,
        emitter: *const ::std::ffi::c_char,
        parameter: *const ::std::ffi::c_char,
        value: *const ffi::CyVar,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .vfx_effect_parameter_set
            .ok_or(CallError::Missing("vfx_effect_parameter_set"))?;
        let raw = unsafe { entry(engine, entity, emitter, parameter, value) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Read one exposed parameter from a playing scene effect entity.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn vfx_effect_parameter_get(
        &self,
        engine: ffi::CyEngine,
        entity: ffi::CyEntity,
        emitter: *const ::std::ffi::c_char,
        parameter: *const ::std::ffi::c_char,
        into: *mut ffi::CyVar,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .vfx_effect_parameter_get
            .ok_or(CallError::Missing("vfx_effect_parameter_get"))?;
        let raw = unsafe { entry(engine, entity, emitter, parameter, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Schedule a module system into a stage by its access list.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn register_system(
        &self,
        engine: ffi::CyEngine,
        desc: *const ffi::CySystemDesc,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .register_system
            .ok_or(CallError::Missing("register_system"))?;
        let raw = unsafe { entry(engine, desc) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Resolve a node path, relative to a node or absolute.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn node_find(
        &self,
        engine: ffi::CyEngine,
        from: ffi::CyEntity,
        path: *const ::std::ffi::c_char,
        into: *mut ffi::CyEntity,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .node_find
            .ok_or(CallError::Missing("node_find"))?;
        let raw = unsafe { entry(engine, from, path, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Add a force at a body's centre of mass for the next step.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn physics_apply_force(
        &self,
        engine: ffi::CyEngine,
        entity: ffi::CyEntity,
        force: *const f32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .physics_apply_force
            .ok_or(CallError::Missing("physics_apply_force"))?;
        let raw = unsafe { entry(engine, entity, force) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Apply an impulse to a body, at a point or its centre.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn physics_apply_impulse(
        &self,
        engine: ffi::CyEngine,
        entity: ffi::CyEntity,
        impulse: *const f32,
        point: *const f32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .physics_apply_impulse
            .ok_or(CallError::Missing("physics_apply_impulse"))?;
        let raw = unsafe { entry(engine, entity, impulse, point) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Add a torque to a body for the next step.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn physics_apply_torque(
        &self,
        engine: ffi::CyEngine,
        entity: ffi::CyEntity,
        torque: *const f32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .physics_apply_torque
            .ok_or(CallError::Missing("physics_apply_torque"))?;
        let raw = unsafe { entry(engine, entity, torque) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Replace a body's linear and or angular velocity.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn physics_set_velocity(
        &self,
        engine: ffi::CyEngine,
        entity: ffi::CyEntity,
        linear: *const f32,
        angular: *const f32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .physics_set_velocity
            .ok_or(CallError::Missing("physics_set_velocity"))?;
        let raw = unsafe { entry(engine, entity, linear, angular) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Read a body's linear and angular velocity.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn physics_get_velocity(
        &self,
        engine: ffi::CyEngine,
        entity: ffi::CyEntity,
        linear: *mut f32,
        angular: *mut f32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .physics_get_velocity
            .ok_or(CallError::Missing("physics_get_velocity"))?;
        let raw = unsafe { entry(engine, entity, linear, angular) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Give an entity a capsule character controller.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn character_create(
        &self,
        engine: ffi::CyEngine,
        entity: ffi::CyEntity,
        desc: *const ffi::CyCharacterDesc,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .character_create
            .ok_or(CallError::Missing("character_create"))?;
        let raw = unsafe { entry(engine, entity, desc) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Remove an entity's character controller and its body.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn character_destroy(
        &self,
        engine: ffi::CyEngine,
        entity: ffi::CyEntity,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .character_destroy
            .ok_or(CallError::Missing("character_destroy"))?;
        let raw = unsafe { entry(engine, entity) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Move a character by one fixed step.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn character_move(
        &self,
        engine: ffi::CyEngine,
        entity: ffi::CyEntity,
        input: *const ffi::CyCharacterInput,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .character_move
            .ok_or(CallError::Missing("character_move"))?;
        let raw = unsafe { entry(engine, entity, input) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Read what a character's last move produced.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn character_state(
        &self,
        engine: ffi::CyEngine,
        entity: ffi::CyEntity,
        into: *mut ffi::CyCharacterState,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .character_state
            .ok_or(CallError::Missing("character_state"))?;
        let raw = unsafe { entry(engine, entity, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// The screen's root interface element.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn ui_root(
        &self,
        engine: ffi::CyEngine,
        into: *mut ffi::CyUiElement,
    ) -> Result<(), CallError> {
        let entry = self.table().ui_root.ok_or(CallError::Missing("ui_root"))?;
        let raw = unsafe { entry(engine, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Create an interface element as the last child of a parent.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn ui_create(
        &self,
        engine: ffi::CyEngine,
        parent: ffi::CyUiElement,
        desc: *const ffi::CyUiElementDesc,
        into: *mut ffi::CyUiElement,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .ui_create
            .ok_or(CallError::Missing("ui_create"))?;
        let raw = unsafe { entry(engine, parent, desc, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Destroy an interface element and its subtree.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn ui_destroy(
        &self,
        engine: ffi::CyEngine,
        element: ffi::CyUiElement,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .ui_destroy
            .ok_or(CallError::Missing("ui_destroy"))?;
        let raw = unsafe { entry(engine, element) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Replace an interface element's layout input.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn ui_set_layout(
        &self,
        engine: ffi::CyEngine,
        element: ffi::CyUiElement,
        layout: *const ffi::CyUiLayout,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .ui_set_layout
            .ok_or(CallError::Missing("ui_set_layout"))?;
        let raw = unsafe { entry(engine, element, layout) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Replace what an interface element draws.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn ui_set_style(
        &self,
        engine: ffi::CyEngine,
        element: ffi::CyUiElement,
        style: *const ffi::CyUiStyle,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .ui_set_style
            .ok_or(CallError::Missing("ui_set_style"))?;
        let raw = unsafe { entry(engine, element, style) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Set a label's or a button's text.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn ui_set_text(
        &self,
        engine: ffi::CyEngine,
        element: ffi::CyUiElement,
        text: *const ::std::ffi::c_char,
        colour: u32,
        pixel_scale: u32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .ui_set_text
            .ok_or(CallError::Missing("ui_set_text"))?;
        let raw = unsafe { entry(engine, element, text, colour, pixel_scale) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Set an image element's atlas page and rectangle.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn ui_set_image(
        &self,
        engine: ffi::CyEngine,
        element: ffi::CyUiElement,
        page: u32,
        uv: *const f32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .ui_set_image
            .ok_or(CallError::Missing("ui_set_image"))?;
        let raw = unsafe { entry(engine, element, page, uv) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Set a progress bar's fraction.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn ui_set_progress(
        &self,
        engine: ffi::CyEngine,
        element: ffi::CyUiElement,
        value: f32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .ui_set_progress
            .ok_or(CallError::Missing("ui_set_progress"))?;
        let raw = unsafe { entry(engine, element, value) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Show, hide or collapse an interface element.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn ui_set_visibility(
        &self,
        engine: ffi::CyEngine,
        element: ffi::CyUiElement,
        visibility: u32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .ui_set_visibility
            .ok_or(CallError::Missing("ui_set_visibility"))?;
        let raw = unsafe { entry(engine, element, visibility) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Set an interface element's opacity.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn ui_set_opacity(
        &self,
        engine: ffi::CyEngine,
        element: ffi::CyUiElement,
        opacity: f32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .ui_set_opacity
            .ok_or(CallError::Missing("ui_set_opacity"))?;
        let raw = unsafe { entry(engine, element, opacity) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Read where the last layout put an interface element.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn ui_element_rect(
        &self,
        engine: ffi::CyEngine,
        element: ffi::CyUiElement,
        into: *mut f32,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .ui_element_rect
            .ok_or(CallError::Missing("ui_element_rect"))?;
        let raw = unsafe { entry(engine, element, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// The module's interface element under a window point.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn ui_hit_test(
        &self,
        engine: ffi::CyEngine,
        position: *const f32,
        into: *mut ffi::CyUiElement,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .ui_hit_test
            .ok_or(CallError::Missing("ui_hit_test"))?;
        let raw = unsafe { entry(engine, position, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// The module's interface element with keyboard focus.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn ui_focus(
        &self,
        engine: ffi::CyEngine,
        into: *mut ffi::CyUiElement,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .ui_focus
            .ok_or(CallError::Missing("ui_focus"))?;
        let raw = unsafe { entry(engine, into) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }

    /// Move keyboard focus to a button, or clear it.
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn ui_set_focus(
        &self,
        engine: ffi::CyEngine,
        element: ffi::CyUiElement,
    ) -> Result<(), CallError> {
        let entry = self
            .table()
            .ui_set_focus
            .ok_or(CallError::Missing("ui_set_focus"))?;
        let raw = unsafe { entry(engine, element) };
        match Status::from_raw(raw) {
            Some(Status::Ok) => Ok(()),
            Some(status) => Err(CallError::Failed(status)),
            None => Err(CallError::UnknownStatus(raw)),
        }
    }
}
