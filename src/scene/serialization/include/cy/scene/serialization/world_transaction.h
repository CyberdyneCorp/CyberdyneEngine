#pragma once
// The editor's operation stream, applied to the engine's world. M8.a task 1.1.
//
// ================================================================================================
// WHY THE ENGINE DECODES THE EDITOR'S TRANSACTIONS AT ALL
// ================================================================================================
//
// `editor-documents-and-transactions` requires that *"the journal SHALL be the same operation
// stream used by diff, live editing, and any future collaboration, rather than a separate
// representation"*. So there is exactly one encoding of an edit, `cy_editor_documents` writes it,
// and anything that wants to follow an editing session reads THAT — not a second wire invented for
// the purpose.
//
// M7 got as far as decoding one shape of one operation. `samples/05b-editor-window/runtime/
// session.h` states what it could not do and why: *"THE ONE THING IT CANNOT KNOW is which field
// that is. The identifiers in a transaction are the DOCUMENT's … not the numbers in
// `types.cytypes`, and not anything this process can derive."* Its answer was to call a changed
// `Vec3` a translation whenever the editor's last stated gizmo mode was Translate — exact for a
// move, and it says itself that it refuses to guess at a scale.
//
// **That whole paragraph goes away here, and the reason is `worldfile.h`.** A `.cyworld` is written
// out of the document's own `DocumentSchema`, so the type and field numbers in its `type` section
// ARE the numbers a transaction addresses. An engine that read the world read the schema, so a
// `SetField` naming component 3 field 4 names exactly the field the file's line `field 4 vec3
// "translation"` declared. Nothing is inferred, and a scale is applied as a scale.
//
// ================================================================================================
// WHAT AN UNKNOWN OPERATION DOES
// ================================================================================================
//
// It is DECODED AND COUNTED, never skipped by a guessed length. Every variant below is
// `cy_editor_documents::operation`'s, restated, including the ones this module has nothing to do
// with — a prefab instantiation, a domain delta — because a reader that skipped one by assuming its
// size would misread the next operation as something it is not. That failure reached M7's artefact
// and reported itself as "a transaction ends before its actor", which reads as a protocol version
// mismatch and is not one.
//
// A tag from a newer editor is where the stream genuinely ends: the length is unknowable, so the
// transaction is refused as a whole and the count of what had already been applied is returned with
// the error rather than thrown away.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/scene/serialization/worldfile.h>

namespace cy::scene::serialization {

/// What applying a transaction did.
struct TransactionReport {
    /// The transaction's own identity, and the document it names.
    u64 transaction = 0;
    EditorId document;
    /// How many operations the transaction carried.
    u32 operations = 0;
    /// How many of them changed this world.
    u32 applied = 0;
    /// How many named a node this world does not hold. Not an error: an editor may hold documents
    /// this runtime never loaded.
    u32 unknown_nodes = 0;
    /// How many were understood and had nothing to do here — a prefab instantiation, a domain
    /// delta, an override on a node with no prefab.
    u32 ignored = 0;
    /// Nodes created, deleted and restored, for a report that can say what a frame changed.
    u32 created = 0;
    u32 deleted = 0;
    u32 restored = 0;
};

/// Apply one encoded `cy_editor_documents::Transaction` to `world`.
///
/// The bytes are exactly what `Message::Apply` carries. Reports `InvalidArgument` for a truncated
/// or unreadable stream, in which case `out` still says how much had been applied before the stream
/// stopped making sense — a caller that has to tell a user why the viewport is half updated needs
/// that number.
[[nodiscard]] Status apply_transaction(World& world, Span<const u8> bytes,
                                       TransactionReport& out) noexcept;

/// Read only a transaction's header: its identity and the document it belongs to.
///
/// What the runtime calls on the first transaction, to check that the world it loaded is the world
/// the editor has open. See `verify_document_identity`.
[[nodiscard]] Expected<TransactionReport, Error> read_transaction_header(
    Span<const u8> bytes) noexcept;

}  // namespace cy::scene::serialization
