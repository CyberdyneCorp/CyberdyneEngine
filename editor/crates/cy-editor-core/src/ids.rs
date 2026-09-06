//! Stable identity. Task 2.2 ("no pointers as identity") and task 3.4.
//!
//! `editor-documents-and-transactions` requires that a transaction operation "SHALL address its
//! target by **stable identity** — document, persistent entity or node identifier, component type
//! identifier, and field identifier — and SHALL NOT use pointers, array indices, or byte offsets",
//! and that consequently "a history SHALL remain valid across an asset reload, a document close and
//! reopen within a session, a rename, and a schema migration".
//!
//! Every identifier here is a value type with no interior state, so a history entry is a plain
//! record that outlives everything it names. That is the whole design: the reason undo survives a
//! reload is not that anything is kept alive, it is that nothing was ever addressed by its address.
//!
//! --- WHY A RENAME DOES NOT MOVE AN IDENTIFIER -----------------------------------------------------
//!
//! [`FieldId`] and [`TypeId`] are opaque numbers a schema assigns once, and the *name* is a separate
//! attribute of the schema entry. Renaming a field rewrites the name and leaves the identifier
//! alone, so every history entry that targeted it still applies — which is exactly the specified
//! scenario, and it is a property of where the name is stored rather than a migration step.
//!
//! Deriving an identifier from the name by hashing would have been less code and would have made
//! that scenario impossible: the hash of the new name is a different number, and every history entry
//! addressing the field would silently target nothing.
//!
//! --- WHY HANDLES CARRY A GENERATION ---------------------------------------------------------------
//!
//! `editor-rust-application` requires engine handles to be "generation-checked value types, never
//! raw pointers". [`Handle`] and [`HandleMap`] are that: a slot index paired with the generation it
//! was issued at, so a handle to a freed slot is *detected* rather than resolving to whatever now
//! occupies it. The failure mode this removes is the one nobody notices — a stale handle that still
//! points at a live object of the same type.

use std::fmt;
use std::marker::PhantomData;

/// A document's identity: stable for the life of the project, not for the life of a session.
///
/// Derived from the document's primary backing asset path, so the same document is the same
/// identifier after a restart. Crash recovery needs exactly that — a journal written before the
/// crash has to find the document it belongs to afterwards.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct DocumentId(u128);

impl DocumentId {
    /// The identity of the document whose primary backing asset is `path`.
    ///
    /// A document may be backed by hundreds of files — a world by its metadata, its authoring
    /// chunks, its layers and its scene instances — and exactly one of them is primary. That is the
    /// one that names the document, so that "a document is not a file" does not become "a document
    /// has no name".
    #[must_use]
    pub fn of_asset(path: &str) -> Self {
        Self(fnv1a_128(path.as_bytes()))
    }

    /// The identity as an integer, for a journal file name or a stable sort.
    #[must_use]
    pub const fn as_u128(self) -> u128 {
        self.0
    }

    /// Rebuild an identity from its integer form, as recovery does when it reads a journal.
    #[must_use]
    pub const fn from_u128(raw: u128) -> Self {
        Self(raw)
    }
}

impl fmt::Debug for DocumentId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "DocumentId({:032x})", self.0)
    }
}

impl fmt::Display for DocumentId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:032x}", self.0)
    }
}

/// A persistent node identity inside a document: an entity, a graph node, a terrain tile.
///
/// Allocated from the document's own ordinal counter and mixed with the document's identity, which
/// gives three properties at once: it is unique across a project without a central allocator, it is
/// reproducible — the same authoring steps in a test produce the same identifiers, so a golden
/// journal is comparable — and it does not collide when two documents are merged.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct NodeId(u128);

impl NodeId {
    /// The `ordinal`-th node ever created in `document`.
    ///
    /// Ordinals are never reused, so a deleted node's identifier stays retired and an undo that
    /// recreates it can put back the same one — which is what makes "undo is exact" true for
    /// anything that referenced it.
    #[must_use]
    pub fn in_document(document: DocumentId, ordinal: u64) -> Self {
        let mut bytes = [0_u8; 24];
        bytes[..16].copy_from_slice(&document.as_u128().to_le_bytes());
        bytes[16..].copy_from_slice(&ordinal.to_le_bytes());
        Self(fnv1a_128(&bytes))
    }

    /// The identity as an integer, for serialisation.
    #[must_use]
    pub const fn as_u128(self) -> u128 {
        self.0
    }

    /// Rebuild a node identity from its integer form.
    #[must_use]
    pub const fn from_u128(raw: u128) -> Self {
        Self(raw)
    }
}

impl fmt::Debug for NodeId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "NodeId({:032x})", self.0)
    }
}

impl fmt::Display for NodeId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:032x}", self.0)
    }
}

/// A component type's identity, assigned once by a schema and never derived from its name.
///
/// See the module note: a name-derived identifier makes the specification's rename scenario
/// impossible, because the new name hashes to a different number and every history entry that
/// addressed the type silently targets nothing.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct TypeId(u64);

impl TypeId {
    /// A type identity with the given raw value. Schemas allocate these; nothing else should.
    #[must_use]
    pub const fn from_raw(raw: u64) -> Self {
        Self(raw)
    }

    /// The raw value, for serialisation and for the SDK's component-type lookup.
    #[must_use]
    pub const fn as_u64(self) -> u64 {
        self.0
    }
}

/// A field's identity within its type, assigned once by a schema and stable across a rename.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct FieldId(u64);

impl FieldId {
    /// A field identity with the given raw value. Schemas allocate these; nothing else should.
    #[must_use]
    pub const fn from_raw(raw: u64) -> Self {
        Self(raw)
    }

    /// The raw value, for serialisation.
    #[must_use]
    pub const fn as_u64(self) -> u64 {
        self.0
    }
}

/// An entity as the *engine* identifies it, opaque to the editor.
///
/// The C ABI's `CyEntity` packs an index and a generation into 64 bits, and `cy_abi.h` says so in
/// prose that no generator can recover. So the editor does not decode it: it is a number the engine
/// issued, carried back to the engine unchanged. The authoring side's identity is [`NodeId`], which
/// is the one a transaction addresses; the mapping between them belongs to a runtime session and
/// lives no longer than one.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct RuntimeEntity(u64);

impl RuntimeEntity {
    /// The null entity, which the ABI uses to mean "no entity".
    pub const NONE: Self = Self(0);

    /// Wrap the identifier the engine issued.
    #[must_use]
    pub const fn from_raw(raw: u64) -> Self {
        Self(raw)
    }

    /// The identifier to hand back to the engine.
    #[must_use]
    pub const fn as_u64(self) -> u64 {
        self.0
    }

    /// Whether this is the ABI's null entity.
    #[must_use]
    pub const fn is_none(self) -> bool {
        self.0 == 0
    }
}

/// A generation-checked handle into a [`HandleMap`].
///
/// Two `Handle<T>`s compare equal only when they name the same slot at the same generation, so a
/// handle held across a removal does not resolve — it reports absence.
pub struct Handle<T> {
    index: u32,
    generation: u32,
    marker: PhantomData<fn() -> T>,
}

// Derived implementations would demand `T: Clone` and the rest, which is wrong: a handle is a pair
// of integers whose behaviour does not depend on what it points at. `PhantomData<fn() -> T>` also
// keeps `Handle<T>` `Send` and `Sync` regardless of `T`, which matters because a handle is exactly
// the thing that crosses a thread boundary when the pointed-at value must not.
impl<T> Clone for Handle<T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<T> Copy for Handle<T> {}

impl<T> PartialEq for Handle<T> {
    fn eq(&self, other: &Self) -> bool {
        self.index == other.index && self.generation == other.generation
    }
}

impl<T> Eq for Handle<T> {}

impl<T> std::hash::Hash for Handle<T> {
    fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
        self.index.hash(state);
        self.generation.hash(state);
    }
}

impl<T> fmt::Debug for Handle<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Handle({}#{})", self.index, self.generation)
    }
}

/// A generational arena: values addressed by [`Handle`], where a stale handle is detected.
///
/// Removal bumps the slot's generation, so every handle issued before it stops resolving. Slots are
/// reused; generations are not.
pub struct HandleMap<T> {
    slots: Vec<Slot<T>>,
    free: Vec<u32>,
}

struct Slot<T> {
    generation: u32,
    value: Option<T>,
}

impl<T> Default for HandleMap<T> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T> HandleMap<T> {
    /// An empty map.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            slots: Vec::new(),
            free: Vec::new(),
        }
    }

    /// Insert a value and return the handle that addresses it.
    pub fn insert(&mut self, value: T) -> Handle<T> {
        if let Some(index) = self.free.pop() {
            let slot = &mut self.slots[index as usize];
            slot.value = Some(value);
            return Handle {
                index,
                generation: slot.generation,
                marker: PhantomData,
            };
        }
        let index =
            u32::try_from(self.slots.len()).expect("a HandleMap holds fewer than 2^32 slots");
        self.slots.push(Slot {
            generation: 1,
            value: Some(value),
        });
        Handle {
            index,
            generation: 1,
            marker: PhantomData,
        }
    }

    /// The value a handle addresses, or `None` when the handle is stale.
    pub fn get(&self, handle: Handle<T>) -> Option<&T> {
        let slot = self.slots.get(handle.index as usize)?;
        if slot.generation != handle.generation {
            return None;
        }
        slot.value.as_ref()
    }

    /// The value a handle addresses, mutably, or `None` when the handle is stale.
    pub fn get_mut(&mut self, handle: Handle<T>) -> Option<&mut T> {
        let slot = self.slots.get_mut(handle.index as usize)?;
        if slot.generation != handle.generation {
            return None;
        }
        slot.value.as_mut()
    }

    /// Remove a value, retiring every handle that addresses it. `None` when already stale.
    pub fn remove(&mut self, handle: Handle<T>) -> Option<T> {
        let slot = self.slots.get_mut(handle.index as usize)?;
        if slot.generation != handle.generation {
            return None;
        }
        let value = slot.value.take()?;
        // Wrapping is the honest arithmetic here: a slot reused four billion times has to keep
        // working, and the alternative — saturating — would silently stop detecting stale handles
        // for that slot forever. Wrapping can collide only with a handle that has been held across
        // 2^32 removals of the same slot, which no editor session reaches.
        slot.generation = slot.generation.wrapping_add(1);
        self.free.push(handle.index);
        Some(value)
    }

    /// Whether a handle still resolves.
    pub fn contains(&self, handle: Handle<T>) -> bool {
        self.get(handle).is_some()
    }

    /// How many values the map holds.
    pub fn len(&self) -> usize {
        self.slots
            .iter()
            .filter(|slot| slot.value.is_some())
            .count()
    }

    /// Whether the map holds nothing.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Every live value with the handle that addresses it, in slot order.
    pub fn iter(&self) -> impl Iterator<Item = (Handle<T>, &T)> {
        self.slots.iter().enumerate().filter_map(|(index, slot)| {
            let value = slot.value.as_ref()?;
            let index = u32::try_from(index).ok()?;
            Some((
                Handle {
                    index,
                    generation: slot.generation,
                    marker: PhantomData,
                },
                value,
            ))
        })
    }
}

/// FNV-1a over 128 bits.
///
/// Chosen for one property and no other: it is four lines, so the identifier a checkout produces
/// does not depend on a dependency's version. It is not a security primitive and nothing here
/// treats it as one — `editor-documents-and-transactions` needs identifiers that are stable and
/// collision-free in practice, and a 128-bit FNV over asset paths is both.
fn fnv1a_128(bytes: &[u8]) -> u128 {
    const OFFSET: u128 = 0x6c62_272e_07bb_0142_62b8_2175_6295_c58d;
    const PRIME: u128 = 0x0000_0000_0100_0000_0000_0000_0000_013b;
    let mut hash = OFFSET;
    for byte in bytes {
        hash ^= u128::from(*byte);
        hash = hash.wrapping_mul(PRIME);
    }
    hash
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_document_identity_is_the_same_after_a_restart() {
        assert_eq!(
            DocumentId::of_asset("worlds/city.cyworld"),
            DocumentId::of_asset("worlds/city.cyworld")
        );
        assert_ne!(
            DocumentId::of_asset("worlds/city.cyworld"),
            DocumentId::of_asset("worlds/forest.cyworld")
        );
    }

    #[test]
    fn node_identities_are_unique_across_documents() {
        let city = DocumentId::of_asset("worlds/city.cyworld");
        let forest = DocumentId::of_asset("worlds/forest.cyworld");
        assert_ne!(NodeId::in_document(city, 1), NodeId::in_document(forest, 1));
        assert_ne!(NodeId::in_document(city, 1), NodeId::in_document(city, 2));
        assert_eq!(NodeId::in_document(city, 7), NodeId::in_document(city, 7));
    }

    #[test]
    fn a_stale_handle_is_detected_rather_than_resolving() {
        let mut map: HandleMap<&str> = HandleMap::new();
        let first = map.insert("document");
        assert_eq!(map.get(first), Some(&"document"));

        map.remove(first);
        assert_eq!(map.get(first), None, "a removed handle must not resolve");

        // The slot is reused. Without a generation, `first` would now resolve to the new value —
        // the failure this type exists to prevent, and the one nobody notices.
        let second = map.insert("another document");
        assert_eq!(map.get(second), Some(&"another document"));
        assert_eq!(
            map.get(first),
            None,
            "a reused slot must not revive an old handle"
        );
        assert_ne!(first, second);
    }

    #[test]
    fn iteration_reports_only_live_slots() {
        let mut map: HandleMap<u32> = HandleMap::new();
        let a = map.insert(1);
        let _b = map.insert(2);
        map.remove(a);
        let live: Vec<u32> = map.iter().map(|(_, value)| *value).collect();
        assert_eq!(live, vec![2]);
        assert_eq!(map.len(), 1);
    }
}
