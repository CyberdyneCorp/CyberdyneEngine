## ADDED Requirements

### Requirement: An authored node carries a name
A node in an authoring document SHALL carry an **author-given name**, distinct from its identity and
distinct from every other property it happens to have.

Today it carries none. Identity is a `NodeId` and nothing else, and the outliner therefore labels a
row with the node's **kind** — the name of the first component it carries — so two authored objects
of the same kind are two rows reading the same word. The one authored world committed to this
repository works around it by putting `"Pillar"`, `"Crate"` and `"Marker"` in the third field of a
`node` line, **which is the node's layer**, so three objects sit in three one-node layers because
there is nowhere else to put a name.

The name SHALL be independent of identity: renaming a node SHALL NOT change what an operation, a
diff, a reference or a transaction addresses, because operations address stable identities and a
name is not one.

The name SHALL be independent of grouping: a node's name SHALL NOT determine or be determined by its
layer, and two nodes SHALL be able to share a layer and differ by name.

The name SHALL survive a round trip through the document's serialized form, and the outliner SHALL
label a row with it, falling back to the node's kind only where a node has no name.

#### Scenario: Two objects of one kind are distinguishable
- **WHEN** an author creates two nodes carrying the same first component and names them differently
- **THEN** the hierarchy SHALL show two rows bearing the two names, and a check SHALL fail if two
  distinct nodes are indistinguishable in the outliner

#### Scenario: A name is not an identity
- **WHEN** a node is renamed
- **THEN** every operation, reference and journal entry addressing it SHALL still address it, and
  undo of the rename SHALL restore the previous name rather than recreating the node

#### Scenario: A name is not a layer
- **WHEN** a world is written and read back
- **THEN** each node's name and its layer SHALL round-trip as separate values, and a document that
  encodes a name in the layer field SHALL be reported rather than accepted
