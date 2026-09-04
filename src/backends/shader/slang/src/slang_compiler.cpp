// The Slang front end. Task 3.1, and the only translation unit in the engine that names a Slang
// type.
//
// WHAT IT DOES, IN ORDER. Create a global session once (it loads Slang's core module, which is the
// expensive part, and amortising it is why the session outlives a compilation). Per compilation:
// build a session with one SPIR-V target and the permutation's preprocessor defines; hand it a
// file system that resolves an `import` through cy::shader::SourceRegistry rather than through the
// operating system — which is what makes a *generated* module importable by exactly the same
// syntax as an authored one; load the module from its source string; find the entry point; compose,
// link, and ask for the entry point's code.
//
// WHAT IT DELIBERATELY DOES NOT DO. It does not reflect. Reflection is read from the SPIR-V
// (cy/backends/shader/reflection.h) so that a module arriving from a cache tier, from a shipped
// library, or from a generator that already compiled it reflects identically to one compiled here.
// Slang's own reflection is richer and is the wrong source for exactly that reason.
//
// THREADING. A Slang global session is not thread-safe, and neither is this class. `shader-system`
// wants compilation on job workers; the way to get it is one compiler per worker, which is what
// `create_compiler` already gives you — each call constructs an independent front end.

#include <cy/backends/shader/slang/slang_compiler.h>

#include <cy/core/base/assert.h>
#include <cy/core/memory/allocator.h>

#include <slang-com-ptr.h>
#include <slang.h>

#include <chrono>
#include <cstdio>
#include <cstring>

namespace cy::shader::slang {
namespace {

using ::slang::IComponentType;
using ::slang::IEntryPoint;
using ::slang::IGlobalSession;
using ::slang::IModule;
using ::slang::ISession;

/// `SlangUUID` has no equality operator of its own, and the interface guids are the only thing
/// `queryInterface` has to compare. A byte comparison over a POD of fixed layout is the whole of
/// it.
[[nodiscard]] bool same_guid(const SlangUUID& a, const SlangUUID& b) noexcept {
    return std::memcmp(&a, &b, sizeof(SlangUUID)) == 0;
}

[[nodiscard]] u64 monotonic_ns() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

// Slang's COM interfaces have no virtual destructor, by design: an object is released through
// `release()` and never deleted through a base pointer, which is what the reference count is for.
// -Wnon-virtual-dtor cannot see that contract, so it is switched off for the two implementations
// below and nowhere else. This is not a case of the warning being wrong about C++; it is the
// warning being right about a rule this interface deliberately does not play by.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"

/// A blob over bytes the caller owns for the call's duration.
///
/// Slang releases every blob it is handed, so the refcount is real; the storage is the engine's and
/// is freed when the count reaches zero. Reference counting is not shared across threads here
/// because a compilation is single-threaded, which is why the counter is a plain `u32`.
class SourceBlob final : public ISlangBlob {
public:
    SourceBlob(Allocator& allocator, const char* data, usize size) noexcept
        : allocator_(&allocator), data_(data), size_(size) {}

    SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(const SlangUUID& uuid,
                                                          void** out) noexcept override {
        if (same_guid(uuid, ISlangUnknown::getTypeGuid()) ||
            same_guid(uuid, ISlangBlob::getTypeGuid())) {
            ++references_;
            *out = static_cast<void*>(this);
            return SLANG_OK;
        }
        return SLANG_E_NO_INTERFACE;
    }
    SLANG_NO_THROW uint32_t SLANG_MCALL addRef() noexcept override { return ++references_; }
    SLANG_NO_THROW uint32_t SLANG_MCALL release() noexcept override {
        const u32 remaining = --references_;
        if (remaining == 0) {
            Allocator& allocator = *allocator_;
            this->~SourceBlob();
            allocator.deallocate(static_cast<void*>(this), sizeof(SourceBlob), alignof(SourceBlob));
        }
        return remaining;
    }
    SLANG_NO_THROW const void* SLANG_MCALL getBufferPointer() noexcept override { return data_; }
    SLANG_NO_THROW size_t SLANG_MCALL getBufferSize() noexcept override { return size_; }

private:
    Allocator* allocator_;
    const char* data_;
    usize size_;
    u32 references_ = 1;
};

/// The file system Slang resolves an `import` through.
///
/// A path Slang asks for is turned back into a module name — `cy/brdf.slang` into `cy.brdf` — and
/// looked up in the registry. That is the whole of the generated-source seam at run time: a module
/// the material compiler published has no file, and this resolves it anyway, so `import` does not
/// care which kind it got.
///
/// Lifetime: it lives on the stack of the compile call, and Slang's refcounting on it is a no-op.
/// That is safe because Slang releases it before `createSession`'s session is released, and the
/// session does not outlive the call. Making it heap-allocated and refcounted would buy nothing and
/// hide that.
class RegistryFileSystem final : public ISlangFileSystem {
public:
    RegistryFileSystem(Allocator& allocator, const SourceResolver& resolver) noexcept
        : allocator_(&allocator), resolver_(resolver) {}

    SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(const SlangUUID& uuid,
                                                          void** out) noexcept override {
        if (same_guid(uuid, ISlangUnknown::getTypeGuid()) ||
            same_guid(uuid, ISlangCastable::getTypeGuid()) ||
            same_guid(uuid, ISlangFileSystem::getTypeGuid())) {
            *out = static_cast<void*>(this);
            return SLANG_OK;
        }
        return SLANG_E_NO_INTERFACE;
    }
    SLANG_NO_THROW uint32_t SLANG_MCALL addRef() noexcept override { return 2; }
    SLANG_NO_THROW uint32_t SLANG_MCALL release() noexcept override { return 2; }
    SLANG_NO_THROW void* SLANG_MCALL castAs(const SlangUUID& guid) noexcept override {
        if (same_guid(guid, ISlangUnknown::getTypeGuid()) ||
            same_guid(guid, ISlangCastable::getTypeGuid()) ||
            same_guid(guid, ISlangFileSystem::getTypeGuid())) {
            return static_cast<void*>(this);
        }
        return nullptr;
    }

    SLANG_NO_THROW SlangResult SLANG_MCALL loadFile(const char* path,
                                                    ISlangBlob** out) noexcept override {
        if (path == nullptr || out == nullptr) {
            return SLANG_E_INVALID_ARG;
        }
        char module_name[512] = {};
        if (!module_name_of(path, module_name, sizeof(module_name))) {
            return SLANG_E_NOT_FOUND;
        }
        SourceUnit unit;
        if (!resolver_(module_name, unit)) {
            return SLANG_E_NOT_FOUND;
        }
        void* storage = allocator_->allocate(sizeof(SourceBlob), alignof(SourceBlob));
        if (storage == nullptr) {
            return SLANG_E_OUT_OF_MEMORY;
        }
        *out = construct_at<SourceBlob>(storage, *allocator_, unit.text.data(), unit.text.size());
        return SLANG_OK;
    }

private:
    /// `cy/brdf.slang` -> `cy.brdf`. A path with no `.slang` extension is not a module and is
    /// reported as not found rather than guessed at.
    static bool module_name_of(const char* path, char* out, usize capacity) noexcept {
        const usize length = std::strlen(path);
        if (length < kSourceExtension.size() ||
            std::strcmp(path + length - kSourceExtension.size(), kSourceExtension.data()) != 0) {
            return false;
        }
        const usize stem = length - kSourceExtension.size();
        if (stem == 0 || stem >= capacity) {
            return false;
        }
        for (usize index = 0; index < stem; ++index) {
            out[index] =
                (path[index] == '/' || path[index] == '\\') ? kModuleSeparator : path[index];
        }
        out[stem] = '\0';
        return true;
    }

    Allocator* allocator_;
    SourceResolver resolver_;
};

#pragma GCC diagnostic pop

[[nodiscard]] const char* profile_name(u32 spirv_version) noexcept {
    return spirv_version >= kSpirv1_6 ? "spirv_1_6" : "spirv_1_5";
}

[[nodiscard]] SlangOptimizationLevel optimization_of(OptimizationLevel level) noexcept {
    switch (level) {
        case OptimizationLevel::None:
            return SLANG_OPTIMIZATION_LEVEL_NONE;
        case OptimizationLevel::Size:
            return SLANG_OPTIMIZATION_LEVEL_DEFAULT;
        case OptimizationLevel::Performance:
            return SLANG_OPTIMIZATION_LEVEL_HIGH;
    }
    return SLANG_OPTIMIZATION_LEVEL_DEFAULT;
}

[[nodiscard]] SlangDebugInfoLevel debug_of(DebugInfoLevel level) noexcept {
    switch (level) {
        case DebugInfoLevel::None:
            return SLANG_DEBUG_INFO_LEVEL_NONE;
        case DebugInfoLevel::LineTables:
            return SLANG_DEBUG_INFO_LEVEL_MINIMAL;
        case DebugInfoLevel::Full:
            return SLANG_DEBUG_INFO_LEVEL_STANDARD;
    }
    return SLANG_DEBUG_INFO_LEVEL_NONE;
}

/// Copy a diagnostics blob into the engine's structured log. Slang emits
/// `file(line): severity code: message`, which `DiagnosticLog::parse_compiler_output` reads.
void absorb(DiagnosticLog& diagnostics, ISlangBlob* blob) noexcept {
    if (blob == nullptr || blob->getBufferSize() == 0) {
        return;
    }
    const std::string_view text(static_cast<const char*>(blob->getBufferPointer()),
                                blob->getBufferSize());
    (void)diagnostics.parse_compiler_output(text);
}

class SlangCompiler final : public ShaderCompiler {
public:
    explicit SlangCompiler(Allocator& allocator) noexcept : allocator_(&allocator) {}

    [[nodiscard]] const char* name() const noexcept override { return kSlangBackendName; }
    [[nodiscard]] bool compiles_source() const noexcept override { return true; }
    /// The compiler's own build tag, asked of the library rather than taken from a header.
    ///
    /// `slang-tag-version.h` is *generated* into Slang's build tree and is not in the source tree
    /// the engine fetches, so a compile-time constant would build from an install and fail from a
    /// FetchContent build. Asking the global session is also the more honest answer: it is the
    /// version that is actually loaded, and this string goes straight into the cache key, where
    /// being wrong means serving a stale binary after a compiler upgrade.
    [[nodiscard]] const char* version() const noexcept override { return version_; }

    [[nodiscard]] Status initialise() noexcept {
        if (SLANG_FAILED(::slang::createGlobalSession(global_.writeRef())) || global_ == nullptr) {
            return fail(ErrorCode::Unavailable,
                        "the Slang global session could not be created; the core module is "
                        "probably missing beside the library");
        }
        const char* tag = global_->getBuildTagString();
        version_ = tag != nullptr ? tag : "unknown";
        return ok();
    }

    [[nodiscard]] Expected<CompiledShader, Error> compile(
        const CompileRequest& request, DiagnosticLog& diagnostics) noexcept override;

private:
    [[nodiscard]] Status collect_defines(const CompileRequest& request) noexcept;

    Allocator* allocator_;
    Slang::ComPtr<IGlobalSession> global_;
    /// Owned by the global session, which outlives this object.
    const char* version_ = "unknown";
    /// Rebuilt per compilation; members so the storage the `PreprocessorMacroDesc` array points at
    /// outlives the `createSession` call.
    Array<::slang::PreprocessorMacroDesc> macros_{*allocator_};
    Array<char> macro_text_{*allocator_};
    Array<u32> macro_offsets_{*allocator_};
    Array<char> source_text_{*allocator_};
};

Status SlangCompiler::collect_defines(const CompileRequest& request) noexcept {
    macros_.clear();
    macro_text_.clear();
    macro_offsets_.clear();
    if (request.permutations == nullptr) {
        return ok();
    }

    Array<Name> names(*allocator_);
    Array<u32> values(*allocator_);
    if (Status collected =
            request.permutations->preprocessor_defines(request.permutation, names, values);
        !collected) {
        return collected;
    }

    // Names and values are written into one arena and referenced by offset, because the array of
    // `PreprocessorMacroDesc` holds `const char*` and a growing arena would move them.
    for (usize index = 0; index < names.size(); ++index) {
        const std::string_view text = names[index].text();
        if (Status pushed = macro_offsets_.push_back(static_cast<u32>(macro_text_.size()));
            !pushed) {
            return pushed;
        }
        if (Status appended = macro_text_.append(Span<const char>(text.data(), text.size()));
            !appended) {
            return appended;
        }
        if (Status terminated = macro_text_.push_back('\0'); !terminated) {
            return terminated;
        }

        char number[16] = {};
        const int length = std::snprintf(number, sizeof(number), "%u", values[index]);
        if (length <= 0) {
            return fail(ErrorCode::Internal, "a permutation value could not be written");
        }
        if (Status pushed = macro_offsets_.push_back(static_cast<u32>(macro_text_.size()));
            !pushed) {
            return pushed;
        }
        if (Status appended =
                macro_text_.append(Span<const char>(number, static_cast<usize>(length) + 1));
            !appended) {
            return appended;
        }
    }

    for (usize index = 0; index + 1 < macro_offsets_.size(); index += 2) {
        ::slang::PreprocessorMacroDesc macro{};
        macro.name = macro_text_.data() + macro_offsets_[index];
        macro.value = macro_text_.data() + macro_offsets_[index + 1];
        if (Status pushed = macros_.push_back(macro); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Expected<CompiledShader, Error> SlangCompiler::compile(const CompileRequest& request,
                                                       DiagnosticLog& diagnostics) noexcept {
    if (global_ == nullptr) {
        return fail(ErrorCode::Unavailable, "the Slang front end was not initialised");
    }
    const u64 started = monotonic_ns();

    if (Status collected = collect_defines(request); !collected) {
        return make_unexpected(collected.error());
    }

    // Slang takes the source as a NUL-terminated string; a `SourceUnit`'s text is a span and
    // carries no terminator, so it is copied once here rather than assumed.
    source_text_.clear();
    if (Status appended = source_text_.append(request.source.text); !appended) {
        return make_unexpected(appended.error());
    }
    if (Status terminated = source_text_.push_back('\0'); !terminated) {
        return make_unexpected(terminated.error());
    }

    RegistryFileSystem file_system(*allocator_, request.resolver);

    ::slang::TargetDesc target;
    target.format = SLANG_SPIRV;
    target.profile = global_->findProfile(profile_name(request.spirv_version));

    const ::slang::CompilerOptionEntry options[] = {
        {::slang::CompilerOptionName::Optimization,
         {::slang::CompilerOptionValueKind::Int,
          static_cast<int32_t>(optimization_of(request.optimization)), 0, nullptr, nullptr}},
        {::slang::CompilerOptionName::DebugInformation,
         {::slang::CompilerOptionValueKind::Int, static_cast<int32_t>(debug_of(request.debug_info)),
          0, nullptr, nullptr}},
        // `shader-system` requires the descriptor set convention to be predictable. Column-major
        // matrices are the engine's convention (`core-math`), stated here rather than left to
        // whichever default the toolchain ships this year.
        {::slang::CompilerOptionName::MatrixLayoutColumn,
         {::slang::CompilerOptionValueKind::Int, 1, 0, nullptr, nullptr}},
    };

    ::slang::SessionDesc session_desc;
    session_desc.targets = &target;
    session_desc.targetCount = 1;
    session_desc.fileSystem = &file_system;
    session_desc.preprocessorMacros = macros_.data();
    session_desc.preprocessorMacroCount = static_cast<SlangInt>(macros_.size());
    session_desc.compilerOptionEntries = options;
    session_desc.compilerOptionEntryCount =
        static_cast<uint32_t>(sizeof(options) / sizeof(options[0]));

    Slang::ComPtr<ISession> session;
    if (SLANG_FAILED(global_->createSession(session_desc, session.writeRef()))) {
        return fail(ErrorCode::Internal, "the Slang session could not be created");
    }

    const char* module_name = request.source.module_name.c_str();
    const char* path = request.source.path.empty() ? module_name : request.source.path.c_str();

    Slang::ComPtr<ISlangBlob> blob;
    IModule* module = session->loadModuleFromSourceString(module_name, path, source_text_.data(),
                                                          blob.writeRef());
    absorb(diagnostics, blob);
    if (module == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the shader module did not compile");
    }

    Slang::ComPtr<IEntryPoint> entry_point;
    if (SLANG_FAILED(
            module->findEntryPointByName(request.entry_point.c_str(), entry_point.writeRef())) ||
        entry_point == nullptr) {
        (void)diagnostics.add(Severity::Error,
                              "the shader module declares no entry point of that name; a Slang "
                              "entry point needs a [shader(\"...\")] attribute");
        return fail(ErrorCode::NotFound, "no such shader entry point");
    }

    IComponentType* components[] = {module, entry_point.get()};
    Slang::ComPtr<IComponentType> composed;
    blob.setNull();
    if (SLANG_FAILED(session->createCompositeComponentType(components, 2, composed.writeRef(),
                                                           blob.writeRef()))) {
        absorb(diagnostics, blob);
        return fail(ErrorCode::Internal, "the shader program could not be composed");
    }
    absorb(diagnostics, blob);

    Slang::ComPtr<IComponentType> linked;
    blob.setNull();
    if (SLANG_FAILED(composed->link(linked.writeRef(), blob.writeRef()))) {
        absorb(diagnostics, blob);
        return fail(ErrorCode::Internal, "the shader program could not be linked");
    }
    absorb(diagnostics, blob);

    Slang::ComPtr<ISlangBlob> code;
    blob.setNull();
    if (SLANG_FAILED(linked->getEntryPointCode(0, 0, code.writeRef(), blob.writeRef())) ||
        code == nullptr) {
        absorb(diagnostics, blob);
        return fail(ErrorCode::Internal, "the shader entry point produced no code");
    }
    absorb(diagnostics, blob);

    const usize byte_size = code->getBufferSize();
    if (byte_size == 0 || (byte_size % sizeof(u32)) != 0) {
        return fail(ErrorCode::Internal, "Slang produced a SPIR-V module of a bad length");
    }
    Array<u32> words(*allocator_);
    if (Status sized = words.resize(byte_size / sizeof(u32)); !sized) {
        return make_unexpected(sized.error());
    }
    std::memcpy(words.data(), code->getBufferPointer(), byte_size);

    CompiledShader shader(*allocator_);
    if (Status adopted =
            shader.adopt(std::move(words), request.entry_point, request.stage, diagnostics);
        !adopted) {
        return make_unexpected(adopted.error());
    }
    shader.stats().compile_ns = monotonic_ns() - started;
    shader.stats().backend = kSlangBackendName;
    return shader;
}

bool slang_is_available() noexcept {
    Slang::ComPtr<IGlobalSession> probe;
    return SLANG_SUCCEEDED(::slang::createGlobalSession(probe.writeRef())) && probe != nullptr;
}

/// Registers the front end when this translation unit is part of the link. See the equivalent
/// comment in the null RHI backend for why this is both an initialiser and a callable function.
[[maybe_unused]] const Status kSlangBackendRegistered = register_slang_backend();

}  // namespace

Expected<ShaderCompiler*, Error> create_slang_compiler(Allocator& allocator) noexcept {
    void* storage = allocator.allocate(sizeof(SlangCompiler), alignof(SlangCompiler));
    if (storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "no memory for the Slang front end");
    }
    auto* compiler = construct_at<SlangCompiler>(storage, allocator);
    if (Status started = compiler->initialise(); !started) {
        compiler->~SlangCompiler();
        allocator.deallocate(storage, sizeof(SlangCompiler), alignof(SlangCompiler));
        return make_unexpected(started.error());
    }
    return compiler;
}

void destroy_slang_compiler(Allocator& allocator, ShaderCompiler* compiler) noexcept {
    if (compiler == nullptr) {
        return;
    }
    // -fno-rtti, and this factory is the only thing that creates one.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto* concrete = static_cast<SlangCompiler*>(compiler);
    concrete->~SlangCompiler();
    allocator.deallocate(static_cast<void*>(concrete), sizeof(SlangCompiler),
                         alignof(SlangCompiler));
}

bool slang_available() noexcept {
    return slang_is_available();
}

Status register_slang_backend() noexcept {
    CompilerRegistration registration;
    registration.name = kSlangBackendName;
    registration.create = &create_slang_compiler;
    registration.destroy = &destroy_slang_compiler;
    registration.is_available = &slang_is_available;
    return register_compiler(registration);
}

}  // namespace cy::shader::slang
