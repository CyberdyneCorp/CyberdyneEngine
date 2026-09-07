//! CyberEditor's interface. Task 4.6, `editor-ui-ux`.
//!
//! Docking and workspaces, density, the command palette, keyboard-first operation, the **generated
//! inspector**, validation surfacing and notifications — as models a test can drive, with no window,
//! no graphics device and no interface toolkit.
//!
//! | Module | The requirement it implements |
//! |---|---|
//! | [`docking`] | Docking, floating, tabbing, multiple windows, named workspaces, reset |
//! | [`panels`] | Stable panel identity, kept apart from the title a person reads |
//! | [`palette`] | The command palette: one input, fuzzy, ranked, incremental, never blocking |
//! | [`keymap`] | Keyboard-first operation, chords, contexts, conflicts reported with both commands |
//! | [`inspector`] | The inspector generated from reflection, with no per-type editor code |
//! | [`problems`] | Problems at the object that has them, each stating what would fix it |
//! | [`progress`] | The unified progress surface: what is running, cancellable, and what a failure left |
//! | [`notifications`] | Notifications that do not steal focus; modals reserved for decisions |
//! | [`virtualise`] | Lists whose cost scales with what is visible rather than with what exists |
//! | [`thumbnails`] | Asset previews the engine renders, and typed placeholders until it has |
//! | [`shell`] | The three rules that hold across all of it: stability, ambient status, vocabulary |
//!
//! --- WHY THERE IS NO TOOLKIT HERE, AND WHY THAT IS THE POINT ----------------------------------------
//!
//! `editor-rust-application` requires the interface toolkit to be "an implementation choice behind
//! editor abstractions, **selected on measurement**". Everything above is the half of the interface
//! that a toolkit does not decide: which panels exist and where, what the palette finds and in what
//! order, which key does what and what happens when two want the same one, which rows the inspector
//! generates for a type nobody wrote code for, where a problem is shown, and what a notification is
//! allowed to interrupt.
//!
//! That half is finished and tested before the choice is made, which is what keeps the choice free —
//! and it is why `cargo test -p cy-editor-interface` needs no display. When a toolkit arrives it
//! renders these values; the tests below keep meaning what they mean.
//!
//! --- WHAT THE INTERFACE MAY NOT REACH -------------------------------------------------------------
//!
//! `cy-editor-sdk`. The generated inspector lays out a component type the **engine** registered, and
//! it reads it as a `cy_editor_reflection::ReflectedType` — a plain value produced by a crate that
//! may name the SDK. A presentation crate holding the workspace's one `unsafe` crate is the
//! reach-through `editor-rust-application` forbids, and this crate's manifest is where that is
//! decided. No C type appears here, and `cy-editor-app`'s `tests/safety.rs` is what keeps it so.
//!
//! --- THE VISUAL LANGUAGE IS ASKED, NOT COPIED ------------------------------------------------------
//!
//! Every colour, hue, form, metric and term comes from `cy-editor-visual`: the axis colours in a
//! vector field are the same function the gizmo calls, the density metrics are the same table the
//! viewport overlays use, and every label this crate can draw is checked against the engine's
//! vocabulary by a test in [`shell`]. Nothing here chooses a colour.

#![forbid(unsafe_code)]

pub mod docking;
pub mod inspector;
pub mod keymap;
pub mod notifications;
pub mod palette;
pub mod panels;
pub mod problems;
pub mod progress;
pub mod shell;
pub mod thumbnails;
pub mod virtualise;

pub use docking::{Layout, PanelId, Workspaces};
pub use inspector::{
    Control, GeneratedInspector, InspectorRow, InspectorSection, SelectionSummary,
};
pub use keymap::{Chord, Keymap, Resolution, Stroke};
pub use notifications::{Modal, NotificationCentre, Offer, Toast};
pub use palette::{Action, Entry, Index, Match};
pub use panels::{PanelKey, PanelTitles};
pub use problems::{Problems, Report, Site};
pub use progress::{Artefact, ProgressSurface};
pub use shell::Shell;
pub use thumbnails::{Kind, Thumbnail, Thumbnails};
pub use virtualise::{Viewport, Window};
