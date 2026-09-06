//! The fixture's world: three component types, a parent link, and an epoch.
//!
//! Small enough to hold in your head, which is the requirement. Every behaviour here is one the C
//! ABI states: entity identifiers pack an index and a generation, a structural change bumps the
//! epoch, `world_child`'s order is the storage's and not the authored one, and a tag component has
//! no column.

use std::ffi::c_char;

use cy_editor_sdk::generated::enums::{Status, VarType};
use cy_editor_sdk::generated::ffi;

use crate::static_name;

/// The transform component: `position` and `scale`, three floats each.
pub const TRANSFORM: u32 = 0;
/// The health component: one float.
pub const HEALTH: u32 = 1;
/// A tag: no data, no fields, no column.
pub const SELECTED: u32 = 2;

/// One component type as this fixture describes it.
struct ComponentType {
    name: *const c_char,
    size: u32,
    alignment: u32,
    fields: Vec<Field>,
}

struct Field {
    name: *const c_char,
    var_type: VarType,
    offset: u32,
    size: u32,
}

/// One entity's storage. Flat and per-entity rather than columnar: this is a fixture, and an
/// archetype table here would be a second ECS to keep correct.
#[derive(Default)]
struct Entity {
    generation: u32,
    alive: bool,
    position: [f32; 3],
    scale: [f32; 3],
    health: f32,
    components: u32,
    parent: u64,
    children: Vec<u64>,
}

/// The fixture's world, which is also its engine: the two handles are the same pointer.
pub struct TestWorld {
    entities: Vec<Entity>,
    types: Vec<ComponentType>,
    epoch: u64,
}

impl Default for TestWorld {
    fn default() -> Self {
        Self::new()
    }
}

impl TestWorld {
    /// A world with the three component types registered and no entities.
    #[must_use]
    pub fn new() -> Self {
        Self {
            entities: Vec::new(),
            types: vec![
                ComponentType {
                    name: static_name("Transform"),
                    size: 24,
                    alignment: 4,
                    fields: vec![
                        Field {
                            name: static_name("position"),
                            var_type: VarType::Vec3,
                            offset: 0,
                            size: 12,
                        },
                        Field {
                            name: static_name("scale"),
                            var_type: VarType::Vec3,
                            offset: 12,
                            size: 12,
                        },
                    ],
                },
                ComponentType {
                    name: static_name("Health"),
                    size: 4,
                    alignment: 4,
                    fields: vec![Field {
                        name: static_name("value"),
                        var_type: VarType::F32,
                        offset: 0,
                        size: 4,
                    }],
                },
                // A tag: `cy_abi.h` says a component with no column reports `size` zero, and its
                // field count is zero. Present because an inspector that assumed every component
                // had fields would be wrong about every tag in a real scene.
                ComponentType {
                    name: static_name("Selected"),
                    size: 0,
                    alignment: 1,
                    fields: Vec::new(),
                },
            ],
            epoch: 0,
        }
    }

    /// The structural epoch.
    #[must_use]
    pub const fn epoch(&self) -> u64 {
        self.epoch
    }

    /// Create an entity, returning its packed identifier.
    pub fn create_entity(&mut self) -> u64 {
        self.epoch += 1;
        if let Some(index) = self.entities.iter().position(|entity| !entity.alive) {
            let entity = &mut self.entities[index];
            entity.generation += 1;
            entity.alive = true;
            entity.components = 0;
            entity.parent = 0;
            entity.children.clear();
            return pack(index, entity.generation);
        }
        self.entities.push(Entity {
            generation: 1,
            alive: true,
            ..Entity::default()
        });
        pack(self.entities.len() - 1, 1)
    }

    /// Whether an identifier names a live entity.
    #[must_use]
    pub fn is_alive(&self, entity: u64) -> bool {
        self.resolve(entity).is_some()
    }

    /// Destroy an entity, detaching it from its parent and orphaning its children.
    pub fn destroy_entity(&mut self, entity: u64) -> Result<(), Status> {
        let index = self.resolve(entity).ok_or(Status::NotFound)?;
        let parent = self.entities[index].parent;
        let children = std::mem::take(&mut self.entities[index].children);
        self.detach(entity, parent);
        for child in children {
            if let Some(child_index) = self.resolve(child) {
                self.entities[child_index].parent = 0;
            }
        }
        self.entities[index].alive = false;
        self.epoch += 1;
        Ok(())
    }

    /// A component type's identifier, by name.
    #[must_use]
    pub fn find_component(&self, name: &str) -> Option<u32> {
        self.types
            .iter()
            .position(|kind| kind.name_str() == name)
            .and_then(|index| u32::try_from(index).ok())
    }

    /// How many component types are registered.
    #[must_use]
    pub fn component_count(&self) -> u32 {
        u32::try_from(self.types.len()).expect("a fixture has three component types")
    }

    /// Describe a component type.
    pub fn component_info(&self, component: u32) -> Result<ffi::CyComponentInfo, Status> {
        let kind = self
            .types
            .get(component as usize)
            .ok_or(Status::OutOfRange)?;
        Ok(ffi::CyComponentInfo {
            struct_size: u32::try_from(size_of::<ffi::CyComponentInfo>())
                .expect("CyComponentInfo is 24 bytes"),
            size: kind.size,
            alignment: kind.alignment,
            field_count: u32::try_from(kind.fields.len()).expect("a fixture type has two fields"),
            name: kind.name,
        })
    }

    /// Describe one field of a component type.
    pub fn component_field(&self, component: u32, field: u32) -> Result<ffi::CyFieldDesc, Status> {
        let kind = self
            .types
            .get(component as usize)
            .ok_or(Status::OutOfRange)?;
        let field = kind.fields.get(field as usize).ok_or(Status::OutOfRange)?;
        Ok(ffi::CyFieldDesc {
            struct_size: u32::try_from(size_of::<ffi::CyFieldDesc>())
                .expect("CyFieldDesc is 24 bytes"),
            r#type: field.var_type.as_raw(),
            offset: field.offset,
            size: field.size,
            name: field.name,
        })
    }

    /// Add a component. Structural.
    pub fn add_component(&mut self, entity: u64, component: u32) -> Result<(), Status> {
        if component as usize >= self.types.len() {
            return Err(Status::OutOfRange);
        }
        let index = self.resolve(entity).ok_or(Status::NotFound)?;
        self.entities[index].components |= 1 << component;
        self.epoch += 1;
        Ok(())
    }

    /// Remove a component. Structural.
    pub fn remove_component(&mut self, entity: u64, component: u32) -> Result<(), Status> {
        if component as usize >= self.types.len() {
            return Err(Status::OutOfRange);
        }
        let index = self.resolve(entity).ok_or(Status::NotFound)?;
        self.entities[index].components &= !(1 << component);
        self.epoch += 1;
        Ok(())
    }

    /// Whether an entity carries a component.
    #[must_use]
    pub fn has_component(&self, entity: u64, component: u32) -> bool {
        self.resolve(entity)
            .is_some_and(|index| self.entities[index].components & (1 << component) != 0)
    }

    /// Read a field as a `CyVar`.
    pub fn get_field(&self, entity: u64, component: u32, field: u32) -> Result<ffi::CyVar, Status> {
        let index = self.resolve(entity).ok_or(Status::NotFound)?;
        if self.entities[index].components & (1 << component) == 0 {
            return Err(Status::NotFound);
        }
        let entity = &self.entities[index];
        match (component, field) {
            (TRANSFORM, 0) => Ok(vec3_var(entity.position)),
            (TRANSFORM, 1) => Ok(vec3_var(entity.scale)),
            (HEALTH, 0) => Ok(ffi::CyVar {
                r#type: VarType::F32.as_raw(),
                flags: 0,
                length: 0,
                payload: ffi::CyVarPayload {
                    as_f32: entity.health,
                },
            }),
            _ => Err(Status::OutOfRange),
        }
    }

    /// Write a field from a `CyVar`, refusing a type the field does not hold.
    pub fn set_field(
        &mut self,
        entity: u64,
        component: u32,
        field: u32,
        value: &ffi::CyVar,
    ) -> Result<(), Status> {
        let index = self.resolve(entity).ok_or(Status::NotFound)?;
        if self.entities[index].components & (1 << component) == 0 {
            return Err(Status::NotFound);
        }
        let kind = VarType::from_raw(value.r#type).ok_or(Status::InvalidArgument)?;
        let entity = &mut self.entities[index];
        match (component, field, kind) {
            (TRANSFORM, 0, VarType::Vec3) => entity.position = read_vec3(value),
            (TRANSFORM, 1, VarType::Vec3) => entity.scale = read_vec3(value),
            // SAFETY: the tag says `as_f32` is the live member.
            (HEALTH, 0, VarType::F32) => entity.health = unsafe { value.payload.as_f32 },
            (TRANSFORM | HEALTH, 0 | 1, _) => return Err(Status::InvalidArgument),
            _ => return Err(Status::OutOfRange),
        }
        Ok(())
    }

    /// An entity's parent, or the null entity.
    #[must_use]
    pub fn parent(&self, entity: u64) -> u64 {
        self.resolve(entity)
            .map_or(0, |index| self.entities[index].parent)
    }

    /// Reparent. Structural.
    pub fn set_parent(&mut self, child: u64, parent: u64) -> Result<(), Status> {
        let child_index = self.resolve(child).ok_or(Status::NotFound)?;
        if parent != 0 && self.resolve(parent).is_none() {
            return Err(Status::NotFound);
        }
        let previous = self.entities[child_index].parent;
        self.detach(child, previous);
        self.entities[child_index].parent = parent;
        if parent != 0 {
            let parent_index = self.resolve(parent).ok_or(Status::NotFound)?;
            self.entities[parent_index].children.push(child);
        }
        self.epoch += 1;
        Ok(())
    }

    /// How many children an entity has. Zero for one that is not alive, which is the ABI's answer.
    #[must_use]
    pub fn child_count(&self, entity: u64) -> u32 {
        self.resolve(entity).map_or(0, |index| {
            u32::try_from(self.entities[index].children.len()).unwrap_or(u32::MAX)
        })
    }

    /// The child at an index, in storage order. Not the authored order; see the ABI.
    #[must_use]
    pub fn child(&self, entity: u64, index: u32) -> u64 {
        self.resolve(entity)
            .and_then(|owner| self.entities[owner].children.get(index as usize).copied())
            .unwrap_or(0)
    }

    /// Remove `child` from `parent`'s children, by swapping the last into the gap.
    ///
    /// The swap is the ECS's own behaviour, reproduced here on purpose: it is what makes
    /// `world_child`'s order *not* the authored order, and a fixture that kept insertion order
    /// would let a test pass that the engine would fail.
    fn detach(&mut self, child: u64, parent: u64) {
        if parent == 0 {
            return;
        }
        let Some(parent_index) = self.resolve(parent) else {
            return;
        };
        let children = &mut self.entities[parent_index].children;
        if let Some(position) = children.iter().position(|held| *held == child) {
            children.swap_remove(position);
        }
    }

    /// The slot a live entity identifier names.
    fn resolve(&self, entity: u64) -> Option<usize> {
        let (index, generation) = unpack(entity)?;
        let slot = self.entities.get(index)?;
        (slot.alive && slot.generation == generation).then_some(index)
    }
}

impl ComponentType {
    fn name_str(&self) -> &str {
        // SAFETY: `name` is a leaked `CString` this crate built, so it is NUL-terminated, valid
        // UTF-8, and lives for the process.
        unsafe { std::ffi::CStr::from_ptr(self.name) }
            .to_str()
            .unwrap_or_default()
    }
}

/// Pack a slot and a generation into an entity identifier, as the engine's `CyEntity` does.
///
/// The layout is this fixture's rather than the engine's — `cy_abi.h` describes the engine's
/// packing in prose that no generator can recover, and the editor is specified never to decode it.
/// What matters is that the identifier is *opaque and generation-checked*, which this is.
const fn pack(index: usize, generation: u32) -> u64 {
    ((index as u64 + 1) & 0xffff_ffff) | ((generation as u64) << 32)
}

const fn unpack(entity: u64) -> Option<(usize, u32)> {
    let index = (entity & 0xffff_ffff) as usize;
    if index == 0 {
        return None;
    }
    Some((index - 1, (entity >> 32) as u32))
}

fn vec3_var(value: [f32; 3]) -> ffi::CyVar {
    ffi::CyVar {
        r#type: VarType::Vec3.as_raw(),
        flags: 0,
        length: 0,
        payload: ffi::CyVarPayload {
            as_f32x4: [value[0], value[1], value[2], 0.0],
        },
    }
}

fn read_vec3(var: &ffi::CyVar) -> [f32; 3] {
    // SAFETY: the caller checked the tag says Vec3, so `as_f32x4` is the live member.
    let lanes = unsafe { var.payload.as_f32x4 };
    [lanes[0], lanes[1], lanes[2]]
}
