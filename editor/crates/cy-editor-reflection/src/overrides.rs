//! Registered overrides: how a plugin replaces what reflection generated.
//!
//! `editor-ui-ux`: "Custom property editors and custom whole-type editors SHALL be registrable to
//! override the generated presentation, and SHALL be usable by plugins."
//!
//! Two kinds, and they are different things. A **whole-type** override says "do not generate a form
//! for this type at all; the editor named here draws it" — a curve editor, a gradient, a material
//! preview. A **property** override refines one field: a unit the ABI could not carry, a range, a
//! tooltip, a disclosure decision, or a named control that replaces the one the kind implies.
//!
//! --- WHY THESE ARE KEYED BY NAME AND EVERYTHING ELSE IS KEYED BY IDENTITY ---------------------------
//!
//! Every other reference in this workspace addresses a stable identity, because a name-derived
//! identifier makes the specification's rename scenario impossible. An override is the exception,
//! deliberately: it is registered by a **plugin**, at start-up, against a type it did not create and
//! whose identity is assigned by whatever describes it — a document's counter, or the engine's
//! registry index, neither of which a plugin can know before the document is open. A name is the
//! only thing a plugin has.
//!
//! The cost is stated rather than avoided: renaming a type or a field **detaches its override**. It
//! detaches loudly — the generated form comes back — rather than silently editing the wrong field,
//! which is what a hashed identity would do.
//!
//! --- WHAT AN OVERRIDE MAY NOT DO -------------------------------------------------------------------
//!
//! Change a field's kind or identity. A custom editor decides how a value is *presented and edited*;
//! it does not decide what the value is, because the write still goes through a transaction against
//! the field the schema declares. [`PropertyOverride`] therefore has no `kind` and no `id`.

use std::collections::BTreeMap;

use crate::presentation::{Disclosure, Presentation, Range, Unit, VisibleWhen};

/// What a plugin says about one field.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct PropertyOverride {
    /// A registered control that replaces the one the field's kind implies.
    pub editor: Option<String>,
    /// The unit, when the plugin knows one the source could not carry.
    pub unit: Option<Unit>,
    /// The accepted range.
    pub range: Option<Range>,
    /// A sentence saying what the field affects.
    pub tooltip: Option<String>,
    /// The section to group it under.
    pub category: Option<String>,
    /// Where it sits within its section.
    pub order: Option<u32>,
    /// Whether it opens visible or collapsed.
    pub disclosure: Option<Disclosure>,
    /// Whether the editor may write it.
    pub writable: Option<bool>,
    /// The condition under which it is shown.
    pub visible_when: Option<VisibleWhen>,
    /// The value a reset restores.
    pub default: Option<cy_editor_core::Value>,
}

impl PropertyOverride {
    /// An override that changes nothing.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Set the unit.
    #[must_use]
    pub const fn with_unit(mut self, unit: Unit) -> Self {
        self.unit = Some(unit);
        self
    }

    /// Set the accepted range.
    #[must_use]
    pub const fn with_range(mut self, range: Range) -> Self {
        self.range = Some(range);
        self
    }

    /// Set the sentence that says what the field affects.
    #[must_use]
    pub fn with_tooltip(mut self, tooltip: impl Into<String>) -> Self {
        self.tooltip = Some(tooltip.into());
        self
    }

    /// Put the field behind the advanced disclosure, or bring it forward.
    #[must_use]
    pub const fn with_disclosure(mut self, disclosure: Disclosure) -> Self {
        self.disclosure = Some(disclosure);
        self
    }

    /// Replace the control the field's kind implies.
    #[must_use]
    pub fn with_editor(mut self, editor: impl Into<String>) -> Self {
        self.editor = Some(editor.into());
        self
    }

    /// Apply this override to a generated presentation.
    pub(crate) fn apply(&self, presentation: &mut Presentation) {
        if let Some(editor) = &self.editor {
            presentation.editor = Some(editor.clone());
        }
        if let Some(unit) = self.unit {
            presentation.unit = Some(unit);
        }
        if let Some(range) = self.range {
            presentation.range = Some(range);
        }
        if let Some(tooltip) = &self.tooltip {
            presentation.tooltip.clone_from(tooltip);
        }
        if let Some(category) = &self.category {
            presentation.category.clone_from(category);
        }
        if let Some(order) = self.order {
            presentation.order = order;
        }
        if let Some(disclosure) = self.disclosure {
            presentation.disclosure = disclosure;
        }
        if let Some(writable) = self.writable {
            presentation.writable = writable;
        }
        if let Some(visible_when) = &self.visible_when {
            presentation.visible_when = Some(visible_when.clone());
        }
        if let Some(default) = &self.default {
            presentation.default = Some(default.clone());
        }
    }
}

/// What a plugin says about one type.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct TypeOverride {
    /// A registered whole-type editor that replaces the generated form entirely.
    pub editor: Option<String>,
    /// Per-field refinements, keyed by the field's name.
    pub fields: BTreeMap<String, PropertyOverride>,
}

/// Every registered override, by type name.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct Overrides {
    types: BTreeMap<String, TypeOverride>,
}

impl Overrides {
    /// Nothing registered: every type gets the generated form.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Register a whole-type editor.
    ///
    /// The editor is named rather than supplied, because a control is a toolkit object and this
    /// crate has no toolkit — the name is resolved by whatever draws, which keeps the choice of
    /// toolkit an implementation detail exactly as `editor-rust-application` requires.
    pub fn register_type_editor(
        &mut self,
        type_name: impl Into<String>,
        editor: impl Into<String>,
    ) {
        self.types.entry(type_name.into()).or_default().editor = Some(editor.into());
    }

    /// Register a property override.
    pub fn register_property(
        &mut self,
        type_name: impl Into<String>,
        field_name: impl Into<String>,
        property: PropertyOverride,
    ) {
        self.types
            .entry(type_name.into())
            .or_default()
            .fields
            .insert(field_name.into(), property);
    }

    /// What is registered for a type, if anything.
    #[must_use]
    pub fn of_type(&self, type_name: &str) -> Option<&TypeOverride> {
        self.types.get(type_name)
    }

    /// How many types have something registered.
    #[must_use]
    pub fn len(&self) -> usize {
        self.types.len()
    }

    /// Whether nothing is registered.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.types.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_property_override_refines_a_presentation_without_replacing_it() {
        let mut presentation = Presentation::at(3);
        presentation.category = "Transform".into();
        PropertyOverride::new()
            .with_unit(Unit::Degrees)
            .with_range(Range::new(-180.0, 180.0))
            .apply(&mut presentation);

        assert_eq!(presentation.unit, Some(Unit::Degrees));
        assert_eq!(presentation.order, 3, "what was not overridden survives");
        assert_eq!(presentation.category, "Transform");
    }

    #[test]
    fn a_whole_type_editor_and_a_property_editor_are_registered_separately() {
        let mut overrides = Overrides::new();
        overrides.register_type_editor("Curve", "curve-editor");
        overrides.register_property(
            "Transform",
            "rotation",
            PropertyOverride::new().with_editor("euler-angles"),
        );

        assert_eq!(
            overrides.of_type("Curve").unwrap().editor.as_deref(),
            Some("curve-editor")
        );
        assert_eq!(
            overrides.of_type("Transform").unwrap().fields["rotation"]
                .editor
                .as_deref(),
            Some("euler-angles")
        );

        let mut presentation = Presentation::at(0);
        overrides.of_type("Transform").unwrap().fields["rotation"].apply(&mut presentation);
        assert_eq!(
            presentation.editor.as_deref(),
            Some("euler-angles"),
            "a registered control reaches the generated form"
        );
        assert!(overrides.of_type("Health").is_none());
        assert_eq!(overrides.len(), 2);
    }
}
