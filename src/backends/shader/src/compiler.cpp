// The compiled artefact, the front-end registry and the SPIR-V passthrough. Tasks 3.1 and 3.2.
//
// The registry is fixed-size and locked, for the same reason cy::rhi's is: there are two front ends
// on the whole roadmap and a test may substitute one, and a registry that allocated would be a
// registry that can fail during start-up.

#include <cy/backends/shader/compiler.h>

#include <cy/core/base/assert.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>

namespace cy::shader {
namespace {

constexpr u32 kMaxCompilers = 8;
/// Front ends alive at once. Each remembers the registration that made it, because
/// `destroy_compiler` must call that entry's destructor and a `ShaderCompiler` has no field to
/// carry it in — deliberately, so the interface stays the interface.
constexpr u32 kMaxLiveCompilers = 16;

struct LiveCompiler {
    ShaderCompiler* compiler = nullptr;
    CompilerDestructor destroy = nullptr;
};

struct Registry {
    std::mutex mutex;
    CompilerRegistration entries[kMaxCompilers] = {};
    u32 count = 0;
    LiveCompiler live[kMaxLiveCompilers] = {};
};

Registry& registry() noexcept {
    static Registry instance;
    return instance;
}

bool same_name(const char* a, const char* b) noexcept {
    return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

bool available(const CompilerRegistration& entry) noexcept {
    return entry.is_available == nullptr || entry.is_available();
}

[[nodiscard]] u64 monotonic_ns() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

/// The passthrough front end: the source unit already holds a SPIR-V module.
///
/// It compiles nothing and reflects everything, which is exactly the shipping runtime's job and
/// exactly what continuous integration needs on a machine with no shader toolchain.
class SpirvCompiler final : public ShaderCompiler {
public:
    explicit SpirvCompiler(Allocator& allocator) noexcept : allocator_(&allocator) {}

    [[nodiscard]] const char* name() const noexcept override { return kSpirvBackendName; }
    [[nodiscard]] bool compiles_source() const noexcept override { return false; }
    [[nodiscard]] const char* version() const noexcept override { return "passthrough-1"; }

    [[nodiscard]] Expected<CompiledShader, Error> compile(
        const CompileRequest& request, DiagnosticLog& diagnostics) noexcept override {
        const u64 started = monotonic_ns();
        const Span<const char> text = request.source.text;
        if (text.size() < sizeof(u32) * 5 || (text.size() % sizeof(u32)) != 0) {
            (void)diagnostics.add(Severity::Error,
                                  "the SPIR-V passthrough was given something that is not a whole "
                                  "number of 32-bit words");
            return fail(ErrorCode::InvalidArgument, "not a SPIR-V module");
        }

        Array<u32> words(*allocator_);
        if (Status sized = words.resize(text.size() / sizeof(u32)); !sized) {
            return make_unexpected(sized.error());
        }
        // memcpy rather than a cast: a SourceUnit's text is a char span with no alignment promise,
        // and reading u32 through a misaligned pointer is undefined behaviour that works on x86
        // right up until it is compiled for something else.
        std::memcpy(words.data(), text.data(), text.size());

        CompiledShader shader(*allocator_);
        if (Status adopted =
                shader.adopt(std::move(words), request.entry_point, request.stage, diagnostics);
            !adopted) {
            return make_unexpected(adopted.error());
        }
        shader.stats().compile_ns = monotonic_ns() - started;
        shader.stats().backend = kSpirvBackendName;
        return shader;
    }

private:
    Allocator* allocator_;
};

Expected<ShaderCompiler*, Error> create_spirv_compiler(Allocator& allocator) noexcept {
    void* storage = allocator.allocate(sizeof(SpirvCompiler), alignof(SpirvCompiler));
    if (storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "no memory for the SPIR-V passthrough front end");
    }
    return construct_at<SpirvCompiler>(storage, allocator);
}

void destroy_spirv_compiler(Allocator& allocator, ShaderCompiler* compiler) noexcept {
    if (compiler == nullptr) {
        return;
    }
    // -fno-rtti, and this factory is the only thing that creates one.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto* concrete = static_cast<SpirvCompiler*>(compiler);
    concrete->~SpirvCompiler();
    allocator.deallocate(static_cast<void*>(concrete), sizeof(SpirvCompiler),
                         alignof(SpirvCompiler));
}

/// Registers the passthrough when this translation unit is part of the link. See the equivalent
/// comment in the null RHI backend: a host that wants the registration to be a statement rather
/// than a link-order property calls `register_spirv_backend()` itself, and both are supported
/// because the registration is idempotent by name.
[[maybe_unused]] const Status kSpirvBackendRegistered = register_spirv_backend();

}  // namespace

// --- The targets, and what two of them have to agree about ---------------------------------------

const char* target_name(Target target) noexcept {
    switch (target) {
        case Target::SpirV:
            return "vulkan-spirv";
        case Target::Msl:
            return "metal-msl";
        case Target::Dxil:
            return "d3d12-dxil";
    }
    return "unknown";
}

const char* target_short_name(Target target) noexcept {
    switch (target) {
        case Target::SpirV:
            return "spirv";
        case Target::Msl:
            return "msl";
        case Target::Dxil:
            return "dxil";
    }
    return "unknown";
}

const char* target_extension(Target target) noexcept {
    switch (target) {
        case Target::SpirV:
            return ".spv";
        case Target::Msl:
            return ".metal";
        case Target::Dxil:
            return ".dxil";
    }
    return ".bin";
}

bool parse_target(const char* text, Target& target) noexcept {
    if (text == nullptr) {
        return false;
    }
    for (usize index = 0; index < kTargetCount; ++index) {
        const auto candidate = static_cast<Target>(index);
        // Both spellings are accepted: a command line says "msl" and a cache key says "metal-msl",
        // and a reader of either should not have to know which one this argument wanted.
        if (same_name(text, target_short_name(candidate)) ||
            same_name(text, target_name(candidate))) {
            target = candidate;
            return true;
        }
    }
    return false;
}

// --- TargetArtefact ------------------------------------------------------------------------------

TargetArtefact::TargetArtefact(Allocator& allocator) noexcept
    : bytes_(allocator), parameters_(allocator) {}

Status TargetArtefact::adopt(Target target, Array<u8>&& bytes, Name entry_point,
                             rhi::ShaderStage stage, Array<TargetParameter>&& parameters,
                             const u32 (&thread_group)[3]) noexcept {
    target_ = target;
    bytes_ = std::move(bytes);
    parameters_ = std::move(parameters);
    entry_point_ = entry_point;
    stage_ = stage;
    thread_group_ = {thread_group[0], thread_group[1], thread_group[2]};
    hash_ = assets::content_hash(bytes_.data(), bytes_.size());
    stats_.spirv_words =
        target == Target::SpirV ? static_cast<u32>(bytes_.size() / sizeof(u32)) : 0;
    return ok();
}

bool bytes_match_target(Target target, Span<const u8> bytes) noexcept {
    switch (target) {
        case Target::SpirV: {
            // The magic word, little-endian, as SPIR-V 1.x has spelled it since 2015.
            constexpr u8 kMagic[4] = {0x03, 0x02, 0x23, 0x07};
            return bytes.size() >= sizeof(kMagic) && (bytes.size() % sizeof(u32)) == 0 &&
                   std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) == 0;
        }
        case Target::Dxil: {
            // A DXC container: the four-character code "DXBC" — kept from DXBC for compatibility,
            // which is why a DXIL blob does not start with "DXIL" — and a "DXIL" chunk inside it.
            // Both, because the header alone is also what a D3D11 bytecode blob starts with.
            if (bytes.size() < 4 || std::memcmp(bytes.data(), "DXBC", 4) != 0) {
                return false;
            }
            const std::string_view whole(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            return whole.find("DXIL") != std::string_view::npos;
        }
        case Target::Msl: {
            // Metal Shading Language is C++14 source: every module Slang emits opens by including
            // the standard library and declaring the namespace, and a shader cannot use a single
            // Metal type without them.
            const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            return text.find("metal_stdlib") != std::string_view::npos &&
                   text.find("using namespace metal") != std::string_view::npos;
        }
    }
    return false;
}

namespace {

/// One difference, written where both targets are named. Kept as a helper because a report that
/// says "they disagree" and stops is a report that costs its reader a bisect.
Status note_difference(DiagnosticLog& diagnostics, Target a, Target b, const char* what,
                       std::string_view left, std::string_view right) noexcept {
    char message[512] = {};
    (void)std::snprintf(message, sizeof(message),
                        "%s differs between %s and %s: '%.*s' against '%.*s'", what,
                        target_name(a), target_name(b), static_cast<int>(left.size()), left.data(),
                        static_cast<int>(right.size()), right.data());
    return diagnostics.add(Severity::Error, message);
}

}  // namespace

bool interfaces_agree(const TargetArtefact& a, const TargetArtefact& b,
                      DiagnosticLog& diagnostics) noexcept {
    bool agree = true;
    if (a.entry_point() != b.entry_point()) {
        (void)note_difference(diagnostics, a.target(), b.target(), "the entry point name",
                              a.entry_point().text(), b.entry_point().text());
        agree = false;
    }
    if (a.stage() != b.stage()) {
        char left_stage[32] = {};
        char right_stage[32] = {};
        (void)std::snprintf(left_stage, sizeof(left_stage), "stage mask %u",
                            static_cast<u32>(a.stage()));
        (void)std::snprintf(right_stage, sizeof(right_stage), "stage mask %u",
                            static_cast<u32>(b.stage()));
        (void)note_difference(diagnostics, a.target(), b.target(), "the stage", left_stage,
                              right_stage);
        agree = false;
    }
    for (usize axis = 0; axis < 3; ++axis) {
        if (a.thread_group()[axis] == b.thread_group()[axis]) {
            continue;
        }
        char left[32] = {};
        char right[32] = {};
        (void)std::snprintf(left, sizeof(left), "%u", a.thread_group()[axis]);
        (void)std::snprintf(right, sizeof(right), "%u", b.thread_group()[axis]);
        (void)note_difference(diagnostics, a.target(), b.target(), "the workgroup size", left,
                              right);
        agree = false;
    }

    const Span<const TargetParameter> left = a.parameters();
    const Span<const TargetParameter> right = b.parameters();
    if (left.size() != right.size()) {
        char left_count[32] = {};
        char right_count[32] = {};
        (void)std::snprintf(left_count, sizeof(left_count), "%zu parameters", left.size());
        (void)std::snprintf(right_count, sizeof(right_count), "%zu parameters", right.size());
        (void)note_difference(diagnostics, a.target(), b.target(), "the parameter count",
                              left_count, right_count);
        return false;
    }
    for (usize index = 0; index < left.size(); ++index) {
        if (left[index].name != right[index].name) {
            (void)note_difference(diagnostics, a.target(), b.target(), "a parameter name",
                                  left[index].name.text(), right[index].name.text());
            agree = false;
            continue;
        }
        if (left[index].kind != right[index].kind) {
            (void)note_difference(diagnostics, a.target(), b.target(),
                                  left[index].name.c_str(),
                                  rhi::descriptor_kind_name(left[index].kind),
                                  rhi::descriptor_kind_name(right[index].kind));
            agree = false;
        }
        if (left[index].count != right[index].count) {
            char left_count[32] = {};
            char right_count[32] = {};
            (void)std::snprintf(left_count, sizeof(left_count), "%u elements", left[index].count);
            (void)std::snprintf(right_count, sizeof(right_count), "%u elements", right[index].count);
            (void)note_difference(diagnostics, a.target(), b.target(), left[index].name.c_str(),
                                  left_count, right_count);
            agree = false;
        }
    }
    return agree;
}

// --- ShaderCompiler's two target methods ----------------------------------------------------------
//
// Defaults rather than pure virtuals: a front end that emits one interchange form and nothing else
// is a legitimate front end — the passthrough is one — and making every implementation restate that
// would be making the interface's second-largest question a copy-and-paste.

Expected<TargetArtefact, Error> ShaderCompiler::compile_for(const CompileRequest& /*request*/,
                                                            Target target,
                                                            DiagnosticLog& diagnostics) noexcept {
    char message[256] = {};
    (void)std::snprintf(message, sizeof(message),
                        "the '%s' front end emits no %s: it does not compile source", name(),
                        target_name(target));
    (void)diagnostics.add(Severity::Error, message);
    return fail(ErrorCode::Unsupported, "this front end emits only the form it was handed");
}

bool ShaderCompiler::emits(Target /*target*/) noexcept {
    // FALSE FOR SPIR-V TOO, WHICH READS WRONG UNTIL THE QUESTION IS READ EXACTLY. `emits` asks
    // whether this front end can produce a target artefact from source — pipeline step 4 — not
    // whether SPIR-V can reach a device through it. The passthrough consumes an already-compiled
    // module and can neither retarget it nor produce a second one, so every answer it gives here is
    // no, and `compile_for` above says the same thing in a sentence.
    return false;
}

// --- CompiledShader --------------------------------------------------------------------------

CompiledShader::CompiledShader(Allocator& allocator) noexcept
    : spirv_(allocator), reflection_(allocator) {}

Status CompiledShader::adopt(Array<u32>&& spirv, Name entry_point, rhi::ShaderStage stage,
                             DiagnosticLog& diagnostics) noexcept {
    spirv_ = std::move(spirv);
    entry_point_ = entry_point;
    stage_ = stage;
    hash_ = assets::content_hash(spirv_.data(), spirv_.size() * sizeof(u32));

    Expected<Reflection, Error> reflection =
        reflect_spirv(spirv_.allocator(), Span<const u32>(spirv_.data(), spirv_.size()));
    if (!reflection) {
        (void)diagnostics.add(Severity::Error, reflection.error().message);
        return make_unexpected(reflection.error());
    }
    reflection_ = std::move(reflection.value());

    stats_.spirv_words = static_cast<u32>(spirv_.size());
    stats_.instruction_count = reflection_.instruction_count();
    if (stage_ == rhi::ShaderStage::None && !reflection_.entry_points().empty()) {
        // A passthrough caller need not know the stage: the module says so.
        stage_ = reflection_.entry_points()[0].stage;
    }
    return ok();
}

Status CompiledShader::restore(Array<u32>&& code, Reflection&& reflection, Name entry_point,
                               rhi::ShaderStage stage, const CompileStats& stats,
                               const assets::ContentHash& hash) noexcept {
    spirv_ = std::move(code);
    reflection_ = std::move(reflection);
    entry_point_ = entry_point;
    stage_ = stage;
    stats_ = stats;
    hash_ = hash;
    return ok();
}

rhi::ShaderModuleDescription CompiledShader::module_description() const noexcept {
    rhi::ShaderModuleDescription description;
    description.name = entry_point_.is_empty() ? "shader" : entry_point_.c_str();
    description.stage = stage_;
    description.spirv = Span<const u32>(spirv_.data(), spirv_.size());
    // SPIR-V records the entry point's name as it appears in the module. Slang rewrites it to
    // "main" when it emits a single-entry module, so the name the device is given is the one the
    // reflection read out of the binary rather than the one the author typed.
    description.entry_point =
        reflection_.entry_points().empty() ? "main" : reflection_.entry_points()[0].name.c_str();
    return description;
}

Expected<CompiledShader, Error> CompiledShader::clone(Allocator& allocator) const noexcept {
    CompiledShader copy(allocator);
    Expected<Array<u32>, Error> words = spirv_.clone();
    if (!words) {
        return make_unexpected(words.error());
    }
    DiagnosticLog discarded(allocator);
    if (Status adopted = copy.adopt(std::move(words.value()), entry_point_, stage_, discarded);
        !adopted) {
        return make_unexpected(adopted.error());
    }
    copy.stats_ = stats_;
    return copy;
}

ShaderCompiler::~ShaderCompiler() = default;

// --- The registry ----------------------------------------------------------------------------

Status register_compiler(const CompilerRegistration& registration) noexcept {
    if (registration.name == nullptr || registration.name[0] == '\0' ||
        registration.create == nullptr || registration.destroy == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "a shader front-end registration needs a name, a factory and a destructor");
    }

    Registry& table = registry();
    const std::lock_guard<std::mutex> lock(table.mutex);
    for (u32 index = 0; index < table.count; ++index) {
        if (same_name(table.entries[index].name, registration.name)) {
            table.entries[index] = registration;
            return ok();
        }
    }
    if (table.count == kMaxCompilers) {
        return fail(ErrorCode::OutOfRange, "the shader front-end table is full");
    }
    table.entries[table.count] = registration;
    ++table.count;
    return ok();
}

Span<const CompilerRegistration> registered_compilers() noexcept {
    Registry& table = registry();
    const std::lock_guard<std::mutex> lock(table.mutex);
    return {table.entries, table.count};
}

const CompilerRegistration* find_compiler(const char* name) noexcept {
    if (name == nullptr) {
        return nullptr;
    }
    Registry& table = registry();
    const std::lock_guard<std::mutex> lock(table.mutex);
    for (u32 index = 0; index < table.count; ++index) {
        if (same_name(table.entries[index].name, name)) {
            return &table.entries[index];
        }
    }
    return nullptr;
}

Status register_spirv_backend() noexcept {
    CompilerRegistration registration;
    registration.name = kSpirvBackendName;
    registration.create = &create_spirv_compiler;
    registration.destroy = &destroy_spirv_compiler;
    registration.is_available = nullptr;  // always
    return register_compiler(registration);
}

namespace {

/// The entry a request selects: the named one, or the first available front end that can compile
/// source, or the passthrough.
const CompilerRegistration* select(Registry& table, const char* requested,
                                   CompilerSelection& selection) noexcept {
    const CompilerRegistration* fallback = nullptr;
    const CompilerRegistration* preferred = nullptr;
    const CompilerRegistration* named = nullptr;
    for (u32 index = 0; index < table.count; ++index) {
        const CompilerRegistration& entry = table.entries[index];
        if (same_name(entry.name, kSpirvBackendName)) {
            fallback = &entry;
        }
        if (requested != nullptr && requested[0] != '\0' && same_name(entry.name, requested)) {
            named = &entry;
        }
        if (preferred == nullptr && !same_name(entry.name, kSpirvBackendName) && available(entry)) {
            preferred = &entry;
        }
    }

    if (requested != nullptr && requested[0] != '\0') {
        if (named == nullptr) {
            selection.reason = "no shader front end is registered under that name";
        } else if (!available(*named)) {
            selection.reason = "the requested shader front end reported itself unavailable";
        } else {
            return named;
        }
        selection.fell_back = true;
        return fallback;
    }
    if (preferred != nullptr) {
        return preferred;
    }
    selection.reason = "no front end in this build compiles shader source";
    return fallback;
}

}  // namespace

Expected<ShaderCompiler*, Error> create_compiler(Allocator& allocator, const char* requested,
                                                 CompilerSelection& selection) noexcept {
    selection = CompilerSelection{};
    selection.requested = requested != nullptr ? requested : "";

    Registry& table = registry();
    const std::lock_guard<std::mutex> lock(table.mutex);

    const CompilerRegistration* entry = select(table, requested, selection);
    if (entry == nullptr) {
        return fail(ErrorCode::Unavailable,
                    "no shader front end is registered, not even the SPIR-V passthrough");
    }

    Expected<ShaderCompiler*, Error> compiler = entry->create(allocator);
    if (!compiler) {
        return compiler;
    }
    for (LiveCompiler& live : table.live) {
        if (live.compiler != nullptr) {
            continue;
        }
        live.compiler = compiler.value();
        live.destroy = entry->destroy;
        selection.selected = entry->name;
        return compiler;
    }

    entry->destroy(allocator, compiler.value());
    return fail(ErrorCode::OutOfRange, "too many shader front ends are alive at once");
}

void destroy_compiler(Allocator& allocator, ShaderCompiler* compiler) noexcept {
    if (compiler == nullptr) {
        return;
    }
    Registry& table = registry();
    const std::lock_guard<std::mutex> lock(table.mutex);
    for (LiveCompiler& live : table.live) {
        if (live.compiler != compiler) {
            continue;
        }
        live.destroy(allocator, compiler);
        live.compiler = nullptr;
        live.destroy = nullptr;
        return;
    }
    CY_ASSERT_MSG(false, "destroy_compiler() on a front end this registry did not create");
}

}  // namespace cy::shader
