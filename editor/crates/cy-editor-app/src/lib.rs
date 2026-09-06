//! CyberEditor's application shell. Tasks 2.1 and 2.7.
//!
//! There is no window here, and that is the state to keep it in for as long as possible.
//! `editor-rust-application` requires the interface toolkit to be "an implementation choice behind
//! editor abstractions, **selected on measurement**" — and every layer below this one is finished
//! and tested before that measurement has been made, which is the order that keeps the choice free.
//!
//! What this crate is, today: the wiring — a registry with the built-in commands, an [`Application`]
//! that owns the services, and a headless driver that runs a script of commands. That driver is not
//! a placeholder. It is what `editor-rust-application`'s testability requirement asks for ("Tests
//! SHALL be able to drive the editor through commands and assert on model and view model state"),
//! it is what task 6.1's scripted session will be built from, and it is what an agent connection
//! will project.

#![forbid(unsafe_code)]

pub mod application;
pub mod script;

pub use application::Application;
pub use script::{ScriptOutcome, run_script};
