// The compiled artefact, the front-end registry and the SPIR-V passthrough. Tasks 3.1 and 3.2.
//
// The registry is fixed-size and locked, for the same reason cy::rhi's is: there are two front ends
// on the whole roadmap and a test may substitute one, and a registry that allocated would be a
// registry that can fail during start-up.

#include <cy/backends/shader/compiler.h>

#include <cy/core/base/assert.h>

#include <chrono>
#include <cstring>
#include <mutex>

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
