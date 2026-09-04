// The source registry: authored modules read through the virtual filesystem, generated modules
// published by an engine generator, and the resolver a compiler front end sees. Tasks 3.1 and 3.8.

#include <cy/backends/shader/source.h>

#include <cy/core/assets/path.h>

#include <cstring>

namespace cy::shader {
namespace {

/// `cy.brdf` -> `cy/brdf.slang`, written into `out`. Returns the length, or zero when the name does
/// not fit — a module name at the path limit is a name that took a wrong turn.
[[nodiscard]] usize module_to_relative_path(std::string_view module_name, char* out,
                                            usize capacity) noexcept {
    const usize length = module_name.size() + kSourceExtension.size();
    if (module_name.empty() || length >= capacity) {
        return 0;
    }
    for (usize index = 0; index < module_name.size(); ++index) {
        const char c = module_name[index];
        out[index] = (c == kModuleSeparator) ? '/' : c;
    }
    std::memcpy(out + module_name.size(), kSourceExtension.data(), kSourceExtension.size());
    out[length] = '\0';
    return length;
}

}  // namespace

const char* source_origin_name(SourceOrigin origin) noexcept {
    switch (origin) {
        case SourceOrigin::Authored:
            return "authored";
        case SourceOrigin::Generated:
            return "generated";
    }
    return "unknown";
}

SourceRegistry::SourceRegistry(Allocator& allocator) noexcept
    : allocator_(&allocator), units_(allocator), scratch_(allocator) {}

Status SourceRegistry::start(assets::VirtualFileSystem& files,
                             const assets::VirtualPath& root) noexcept {
    files_ = &files;
    root_ = root;
    return ok();
}

void SourceRegistry::stop() noexcept {
    files_ = nullptr;
    root_ = assets::VirtualPath{};
}

Expected<assets::VirtualPath, Error> SourceRegistry::path_for(Name module_name) const noexcept {
    char relative[assets::kMaxPathLength + 1] = {};
    const usize length = module_to_relative_path(module_name.text(), relative, sizeof(relative));
    if (length == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a shader module name is empty or longer than a virtual path");
    }
    if (root_.empty()) {
        return assets::VirtualPath::normalise(std::string_view(relative, length));
    }
    return root_.join(std::string_view(relative, length));
}

Expected<Name, Error> SourceRegistry::module_for(const assets::VirtualPath& path) const noexcept {
    if (path.extension() != kSourceExtension) {
        return fail(ErrorCode::InvalidArgument,
                    "a shader source path ends in .slang; the engine authors one language");
    }
    std::string_view relative = path.view();
    if (!root_.empty()) {
        if (!path.is_within(root_)) {
            return fail(ErrorCode::NotFound, "the path is outside the shader source root");
        }
        // `is_within` is segment-aware, so the separator after the root is always there unless the
        // path *is* the root, which cannot happen for a path with an extension.
        relative = relative.substr(root_.size() + 1);
    }
    relative = relative.substr(0, relative.size() - kSourceExtension.size());
    if (relative.empty()) {
        return fail(ErrorCode::InvalidArgument, "a shader source path names no module");
    }

    char name[assets::kMaxPathLength + 1] = {};
    for (usize index = 0; index < relative.size(); ++index) {
        name[index] = (relative[index] == '/') ? kModuleSeparator : relative[index];
    }
    return Name::intern(std::string_view(name, relative.size()));
}

usize SourceRegistry::find_index(Name module_name) const noexcept {
    for (usize index = 0; index < units_.size(); ++index) {
        if (units_[index].module_name == module_name) {
            return index;
        }
    }
    return units_.size();
}

SourceUnit SourceRegistry::unit_of(const Entry& entry) noexcept {
    SourceUnit unit;
    unit.module_name = entry.module_name;
    unit.path = entry.path;
    unit.origin = entry.origin;
    unit.generator = entry.generator;
    unit.hash = entry.hash;
    unit.text = Span<const char>(entry.text.data(), entry.text.size());
    return unit;
}

Expected<usize, Error> SourceRegistry::store(Name module_name, const assets::VirtualPath& path,
                                             SourceOrigin origin, Name generator,
                                             std::string_view text) noexcept {
    usize index = find_index(module_name);
    if (index == units_.size()) {
        Expected<Entry*, Error> slot = units_.emplace_back(Entry{
            module_name, path, origin, generator, assets::ContentHash{}, Array<char>(*allocator_)});
        if (!slot) {
            return make_unexpected(slot.error());
        }
    } else {
        units_[index].path = path;
        units_[index].origin = origin;
        units_[index].generator = generator;
        units_[index].text.clear();
    }

    Entry& entry = units_[index];
    if (Status appended = entry.text.append(Span<const char>(text.data(), text.size()));
        !appended) {
        return make_unexpected(appended.error());
    }
    entry.hash = assets::content_hash(entry.text.data(), entry.text.size());
    return index;
}

Expected<SourceUnit, Error> SourceRegistry::load(Name module_name) noexcept {
    const usize index = find_index(module_name);
    if (index != units_.size()) {
        return unit_of(units_[index]);
    }
    return reload(module_name);
}

Expected<SourceUnit, Error> SourceRegistry::reload(Name module_name) noexcept {
    if (files_ == nullptr) {
        return fail(ErrorCode::Unavailable,
                    "the shader source registry has no filesystem; only generated modules resolve");
    }
    const usize existing = find_index(module_name);
    if (existing != units_.size() && units_[existing].origin == SourceOrigin::Generated) {
        // A generated module has no file behind it. Re-reading one would silently replace it with
        // whatever happens to sit at the path its name maps to, which is a defect that would only
        // show up as a material rendering somebody else's shader.
        return fail(ErrorCode::InvalidArgument,
                    "a generated shader module has no file to reload; re-publish it instead");
    }

    Expected<assets::VirtualPath, Error> path = path_for(module_name);
    if (!path) {
        return make_unexpected(path.error());
    }
    if (Status read = files_->read(path.value(), scratch_); !read) {
        return make_unexpected(read.error());
    }

    const std::string_view text(reinterpret_cast<const char*>(scratch_.data()), scratch_.size());
    Expected<usize, Error> index =
        store(module_name, path.value(), SourceOrigin::Authored, Name{}, text);
    if (!index) {
        return make_unexpected(index.error());
    }
    return unit_of(units_[index.value()]);
}

Expected<SourceUnit, Error> SourceRegistry::add_generated(Name module_name, Name generator,
                                                          std::string_view text) noexcept {
    if (module_name.is_empty()) {
        return fail(ErrorCode::InvalidArgument, "a generated shader module must be named");
    }
    if (generator.is_empty()) {
        // An anonymous generated module is one whose bad diagnostic nobody can attribute. See the
        // "The boundary is explicit" scenario in `shader-system`.
        return fail(ErrorCode::InvalidArgument,
                    "generated shader source must name the generator that produced it");
    }
    Expected<usize, Error> index =
        store(module_name, assets::VirtualPath{}, SourceOrigin::Generated, generator, text);
    if (!index) {
        return make_unexpected(index.error());
    }
    return unit_of(units_[index.value()]);
}

bool SourceRegistry::find(Name module_name, SourceUnit& out) const noexcept {
    const usize index = find_index(module_name);
    if (index == units_.size()) {
        return false;
    }
    out = unit_of(units_[index]);
    return true;
}

bool SourceRegistry::contains(Name module_name) const noexcept {
    return find_index(module_name) != units_.size();
}

Status SourceRegistry::remove(Name module_name) noexcept {
    const usize index = find_index(module_name);
    if (index == units_.size()) {
        return fail(ErrorCode::NotFound, "no such shader module");
    }
    units_.erase(index);
    return ok();
}

void SourceRegistry::clear() noexcept {
    units_.clear();
}

SourceUnit SourceRegistry::unit_at(usize index) const noexcept {
    CY_ASSERT_MSG(index < units_.size(), "SourceRegistry::unit_at() past the end");
    return unit_of(units_[index]);
}

bool SourceRegistry::resolve_thunk(void* user, std::string_view module_name,
                                   SourceUnit& out) noexcept {
    auto* registry = static_cast<SourceRegistry*>(user);
    if (module_name.empty()) {
        return false;
    }
    // Interned rather than looked up. An authored module that has not been read yet has never had
    // its name interned, and a `find` would refuse to resolve the first `import` of every module in
    // the project — which is exactly what it did before this line said `intern`. The table grows by
    // one entry per distinct import name, which is bounded by the project's module count and by the
    // number of distinct misspellings in its source, both of which are small.
    const Name name = Name::intern(module_name);
    const usize index = registry->find_index(name);
    if (index != registry->units_.size()) {
        out = registry->unit_of(registry->units_[index]);
        return true;
    }
    Expected<SourceUnit, Error> loaded = registry->load(name);
    if (!loaded) {
        return false;
    }
    out = loaded.value();
    return true;
}

SourceResolver SourceRegistry::resolver() noexcept {
    SourceResolver resolver;
    resolver.resolve = &SourceRegistry::resolve_thunk;
    resolver.user = this;
    return resolver;
}

}  // namespace cy::shader
