#pragma once
// The GPU-visible image of a field, and the sampler that reads it the way a shader would. Task 1.1,
// the half of it that says CPU and GPU access the SAME field.
//
// `environment-fields` — "CPU and GPU access": "Fields SHALL be samplable from both CPU code and
// GPU shaders, through interfaces that produce the same value for the same position and resolution"
// and "GPU access SHALL be through bindless resources reachable from the GPU scene, so a shader can
// sample a field without per-draw binding."
//
// ================================================================================================
// WHAT THIS FILE IS, AND WHAT IT HONESTLY IS NOT
// ================================================================================================
//
// IT IS the field's GPU memory image — one flat buffer of 32-bit words holding a header, a sorted
// tile table and the tile payloads, in exactly the bytes a shader would be handed — together with
// `sample_field_image()`, which resolves a position against that buffer AND NOTHING ELSE. It does
// not consult the store, the registry or the declaration: it reads the header for the encoding, the
// range, the interpolation and the layer rule, exactly as a shader must, and it decodes the stored
// bytes with its own arithmetic rather than calling `decode_value()`.
//
// **The independence is the point.** A sampler that called the store's own decoder would agree with
// it by construction and would prove nothing. `test_gpu.cpp` compares two implementations that
// share no code, which is what makes "the same value for the same position" a measurement rather
// than a tautology — and what makes the mutation that breaks one of them go red.
//
// IT IS NOT a shader, a descriptor, or a binding. No `.slang` module accompanies it and none is
// claimed: the buffer's layout is fixed and documented here so that the renderer-facing row that
// binds it writes `import cy.field` against a layout that already exists and is already tested on
// the CPU, and an agreement measured against a real device is that row's to make and this module's
// to have made possible. `README.md` records that as an open gap rather than as a footnote.
//
// ================================================================================================
// WHY THE SAMPLER TAKES A POSITION RELATIVE TO THE IMAGE
// ================================================================================================
//
// A world position is f64 (`world::WorldVec3d`) and a shader's is f32, and the reason is the one
// `world/coordinates.h` gives: at a thousand kilometres an f32 has 64 mm of spacing. So an image
// carries an ORIGIN — the corner of its lowest tile — and the sampler takes metres relative to it,
// which for any image a frame binds is a few hundred metres and exact in f32. `image_local()` is
// the conversion, done once on the CPU in f64, which is the same arrangement the engine already
// uses for `world::to_simulation_local()`.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/environment/store.h>

namespace cy::environment {

/// `CYFD`. A shader reading a buffer bound to the wrong slot sees this and can say so, rather than
/// decoding a shadow page as moisture.
inline constexpr u32 kFieldImageMagic = 0x4359'4644U;

/// Bumped whenever the layout below changes. A shader compiled against an older layout must fail
/// loudly, and the only way it can is if the number it checks is in the buffer.
inline constexpr u32 kFieldImageVersion = 1;

/// Words before the tile table.
inline constexpr u32 kFieldImageHeaderWords = 16;
/// Words per tile table entry: layer, tile x, tile z, payload offset.
inline constexpr u32 kFieldImageEntryWords = 4;

/// A field's tiles at one residency level, as a GPU buffer.
///
/// The layout, word by word, because a shader that reads it has no header file:
///
///   0   magic `CYFD`                       8   range_max (f32 bits)
///   1   layout version                     9   default component 0 (f32 bits)
///   2   components | encoding<<8 |         10  default component 1
///       interpolation<<16 | rule<<24       11  default component 2
///   3   vertical cells                     12  default component 3
///   4   cell metres (f32 bits)             13  tile table entry count
///   5   vertical metres (f32 bits)         14  origin tile x (i32 bits)
///   6   vertical origin (f32 bits)         15  origin tile z (i32 bits)
///   7   range_min (f32 bits)
///
/// then `entry count` entries of four words — layer, tile x and tile z RELATIVE TO THE ORIGIN TILE,
/// and the payload's offset in words — sorted by (layer, z, x) so a lookup is a binary search, then
/// the payloads, each `FieldStore::tile_bytes()` long and padded to a word.
struct FieldGpuImage {
    Array<u32> words;
    FieldId field;
    FieldResidency level = FieldResidency::Local;
    /// The absolute metres of the image's origin corner, for `image_local()`. Not in the buffer:
    /// a shader is given a position already made relative, and putting an f64 in a GPU buffer would
    /// invite a shader to do the subtraction in f32, which is the thing this arrangement avoids.
    f64 origin_x = 0.0;
    f64 origin_z = 0.0;
    u32 tiles = 0;

    explicit FieldGpuImage(Allocator& allocator) noexcept : words(allocator) {}
};

/// Build the image for one field at one level, from the tiles that are resident.
///
/// Deterministic: the tile table is in address order, so two runs that made the same tiles resident
/// in a different order produce byte-identical buffers. Streaming order is not content.
[[nodiscard]] Expected<FieldGpuImage, Error> build_field_image(const FieldStore& store,
                                                               FieldId field,
                                                               FieldResidency level) noexcept;

/// The position a shader would be handed, in metres relative to the image's origin. The f64
/// subtraction happens here, once, on the CPU.
struct FieldImageLocal {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 z = 0.0F;
};

[[nodiscard]] FieldImageLocal image_local(const FieldGpuImage& image,
                                          const world::WorldVec3d& at) noexcept;

/// Sample the image, reading nothing but the image. This is the shader's algorithm, executed on the
/// CPU so that it can be compared against the store's.
///
/// `y` is ABSOLUTE metres, not relative: a field's vertical extent is declared in absolute metres
/// (`FieldDeclaration::vertical_origin_metres`) and worlds are thin enough vertically that f32 is
/// exact there — it is the horizontal extent that needs an origin.
[[nodiscard]] FieldValue sample_field_image(Span<const u32> words, f32 x, f32 y, f32 z) noexcept;

/// Whether a buffer is a field image of a layout this build understands. What a shader's first two
/// word reads are, and what a test asserts before trusting anything else it read.
[[nodiscard]] bool is_field_image(Span<const u32> words) noexcept;

}  // namespace cy::environment
