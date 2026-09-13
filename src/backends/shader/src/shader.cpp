// The vocabulary's tables. Task 3.1.
//
// Every function here is a table lookup with a default. The default is never "unknown" silently: an
// out-of-range enumerator returns a string that says so, because these are called from diagnostics
// and a diagnostic that prints an empty string for a corrupt value is a diagnostic that hides one.

#include <cy/shader/shader.h>

#include <cstring>

namespace cy::shader {
namespace {

/// Indexed by `Stage`. Kept beside `kStageProfiles` so a stage added to one and not the other is a
/// compile error rather than a mismatch — the static_assert below is what makes that true.
constexpr const char* kStageNames[] = {
    "vertex",   "fragment",       "compute",     "geometry",    "tessellation-control",
    "tessellation-evaluation",    "task",        "mesh",        "ray-generation",
    "intersection",               "any-hit",     "closest-hit", "miss",
    "callable",
};

/// The Slang `-stage` argument for each stage. Slang's spelling, not the engine's: this string is
/// handed to somebody else's command line, and the two vocabularies agreeing today is a coincidence
/// worth not depending on.
constexpr const char* kStageProfiles[] = {
    "vertex",       "fragment", "compute",     "geometry", "hull", "domain",          "amplification",
    "mesh",         "raygeneration",           "intersection",     "anyhit",          "closesthit",
    "miss",         "callable",
};

static_assert(sizeof(kStageNames) / sizeof(kStageNames[0]) == kStageCount);
static_assert(sizeof(kStageProfiles) / sizeof(kStageProfiles[0]) == kStageCount);

template <usize N>
const char* lookup(const char* const (&table)[N], u32 index, const char* fallback) noexcept {
    return index < N ? table[index] : fallback;
}

}  // namespace

const char* stage_name(Stage stage) noexcept {
    return lookup(kStageNames, static_cast<u32>(stage), "<invalid stage>");
}

const char* stage_profile(Stage stage) noexcept {
    return lookup(kStageProfiles, static_cast<u32>(stage), "<invalid stage>");
}

const char* target_name(Target target) noexcept {
    constexpr const char* kNames[] = {"spirv", "msl", "dxil"};
    return lookup(kNames, static_cast<u32>(target), "<invalid target>");
}

const char* optimisation_name(Optimisation level) noexcept {
    constexpr const char* kNames[] = {"none", "debug", "default", "size"};
    return lookup(kNames, static_cast<u32>(level), "<invalid optimisation>");
}

const char* renderer_profile_name(RendererProfile profile) noexcept {
    constexpr const char* kNames[] = {"desktop", "mobile", "console"};
    return lookup(kNames, static_cast<u32>(profile), "<invalid profile>");
}

void ShortName::assign(std::string_view text) noexcept {
    const usize length = text.size() < kMaxNameLength ? text.size() : kMaxNameLength;
    if (length != 0) {
        std::memcpy(text_, text.data(), length);
    }
    text_[length] = '\0';
    size_ = static_cast<u8>(length);
    truncated_ = text.size() > kMaxNameLength;
}

}  // namespace cy::shader
