//! What the engine says its components are. Task 2.3, and the input the generated inspector needs.
//!
//! ABI 1.1 added the four entries an editor needs to describe a world it did not build:
//! `world_component_count`, `world_component_info`, `world_component_field` and the hierarchy pair.
//! Before them, the ABI could describe only components the *caller* had registered — which is the
//! wrong side of the boundary for an editor, and the reason M4 recorded "nothing is reflected, so
//! the editor's inspector has nothing to read" as a debt.
//!
//! Everything here is **owned**. `cy_abi.h` says a component's name is "the engine's own, valid for
//! the life of the world"; that is a longer lifetime than an editor value wants to be tied to, and a
//! schema that borrowed it could not outlive a runtime restart. The editor keeps its own copy, and a
//! restart re-reads rather than dangles.
//!
//! --- THE FIELD COUNT IS WHAT IS DESCRIBABLE, NOT WHAT THE STRUCT HOLDS ------------------------------
//!
//! `cy_abi.h` is explicit: "A COMPONENT MAY HAVE FEWER FIELDS HERE THAN IT HAS MEMBERS. Only fields
//! whose type has a fixed width this ABI can name are described." So [`ComponentSchema::fields`] is
//! the describable set, and an inspector built on it shows what it can edit rather than guessing at
//! what it cannot. That is a property to surface in the interface, not to paper over here.

use cy_editor_core::ids::{FieldId, TypeId};
use cy_editor_core::value::ValueKind;

use crate::generated::enums::VarType;

/// One component type as the engine describes it.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct ComponentSchema {
    /// The engine's identifier for the type, which is its index in the world's registry.
    pub id: TypeId,
    /// The engine's name, copied.
    pub name: String,
    /// Bytes per row. Zero for a tag component, which has no column.
    pub size: u32,
    /// The type's alignment.
    pub alignment: u32,
    /// The fields this ABI can describe. See the module note: this may be fewer than the C++ struct
    /// has members, and that is information rather than an omission.
    pub fields: Vec<FieldSchema>,
}

impl ComponentSchema {
    /// Whether this is a tag: a component that marks an entity and carries no data.
    #[must_use]
    pub const fn is_tag(&self) -> bool {
        self.size == 0
    }

    /// The field with this identity, if the type has one.
    #[must_use]
    pub fn field(&self, id: FieldId) -> Option<&FieldSchema> {
        self.fields.iter().find(|field| field.id == id)
    }

    /// The field with this name, if the type has one.
    #[must_use]
    pub fn field_named(&self, name: &str) -> Option<&FieldSchema> {
        self.fields.iter().find(|field| field.name == name)
    }
}

/// One field of one component type.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct FieldSchema {
    /// The field's identity. See [`FieldSchema::id_for_index`] for what it is made of and why.
    pub id: FieldId,
    /// The engine's name, copied.
    pub name: String,
    /// The kind of value the field holds.
    pub kind: ValueKind,
    /// The field's index in its component, which is what the ABI's accessors take.
    pub index: u32,
    /// The byte offset within the component, for a caller reading a chunk column directly.
    pub offset: u32,
    /// The field's byte size.
    pub size: u32,
}

impl FieldSchema {
    /// The identity of the `index`-th field of the component type `component`.
    ///
    /// Composed from the two numbers rather than hashed from the name, for the reason
    /// `cy_editor_core::ids` gives at length: a name-derived identifier makes the specification's
    /// rename scenario impossible, because the new name is a different number and every history
    /// entry addressing the field silently targets nothing.
    ///
    /// This is the identity of a field *of a runtime component*, and it is stable exactly as far as
    /// the engine's registration order is — which is the same guarantee the ABI gives for component
    /// identifiers themselves. A document's own fields are identified by its schema, which assigns
    /// and records them; see `cy-editor-documents`.
    #[must_use]
    pub const fn id_for_index(component: TypeId, index: u32) -> FieldId {
        FieldId::from_raw((component.as_u64() << 32) | (index as u64))
    }
}

/// The `ValueKind` an ABI `CyVarType` presents as.
///
/// The eight narrow integer types collapse onto `Int`, exactly as they do in the Swift overlay and
/// for the same reason: the payload is 64 bits and the storage width is the *field's* property,
/// enforced on the write by the engine's range check. An editor-side narrow integer type would be a
/// second place that decision is made.
#[must_use]
pub const fn kind_of(var_type: VarType) -> ValueKind {
    match var_type {
        VarType::Nil => ValueKind::Nil,
        VarType::Bool => ValueKind::Bool,
        VarType::I64
        | VarType::I8
        | VarType::I16
        | VarType::I32
        | VarType::U8
        | VarType::U16
        | VarType::U32
        | VarType::U64 => ValueKind::Int,
        VarType::F32 => ValueKind::Float,
        VarType::F64 => ValueKind::Double,
        VarType::Vec2 => ValueKind::Vec2,
        VarType::Vec3 => ValueKind::Vec3,
        VarType::Vec4 => ValueKind::Vec4,
        VarType::Quat => ValueKind::Quat,
        VarType::String => ValueKind::Text,
        VarType::Bytes => ValueKind::Bytes,
        VarType::Entity => ValueKind::Entity,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_abi_var_type_has_an_editor_kind() {
        // `VarType::ALL` is generated from the ABI, so appending a `CyVarType` makes this test the
        // place that notices — which is the point of it existing at all.
        for var_type in VarType::ALL {
            let kind = kind_of(var_type);
            if var_type == VarType::Nil {
                assert_eq!(kind, ValueKind::Nil);
            } else {
                assert_ne!(kind, ValueKind::Nil, "{var_type:?} mapped to nil");
            }
        }
    }

    #[test]
    fn a_fields_identity_is_its_component_and_its_index() {
        let component = TypeId::from_raw(7);
        let first = FieldSchema::id_for_index(component, 0);
        let second = FieldSchema::id_for_index(component, 1);
        assert_ne!(first, second);
        assert_ne!(first, FieldSchema::id_for_index(TypeId::from_raw(8), 0));
    }
}
