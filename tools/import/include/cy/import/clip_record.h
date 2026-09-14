#ifndef CY_IMPORT_CLIP_RECORD_H
#define CY_IMPORT_CLIP_RECORD_H
// The cooked clip payload's WRITER, shared by every importer that reaches step 8. M11.b task 6.1.
//
// `asset-import-pipeline` — "Model import", step 8: "Import animation clips, resample or preserve
// keys per configuration, and compress with error bounds."
//
// ================================================================================================
// WHY THIS FILE EXISTS, AND WHAT WOULD HAVE HAPPENED WITHOUT IT
// ================================================================================================
//
// M8.d landed step 8 for FBX and wrote the record inside `src/fbx_clip.cpp`, which was right while
// there was one animated format. M11.b adds a second — glTF carries skins and animations and this
// tree refused both by name until now — and the alternative to this file is a second writer beside
// the first. That is the failure `model.h` was created to prevent, restated for clips: the same
// animation exported as FBX and as glTF would cook to two different records, every downstream
// `AssetId` would rebind, and the day one writer gained a field the other would not.
//
// So the format-specific half of step 8 is: read the source's curves, author them onto a
// `cy::animation::Clip`, and call this. The BYTES are decided here and nowhere else, and
// `read_cooked_clip` in `cy/import/fbx_clip.h` is the one reader of them.
//
// BEHIND `CY_IMPORT_ANIMATION`, for the reason `fbx_clip.h` states at length: a clip's
// error-bounded codec IS `cy::animation::Clip`, so with `-D CY_ANIMATION=OFF` there is nothing to
// compress against and step 8 reports itself as not reached rather than pretending it was. The
// READER is not behind the guard — a tool that inspects a package must work in either build — and
// that asymmetry is the same one M8.d chose.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

#include <string_view>

// `cy::animation` is a PRIVATE dependency of `cy_import` (tools/import/CMakeLists.txt) while
// `CY_IMPORT_ANIMATION` is a public definition, so a consumer of this header sees the guard and not
// the include path. The class is therefore forward-declared rather than included: a reference
// parameter needs a declaration, and only `src/clip_record.cpp` — which links the runtime — needs
// the definition. Including it here would break every target that uses this header without the
// animation runtime on its include path, which is most of them.
namespace cy::animation {
class Clip;
}  // namespace cy::animation

namespace cy::import {

#ifdef CY_IMPORT_ANIMATION

/// Write a compressed clip as the cooked payload, with the joint names its tracks are indexed
/// against.
///
/// THE JOINT NAMES RIDE WITH IT and the reason is in `fbx_clip.h`: a track addresses a joint by
/// index, `AnimationRig::bind` checks only counts, and a clip bound to the wrong rig drives the
/// wrong bones in silence. The names are what a loader can check or rebind by.
///
/// A pure function of the clip and the names, so two cooks of one source produce identical bytes —
/// which is `asset-import-pipeline`'s determinism requirement and what the content-addressed cook
/// cache rests on. `clip` must have been compressed: the payload is the STORED keys, and an
/// uncompressed clip has none.
[[nodiscard]] Status write_cooked_clip(const animation::Clip& clip,
                                       Span<const std::string_view> joint_names,
                                       Array<u8>& out) noexcept;

#endif  // CY_IMPORT_ANIMATION

}  // namespace cy::import

#endif  // CY_IMPORT_CLIP_RECORD_H
