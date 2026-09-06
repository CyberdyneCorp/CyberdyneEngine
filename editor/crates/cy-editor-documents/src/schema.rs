//! What a document's types and fields are, and why a rename does not move an identifier.
//!
//! `editor-documents-and-transactions` requires that a history "SHALL remain valid across an asset
//! reload, a document close and reopen within a session, **a rename**, and a schema migration", and
//! gives the scenario: "WHEN a field is renamed and its identity is unchanged THEN existing history
//! entries targeting it SHALL still apply."
//!
//! That is a property of where the name lives. Here, a [`FieldDefinition`] owns a mutable `name` and
//! an immutable [`FieldId`] assigned once at declaration. Renaming rewrites the name; every history
//! entry addresses the identifier and keeps working. Had the identifier been derived from the name —
//! which is the shorter implementation — the scenario would be impossible rather than merely
//! unimplemented.

use std::collections::BTreeMap;

use cy_editor_core::ids::{FieldId, TypeId};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::ValueKind;

/// One field of one component type.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct FieldDefinition {
    /// Assigned once, never derived from the name, never reused. See the module note.
    pub id: FieldId,
    /// The name a person sees. Mutable; a rename does not touch [`FieldDefinition::id`].
    pub name: String,
    /// What the field holds.
    pub kind: ValueKind,
    /// One sentence for a caller that cannot see the interface, per `editor-agent-interface`.
    pub description: String,
    /// Whether the editor may write it. A read-only field is one the inspector shows and refuses.
    pub writable: bool,
}

/// One component type a document can hold.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct TypeDefinition {
    /// Assigned once at declaration.
    pub id: TypeId,
    /// The name a person sees. Mutable.
    pub name: String,
    /// Whether this type is authoring-only, and so must not survive the compilation to a runtime
    /// world. See [`crate::worlds`], where that becomes a checkable property.
    pub authoring_only: bool,
    /// The type's fields, in declaration order.
    pub fields: Vec<FieldDefinition>,
}

impl TypeDefinition {
    /// The field with this identity.
    #[must_use]
    pub fn field(&self, id: FieldId) -> Option<&FieldDefinition> {
        self.fields.iter().find(|field| field.id == id)
    }

    /// The field with this name, as it is named *now*.
    #[must_use]
    pub fn field_named(&self, name: &str) -> Option<&FieldDefinition> {
        self.fields.iter().find(|field| field.name == name)
    }
}

/// The types a document's content is described by.
///
/// A document's schema is the editor's own. It is not the engine's component registry: a document
/// exists before any runtime does, is edited with no runtime at all in `NoRuntime` mode, and must
/// keep its history across a runtime restart. The two are related by name at the compilation step,
/// which is where a mismatch is reported.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct DocumentSchema {
    types: BTreeMap<TypeId, TypeDefinition>,
    next_type: u64,
    next_field: u64,
}

impl DocumentSchema {
    /// An empty schema.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            types: BTreeMap::new(),
            next_type: 1,
            next_field: 1,
        }
    }

    /// Declare a component type, returning its identity.
    ///
    /// Identifiers are issued from a counter that never goes backwards, so a type removed and
    /// re-declared gets a new identity — which is correct: a history entry addressing the old one
    /// must not silently start applying to the new.
    pub fn declare_type(&mut self, name: impl Into<String>, authoring_only: bool) -> TypeId {
        let id = TypeId::from_raw(self.next_type);
        self.next_type += 1;
        self.types.insert(
            id,
            TypeDefinition {
                id,
                name: name.into(),
                authoring_only,
                fields: Vec::new(),
            },
        );
        id
    }

    /// Declare a field on a type, returning its identity.
    pub fn declare_field(
        &mut self,
        type_id: TypeId,
        name: impl Into<String>,
        kind: ValueKind,
        description: impl Into<String>,
    ) -> Result<FieldId> {
        let id = FieldId::from_raw(self.next_field);
        let definition = self.types.get_mut(&type_id).ok_or_else(|| {
            Problem::not_found(format!("a type with identity {}", type_id.as_u64()))
        })?;
        self.next_field += 1;
        definition.fields.push(FieldDefinition {
            id,
            name: name.into(),
            kind,
            description: description.into(),
            writable: true,
        });
        Ok(id)
    }

    /// Rename a field. Its identity does not move, so history keeps applying to it.
    pub fn rename_field(
        &mut self,
        type_id: TypeId,
        field: FieldId,
        name: impl Into<String>,
    ) -> Result<()> {
        let definition = self.types.get_mut(&type_id).ok_or_else(|| {
            Problem::not_found(format!("a type with identity {}", type_id.as_u64()))
        })?;
        let field = definition
            .fields
            .iter_mut()
            .find(|candidate| candidate.id == field)
            .ok_or_else(|| Problem::not_found("a field with that identity on that type"))?;
        field.name = name.into();
        Ok(())
    }

    /// Mark a field read-only, so the inspector shows it and refuses to write it.
    pub fn set_writable(&mut self, type_id: TypeId, field: FieldId, writable: bool) -> Result<()> {
        let definition = self
            .types
            .get_mut(&type_id)
            .ok_or_else(|| Problem::not_found("a type with that identity"))?;
        let field = definition
            .fields
            .iter_mut()
            .find(|candidate| candidate.id == field)
            .ok_or_else(|| Problem::not_found("a field with that identity on that type"))?;
        field.writable = writable;
        Ok(())
    }

    /// The type with this identity.
    #[must_use]
    pub fn type_of(&self, id: TypeId) -> Option<&TypeDefinition> {
        self.types.get(&id)
    }

    /// The type with this name, as it is named now.
    #[must_use]
    pub fn type_named(&self, name: &str) -> Option<&TypeDefinition> {
        self.types
            .values()
            .find(|definition| definition.name == name)
    }

    /// Every type, in identity order.
    pub fn types(&self) -> impl Iterator<Item = &TypeDefinition> {
        self.types.values()
    }

    /// Look a field's definition up by both identities.
    #[must_use]
    pub fn field(&self, type_id: TypeId, field: FieldId) -> Option<&FieldDefinition> {
        self.types.get(&type_id)?.field(field)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_rename_does_not_move_an_identity() {
        let mut schema = DocumentSchema::new();
        let transform = schema.declare_type("Transform", false);
        let field = schema
            .declare_field(transform, "position", ValueKind::Vec3, "where it is")
            .unwrap();

        schema
            .rename_field(transform, field, "translation")
            .unwrap();

        // The scenario the specification names: a history entry addresses the identity, and the
        // identity is what survived.
        assert_eq!(schema.field(transform, field).unwrap().name, "translation");
        assert_eq!(
            schema.type_of(transform).unwrap().field_named("position"),
            None
        );
        assert!(
            schema
                .type_of(transform)
                .unwrap()
                .field_named("translation")
                .is_some()
        );
    }

    #[test]
    fn identities_are_never_reused() {
        let mut schema = DocumentSchema::new();
        let first = schema.declare_type("Health", false);
        let second = schema.declare_type("Health", false);
        assert_ne!(first, second, "a re-declared type is a different type");
    }
}
