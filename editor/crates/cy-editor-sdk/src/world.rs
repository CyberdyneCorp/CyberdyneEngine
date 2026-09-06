//! A world, safely. Task 2.3 and the "no pointers as identity" rule of task 2.2.
//!
//! Everything a caller of this type touches is a value: entities are
//! [`cy_editor_core::ids::RuntimeEntity`], component types are [`cy_editor_core::ids::TypeId`],
//! field values are [`cy_editor_core::Value`], names are owned `String`s, and every failure is a
//! [`Problem`] with a reason. No raw pointer, no borrowed engine memory, and no sentinel: the ABI's
//! `CY_ENTITY_NULL` and `CY_COMPONENT_TYPE_INVALID` are converted at this boundary into `Option`
//! and into typed failures, because "failure SHALL NOT be signalled by sentinel values reaching
//! editor logic".
//!
//! --- THE LIFETIME IS THE SAFETY ARGUMENT ------------------------------------------------------------
//!
//! `World<'engine>` borrows the thing that owns the world — an [`crate::host::EmbeddedEngine`], or a
//! session — so it cannot outlive it. That is the whole of the memory-safety story for this type:
//! the ABI's handles are valid while the engine is, and the borrow checker is what enforces "while".
//!
//! A `World` is deliberately neither `Send` nor `Sync`, by the presence of a `PhantomData<*const
//! ()>`. The C ABI's world is not thread-safe and `src/ecs/include/cy/ecs/world.h` refuses
//! structural change during iteration; a world handle that could be moved to another thread would
//! make that unenforceable from Rust, and the failure would be a data race rather than a refusal.

use std::marker::PhantomData;

use cy_editor_core::Value;
use cy_editor_core::ids::{RuntimeEntity, TypeId};
use cy_editor_core::problem::{Problem, Result};

use crate::generated::enums::VarType;
use crate::generated::ffi;
use crate::generated::interface::Interface;
use crate::host::call_problem;
use crate::schema::{ComponentSchema, FieldSchema, kind_of};
use crate::value::{OwnedVar, read_var, with_var};

/// The ABI's "no such component type", from `cy_abi.h`.
const COMPONENT_TYPE_INVALID: u32 = u32::MAX;

/// A world reached through the C ABI.
pub struct World<'engine> {
    interface: Interface,
    handle: ffi::CyWorld,
    /// Borrows the engine, and un-implements `Send`/`Sync`. See the module note.
    marker: PhantomData<(&'engine (), *const ())>,
}

impl World<'_> {
    /// Wrap a world handle.
    ///
    /// # Safety
    ///
    /// `world` must be a live `CyWorld` produced by `interface`, and must remain live for `'engine`.
    /// [`crate::host::EmbeddedEngine::world`] is the only caller in this crate.
    #[must_use]
    pub const unsafe fn from_raw(interface: Interface, world: ffi::CyWorld) -> Self {
        Self {
            interface,
            handle: world,
            marker: PhantomData,
        }
    }

    /// The world's structural epoch: every structural change increments it.
    ///
    /// The editor reads this to know when a cached view of the hierarchy is stale, which is the
    /// same question `CyBorrow` asks and the same answer.
    pub fn epoch(&self) -> Result<u64> {
        // SAFETY, once for this type: `self.handle` is live for `'engine` by the constructor's
        // contract, and every entry below is called with arguments of the types the ABI declares.
        // Each call is a plain foreign call with no aliasing of Rust memory except the explicit
        // out-parameters, which are local `MaybeUninit`-free values written before they are read.
        unsafe { self.interface.world_epoch(self.handle) }
            .map_err(|error| call_problem("read the world's epoch", error))
    }

    /// Create an entity. Structural: it bumps the epoch.
    pub fn create_entity(&self) -> Result<RuntimeEntity> {
        let raw = unsafe { self.interface.world_create_entity(self.handle) }
            .map_err(|error| call_problem("create an entity", error))?;
        let entity = RuntimeEntity::from_raw(raw);
        if entity.is_none() {
            return Err(self.last_error("create an entity"));
        }
        Ok(entity)
    }

    /// Destroy an entity. Structural.
    pub fn destroy_entity(&self, entity: RuntimeEntity) -> Result<()> {
        unsafe {
            self.interface
                .world_destroy_entity(self.handle, entity.as_u64())
        }
        .map_err(|error| call_problem("destroy an entity", error))
    }

    /// Whether an entity identifier still names a live entity.
    pub fn entity_alive(&self, entity: RuntimeEntity) -> Result<bool> {
        unsafe {
            self.interface
                .world_entity_alive(self.handle, entity.as_u64())
        }
        .map_err(|error| call_problem("ask whether an entity is alive", error))
    }

    /// The entity's parent, or `None` when it is a root or is not alive.
    pub fn parent(&self, entity: RuntimeEntity) -> Result<Option<RuntimeEntity>> {
        let raw = unsafe { self.interface.world_parent(self.handle, entity.as_u64()) }
            .map_err(|error| call_problem("read an entity's parent", error))?;
        let parent = RuntimeEntity::from_raw(raw);
        Ok(if parent.is_none() { None } else { Some(parent) })
    }

    /// Reparent an entity. `None` makes it a root. Structural.
    pub fn set_parent(&self, child: RuntimeEntity, parent: Option<RuntimeEntity>) -> Result<()> {
        let parent = parent.unwrap_or(RuntimeEntity::NONE);
        unsafe {
            self.interface
                .world_set_parent(self.handle, child.as_u64(), parent.as_u64())
        }
        .map_err(|error| call_problem("reparent an entity", error))
    }

    /// An entity's children, **in the ECS's order**.
    ///
    /// `cy_abi.h` states in capitals that this is not the authored order — `ecs-core` leaves the
    /// children buffer unordered and removing a child swaps the last into the gap. A hierarchy panel
    /// that showed these in this order would reorder itself when an unrelated sibling was deleted.
    /// The authored order is `cy::scene::ChildOrder`, which this ABI cannot yet read; until it can,
    /// a caller that needs it must sort by something it knows.
    pub fn children(&self, entity: RuntimeEntity) -> Result<Vec<RuntimeEntity>> {
        let count = unsafe {
            self.interface
                .world_child_count(self.handle, entity.as_u64())
        }
        .map_err(|error| call_problem("count an entity's children", error))?;
        let mut children = Vec::with_capacity(count as usize);
        for index in 0..count {
            let raw = unsafe {
                self.interface
                    .world_child(self.handle, entity.as_u64(), index)
            }
            .map_err(|error| call_problem("read a child", error))?;
            let child = RuntimeEntity::from_raw(raw);
            if !child.is_none() {
                children.push(child);
            }
        }
        Ok(children)
    }

    /// Look a component type up by name.
    pub fn find_component(&self, name: &str) -> Result<TypeId> {
        let c_name = std::ffi::CString::new(name).map_err(|_| {
            Problem::new(
                format!("find the component {name:?}"),
                "the name contains a NUL byte",
            )
        })?;
        let raw = unsafe {
            self.interface
                .world_find_component(self.handle, c_name.as_ptr())
        }
        .map_err(|error| call_problem("find a component type", error))?;
        if raw == COMPONENT_TYPE_INVALID {
            return Err(Problem::new(
                format!("find the component {name:?}"),
                "this world has no component type of that name",
            )
            .with_remedy("list the world's components to see what it does have"));
        }
        Ok(TypeId::from_raw(u64::from(raw)))
    }

    /// Whether an entity carries a component.
    pub fn has_component(&self, entity: RuntimeEntity, component: TypeId) -> Result<bool> {
        unsafe {
            self.interface.world_has_component(
                self.handle,
                entity.as_u64(),
                component_id(component)?,
            )
        }
        .map_err(|error| call_problem("ask whether an entity has a component", error))
    }

    /// Add a component, zero-initialised. Structural.
    pub fn add_component(&self, entity: RuntimeEntity, component: TypeId) -> Result<()> {
        unsafe {
            self.interface.world_add_component(
                self.handle,
                entity.as_u64(),
                component_id(component)?,
                std::ptr::null(),
            )
        }
        .map_err(|error| call_problem("add a component", error))
    }

    /// Remove a component. Structural.
    pub fn remove_component(&self, entity: RuntimeEntity, component: TypeId) -> Result<()> {
        unsafe {
            self.interface.world_remove_component(
                self.handle,
                entity.as_u64(),
                component_id(component)?,
            )
        }
        .map_err(|error| call_problem("remove a component", error))
    }

    /// How many component types this world knows, including the engine's own.
    pub fn component_count(&self) -> Result<u32> {
        unsafe { self.interface.world_component_count(self.handle) }
            .map_err(|error| call_problem("count the world's component types", error))
    }

    /// Describe one component type, with its describable fields.
    pub fn component_schema(&self, component: TypeId) -> Result<ComponentSchema> {
        let id = component_id(component)?;
        let mut info = ffi::CyComponentInfo {
            // The ABI's forward-compatibility protocol: the caller states the size it knows and the
            // engine writes only that prefix. Passing zero would also work — the header says zero
            // means "the size I know" — but stating it is what makes a future engine writing a
            // longer struct a non-event rather than an overrun.
            struct_size: u32::try_from(size_of::<ffi::CyComponentInfo>())
                .expect("CyComponentInfo is 24 bytes"),
            size: 0,
            alignment: 0,
            field_count: 0,
            name: std::ptr::null(),
        };
        unsafe {
            self.interface
                .world_component_info(self.handle, id, &raw mut info)
        }
        .map_err(|error| call_problem("describe a component type", error))?;

        let mut fields = Vec::with_capacity(info.field_count as usize);
        for index in 0..info.field_count {
            fields.push(self.component_field(component, index)?);
        }
        Ok(ComponentSchema {
            id: component,
            name: owned_c_string(info.name),
            size: info.size,
            alignment: info.alignment,
            fields,
        })
    }

    /// Every component type the world knows, in registry order.
    pub fn component_schemas(&self) -> Result<Vec<ComponentSchema>> {
        let count = self.component_count()?;
        (0..count)
            .map(|id| self.component_schema(TypeId::from_raw(u64::from(id))))
            .collect()
    }

    /// Describe one field of one component type.
    pub fn component_field(&self, component: TypeId, index: u32) -> Result<FieldSchema> {
        let mut descriptor = ffi::CyFieldDesc {
            struct_size: u32::try_from(size_of::<ffi::CyFieldDesc>())
                .expect("CyFieldDesc is 24 bytes"),
            r#type: 0,
            offset: 0,
            size: 0,
            name: std::ptr::null(),
        };
        unsafe {
            self.interface.world_component_field(
                self.handle,
                component_id(component)?,
                index,
                &raw mut descriptor,
            )
        }
        .map_err(|error| call_problem("describe a component field", error))?;

        let var_type = VarType::from_raw(descriptor.r#type).ok_or_else(|| {
            Problem::new(
                "describe a component field",
                format!(
                    "the engine reported CyVarType {}, which this editor has no name for",
                    descriptor.r#type
                ),
            )
            .with_remedy("the runtime is newer than this editor; rebuild them from one revision")
        })?;

        Ok(FieldSchema {
            id: FieldSchema::id_for_index(component, index),
            name: owned_c_string(descriptor.name),
            kind: kind_of(var_type),
            index,
            offset: descriptor.offset,
            size: descriptor.size,
        })
    }

    /// Read one field as a [`Value`].
    ///
    /// The generic path, through `CyVar`. Correct for anything reflective, which is what an
    /// inspector is; `cy_abi.h` says it is not for a hot loop, and the editor does not have one.
    pub fn get_field(&self, entity: RuntimeEntity, component: TypeId, field: u32) -> Result<Value> {
        let mut var = ffi::CyVar {
            r#type: VarType::Nil.as_raw(),
            flags: 0,
            length: 0,
            payload: ffi::CyVarPayload { as_i64: 0 },
        };
        unsafe {
            self.interface.component_get_var(
                self.handle,
                entity.as_u64(),
                component_id(component)?,
                field,
                &raw mut var,
            )
        }
        .map_err(|error| call_problem("read a component field", error))?;

        // Taken into an owning wrapper immediately, so that the `?` below cannot leak a heap
        // payload. `var_live_count` is the counter that would otherwise climb through a session.
        let owned = OwnedVar::new(self.interface, var);
        read_var(owned.get())
    }

    /// Write one field from a [`Value`].
    pub fn set_field(
        &self,
        entity: RuntimeEntity,
        component: TypeId,
        field: u32,
        value: &Value,
    ) -> Result<()> {
        let id = component_id(component)?;
        with_var(value, |borrowed| {
            unsafe {
                self.interface.component_set_var(
                    self.handle,
                    entity.as_u64(),
                    id,
                    field,
                    borrowed.as_ptr(),
                )
            }
            .map_err(|error| call_problem("write a component field", error))
        })
    }

    /// The engine's last error on this thread, as a [`Problem`].
    ///
    /// Used where the ABI signals failure with a sentinel rather than a `CyResult` — creating an
    /// entity, looking up a component — so that the sentinel is converted into a reason here and
    /// never reaches editor logic.
    fn last_error(&self, what: &str) -> Problem {
        let Some(get) = self.interface.table().get_last_error else {
            return Problem::new(
                what.to_string(),
                "the engine reported a failure with no message",
            );
        };
        // SAFETY: `get_last_error` takes nothing and returns a pointer the ABI documents as "valid
        // until this thread's next failing call". It is copied out before anything else is called.
        let message = unsafe { get() };
        let because = if message.is_null() {
            "the engine reported a failure with no message".to_string()
        } else {
            owned_c_string(message)
        };
        Problem::new(what.to_string(), because)
    }
}

/// Narrow a [`TypeId`] to the ABI's 32-bit component identifier.
///
/// The editor's `TypeId` is 64 bits because a *document's* types are the editor's own and there are
/// more of them than a world's registry holds. A runtime component identifier is the world's
/// registry index, which the ABI declares as `uint32_t`; a value that does not fit did not come
/// from a world.
fn component_id(component: TypeId) -> Result<u32> {
    u32::try_from(component.as_u64()).map_err(|_| {
        Problem::new(
            "address a component type",
            format!(
                "{} is not a runtime component identifier",
                component.as_u64()
            ),
        )
        .with_remedy("look the component up by name in the world first")
    })
}

/// Copy a NUL-terminated engine string into an owned `String`.
///
/// Lossy on invalid UTF-8 rather than failing: a component's *name* being mis-encoded is a defect
/// worth seeing in an inspector, and refusing to describe the whole world because one name is
/// malformed would hide it behind an error about something else.
fn owned_c_string(pointer: *const std::ffi::c_char) -> String {
    if pointer.is_null() {
        return String::new();
    }
    // SAFETY: the ABI's contract for every name it returns is that it is NUL-terminated and valid
    // for at least the life of the world. It is copied here and not retained.
    unsafe { std::ffi::CStr::from_ptr(pointer) }
        .to_string_lossy()
        .into_owned()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_component_identifier_that_is_not_a_worlds_is_refused_with_a_remedy() {
        let problem = component_id(TypeId::from_raw(u64::from(u32::MAX) + 1)).unwrap_err();
        assert!(problem.remedy.is_some());
    }

    #[test]
    fn a_null_engine_string_is_empty_rather_than_a_fault() {
        assert_eq!(owned_c_string(std::ptr::null()), "");
    }
}
