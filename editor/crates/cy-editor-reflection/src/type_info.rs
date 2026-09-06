//! One description of a type, whatever described it.
//!
//! The generated inspector needs a type it can lay out. Two things can describe one: a **document's
//! schema**, which is the editor's own and exists before any runtime does, and the **engine**, over
//! the C ABI's `world_component_info` and `world_component_field`. They are different shapes, and
//! the inspector must not know which it is looking at — a form generator with two input types is two
//! form generators, and the second one is the one nobody tests.
//!
//! So both are converted into this, once, at the boundary. [`Origin`] records which, because the two
//! differ in ways an inspector legitimately shows: a document's field can be renamed and keeps its
//! identity, a runtime component's identity is its registry index, and a runtime type may have fewer
//! describable fields than the C++ struct has members — which `cy_abi.h` states in capitals and
//! which is information for the user rather than something to paper over.

use cy_editor_core::ids::{FieldId, TypeId};
use cy_editor_core::value::ValueKind;

use crate::presentation::Presentation;

/// What described a type.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub enum Origin {
    /// A document's own schema: the authoring types, which exist with no runtime attached.
    Document,
    /// The engine's component registry, read over the C ABI.
    Runtime,
}

impl Origin {
    /// The word an inspector's section header uses to say where a type came from.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            Origin::Document => "document",
            Origin::Runtime => "runtime",
        }
    }
}

/// One field, described for presentation.
#[derive(Clone, PartialEq, Debug)]
pub struct ReflectedField {
    /// Its identity. Stable across a rename; see `cy-editor-documents`' schema note.
    pub id: FieldId,
    /// Its name, as whatever described it names it **now**.
    pub name: String,
    /// What it holds.
    pub kind: ValueKind,
    /// One sentence for a caller that cannot see the interface. Empty when the source has none —
    /// which is every runtime field at ABI 1.1; see [`crate::presentation`].
    pub description: String,
    /// The index the ABI's accessors take, for a runtime field. Zero for a document's.
    pub index: u32,
    /// How it is presented.
    pub presentation: Presentation,
}

impl ReflectedField {
    /// The text a tooltip shows: the description when there is one, and what the field *is* when
    /// there is not.
    ///
    /// A fallback rather than an empty tooltip, because `editor-ui-ux` requires "tooltips with
    /// meaning rather than restated labels" and the honest fallback for a field the engine described
    /// without a sentence is its type — which is more than the label already says.
    #[must_use]
    pub fn tooltip(&self) -> String {
        if !self.presentation.tooltip.trim().is_empty() {
            return self.presentation.tooltip.clone();
        }
        if !self.description.trim().is_empty() {
            return self.description.clone();
        }
        match self.presentation.unit {
            Some(unit) => format!("{} ({}), in {}", self.name, self.kind, unit.suffix()),
            None => format!("{} ({})", self.name, self.kind),
        }
    }
}

/// One component type, described for presentation.
#[derive(Clone, PartialEq, Debug)]
pub struct ReflectedType {
    /// Its identity, in the terms of whatever described it.
    pub id: TypeId,
    /// Its name.
    pub name: String,
    /// What described it.
    pub origin: Origin,
    /// One sentence about the type, when the source has one.
    pub description: String,
    /// The identifier of a registered whole-type editor that replaces the generated form, when one
    /// is registered. See [`crate::overrides`].
    pub custom_editor: Option<String>,
    /// Whether the type carries data at all. A tag component has no fields and no column.
    pub is_tag: bool,
    /// Whether the source described fewer fields than the type has members.
    ///
    /// `cy_abi.h`: "A COMPONENT MAY HAVE FEWER FIELDS HERE THAN IT HAS MEMBERS. Only fields whose
    /// type has a fixed width this ABI can name are described." An inspector shows what it can edit
    /// and says so, which is why this is a flag rather than a silence.
    pub partially_described: bool,
    /// Its fields, in presentation order.
    pub fields: Vec<ReflectedField>,
}

impl ReflectedType {
    /// The field with this identity.
    #[must_use]
    pub fn field(&self, id: FieldId) -> Option<&ReflectedField> {
        self.fields.iter().find(|field| field.id == id)
    }

    /// The field with this name, as it is named now.
    #[must_use]
    pub fn field_named(&self, name: &str) -> Option<&ReflectedField> {
        self.fields.iter().find(|field| field.name == name)
    }

    /// The fields shown when the inspector opens, in order.
    #[must_use]
    pub fn default_fields(&self) -> Vec<&ReflectedField> {
        self.ordered(crate::presentation::Disclosure::Default)
    }

    /// The fields behind the "advanced" disclosure, in order.
    #[must_use]
    pub fn advanced_fields(&self) -> Vec<&ReflectedField> {
        self.ordered(crate::presentation::Disclosure::Advanced)
    }

    fn ordered(&self, disclosure: crate::presentation::Disclosure) -> Vec<&ReflectedField> {
        let mut fields: Vec<&ReflectedField> = self
            .fields
            .iter()
            .filter(|field| field.presentation.disclosure == disclosure)
            .collect();
        fields.sort_by_key(|field| field.presentation.order);
        fields
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::presentation::{Disclosure, Unit};

    fn field(name: &str, order: u32, disclosure: Disclosure) -> ReflectedField {
        ReflectedField {
            id: FieldId::from_raw(u64::from(order) + 1),
            name: name.into(),
            kind: ValueKind::Float,
            description: String::new(),
            index: order,
            presentation: Presentation {
                disclosure,
                ..Presentation::at(order)
            },
        }
    }

    #[test]
    fn advanced_detail_is_present_and_separate_rather_than_hidden() {
        let reflected = ReflectedType {
            id: TypeId::from_raw(1),
            name: "Rendering".into(),
            origin: Origin::Document,
            description: String::new(),
            custom_editor: None,
            is_tag: false,
            partially_described: false,
            fields: vec![
                field("castShadows", 0, Disclosure::Default),
                field("occlusionBias", 1, Disclosure::Advanced),
                field("visible", 2, Disclosure::Default),
            ],
        };
        assert_eq!(reflected.default_fields().len(), 2);
        assert_eq!(reflected.advanced_fields().len(), 1);
        assert_eq!(
            reflected.fields.len(),
            reflected.default_fields().len() + reflected.advanced_fields().len(),
            "nothing is hidden; advanced detail is collapsed and still there"
        );
    }

    #[test]
    fn a_field_the_engine_described_without_a_sentence_still_has_a_useful_tooltip() {
        let mut field = field("mass", 0, Disclosure::Default);
        assert_eq!(field.tooltip(), "mass (float)");
        field.presentation.unit = Some(Unit::Kilograms);
        assert_eq!(field.tooltip(), "mass (float), in kg");
        field.presentation.tooltip = "How heavy the body is.".into();
        assert_eq!(field.tooltip(), "How heavy the body is.");
    }
}
