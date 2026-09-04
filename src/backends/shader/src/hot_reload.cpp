// Shader hot reload over cy::assets::FileWatcher. Task 3.5.
//
// `poll` detects and queues; `rebuild` compiles and swaps. See the header for why the two are
// separate calls rather than one.

#include <cy/backends/shader/hot_reload.h>

#include <algorithm>

namespace cy::shader {

ShaderHotReload::ShaderHotReload(Allocator& allocator) noexcept
    : allocator_(&allocator), watcher_(allocator), pending_(allocator) {}

Status ShaderHotReload::start(SourceRegistry& sources, assets::VirtualFileSystem& files,
                              const HotReloadConfig& config) noexcept {
    sources_ = &sources;
    config_ = config;
    if (Status started = watcher_.start(files, config.watcher); !started) {
        sources_ = nullptr;
        return started;
    }
    // The registry's root is the tree being iterated on. A directory watch is recursive and reports
    // files, so one watch covers a shader tree of any depth.
    if (Status watched = watcher_.watch(sources.root()); !watched) {
        watcher_.stop();
        sources_ = nullptr;
        return watched;
    }
    return ok();
}

void ShaderHotReload::stop() noexcept {
    watcher_.stop();
    pending_.clear();
    sources_ = nullptr;
}

Status ShaderHotReload::prime(i64 now_ns) noexcept {
    if (sources_ == nullptr) {
        return fail(ErrorCode::Unavailable, "shader hot reload has not been started");
    }
    return watcher_.prime(now_ns);
}

void ShaderHotReload::set_observer(ShaderReloadObserver observer, void* user) noexcept {
    observer_ = observer;
    observer_user_ = user;
}

Span<const Name> ShaderHotReload::pending() const noexcept {
    return {pending_.data(), pending_.size()};
}

bool ShaderHotReload::is_pending(Name module_name) const noexcept {
    return std::ranges::any_of(pending_,
                               [module_name](Name name) noexcept { return name == module_name; });
}

Status ShaderHotReload::queue(Name module_name) noexcept {
    if (is_pending(module_name)) {
        return ok();
    }
    if (pending_.size() >= config_.max_pending) {
        // The oldest entries stand. Dropping the *new* edit would be the wrong half to lose: the
        // developer is looking at the file they just saved.
        ++stats_.overflowed;
        return fail(ErrorCode::OutOfRange, "the shader reload queue is full");
    }
    if (Status pushed = pending_.push_back(module_name); !pushed) {
        return pushed;
    }
    ++stats_.modules_queued;
    return ok();
}

Status ShaderHotReload::invalidate(Name module_name) noexcept {
    return queue(module_name);
}

void ShaderHotReload::handle_event(const assets::WatchEvent& event) noexcept {
    ++stats_.events;
    if (event.path == nullptr) {
        return;
    }
    Expected<Name, Error> module_name = sources_->module_for(*event.path);
    if (!module_name) {
        // A file under the shader root that is not a `.slang` module: a README, an editor's
        // swap file. Not an error and not a reload.
        return;
    }

    ShaderReloadEvent reload;
    reload.module_name = module_name.value();
    reload.change = event.change;

    if (event.change == assets::FileChange::Removed) {
        // A deleted module is dropped from the registry; the library keeps its last good artefact,
        // because a file that is gone is usually a file being moved.
        (void)sources_->remove(module_name.value());
    } else {
        reload.source_reloaded = sources_->reload(module_name.value()).has_value();
        if (reload.source_reloaded) {
            if (Status queued = queue(module_name.value()); !queued) {
                sweep_status_ = queued;
            }
        }
    }

    if (observer_ != nullptr) {
        observer_(observer_user_, reload);
    }
}

void ShaderHotReload::on_watch_event(void* user, const assets::WatchEvent& event) noexcept {
    static_cast<ShaderHotReload*>(user)->handle_event(event);
}

Expected<u32, Error> ShaderHotReload::poll(i64 now_ns) noexcept {
    if (sources_ == nullptr) {
        return fail(ErrorCode::Unavailable, "shader hot reload has not been started");
    }
    ++stats_.polls;
    const usize before = pending_.size();
    sweep_status_ = ok();

    Expected<u32, Error> reported = watcher_.poll(now_ns, &ShaderHotReload::on_watch_event, this);
    if (!reported) {
        return reported;
    }
    if (!sweep_status_) {
        return make_unexpected(sweep_status_.error());
    }
    return static_cast<u32>(pending_.size() - before);
}

Expected<u32, Error> ShaderHotReload::rebuild_module(Name module_name, ShaderCompiler& compiler,
                                                     ShaderLibrary& library,
                                                     const RebuildOptions& options,
                                                     DiagnosticLog& diagnostics) noexcept {
    SourceUnit source;
    if (!sources_->find(module_name, source)) {
        return fail(ErrorCode::NotFound, "the module to rebuild is no longer in the registry");
    }

    u32 replaced = 0;
    // Every variant of this module. Walking the library rather than keeping a module->variant index
    // is deliberate at this scale: a library holds thousands of variants, a rebuild happens when a
    // person saves a file, and an index would be a second thing to keep in step with `replace`.
    for (u32 index = 0; index < library.variant_count(); ++index) {
        const ShaderVariantId id{index};
        const VariantKey* key = library.key_at(id);
        if (key == nullptr || key->module_name != module_name) {
            continue;
        }

        CompileRequest request;
        request.source = source;
        request.entry_point = key->entry_point;
        request.stage = key->stage;
        request.permutations = options.permutations(module_name);
        request.permutation = key->permutation;
        request.optimization = options.optimization;
        request.debug_info = options.debug_info;
        request.spirv_version = options.spirv_version;
        request.resolver = sources_->resolver();

        Expected<CompiledShader, Error> compiled = compiler.compile(request, diagnostics);
        if (!compiled) {
            // The library is untouched and the old artefact is still what everything is using.
            return make_unexpected(compiled.error());
        }

        const assets::ContentHash previous = library.shader_at(id) != nullptr
                                                 ? library.shader_at(id)->hash()
                                                 : assets::ContentHash{};
        const assets::ContentHash rebuilt = compiled.value().hash();
        if (Status swapped = library.replace(id, std::move(compiled.value())); !swapped) {
            return make_unexpected(swapped.error());
        }
        ++replaced;

        if (options.pipelines != nullptr && !(previous == rebuilt)) {
            stats_.pipelines_invalidated += options.pipelines->invalidate_by_program(previous);
        }
    }
    return replaced;
}

Expected<RebuildResult, Error> ShaderHotReload::rebuild(ShaderCompiler& compiler,
                                                        ShaderLibrary& library,
                                                        const RebuildOptions& options,
                                                        DiagnosticLog& diagnostics) noexcept {
    if (sources_ == nullptr) {
        return fail(ErrorCode::Unavailable, "shader hot reload has not been started");
    }

    RebuildResult result;
    const u64 invalidated_before = stats_.pipelines_invalidated;
    while (!pending_.empty()) {
        // Taken from the front so edits are rebuilt in the order they were made; a module leaves
        // the queue whether it compiled or not, so a broken shader is reported once per edit rather
        // than once per frame.
        const Name module_name = pending_[0];
        pending_.erase(0);

        Expected<u32, Error> replaced =
            rebuild_module(module_name, compiler, library, options, diagnostics);
        if (!replaced) {
            ++result.failures;
            ++stats_.rebuild_failures;
            continue;
        }
        ++result.modules_rebuilt;
        ++stats_.modules_rebuilt;
        result.variants_replaced += replaced.value();
        stats_.variants_replaced += replaced.value();
    }
    result.pipelines_invalidated =
        static_cast<u32>(stats_.pipelines_invalidated - invalidated_before);
    return result;
}

}  // namespace cy::shader
