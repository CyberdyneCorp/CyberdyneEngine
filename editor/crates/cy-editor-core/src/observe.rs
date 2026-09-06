//! Change propagation by revision. Task 2.6, `editor-rust-application` "Change propagation".
//!
//! "State changes SHALL propagate to view models through **change notifications, versions, or
//! streams** — not by polling engine or document state each interface frame. A view model SHALL be
//! able to determine cheaply whether its inputs changed, and SHALL rebuild only what changed. The
//! editor SHALL NOT re-query reflected properties, asset listings, or runtime state at interface
//! frame rate when nothing has changed."
//!
//! --- WHY REVISIONS AND NOT CALLBACKS ---------------------------------------------------------------
//!
//! Callbacks are the obvious implementation and they are the wrong one here, for three reasons that
//! are all about testing and none about performance:
//!
//!   * A callback graph has an initialisation order, and `editor-rust-application` spends a whole
//!     requirement on the fact that a panel graph with no workable initialisation order is what
//!     peer dependencies produce. Revisions have no order at all.
//!   * A callback fires during someone else's mutation, so re-entrancy becomes a rule every
//!     subscriber has to know. A revision is read when the reader is ready.
//!   * A test asserting "the inspector performed no engine queries" has to count something. With
//!     revisions it counts rebuilds, which is a number the [`Watch`] already holds.
//!
//! The cost is that something has to *ask*. That is deliberate: the asking happens once per
//! interface frame per view model and compares two integers, which is the cheap half of the rule
//! the specification writes. What it forbids — re-querying the engine — is the expensive half, and
//! a [`Watch`] that reports "unchanged" is what stops it.
//!
//! [`EventLog`] is the stream half, for state that is a sequence rather than a value: notifications,
//! and a journal's committed transactions. A reader holds a cursor and drains; nothing is dropped
//! because nobody was listening yet.

use std::cell::Cell;
use std::collections::VecDeque;

/// A monotonically increasing version of some piece of state.
///
/// Revisions are comparable only against revisions of the *same* [`Versioned`]; comparing two
/// sources' revisions is meaningless and the type does not stop it, because making it stop would
/// mean a type parameter on every view model field for no benefit a test would catch.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug, Default)]
pub struct Revision(u64);

impl Revision {
    /// The revision of state that has never changed.
    pub const INITIAL: Self = Self(0);

    /// The revision as an integer, for a diagnostic or a serialised session.
    #[must_use]
    pub const fn as_u64(self) -> u64 {
        self.0
    }

    /// A revision with a given number.
    ///
    /// For state that keeps its own counter rather than living in a [`Versioned`] — a document's
    /// content does, because its mutation path is a token-gated method rather than a closure.
    /// Constructing one out of order would make a watcher miss a change, so the callers that use
    /// this increment monotonically and nothing else calls it.
    #[must_use]
    pub const fn from_u64(raw: u64) -> Self {
        Self(raw)
    }
}

/// A value that records when it last changed.
///
/// Mutation goes through [`Versioned::update`] or [`Versioned::set`], each of which bumps the
/// revision. There is no `DerefMut`: a mutable borrow that did not bump the revision would produce
/// exactly the defect this type exists to prevent — a change nothing observes — and it would look
/// like ordinary code.
#[derive(Clone, Debug, Default)]
pub struct Versioned<T> {
    value: T,
    revision: Revision,
}

impl<T> Versioned<T> {
    /// Wrap a value at the initial revision.
    pub const fn new(value: T) -> Self {
        Self {
            value,
            revision: Revision::INITIAL,
        }
    }

    /// The current value.
    pub const fn get(&self) -> &T {
        &self.value
    }

    /// The revision the value is at.
    pub const fn revision(&self) -> Revision {
        self.revision
    }

    /// Replace the value and bump the revision.
    pub fn set(&mut self, value: T) {
        self.value = value;
        self.revision.0 += 1;
    }

    /// Mutate the value in place and bump the revision.
    ///
    /// The revision moves whether or not the closure changed anything, because asking a closure
    /// whether it changed something requires `T: PartialEq` and gets the answer wrong for every
    /// type where equality is not identity. A spurious rebuild is cheap; a missed one is a stale
    /// panel.
    pub fn update<R>(&mut self, mutate: impl FnOnce(&mut T) -> R) -> R {
        let result = mutate(&mut self.value);
        self.revision.0 += 1;
        result
    }
}

/// One observer's memory of a revision it has already handled.
///
/// A view model holds one per input. [`Watch::changed`] answers "is there anything to rebuild"
/// without touching the state itself, which is what makes an idle panel free.
#[derive(Debug, Default)]
pub struct Watch {
    seen: Cell<Option<Revision>>,
    rebuilds: Cell<u64>,
}

impl Watch {
    /// A watch that has seen nothing, so its first question answers "changed".
    #[must_use]
    pub const fn new() -> Self {
        Self {
            seen: Cell::new(None),
            rebuilds: Cell::new(0),
        }
    }

    /// Whether `current` differs from the last revision this watch accepted.
    ///
    /// Does not consume it: a view model that decides not to rebuild this frame must still see
    /// "changed" on the next.
    pub fn changed(&self, current: Revision) -> bool {
        self.seen.get() != Some(current)
    }

    /// Record that a rebuild happened at `current`, so the next question answers "unchanged".
    pub fn accept(&self, current: Revision) {
        self.seen.set(Some(current));
        self.rebuilds.set(self.rebuilds.get() + 1);
    }

    /// Rebuild if the revision moved, and report whether it did.
    ///
    /// The whole propagation rule in one call: nothing runs when nothing changed.
    pub fn rebuild_if_changed(&self, current: Revision, rebuild: impl FnOnce()) -> bool {
        if !self.changed(current) {
            return false;
        }
        rebuild();
        self.accept(current);
        true
    }

    /// How many rebuilds this watch has performed.
    ///
    /// A test asserting "an idle inspector costs nothing" counts this, which is why it is public.
    pub fn rebuilds(&self) -> u64 {
        self.rebuilds.get()
    }
}

/// A bounded stream of events with per-reader cursors.
///
/// Used where the state is a sequence rather than a value: notifications, and the committed
/// transactions a live-editing bridge consumes. A reader holds a [`Cursor`] and drains what it has
/// not seen, so nothing is missed by a subscriber that connected late — within the log's capacity,
/// which is finite on purpose and reports what it dropped.
#[derive(Debug)]
pub struct EventLog<T> {
    events: VecDeque<(u64, T)>,
    next_sequence: u64,
    capacity: usize,
    dropped: u64,
}

/// A reader's position in an [`EventLog`].
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct Cursor(u64);

impl<T> EventLog<T> {
    /// A log holding at most `capacity` events.
    #[must_use]
    pub fn new(capacity: usize) -> Self {
        assert!(
            capacity > 0,
            "an event log with no capacity drops everything silently"
        );
        Self {
            events: VecDeque::new(),
            next_sequence: 0,
            capacity,
            dropped: 0,
        }
    }

    /// Append an event.
    pub fn push(&mut self, event: T) {
        if self.events.len() == self.capacity {
            self.events.pop_front();
            self.dropped += 1;
        }
        self.events.push_back((self.next_sequence, event));
        self.next_sequence += 1;
    }

    /// A cursor positioned at the end, for a reader that only wants what happens next.
    #[must_use]
    pub const fn cursor_at_end(&self) -> Cursor {
        Cursor(self.next_sequence)
    }

    /// Everything after `cursor`, advancing it.
    ///
    /// When the log has dropped events the cursor had not reached, the reader is skipped forward to
    /// the oldest event still held — a gap, but a gap the reader can detect by comparing the cursor
    /// it passed with the one it gets back.
    pub fn drain_from(&self, cursor: &mut Cursor) -> Vec<&T> {
        let mut out = Vec::new();
        for (sequence, event) in &self.events {
            if *sequence >= cursor.0 {
                out.push(event);
            }
        }
        cursor.0 = self.next_sequence;
        out
    }

    /// How many events were dropped because the log was full.
    ///
    /// Reported rather than silent, for the same reason a truncated undo history is reported: a
    /// subscriber that missed a notification and cannot tell is worse off than one that knows.
    #[must_use]
    pub const fn dropped(&self) -> u64 {
        self.dropped
    }

    /// How many events the log currently holds.
    pub fn len(&self) -> usize {
        self.events.len()
    }

    /// Whether the log is empty.
    pub fn is_empty(&self) -> bool {
        self.events.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_idle_observer_never_rebuilds() {
        let state = Versioned::new(41_u32);
        let watch = Watch::new();

        assert!(
            watch.rebuild_if_changed(state.revision(), || {}),
            "the first look must rebuild"
        );
        for _ in 0..1000 {
            assert!(
                !watch.rebuild_if_changed(state.revision(), || {}),
                "nothing changed, so nothing may run"
            );
        }
        assert_eq!(
            watch.rebuilds(),
            1,
            "an idle observer costs one rebuild, not a thousand"
        );
    }

    #[test]
    fn a_change_is_seen_exactly_once() {
        let mut state = Versioned::new(0_u32);
        let watch = Watch::new();
        watch.accept(state.revision());

        state.set(1);
        assert!(watch.changed(state.revision()));
        watch.accept(state.revision());
        assert!(!watch.changed(state.revision()));
    }

    #[test]
    fn update_moves_the_revision() {
        let mut state = Versioned::new(vec![1, 2, 3]);
        let before = state.revision();
        state.update(|values| values.push(4));
        assert!(state.revision() > before);
        assert_eq!(state.get().len(), 4);
    }

    #[test]
    fn a_late_reader_sees_everything_still_held() {
        let mut log = EventLog::new(4);
        log.push("a");
        log.push("b");

        let mut cursor = Cursor::default();
        assert_eq!(log.drain_from(&mut cursor), vec![&"a", &"b"]);
        assert!(log.drain_from(&mut cursor).is_empty());

        log.push("c");
        assert_eq!(log.drain_from(&mut cursor), vec![&"c"]);
    }

    #[test]
    fn overflow_is_reported_rather_than_silent() {
        let mut log = EventLog::new(2);
        for event in ["a", "b", "c", "d"] {
            log.push(event);
        }
        assert_eq!(log.dropped(), 2);
        let mut cursor = Cursor::default();
        assert_eq!(log.drain_from(&mut cursor), vec![&"c", &"d"]);
    }
}
