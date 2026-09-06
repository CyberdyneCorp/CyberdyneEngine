//! Type descriptions the inspector is generated from. Task 4.6, `editor-ui-ux`.
//!
//! > The inspector SHALL be **generated from the engine's reflection data** rather than hand-written
//! > per type, so a new type is editable without editor code.
//!
//! That requires something to read. Two things can describe a component type — a document's own
//! schema, and the engine's registry over the C ABI — and this crate converts both into one shape so
//! that the form generator above has a single input. A generator with two input types is two
//! generators, and the second one is the one nobody tests.
//!
//! | Module | What it holds |
//! |---|---|
//! | [`type_info`] | [`ReflectedType`] and [`ReflectedField`]: one description, whatever described it |
//! | [`presentation`] | Units, ranges, ordering, disclosure, conditional visibility, validation |
//! | [`overrides`] | Registrable custom property and whole-type editors, as `editor-ui-ux` requires |
//! | [`catalogue`] | [`Catalogue::of_document`] and [`Catalogue::of_world`], the two sources |
//!
//! --- WHY THIS IS A CRATE AT LAYER 3 AND NOT A MODULE OF THE INTERFACE -------------------------------
//!
//! Because of what it must be able to name. [`Catalogue::of_world`] reads the engine through
//! `cy-editor-sdk`, and `cy-editor-sdk` is the one crate in this workspace permitted `unsafe`. A
//! *presentation* crate holding it would be the reach-through `editor-rust-application` forbids —
//! "`unsafe` code ... SHALL NOT appear in panels, view models, services, or domain logic" — so the
//! dependency is inverted here instead: this crate names the SDK, produces plain values, and
//! `cy-editor-interface` names this. The panels never see a C type, and the inspector is still
//! generated from what the engine said.
//!
//! --- THE HONEST LIMIT ------------------------------------------------------------------------------
//!
//! ABI 1.1 describes a field's *storage* — type, offset, size, name — and not its *meaning*: no
//! unit, no range, no tooltip, no disclosure decision. The engine's own `reflect::TypeInfo` carries
//! some of those and they do not cross the boundary. So a runtime type is editable here and is
//! presented plainly, and the metadata that would refine it comes from a document's schema or from a
//! registered override until the ABI carries attributes. That is written down in
//! [`presentation`] rather than worked around, because the obvious workaround — inferring a unit
//! from a field's name — is how an editor comes to believe that `angle` is in degrees when the
//! engine stores radians.

#![forbid(unsafe_code)]

pub mod catalogue;
pub mod overrides;
pub mod presentation;
pub mod type_info;

pub use catalogue::Catalogue;
pub use overrides::{Overrides, PropertyOverride, TypeOverride};
pub use presentation::{Disclosure, Presentation, Range, Unit, VisibleWhen};
pub use type_info::{Origin, ReflectedField, ReflectedType};
