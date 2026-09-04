#pragma once
// The two forms of an authoring document, from one traversal. Tasks 3.2.2 and 3.2.3.
//
// design.md §6: "One traversal, two writers. The text form is the authoring artefact — diffable,
// mergeable, and the thing a designer's tool edits. The binary form is a cooked derivation of it,
// never authored directly, and never the source of truth for anything under version control."
//
// Both forms carry the same four things — entities, instances, parameters, and the variant base —
// in the same order, addressed by the same identifiers, so the round trip is exact. The text form
// spells them as indented lines (`<cy/core/serialize/text.h>` owns the lexical layer); the binary
// form spells them as tagged chunks (`<cy/core/serialize/tagged.h>`).
//
// **The binary form here is the tagged one, not the cooked one.** They answer different questions
// and the specification keeps them apart by name. A document's binary form is what a save, a replay
// or an editor round trip uses: it evolves, it skips unknown fields, and it preserves data a build
// does not understand. The *cooked* form (cook.h) is archetype blocks with no tags and no evolution
// at all, and no authoring structure survives into it.
//
// WHAT THE ROUND TRIP GUARANTEES. text → binary → text reproduces the file byte for byte, and
// binary → text → binary reproduces the stream byte for byte, for any document either can express.
// That is the milestone gate ("a scene round-trips text → binary → text with no semantic change")
// stated as something a test can assert on the bytes rather than on a notion of sameness.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/serialize/text.h>
#include <cy/scene/serialization/document.h>

#include <string_view>

namespace cy::scene::serialization {

/// The document text form's own version, distinct from any type's schema version.
inline constexpr u32 kDocumentTextVersion = 1;

/// Write the canonical text form. Deterministic: the same document always produces the same bytes.
[[nodiscard]] Status write_text(const Document& document, Array<char>& out,
                                serialize::TextOptions options = {}) noexcept;

/// Read one. `out` is replaced.
[[nodiscard]] Status read_text(std::string_view text, Document& out) noexcept;

/// Write the tagged binary form.
[[nodiscard]] Status write_binary(const Document& document, Array<u8>& out) noexcept;

/// Read one. `out` is replaced.
[[nodiscard]] Status read_binary(Span<const u8> bytes, Document& out) noexcept;

}  // namespace cy::scene::serialization
