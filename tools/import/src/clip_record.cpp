// The cooked clip payload's writer. See cy/import/clip_record.h for why it is not in fbx_clip.cpp.

#include <cy/import/clip_record.h>

#include <cy/import/fbx_clip.h>

#ifdef CY_IMPORT_ANIMATION
// The definition, which `cy/import/clip_record.h` only forward-declares — see the note there for
// why a public header may not include it.
#    include <cy/animation/clip.h>
#    include <cy/animation/cooked.h>
#    include <cy/core/memory/system_allocator.h>

namespace cy::import {

Status write_cooked_clip(const animation::Clip& clip, Span<const std::string_view> joint_names,
                         Array<u8>& out) noexcept {
    // The runtime's writer, over the names as the runtime holds them. One writer for the record,
    // beside the one reader the runtime loads with.
    Array<Name> names(system_allocator(MemoryDomain::Animation));
    for (const std::string_view joint : joint_names) {
        if (Status pushed = names.push_back(Name::intern(joint)); !pushed) {
            return pushed;
        }
    }
    return animation::encode_clip(clip, names.span(), out);
}

}  // namespace cy::import

#endif  // CY_IMPORT_ANIMATION
