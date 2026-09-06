//! Every type one source describes, converted once at the boundary.
//!
//! This is the crate's whole purpose and the M4 debt it closes: *"Nothing is reflected, so the
//! editor's inspector has nothing to read."* ABI 1.1 fixed the ABI half —
//! `world_component_count`, `world_component_info` and `world_component_field` describe a world the
//! editor did not build — and [`Catalogue::of_world`] is the editor half, which nothing had yet
//! called. With both, `cy-editor-interface` generates an inspector for a component type the *engine*
//! registered, with no per-type editor code anywhere.
//!
//! --- ONE CATALOGUE HAS ONE SOURCE, AND THEY ARE NOT MERGED ------------------------------------------
//!
//! A document's [`cy_editor_core::ids::TypeId`] comes from its own counter, starting at one. A
//! runtime component's is the engine's registry index, starting at zero. They collide by
//! construction, and a merged catalogue would answer `type_of(TypeId(1))` with whichever source
//! happened to be inserted last — a defect that shows up as an inspector editing the wrong
//! component, days later, in a session with a runtime attached.
//!
//! So there is no `merge`. A caller holds the authoring catalogue and, when a runtime is attached,
//! the runtime one beside it, and [`Catalogue::origin`] says which is which. That is also the
//! honest model: the authoring document and the runtime world are *different worlds*, which
//! `editor-documents-and-transactions` requires to be kept distinct.

use cy_editor_core::ids::TypeId;
use cy_editor_core::problem::Result;
use cy_editor_documents::schema::DocumentSchema;
use cy_editor_sdk::World;

use crate::overrides::Overrides;
use crate::presentation::Presentation;
use crate::type_info::{Origin, ReflectedField, ReflectedType};

/// Every type one source describes.
#[derive(Clone, PartialEq, Debug)]
pub struct Catalogue {
    origin: Origin,
    types: Vec<ReflectedType>,
}

impl Catalogue {
    /// An empty catalogue of a given origin. What an editor holds before a document is open.
    #[must_use]
    pub const fn empty(origin: Origin) -> Self {
        Self {
            origin,
            types: Vec::new(),
        }
    }

    /// Describe a document's own schema.
    ///
    /// The authoring path, and the one that works with no runtime attached at all — which is the
    /// mode `editor-rust-application` calls `NoRuntime` and the mode most of a session is spent in.
    #[must_use]
    pub fn of_document(schema: &DocumentSchema) -> Self {
        let types = schema
            .types()
            .map(|definition| ReflectedType {
                id: definition.id,
                name: definition.name.clone(),
                origin: Origin::Document,
                description: String::new(),
                custom_editor: None,
                is_tag: definition.fields.is_empty(),
                partially_described: false,
                fields: definition
                    .fields
                    .iter()
                    .enumerate()
                    .map(|(order, field)| ReflectedField {
                        id: field.id,
                        name: field.name.clone(),
                        kind: field.kind,
                        description: field.description.clone(),
                        index: 0,
                        presentation: Presentation {
                            writable: field.writable,
                            ..Presentation::at(order_of(order))
                        },
                    })
                    .collect(),
            })
            .collect();
        Self {
            origin: Origin::Document,
            types,
        }
    }

    /// Describe the component types the **engine** has registered, over the C ABI.
    ///
    /// Every field arrives with [`Presentation::at`] and nothing else, because `CyFieldDesc` carries
    /// a type, an offset, a size and a name and no attributes — see [`crate::presentation`], which
    /// says what that costs and where the missing metadata comes from until the ABI carries it.
    pub fn of_world(world: &World<'_>) -> Result<Self> {
        let schemas = world.component_schemas()?;
        let types = schemas
            .into_iter()
            .map(|schema| {
                let is_tag = schema.is_tag();
                // A component with a size but no describable fields is one whose members the ABI
                // has no fixed-width name for. Saying so is information; showing an empty section
                // with no explanation is the thing an inspector must not do.
                let partially_described = !is_tag && schema.fields.is_empty();
                ReflectedType {
                    id: schema.id,
                    name: schema.name,
                    origin: Origin::Runtime,
                    description: String::new(),
                    custom_editor: None,
                    is_tag,
                    partially_described,
                    fields: schema
                        .fields
                        .into_iter()
                        .map(|field| ReflectedField {
                            id: field.id,
                            name: field.name,
                            kind: field.kind,
                            description: String::new(),
                            index: field.index,
                            presentation: Presentation::at(field.index),
                        })
                        .collect(),
                }
            })
            .collect();
        Ok(Self {
            origin: Origin::Runtime,
            types,
        })
    }

    /// What described these types.
    #[must_use]
    pub const fn origin(&self) -> Origin {
        self.origin
    }

    /// Every type, in the order its source lists them.
    #[must_use]
    pub fn types(&self) -> &[ReflectedType] {
        &self.types
    }

    /// The type with this identity.
    #[must_use]
    pub fn type_of(&self, id: TypeId) -> Option<&ReflectedType> {
        self.types.iter().find(|reflected| reflected.id == id)
    }

    /// The type with this name, as it is named now.
    #[must_use]
    pub fn type_named(&self, name: &str) -> Option<&ReflectedType> {
        self.types.iter().find(|reflected| reflected.name == name)
    }

    /// Apply every registered override, by name.
    ///
    /// Returns the catalogue rather than mutating in place at the call site, so that the overridden
    /// catalogue is a value a view model can hold and compare — and so that "which overrides were
    /// applied" is answered by which catalogue you are holding rather than by when you called this.
    #[must_use]
    pub fn with_overrides(mut self, overrides: &Overrides) -> Self {
        for reflected in &mut self.types {
            let Some(registered) = overrides.of_type(&reflected.name) else {
                continue;
            };
            reflected.custom_editor.clone_from(&registered.editor);
            for field in &mut reflected.fields {
                if let Some(property) = registered.fields.get(&field.name) {
                    property.apply(&mut field.presentation);
                }
            }
        }
        self
    }
}

/// A declaration position as a presentation order.
///
/// Saturating rather than panicking on a type with more than four billion fields, which is not a
/// case worth a `Result` but is one worth not aborting the inspector for.
fn order_of(position: usize) -> u32 {
    u32::try_from(position).unwrap_or(u32::MAX)
}

#[cfg(test)]
mod tests {
    use cy_editor_core::value::{Value, ValueKind};

    use super::*;
    use crate::overrides::PropertyOverride;
    use crate::presentation::{Disclosure, Range, Unit};

    fn schema() -> DocumentSchema {
        let mut schema = DocumentSchema::new();
        let transform = schema.declare_type("Transform", false);
        schema
            .declare_field(transform, "position", ValueKind::Vec3, "where the node is")
            .unwrap();
        let rotation = schema
            .declare_field(transform, "rotation", ValueKind::Quat, "how it is turned")
            .unwrap();
        schema.set_writable(transform, rotation, false).unwrap();
        schema.declare_type("Selected", true);
        schema
    }

    #[test]
    fn a_documents_schema_describes_itself_in_declaration_order() {
        let catalogue = Catalogue::of_document(&schema());
        assert_eq!(catalogue.origin(), Origin::Document);

        let transform = catalogue.type_named("Transform").unwrap();
        assert_eq!(transform.fields.len(), 2);
        assert_eq!(transform.fields[0].name, "position");
        assert_eq!(transform.fields[0].presentation.order, 0);
        assert_eq!(transform.fields[1].presentation.order, 1);
        assert!(
            !transform.fields[1].presentation.writable,
            "read-only survives"
        );
        assert_eq!(transform.fields[0].description, "where the node is");
    }

    #[test]
    fn a_type_with_no_fields_is_a_tag_rather_than_an_empty_form() {
        let catalogue = Catalogue::of_document(&schema());
        assert!(catalogue.type_named("Selected").unwrap().is_tag);
    }

    #[test]
    fn an_override_refines_what_reflection_could_not_say() {
        // The mechanism that stands in for the attributes the ABI cannot yet carry, and the one
        // `editor-ui-ux` requires for plugins regardless.
        let mut overrides = Overrides::new();
        overrides.register_property(
            "Transform",
            "position",
            PropertyOverride::new()
                .with_unit(Unit::Metres)
                .with_range(Range::new(-10_000.0, 10_000.0))
                .with_tooltip("Where the node sits in its parent's space.")
                .with_disclosure(Disclosure::Default),
        );
        overrides.register_type_editor("Transform", "transform-editor");

        let catalogue = Catalogue::of_document(&schema()).with_overrides(&overrides);
        let transform = catalogue.type_named("Transform").unwrap();
        let position = transform.field_named("position").unwrap();

        assert_eq!(position.presentation.unit, Some(Unit::Metres));
        assert_eq!(transform.custom_editor.as_deref(), Some("transform-editor"));
        assert_eq!(
            position.tooltip(),
            "Where the node sits in its parent's space."
        );
    }

    #[test]
    fn a_rename_detaches_an_override_loudly_rather_than_moving_it_silently() {
        // The cost of keying overrides by name, stated in the module note and asserted here so that
        // a future change to identity-keyed overrides has to notice this test.
        let mut schema = schema();
        let transform = schema.type_named("Transform").unwrap().id;
        let position = schema.type_of(transform).unwrap().fields[0].id;

        let mut overrides = Overrides::new();
        overrides.register_property(
            "Transform",
            "position",
            PropertyOverride::new().with_unit(Unit::Metres),
        );
        schema
            .rename_field(transform, position, "translation")
            .unwrap();

        let catalogue = Catalogue::of_document(&schema).with_overrides(&overrides);
        let field = catalogue
            .type_named("Transform")
            .unwrap()
            .field_named("translation")
            .unwrap();
        assert_eq!(
            field.presentation.unit, None,
            "the generated presentation comes back, rather than the override landing on a \
             different field"
        );
        assert_eq!(field.id, position, "and the identity did not move");
    }

    #[test]
    fn a_default_makes_a_row_resettable() {
        let mut overrides = Overrides::new();
        overrides.register_property(
            "Transform",
            "position",
            PropertyOverride {
                default: Some(Value::Vec3([0.0; 3])),
                ..PropertyOverride::new()
            },
        );
        let catalogue = Catalogue::of_document(&schema()).with_overrides(&overrides);
        let position = catalogue
            .type_named("Transform")
            .unwrap()
            .field_named("position")
            .unwrap();
        assert!(!position.presentation.is_modified(&Value::Vec3([0.0; 3])));
        assert!(
            position
                .presentation
                .is_modified(&Value::Vec3([1.0, 0.0, 0.0]))
        );
    }
}
