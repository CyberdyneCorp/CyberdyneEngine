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

// --- The three targets ---------------------------------------------------------------------------
//
// M11.c task 1.7. `shader-system`'s pipeline step 4, as three lines of mapping rather than as a
// sentence in a comment: the SAME Slang program is compiled a second and a third time with a
// different `TargetDesc::format`, which is what a multi-target compiler is for.

[[nodiscard]] SlangCompileTarget format_of(Target target) noexcept {
    switch (target) {
        case Target::SpirV:
            return SLANG_SPIRV;
        case Target::Msl:
            // MSL SOURCE AND NOT SLANG_METAL_LIB. A `.metallib` is produced by Apple's `metal`
            // driver, which exists only on macOS with Xcode installed — asking for one here would
            // make the target unavailable on every machine that is not a Mac, including every
            // machine this engine's continuous integration runs on. Source is what the Metal
            // backend compiles at cook time on the platform that can, and it is the form that can
            // be read, diffed and checked anywhere.
            return SLANG_METAL;
        case Target::Dxil:
            return SLANG_DXIL;
    }
    return SLANG_TARGET_UNKNOWN;
}

/// The profile each target is compiled at.
///
/// SHADER MODEL 6.5 FOR DXIL, chosen rather than defaulted: 6.5 is the first model with mesh and
/// amplification shaders and DXR 1.1, both of which this engine's Vulkan path already uses, so a
/// lower floor would make the D3D12 leg unable to compile shaders the Vulkan leg compiles. Metal
/// takes no profile here — Slang picks its own language version for the target, and naming one
/// would pin a Metal version this engine has no reason to have an opinion about yet.
[[nodiscard]] const char* profile_for(Target target, u32 spirv_version) noexcept {
    switch (target) {
        case Target::SpirV:
            return profile_name(spirv_version);
        case Target::Msl:
            return "";
        case Target::Dxil:
            return "sm_6_5";
    }
    return "";
}

[[nodiscard]] rhi::ShaderStage stage_of(SlangStage stage) noexcept {
    switch (stage) {
        case SLANG_STAGE_VERTEX:
            return rhi::ShaderStage::Vertex;
        case SLANG_STAGE_FRAGMENT:
            return rhi::ShaderStage::Fragment;
        case SLANG_STAGE_COMPUTE:
            return rhi::ShaderStage::Compute;
        case SLANG_STAGE_GEOMETRY:
            return rhi::ShaderStage::Geometry;
        case SLANG_STAGE_HULL:
            return rhi::ShaderStage::TessellationControl;
        case SLANG_STAGE_DOMAIN:
            return rhi::ShaderStage::TessellationEvaluation;
        case SLANG_STAGE_AMPLIFICATION:
            return rhi::ShaderStage::Task;
        case SLANG_STAGE_MESH:
            return rhi::ShaderStage::Mesh;
        default:
            return rhi::ShaderStage::None;
    }
}

/// What a parameter IS, in the engine's vocabulary — and the half of a declaration that two targets
/// must agree about.
///
/// It is read from the Slang TYPE and never from the target layout: a `Texture2D` is a sampled
/// texture whether the target put it in a descriptor set, a Metal texture slot or a `t` register,
/// and that is exactly why this is the comparable half and the binding index is not.
[[nodiscard]] rhi::DescriptorKind kind_of(::slang::TypeLayoutReflection* layout) noexcept {
    if (layout == nullptr) {
        return rhi::DescriptorKind::UniformBuffer;
    }
    switch (layout->getKind()) {
        case ::slang::TypeReflection::Kind::ConstantBuffer:
        case ::slang::TypeReflection::Kind::ParameterBlock:
            return rhi::DescriptorKind::UniformBuffer;
        case ::slang::TypeReflection::Kind::SamplerState:
            return rhi::DescriptorKind::Sampler;
        case ::slang::TypeReflection::Kind::ShaderStorageBuffer:
            return rhi::DescriptorKind::StorageBuffer;
        case ::slang::TypeReflection::Kind::TextureBuffer:
            return rhi::DescriptorKind::SampledTexture;
        case ::slang::TypeReflection::Kind::Resource:
            break;
        default:
            return rhi::DescriptorKind::UniformBuffer;
    }

    const SlangResourceShape shape = static_cast<SlangResourceShape>(
        layout->getResourceShape() & SLANG_RESOURCE_BASE_SHAPE_MASK);
    if (shape == SLANG_STRUCTURED_BUFFER || shape == SLANG_BYTE_ADDRESS_BUFFER) {
        // A read-only `StructuredBuffer` is still a storage buffer where it lands: Vulkan has no
        // read-only class for one, and the access is carried by the descriptor's usage rather than
        // by its kind.
        return rhi::DescriptorKind::StorageBuffer;
    }
    return layout->getResourceAccess() == SLANG_RESOURCE_ACCESS_READ
               ? rhi::DescriptorKind::SampledTexture
               : rhi::DescriptorKind::StorageTexture;
}

/// Read one target's declared parameters out of the program layout that target produced.
[[nodiscard]] Status read_parameters(::slang::ProgramLayout* layout,
                                     Array<TargetParameter>& out) noexcept {
    out.clear();
    if (layout == nullptr) {
        return fail(ErrorCode::Internal, "the Slang program produced no layout for the target");
    }
    for (unsigned index = 0; index < layout->getParameterCount(); ++index) {
        ::slang::VariableLayoutReflection* parameter = layout->getParameterByIndex(index);
        if (parameter == nullptr) {
            continue;
        }
        TargetParameter declared;
        const char* text = parameter->getName();
        declared.name = Name::intern(text != nullptr ? std::string_view(text) : std::string_view());
        declared.space = static_cast<u32>(parameter->getBindingSpace());
        declared.index = static_cast<u32>(parameter->getBindingIndex());

        ::slang::TypeLayoutReflection* type = parameter->getTypeLayout();
        if (type != nullptr && type->getKind() == ::slang::TypeReflection::Kind::Array) {
            // `count` is `rhi::DescriptorBinding::count`'s encoding: zero is a runtime-sized array,
            // which is what a bindless table is, and the element type is what the binding holds.
            const size_t elements = type->getElementCount();
            declared.count = (elements == SLANG_UNBOUNDED_SIZE || elements == SLANG_UNKNOWN_SIZE)
                                 ? 0
                                 : static_cast<u32>(elements);
            type = type->getElementTypeLayout();
        }
        declared.kind = kind_of(type);
        if (Status pushed = out.push_back(declared); !pushed) {
            return pushed;
        }
    }
    return ok();
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

    [[nodiscard]] Expected<TargetArtefact, Error> compile_for(
        const CompileRequest& request, Target target, DiagnosticLog& diagnostics) noexcept override;

    [[nodiscard]] bool emits(Target target) noexcept override;

private:
    [[nodiscard]] Status collect_defines(const CompileRequest& request) noexcept;

    /// Everything both compilation paths do: the session for one target, the module, the entry
    /// point, the composite, the link, and the code. Extracted rather than duplicated because the
    /// ONE thing the agreement check needs to be true is that the SPIR-V and the MSL came out of
    /// the same sequence of calls with one field changed.
    [[nodiscard]] Status link_program(const CompileRequest& request, Target target,
                                      DiagnosticLog& diagnostics,
                                      Slang::ComPtr<IComponentType>& linked,
                                      Slang::ComPtr<ISlangBlob>& code) noexcept;

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
    /// What `emits` found when it last asked, per target. Unknown until something asks, because the
    /// probe is a real compilation and a front end nobody asked about should not pay for three.
    enum class Probe : u8 { Unknown = 0, Yes = 1, No = 2 };
    Probe probed_[kTargetCount] = {};
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

Status SlangCompiler::link_program(const CompileRequest& request, Target target,
                                   DiagnosticLog& diagnostics,
                                   Slang::ComPtr<IComponentType>& linked,
                                   Slang::ComPtr<ISlangBlob>& code) noexcept {
    if (global_ == nullptr) {
        return fail(ErrorCode::Unavailable, "the Slang front end was not initialised");
    }
    if (Status collected = collect_defines(request); !collected) {
        return collected;
    }

    // Slang takes the source as a NUL-terminated string; a `SourceUnit`'s text is a span and
    // carries no terminator, so it is copied once here rather than assumed.
    source_text_.clear();
    if (Status appended = source_text_.append(request.source.text); !appended) {
        return appended;
    }
    if (Status terminated = source_text_.push_back('\0'); !terminated) {
        return terminated;
    }

    RegistryFileSystem file_system(*allocator_, request.resolver);

    ::slang::TargetDesc target_desc;
    target_desc.format = format_of(target);
    const char* profile = profile_for(target, request.spirv_version);
    if (profile[0] != '\0') {
        target_desc.profile = global_->findProfile(profile);
    }

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
    session_desc.targets = &target_desc;
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

    blob.setNull();
    if (SLANG_FAILED(composed->link(linked.writeRef(), blob.writeRef()))) {
        absorb(diagnostics, blob);
        return fail(ErrorCode::Internal, "the shader program could not be linked");
    }
    absorb(diagnostics, blob);

    blob.setNull();
    if (SLANG_FAILED(linked->getEntryPointCode(0, 0, code.writeRef(), blob.writeRef())) ||
        code == nullptr) {
        absorb(diagnostics, blob);
        // The one failure a caller must be able to tell from a broken shader: DXIL is emitted by a
        // compiler Slang loads at compile time, and when that library is not beside it the message
        // in the diagnostics is "failed to load downstream compiler" rather than anything about
        // this program. Named here so `emits` can report an absent toolchain as an absent toolchain.
        char message[192] = {};
        (void)std::snprintf(message, sizeof(message), "no %s was produced for this entry point",
                            target_name(target));
        (void)diagnostics.add(Severity::Error, message);
        return fail(ErrorCode::Unavailable, "the shader entry point produced no code");
    }
    absorb(diagnostics, blob);
    return ok();
}

Expected<CompiledShader, Error> SlangCompiler::compile(const CompileRequest& request,
                                                       DiagnosticLog& diagnostics) noexcept {
    const u64 started = monotonic_ns();

    Slang::ComPtr<IComponentType> linked;
    Slang::ComPtr<ISlangBlob> code;
    if (Status built = link_program(request, Target::SpirV, diagnostics, linked, code); !built) {
        return make_unexpected(built.error());
    }

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

Expected<TargetArtefact, Error> SlangCompiler::compile_for(const CompileRequest& request,
                                                           Target target,
                                                           DiagnosticLog& diagnostics) noexcept {
    const u64 started = monotonic_ns();

    Slang::ComPtr<IComponentType> linked;
    Slang::ComPtr<ISlangBlob> code;
    if (Status built = link_program(request, target, diagnostics, linked, code); !built) {
        return make_unexpected(built.error());
    }

    const usize byte_size = code->getBufferSize();
    if (byte_size == 0) {
        return fail(ErrorCode::Internal, "the target produced an empty artefact");
    }
    Array<u8> bytes(*allocator_);
    if (Status sized = bytes.resize(byte_size); !sized) {
        return make_unexpected(sized.error());
    }
    std::memcpy(bytes.data(), code->getBufferPointer(), byte_size);

    // THE DECLARATION IS READ FROM THE LAYOUT THIS TARGET PRODUCED, not from the SPIR-V and not
    // from the source. That is what makes "the two targets declare the same interface" a
    // measurement rather than a restatement of the input.
    Slang::ComPtr<ISlangBlob> blob;
    ::slang::ProgramLayout* layout = linked->getLayout(0, blob.writeRef());
    absorb(diagnostics, blob);
    Array<TargetParameter> parameters(*allocator_);
    if (Status read = read_parameters(layout, parameters); !read) {
        return make_unexpected(read.error());
    }

    u32 thread_group[3] = {0, 0, 0};
    Name entry = request.entry_point;
    rhi::ShaderStage stage = request.stage;
    if (layout->getEntryPointCount() > 0) {
        ::slang::EntryPointReflection* reflected = layout->getEntryPointByIndex(0);
        if (reflected != nullptr) {
            SlangUInt sizes[3] = {0, 0, 0};
            reflected->getComputeThreadGroupSize(3, sizes);
            for (usize axis = 0; axis < 3; ++axis) {
                thread_group[axis] = static_cast<u32>(sizes[axis]);
            }
            if (const char* name = reflected->getName(); name != nullptr) {
                entry = Name::intern(name);
            }
            if (const rhi::ShaderStage reported = stage_of(reflected->getStage());
                reported != rhi::ShaderStage::None) {
                stage = reported;
            }
        }
    }

    TargetArtefact artefact(*allocator_);
    if (Status adopted = artefact.adopt(target, std::move(bytes), entry, stage,
                                        std::move(parameters), thread_group);
        !adopted) {
        return make_unexpected(adopted.error());
    }
    artefact.stats().compile_ns = monotonic_ns() - started;
    artefact.stats().backend = kSlangBackendName;

    // THE FRONT END REFUSES TO HAND BACK SOMETHING THAT IS NOT IN ITS TARGET'S FORM. A caller that
    // checked this itself could be given a stub by a front end that did not; a caller that does not
    // check would ship one. It costs a few bytes of comparison per compilation.
    if (!bytes_match_target(target, artefact.bytes())) {
        char message[192] = {};
        (void)std::snprintf(message, sizeof(message),
                            "what Slang returned for %s is not in that form", target_name(target));
        (void)diagnostics.add(Severity::Error, message);
        return fail(ErrorCode::Internal, "the artefact is not in the form its target names");
    }
    return artefact;
}

bool SlangCompiler::emits(Target target) noexcept {
    const auto slot = static_cast<usize>(target);
    if (slot >= kTargetCount) {
        return false;
    }
    if (probed_[slot] != Probe::Unknown) {
        return probed_[slot] == Probe::Yes;
    }

    // A REAL COMPILATION OF A REAL SHADER, and nothing shorter would answer the question. Whether
    // DXIL can be emitted depends on a library Slang loads when it is first asked to emit DXIL; no
    // flag, header, version string or file test observes that, and each of them would answer yes on
    // a machine where the emission fails.
    static constexpr char kProbeSource[] =
        "[[vk::binding(0, 0)]] RWStructuredBuffer<float> cyTargetProbeOutput;\n"
        "[shader(\"compute\")]\n"
        "[numthreads(1, 1, 1)]\n"
        "void cyTargetProbe(uint3 id : SV_DispatchThreadID) {\n"
        "    cyTargetProbeOutput[id.x] = 1.0f;\n"
        "}\n";

    CompileRequest request;
    request.source.module_name = Name::intern("cy.target.probe");
    request.source.text = Span<const char>(kProbeSource, sizeof(kProbeSource) - 1);
    request.entry_point = Name::intern("cyTargetProbe");
    request.stage = rhi::ShaderStage::Compute;
    request.optimization = OptimizationLevel::None;

    DiagnosticLog diagnostics(*allocator_);
    Expected<TargetArtefact, Error> artefact = compile_for(request, target, diagnostics);
    probed_[slot] = artefact ? Probe::Yes : Probe::No;
    return probed_[slot] == Probe::Yes;
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
