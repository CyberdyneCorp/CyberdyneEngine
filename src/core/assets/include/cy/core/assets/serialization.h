#ifndef CY_CORE_ASSETS_SERIALIZATION_H
#define CY_CORE_ASSETS_SERIALIZATION_H
// The two serialization forms for reflected data. Task 3.3.5.
//
// `core-assets-and-io` — "Serialization formats": the engine supports two forms for the same
// reflected data. **Binary** is the runtime format — compact, versioned, endian-defined (little),
// suitable for memory mapping. **Text** is human-readable and diff-friendly, used for scenes,
// prefabs and project configuration in source control. Both round-trip: text → binary → text
// preserves values and ordering.
//
// THE BINARY FORM IS `cy::reflect`'s RECORD, NOT A SECOND ONE. reflect/serialize.h already writes a
// record that addresses fields by FieldId, skips Transient fields and preserves an unknown record
// verbatim — every property the identity model exists to provide. Writing a second binary encoding
// here would be a second thing to keep in step with the manifest. What this file adds is the
// ENVELOPE: a magic, a format version and an explicit endianness statement, so a file is
// self-describing and a mismatched one is refused instead of misparsed.
//
// THE TEXT FORM IS ORDERED BY FIELD IDENTIFIER, ASCENDING. That is what makes a diff meaningful:
// two writers of the same object emit the same lines in the same order whatever order the fields
// happen to be declared or laid out in, so changing one value changes exactly one line. Ordering by
// declaration would make inserting a field reorder the file; ordering by name would make renaming
// one do it. The identifier is the only thing that does not move.
//
// WHAT IS NOT HERE. Scenes, prefabs, entity graphs, overrides and patches: `serialization-and-
// prefabs` owns those at a later milestone, and this file deliberately serializes ONE reflected
// object rather than a document of them. Nested reflected structs, strings and containers are not
// serializable at M1 either — `reflect::FieldKind` covers scalars and enumerations, and a field
// outside that is reported by name rather than silently dropped.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/serialize.h>
#include <cy/core/reflect/type_info.h>

#include <string_view>

namespace cy::assets {

/// The binary envelope's magic: "CYBIN" and three bytes that make a text file fail the check.
inline constexpr u8 kBinaryMagic[8] = {'C', 'Y', 'B', 'I', 'N', 0x00, 0x0D, 0x0A};

/// The envelope's own version. The RECORD inside it is versioned by the identity manifest instead —
/// that is the whole point of addressing fields by identifier — so this number moves only when the
/// envelope changes.
inline constexpr u32 kBinaryFormatVersion = 1;

/// The envelope is 16 bytes: magic, version, and a byte-order mark that states the format's
/// endianness rather than the host's.
inline constexpr usize kBinaryEnvelopeBytes = 16;

/// Write one reflected object as a binary document: the envelope, then `reflect`'s record.
[[nodiscard]] Status write_binary(const reflect::TypeInfo& type, const void* object,
                                  reflect::ByteBuffer& out) noexcept;

/// Read a binary document into `object`.
///
/// Fails with InvalidArgument on a bad magic, and with Unsupported on a newer envelope version —
/// `core-assets-and-io`'s "loading SHALL fail with a clear diagnostic rather than misparsing".
[[nodiscard]] Status read_binary(const reflect::TypeInfo& type, const u8* data, usize size,
                                 void* object) noexcept;

/// The TypeId a binary document carries, without reading its payload. What a host uses to pick the
/// type before it has one, and what lets an unknown record be carried forward.
[[nodiscard]] Expected<reflect::TypeId, Error> peek_binary(const u8* data, usize size) noexcept;

/// Write one reflected object as text.
///
/// The form, which is deliberately boring:
///
///     type "cy::demo::Health" 1
///       1 maximum = 100
///       2 current = 87.5
///
/// The header line names the type and its TypeId. Each field line is `<field id> <name> = <value>`,
/// ordered by field id ascending. The NAME IS A COMMENT in effect — the reader keys on the id and
/// ignores the name, so renaming a field is a manifest edit and a one-line diff, and a hand-edited
/// file whose names have gone stale still loads.
///
/// `out` is replaced, and is NUL-terminated so it can be handed straight to a text writer.
[[nodiscard]] Status write_text(const reflect::TypeInfo& type, const void* object,
                                Array<char>& out) noexcept;

/// Read a text document into `object`.
///
/// A field the text carries that the type no longer has is skipped — that is a removed field, and
/// the manifest's tombstone is what makes skipping it safe. A field the type has that the text does
/// not carry keeps whatever the caller initialised it to, which is how a field added later gets its
/// default. A malformed line is an error rather than a silent skip.
[[nodiscard]] Status read_text(const reflect::TypeInfo& type, std::string_view text,
                               void* object) noexcept;

/// The type a text document declares, without applying it.
[[nodiscard]] Expected<reflect::TypeId, Error> peek_text(std::string_view text) noexcept;

}  // namespace cy::assets

#endif  // CY_CORE_ASSETS_SERIALIZATION_H
