// The shader set, compiled for every target this build emits. M11.c task 1.7.
//
// See include/cy/shaders/targets.h for what is being claimed and why a file-exists check would not
// claim it.

#include <cy/shaders/targets.h>

#include <cy/backends/shader/slang/slang_compiler.h>
#include <cy/backends/shader/source.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/memory/scope.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cy::shadertool {
namespace {

using shader::Target;
using shader::kTargetCount;

constexpr std::string_view kExtension = ".slang";

/// One `.slang` file, its text, and the names an `import` or an `#include` could reach it by.
struct Module {
    std::string path;
    std::string text;
    Name module_name;
};

/// `src/rendering/shaders/cy/view.slang` -> `src.rendering.shaders.cy.view`.
std::string dotted(std::string_view path) {
    std::string name(path.substr(0, path.size() - kExtension.size()));
    std::replace(name.begin(), name.end(), '/', '.');
    return name;
}

/// The module set a compilation can see, and the resolver the front end reaches it through.
///
/// EVERY SUFFIX OF THE DOTTED PATH IS AN ALIAS, and that is not laxity. Slang resolves `import
/// cy.view` and the preprocessor resolves `#include "hzb_common.slang"` — the first is a module
/// name and the second a path relative to an include directory the engine's file system does not
/// have. Registering `src.rendering.shaders.cy.view`, `rendering.shaders.cy.view`, …, `cy.view`,
/// `view` makes both spellings reach the one file, which is what `slangc -I <dir>` does for the
/// checked-in invocations in each shader's header comment.
class ModuleSet {
public:
    [[nodiscard]] Status load(assets::VirtualFileSystem& files, std::string_view root,
                              std::FILE* out) noexcept;

    [[nodiscard]] shader::SourceResolver resolver() noexcept {
        return shader::SourceResolver{&ModuleSet::resolve, this};
    }

    [[nodiscard]] usize size() const noexcept { return modules_.size(); }
    [[nodiscard]] const Module& at(usize index) const noexcept { return *modules_[index]; }

private:
    static bool resolve(void* user, std::string_view module_name,
                        shader::SourceUnit& out) noexcept {
        auto* self = static_cast<ModuleSet*>(user);
        const auto found = self->by_name_.find(std::string(module_name));
        if (found == self->by_name_.end()) {
            return false;
        }
        const Module& module = *self->modules_[found->second];
        out = shader::SourceUnit{};
        out.module_name = module.module_name;
        out.text = Span<const char>(module.text.data(), module.text.size());
        return true;
    }

    /// First wins, and a collision is reported rather than resolved silently: two files with one
    /// basename mean an `#include` of that basename is ambiguous, and the reader should know which
    /// one the compiler got.
    void alias(std::string name, usize index, std::FILE* out) {
        const auto existing = by_name_.find(name);
        if (existing == by_name_.end()) {
            by_name_.emplace(std::move(name), index);
            return;
        }
        if (existing->second != index) {
            std::fprintf(out, "  note: '%s' names both %s and %s; the first is what imports reach\n",
                         name.c_str(), modules_[existing->second]->path.c_str(),
                         modules_[index]->path.c_str());
        }
    }

    std::vector<std::unique_ptr<Module>> modules_;
    std::unordered_map<std::string, usize> by_name_;
};

struct Walk {
    std::vector<std::string> paths;
};

bool collect(void* user, const assets::VirtualEntry& entry) noexcept {
    if (entry.is_directory || entry.path == nullptr) {
        return true;
    }
    if (entry.path->extension() != kExtension) {
        return true;
    }
    static_cast<Walk*>(user)->paths.emplace_back(entry.path->view());
    return true;
}

Status ModuleSet::load(assets::VirtualFileSystem& files, std::string_view root,
                       std::FILE* out) noexcept {
    auto directory = assets::VirtualPath::normalise(root);
    if (!directory) {
        return make_unexpected(directory.error());
    }

    Walk walk;
    if (Status walked = files.enumerate(*directory, true, &collect, &walk); !walked) {
        return walked;
    }
    // The enumeration is sorted per mount; sorting again makes the order independent of how many
    // roots were walked, so two runs produce the same report.
    std::sort(walk.paths.begin(), walk.paths.end());

    for (const std::string& path : walk.paths) {
        auto file = assets::VirtualPath::normalise(path);
        if (!file) {
            continue;
        }
        Array<u8> bytes(current_allocator());
        if (Status read = files.read(*file, bytes); !read) {
            std::fprintf(out, "  note: %s could not be read; not in the module set\n",
                         path.c_str());
            continue;
        }
        auto module = std::make_unique<Module>();
        module->path = path;
        module->text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const std::string name = dotted(path);
        module->module_name = Name::intern(name);
        modules_.push_back(std::move(module));

        const usize index = modules_.size() - 1;
        std::string_view suffix(name);
        alias(std::string(suffix), index, out);
        for (usize dot = suffix.find('.'); dot != std::string_view::npos;
             dot = suffix.find('.')) {
            suffix.remove_prefix(dot + 1);
            alias(std::string(suffix), index, out);
        }
    }
    return ok();
}

// --- Entry points --------------------------------------------------------------------------------

struct EntryPoint {
    usize module_index = 0;
    std::string name;
    std::string stage;
};

rhi::ShaderStage stage_of(std::string_view stage) noexcept {
    if (stage == "compute") {
        return rhi::ShaderStage::Compute;
    }
    if (stage == "vertex") {
        return rhi::ShaderStage::Vertex;
    }
    if (stage == "fragment" || stage == "pixel") {
        return rhi::ShaderStage::Fragment;
    }
    if (stage == "geometry") {
        return rhi::ShaderStage::Geometry;
    }
    if (stage == "hull") {
        return rhi::ShaderStage::TessellationControl;
    }
    if (stage == "domain") {
        return rhi::ShaderStage::TessellationEvaluation;
    }
    if (stage == "amplification") {
        return rhi::ShaderStage::Task;
    }
    if (stage == "mesh") {
        return rhi::ShaderStage::Mesh;
    }
    // A ray-tracing stage, or one Slang gains after this was written. The front end reads the
    // stage back out of the program layout anyway; this is the request's hint, not its authority.
    return rhi::ShaderStage::None;
}

/// Skip whitespace, line comments and block comments from `at`.
usize skip_trivia(std::string_view text, usize at) noexcept {
    while (at < text.size()) {
        if (std::isspace(static_cast<unsigned char>(text[at])) != 0) {
            ++at;
        } else if (text.compare(at, 2, "//") == 0) {
            const usize end = text.find('\n', at);
            at = end == std::string_view::npos ? text.size() : end + 1;
        } else if (text.compare(at, 2, "/*") == 0) {
            const usize end = text.find("*/", at);
            at = end == std::string_view::npos ? text.size() : end + 2;
        } else {
            return at;
        }
    }
    return at;
}

/// Skip a bracketed attribute — `[numthreads(16, 2, 1)]` — from its opening bracket.
usize skip_attribute(std::string_view text, usize at) noexcept {
    usize depth = 0;
    for (; at < text.size(); ++at) {
        if (text[at] == '[') {
            ++depth;
        } else if (text[at] == ']') {
            --depth;
            if (depth == 0) {
                return at + 1;
            }
        }
    }
    return at;
}

/// The declared name after a `[shader("...")]` attribute: the identifier before the parameter list,
/// with any further attributes and the return type in between.
std::string declared_name(std::string_view text, usize at) noexcept {
    while (at < text.size()) {
        at = skip_trivia(text, at);
        if (at < text.size() && text[at] == '[') {
            at = skip_attribute(text, at);
            continue;
        }
        break;
    }
    const usize open = text.find('(', at);
    if (open == std::string_view::npos) {
        return {};
    }
    usize end = open;
    while (end > at && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    usize begin = end;
    while (begin > at && (std::isalnum(static_cast<unsigned char>(text[begin - 1])) != 0 ||
                          text[begin - 1] == '_')) {
        --begin;
    }
    return begin == end ? std::string() : std::string(text.substr(begin, end - begin));
}

/// Every `[shader("stage")] … name(` in one module. This is how Slang itself finds an entry point,
/// which is why it is what is read rather than a list somebody maintains beside the files.
std::vector<EntryPoint> entry_points_of(std::string_view text, usize module_index) {
    std::vector<EntryPoint> found;
    constexpr std::string_view kAttribute = "[shader(\"";
    for (usize at = text.find(kAttribute); at != std::string_view::npos;
         at = text.find(kAttribute, at + 1)) {
        const usize stage_begin = at + kAttribute.size();
        const usize stage_end = text.find('"', stage_begin);
        if (stage_end == std::string_view::npos) {
            continue;
        }
        const usize close = text.find(']', stage_end);
        if (close == std::string_view::npos) {
            continue;
        }
        std::string name = declared_name(text, close + 1);
        if (name.empty()) {
            continue;
        }
        found.push_back(EntryPoint{module_index, std::move(name),
                                   std::string(text.substr(stage_begin, stage_end - stage_begin))});
    }
    return found;
}

// --- The run -------------------------------------------------------------------------------------

struct CompilerHandle {
    shader::ShaderCompiler* handle = nullptr;
    shader::CompilerSelection selection;
    Allocator* allocator = nullptr;

    ~CompilerHandle() {
        if (handle != nullptr) {
            shader::destroy_compiler(*allocator, handle);
        }
    }
    CompilerHandle() = default;
    CompilerHandle(const CompilerHandle&) = delete;
    CompilerHandle& operator=(const CompilerHandle&) = delete;
};

[[nodiscard]] Status open_compiler(Allocator& allocator, CompilerHandle& out) noexcept {
    out.allocator = &allocator;
    // A STATEMENT RATHER THAN A LINK-ORDER PROPERTY, which is what the front end's header says this
    // function is for. The Slang back end registers itself from a static initialiser, and a static
    // initialiser in a static library is dropped by the linker when nothing in that object file is
    // referenced — so a tool that relied on it would report "no shader front end" on a build that
    // has one. That is not hypothetical: it is what this tool did before this line.
    if (Status registered = shader::slang::register_slang_backend(); !registered) {
        return registered;
    }
    auto created =
        shader::create_compiler(allocator, shader::kSlangBackendName, out.selection);
    if (!created) {
        return make_unexpected(created.error());
    }
    out.handle = created.value();
    return ok();
}

void write_artefact(std::string_view out_dir, const EntryPoint& entry, const Module& module,
                    const shader::TargetArtefact& artefact, std::FILE* out) {
    std::string stem(module.module_name.text());
    std::string path;
    path.reserve(out_dir.size() + stem.size() + entry.name.size() + 16);
    path.append(out_dir).append("/").append(stem).append(".").append(entry.name).append(
        shader::target_extension(artefact.target()));
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        std::fprintf(out, "  note: %s could not be written\n", path.c_str());
        return;
    }
    const Span<const u8> bytes = artefact.bytes();
    (void)std::fwrite(bytes.data(), 1, bytes.size(), file);
    (void)std::fclose(file);
}

void print_diagnostics(const shader::DiagnosticLog& log, std::FILE* out) {
    for (usize index = 0; index < log.size(); ++index) {
        const shader::Diagnostic entry = log.at(index);
        if (entry.severity == shader::Severity::Error) {
            std::fprintf(out, "      %s\n", entry.message);
        }
    }
}

}  // namespace

Status print_targets(Allocator& allocator, std::FILE* out) noexcept {
    CompilerHandle compiler;
    if (Status opened = open_compiler(allocator, compiler); !opened) {
        std::fprintf(out, "no shader front end: %s\n", opened.error().message);
        return opened;
    }

    std::fprintf(out, "front end: %s", compiler.handle->name());
    if (compiler.selection.fell_back) {
        std::fprintf(out, " (asked for %s: %s)", compiler.selection.requested,
                     compiler.selection.reason);
    }
    std::fprintf(out, ", version %s\n", compiler.handle->version());

    usize emitted = 0;
    std::string summary = "targets:";
    for (usize index = 0; index < kTargetCount; ++index) {
        const auto target = static_cast<Target>(index);
        const bool yes = compiler.handle->emits(target);
        emitted += yes ? 1 : 0;
        std::fprintf(out, "  %-13s %-6s %s\n", shader::target_name(target),
                     shader::target_short_name(target),
                     yes ? "emitted"
                         : "unavailable — the compiler for it did not answer on this machine");
        summary.append(" ").append(shader::target_name(target)).append("=").append(
            yes ? "emitted" : "unavailable");
    }
    // ONE PARSEABLE LINE, so a ledger criterion greps for a target being emitted rather than for
    // the word "msl" appearing somewhere in a table — which a stub could print.
    std::fprintf(out, "%s\n", summary.c_str());
    std::fprintf(out, "%zu of %zu targets emitted\n", emitted, kTargetCount);
    return emitted == 0 ? fail(ErrorCode::Unavailable, "this build emits no shader target") : ok();
}

Expected<Report, Error> build_shader_set(Allocator& allocator, const Options& options,
                                         std::FILE* out) noexcept {
    CompilerHandle compiler;
    if (Status opened = open_compiler(allocator, compiler); !opened) {
        return make_unexpected(opened.error());
    }
    if (!compiler.handle->compiles_source()) {
        std::fprintf(out,
                     "the '%s' front end compiles no source: this build has no shader toolchain\n",
                     compiler.handle->name());
        return fail(ErrorCode::Unavailable, "no shader front end in this build");
    }

    std::vector<Target> targets;
    for (usize index = 0; index < options.targets.size(); ++index) {
        targets.push_back(options.targets[index]);
    }
    if (targets.empty()) {
        for (usize index = 0; index < kTargetCount; ++index) {
            const auto target = static_cast<Target>(index);
            if (compiler.handle->emits(target)) {
                targets.push_back(target);
            }
        }
    }
    if (targets.size() < 2) {
        std::fprintf(out, "only %zu target(s) can be emitted: there is nothing to compare\n",
                     targets.size());
        return fail(ErrorCode::Unavailable, "fewer than two targets");
    }

    assets::VirtualFileSystem files;
    auto mount = assets::DirectoryMount::create(".", assets::MountKind::Project, false);
    if (!mount) {
        return make_unexpected(mount.error());
    }
    if (auto mounted = files.mount_owned(std::move(mount.value()), 0); !mounted) {
        return make_unexpected(mounted.error());
    }

    ModuleSet modules;
    for (usize index = 0; index < options.roots.size(); ++index) {
        if (Status loaded = modules.load(files, options.roots[index], out); !loaded) {
            std::fprintf(out, "  note: root '%.*s' could not be walked\n",
                         static_cast<int>(options.roots[index].size()),
                         options.roots[index].data());
        }
    }

    Report report;
    report.modules = modules.size();
    std::fprintf(out, "%zu module(s), targets:", report.modules);
    for (const Target target : targets) {
        std::fprintf(out, " %s", shader::target_name(target));
    }
    std::fprintf(out, "\n");

    for (usize index = 0; index < modules.size(); ++index) {
        const Module& module = modules.at(index);
        const std::vector<EntryPoint> entries = entry_points_of(module.text, index);
        if (entries.empty()) {
            ++report.modules_without_entry_points;
            continue;
        }
        for (const EntryPoint& entry : entries) {
            ++report.entry_points;
            std::fprintf(out, "%s  %s (%s)\n", module.path.c_str(), entry.name.c_str(),
                         entry.stage.c_str());

            shader::CompileRequest request;
            request.source.module_name = module.module_name;
            request.source.text = Span<const char>(module.text.data(), module.text.size());
            request.entry_point = Name::intern(entry.name);
            request.stage = stage_of(entry.stage);
            request.resolver = modules.resolver();

            std::vector<shader::TargetArtefact> artefacts;
            for (const Target target : targets) {
                shader::DiagnosticLog diagnostics(allocator);
                auto artefact = compiler.handle->compile_for(request, target, diagnostics);
                if (!artefact) {
                    ++report.failures;
                    std::fprintf(out, "    %-13s FAILED: %s\n", shader::target_name(target),
                                 artefact.error().message);
                    print_diagnostics(diagnostics, out);
                    continue;
                }
                ++report.artefacts;
                if (options.verbose) {
                    std::fprintf(out, "    %-13s %zu bytes, %zu parameter(s)\n",
                                 shader::target_name(target), artefact->bytes().size(),
                                 artefact->parameters().size());
                }
                if (!options.out_dir.empty()) {
                    write_artefact(options.out_dir, entry, module, *artefact, out);
                }
                artefacts.push_back(std::move(artefact.value()));
            }

            // EVERY PAIR, not every artefact against the first: a disagreement between the second
            // and the third target is a disagreement, and comparing everything against SPIR-V would
            // not see it.
            for (usize left = 0; left < artefacts.size(); ++left) {
                for (usize right = left + 1; right < artefacts.size(); ++right) {
                    ++report.comparisons;
                    shader::DiagnosticLog differences(allocator);
                    if (shader::interfaces_agree(artefacts[left], artefacts[right], differences)) {
                        std::fprintf(out, "    %s = %s: %zu parameter(s), entry '%s'\n",
                                     shader::target_name(artefacts[left].target()),
                                     shader::target_name(artefacts[right].target()),
                                     artefacts[left].parameters().size(),
                                     artefacts[left].entry_point().c_str());
                        continue;
                    }
                    ++report.disagreements;
                    std::fprintf(out, "    %s != %s\n",
                                 shader::target_name(artefacts[left].target()),
                                 shader::target_name(artefacts[right].target()));
                    print_diagnostics(differences, out);
                }
            }
        }
    }

    // ONE PARSEABLE LINE, for the reason `print_targets` writes one: a criterion that asserts a
    // floor on the comparisons cannot be satisfied by a run that compared nothing.
    std::fprintf(out,
                 "\nshader-set: modules=%zu entry_points=%zu artefacts=%zu comparisons=%zu "
                 "disagreements=%zu failures=%zu modules_without_entry_points=%zu\n",
                 report.modules, report.entry_points, report.artefacts, report.comparisons,
                 report.disagreements, report.failures, report.modules_without_entry_points);
    return report;
}

}  // namespace cy::shadertool
