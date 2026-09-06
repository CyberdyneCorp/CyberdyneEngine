//! CyberEditor's visual language. Task 4.7, `editor-visual-language`.
//!
//! What the editor looks like, what its colours mean, and what it calls things — as data and
//! predicates rather than as a style guide, because the failure this capability exists to prevent is
//! cumulative:
//!
//! > What decides a tool's appearance is a long series of small local decisions — this header needs
//! > emphasis, this state needs a colour, this plugin needs an icon — each reasonable alone. By the
//! > time an editor looks inconsistent, the inconsistency is spread across a hundred files and
//! > fixing it is a rewrite of the interface rather than a change to it.
//!
//! A style guide cannot stop that; it is a document somebody has to remember to read. What can stop
//! it is there being exactly one place to obtain a colour, one place to obtain an axis hue, one
//! vocabulary table, and a check for each forbidden pattern. That is this crate.
//!
//! | Module | The requirement it implements |
//! |---|---|
//! | [`colour`] | Semantic colour, the surface system, theming and legibility |
//! | [`axis`] | X red, Y green, Z blue — everywhere, not user-remappable |
//! | [`gizmo`] | Gizmo legibility: distinct by shape, acquirable without precision |
//! | [`orientation`] | The view-orientation widget, which is **not** a manipulator |
//! | [`selection`] | Selection appearance, and editor against gameplay selection |
//! | [`chrome`] | Viewport chrome as overlay, and the default workspace composition |
//! | [`density`] | Density over decoration, typography, interface scale |
//! | [`vocabulary`] | The engine's own words, with the familiar ones as search aliases |
//! | [`rules`] | The twelve forbidden patterns, each with the check that catches it |
//!
//! --- WHY THIS IS LAYER 1 AND NOT A PANEL'S BUSINESS -------------------------------------------------
//!
//! Every layer above needs it. A *service* reporting a warning wants the severity's word and shape;
//! a *view model* labelling a vector field wants the axis language; a viewport overlay wants the
//! chrome rules. If the language lived beside the panels, the layers below could not obey it, and
//! "one colour vocabulary across every panel, overlay, gizmo, graph, and visualisation" would be
//! true of the panels only.
//!
//! --- WHAT IS DELIBERATELY ABSENT ------------------------------------------------------------------
//!
//! A widget, a font file, an icon bitmap, a draw call, and any dependency that could supply one.
//! `editor-rust-application` requires the interface toolkit to be "an implementation choice behind
//! editor abstractions, selected on measurement" — so the whole of this capability is stated as
//! values a test can assert on, and every test in this crate runs with no window and no graphics
//! device. The day a toolkit is chosen, it reads these values; it does not replace them.
//!
//! The normative reference imagery is `docs/design/`, and `docs/design/editor-visual-language.md` is
//! the illustrated view of the same requirements. Where an image and the specification disagree, the
//! specification wins — and where this crate and the specification disagree, this crate is wrong.

#![forbid(unsafe_code)]

pub mod axis;
pub mod chrome;
pub mod colour;
pub mod density;
pub mod gizmo;
pub mod orientation;
pub mod rules;
pub mod selection;
pub mod vocabulary;

pub use axis::Axis;
pub use chrome::{Chrome, Composition, Overlay, Region};
pub use colour::{Mode, Rgb, Semantic, Surface, Theme, Vision};
pub use density::{Density, Metrics, Scale, TextRole};
pub use gizmo::{GizmoMode, Handle};
pub use orientation::OrientationWidget;
pub use selection::{SelectionAppearance, SelectionKind};
pub use vocabulary::{Term, check_label};
