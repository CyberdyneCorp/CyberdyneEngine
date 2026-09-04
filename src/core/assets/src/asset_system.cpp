// Asynchronous asset loading over the job system. Task 3.3.4.
//
// Read the note at the top of asset_system.h first: it states the rule that shapes this file — a
// worker never blocks — and where each stage is therefore allowed to run.
//
// THE SHAPE OF ONE LOAD, in the order it happens:
//
//   begin_load          the caller's thread, under the mutex. Finds or creates the slot for this
//                       (id, variant). An existing slot is RIDDEN, which is how two requests for
//                       one asset perform one load.
//   completion          a GATED job, created with the slot. Everything that must finish before the
//                       asset is ready ends by signalling it, and a waiter waits on it.
//   read_operation      the AsyncService thread. Resolves the path, finds the package entry, and
//                       reads the STORED bytes — still compressed. The only blocking call in the
//                       whole load, on the one thread where blocking is legal.
//   process             a worker, gated on the read's handle. Decompresses, verifies, publishes.
//   dependencies        started by `process`, from the entry's own dependency list. A `finish` job
//                       gated on their completions signals this slot's, so a material reports
//                       complete only when its textures are.
//
// Each of those begins by asking the cancellation token. That is what "stops at the next stage
// boundary" means, and it is why the check is at the top of a stage rather than sprinkled inside
// one.

#include <cy/core/assets/asset_system.h>

#include <cy/core/assets/diagnostics.h>
#include <cy/core/base/assert.h>
#include <cy/core/memory/scope.h>
#include <cy/core/memory/system_allocator.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

namespace cy::assets {
namespace {

i64 monotonic_now_ns() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

}  // namespace

const char* load_stage_name(LoadStage stage) noexcept {
    switch (stage) {
        case LoadStage::Queued:
            return "queued";
        case LoadStage::Reading:
            return "reading";
        case LoadStage::Decompressing:
            return "decompressing";
        case LoadStage::Deserializing:
            return "deserializing";
        case LoadStage::Uploading:
            return "uploading";
        case LoadStage::Complete:
            return "complete";
        case LoadStage::Failed:
            return "failed";
        case LoadStage::Cancelled:
            return "cancelled";
    }
    return "unknown";
}

namespace detail {

/// One (id, variant) the system knows about: in flight, resident, or retired.
struct AssetSlot {
    cy::AssetId id;
    VariantKey variant;
    AssetKind expected_kind = AssetKind::Unknown;
    cy::AssetId referrer;

    AssetData* data = nullptr;  ///< published payload; the slot holds no reference to it
    LoadStage stage = LoadStage::Queued;
    Error error{};

    bool loading = false;
    bool published = false;
    /// Nothing holds a `Ref` any more and the retention policy is keeping it anyway.
    bool retired = false;
    /// Being dropped on purpose: the next last-release destroys rather than retiring again.
    bool evicting = false;
    i64 retired_at_ns = 0;
    u64 last_use = 0;
    usize bytes = 0;

    /// `preload` holds, and the reference the system keeps until a request is forgotten.
    Ref<AssetData> hold;
    Ref<AssetData> pending;

    jobs::JobHandle completion;
    jobs::CancellationSource cancellation;
    /// How many live requests name this slot. A cancel only cancels when it reaches zero.
    u32 request_refs = 0;
    u32 index = 0;

    // --- staging, written by the I/O thread and read by the process job ---------------------
    AssetSystemImpl* owner = nullptr;
    PackageMount* package = nullptr;
    const PackageEntry* entry = nullptr;
    Array<u8> stored;
    Array<cy::AssetId> dependencies;
    Span<const u8> mapped;
    bool have_mapping = false;
    Status read_status = ok();
    jobs::JobHandle read_handle;
};

struct LoadRequestRecord {
    LoadRequestId id = kInvalidRequest;
    Array<u32> slots;
    bool cancelled = false;
};

struct AssetSystemImpl {
    jobs::JobSystem* jobs = nullptr;
    jobs::AsyncService* async = nullptr;
    VirtualFileSystem* files = nullptr;
    AssetSystemConfig config;
    bool running = false;

    mutable std::mutex mutex;
    Array<UniquePtr<AssetSlot>> slots;
    Array<UniquePtr<LoadRequestRecord>> requests;
    LoadRequestId next_request = 1;
    u64 use_clock = 1;

    AssetSystemStats stats;

    /// Payloads a reload replaced, freed at the next `update()` rather than at the swap. A reader
    /// that took `AssetData::bytes()` before the swap holds a span into one of these; see
    /// `AssetSystem::reload`.
    Array<Array<u8>> retired_payloads{default_allocator()};

    struct ReloadListener {
        ReloadObserver observer = nullptr;
        void* user = nullptr;
    };
    Array<ReloadListener> reload_listeners{default_allocator()};

    // --- slot lookup -----------------------------------------------------------------------

    [[nodiscard]] AssetSlot* find_slot(cy::AssetId id, VariantKey variant) noexcept {
        for (UniquePtr<AssetSlot>& slot : slots) {
            if (slot->id == id && slot->variant == variant) {
                return slot.get();
            }
        }
        return nullptr;
    }

    [[nodiscard]] LoadRequestRecord* find_request(LoadRequestId id) const noexcept {
        for (const UniquePtr<LoadRequestRecord>& request : requests) {
            if (request->id == id) {
                return request.get();
            }
        }
        return nullptr;
    }

    // --- the asset object ------------------------------------------------------------------

    static void release_asset(RefCounted* object, Allocator* allocator) noexcept;

    void on_last_reference(AssetData* data, Allocator* allocator) noexcept;

    static void destroy_asset(AssetData* data, Allocator* allocator) noexcept {
        data->~AssetData();
        allocator->deallocate(static_cast<void*>(data), sizeof(AssetData), alignof(AssetData));
    }

    /// Take a retired slot back into ordinary use, and stamp it as recently used.
    ///
    /// A RETIRED SLOT'S ASSET IS HELD BY NOBODY. `on_last_reference` resurrected it — raised the
    /// count from zero back to one — so that the retention policy could keep it, and that one
    /// reference is owned by a *flag* rather than by a `Ref`. Reviving must therefore MOVE it into
    /// `pending`, not merely clear the flag: clearing the flag alone leaves a reference nothing
    /// will ever release, which is a leak that only a sanitiser sees. Called with the mutex held.
    void revive(AssetSlot& slot) noexcept {
        if (slot.retired && slot.data != nullptr) {
            slot.pending = Ref<AssetData>::adopt(slot.data);
            slot.retired = false;
        }
        slot.last_use = use_clock++;
    }

    /// Replace a resident asset's bytes in the object every `Ref` already points at. Called with
    /// the mutex held, by `AssetSystem::reload`, which is where the whole argument lives.
    ///
    /// A member of the impl rather than of `AssetSystem` because `AssetData`'s storage is private
    /// and this struct is its friend — the same reason `publish` lives here.
    [[nodiscard]] Status apply_reload(AssetSlot& slot, Array<u8>&& replacement,
                                      ReloadEvent& event) noexcept;

    /// Build and publish the payload for a slot. Called with the mutex held.
    [[nodiscard]] Status publish(AssetSlot& slot, Array<u8>&& payload, Span<const u8> mapped,
                                 AssetKind kind, bool placeholder) noexcept;

    // --- the stages ------------------------------------------------------------------------

    static void read_operation(void* user) noexcept;
    static void process_job(const jobs::TaskContext& context, void* user) noexcept;
    static void finish_job(const jobs::TaskContext& context, void* user) noexcept;

    void complete_slot(AssetSlot& slot, LoadStage stage) noexcept;

    [[nodiscard]] Expected<u32, Error> begin_load(cy::AssetId id, const LoadOptions& options,
                                                  bool is_dependency) noexcept;

    /// Drop the references in `victims`. MUST be called with the mutex NOT held: the last release
    /// re-enters `on_last_reference`, which takes it.
    static void evict(std::vector<Ref<AssetData>>& victims) noexcept { victims.clear(); }

    /// Cut every still-published asset loose from this system.
    ///
    /// Shutdown releases the system's own references, but a caller may still be holding one — a
    /// `Ref` in a local variable that outlives the system it came from. Clearing `owner_` makes
    /// that object release through the plain destroy path instead of calling back into a system
    /// that no longer exists, which turns a use-after-free into an ordinary deallocation.
    void detach_survivors() noexcept {
        for (UniquePtr<AssetSlot>& slot : slots) {
            if (slot->data != nullptr) {
                slot->data->owner_ = nullptr;
            }
        }
    }
};

void AssetSystemImpl::release_asset(RefCounted* object, Allocator* allocator) noexcept {
    // A downcast, and a safe one: this function is installed by `publish()` as the release policy
    // of an object it has just constructed as an AssetData, so `object` cannot be anything else.
    // The engine builds with -fno-rtti, so a checked cast does not exist to prefer.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto* data = static_cast<AssetData*>(object);
    AssetSystemImpl* owner = data->owner_;
    if (owner == nullptr) {
        destroy_asset(data, allocator);
        return;
    }
    owner->on_last_reference(data, allocator);
}

void AssetSystemImpl::on_last_reference(AssetData* data, Allocator* allocator) noexcept {
    std::lock_guard<std::mutex> guard(mutex);

    AssetSlot* slot = data->slot_ < slots.size() ? slots[data->slot_].get() : nullptr;
    const bool keep = slot != nullptr && !slot->evicting && running &&
                      config.retention != RetentionPolicy::Immediate;

    if (keep) {
        // Resurrect. The count is zero and only this system can raise it: every path that hands out
        // a `Ref` goes through the mutex this function holds, so nothing can be racing a retain.
        data->add_ref();
        slot->retired = true;
        slot->retired_at_ns = monotonic_now_ns();
        return;
    }

    if (slot != nullptr) {
        slot->data = nullptr;
        slot->published = false;
        slot->retired = false;
        slot->evicting = false;
        stats.resident_bytes -= slot->bytes;
        stats.resident_assets -= 1;
        slot->bytes = 0;
        ++stats.evictions;
    }
    destroy_asset(data, allocator);
}

Status AssetSystemImpl::apply_reload(AssetSlot& slot, Array<u8>&& replacement,
                                     ReloadEvent& event) noexcept {
    AssetData& data = *slot.data;
    event.previous_bytes = data.bytes_.size();
    event.bytes = replacement.size();

    // Room for the retired buffer FIRST, before anything is moved. A push_back that fails after the
    // old payload has been moved out of the asset would leave the asset holding nothing while this
    // function reports that it changed nothing — the one outcome worse than a failed reload.
    if (Status room = retired_payloads.reserve(retired_payloads.size() + 1); !room) {
        return room;
    }

    // The swap. Every `Ref` points at this AssetData and keeps pointing at it; what changes is what
    // it holds. The previous buffer is retired rather than dropped, because a reader may hold a
    // span into it — see AssetSystem::reload.
    if (Status retired = retired_payloads.push_back(std::move(data.storage_)); !retired) {
        return retired;
    }
    data.storage_ = std::move(replacement);
    data.bytes_ = data.storage_.span();
    data.mapped_ = false;
    data.placeholder_ = false;

    stats.resident_bytes -= slot.bytes;
    slot.bytes = data.bytes_.size();
    stats.resident_bytes += slot.bytes;
    revive(slot);
    ++stats.reloads_completed;
    return ok();
}

Status AssetSystemImpl::publish(AssetSlot& slot, Array<u8>&& payload, Span<const u8> mapped,
                                AssetKind kind, bool placeholder) noexcept {
    Allocator& allocator = default_allocator();
    void* storage = allocator.allocate(sizeof(AssetData), alignof(AssetData));
    if (storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "an asset could not be allocated");
    }
    auto* data = construct_at<AssetData>(storage);
    data->owner_ = this;
    data->slot_ = slot.index;
    data->id_ = slot.id;
    data->variant_ = slot.variant;
    data->kind_ = kind;
    data->placeholder_ = placeholder;
    data->mapped_ = !mapped.empty();
    data->storage_ = std::move(payload);
    data->bytes_ = mapped.empty() ? data->storage_.span() : mapped;
    data->set_release_policy(&AssetSystemImpl::release_asset, &allocator);

    slot.data = data;
    slot.published = true;
    slot.retired = false;
    slot.bytes = data->bytes_.size();
    slot.last_use = use_clock++;
    stats.resident_assets += 1;
    stats.resident_bytes += slot.bytes;
    if (placeholder) {
        ++stats.placeholders_served;
    }
    if (data->mapped_) {
        ++stats.entries_mapped;
    }

    // The system keeps the initial reference until the request that started the load is forgotten,
    // so a load whose result is never taken does not evaporate before it can be.
    slot.pending = Ref<AssetData>::adopt(data);
    return ok();
}

void AssetSystemImpl::complete_slot(AssetSlot& slot, LoadStage stage) noexcept {
    slot.stage = stage;
    slot.loading = false;
    slot.stored.clear();
    switch (stage) {
        case LoadStage::Complete:
            ++stats.loads_completed;
            break;
        case LoadStage::Cancelled:
            ++stats.loads_cancelled;
            break;
        case LoadStage::Failed:
            ++stats.loads_failed;
            break;
        default:
            break;
    }
}

void AssetSystemImpl::read_operation(void* user) noexcept {
    auto* slot = static_cast<AssetSlot*>(user);
    AssetSystemImpl& self = *slot->owner;

    // Stage boundary: a request cancelled before the read started never opens the file.
    if (slot->cancellation.is_cancelled()) {
        slot->read_status = fail(ErrorCode::Unavailable, "the load was cancelled before the read");
        return;
    }
    {
        std::lock_guard<std::mutex> guard(self.mutex);
        slot->stage = LoadStage::Reading;
    }

    Expected<VirtualPath, Error> path = package_entry_path(slot->id, slot->variant);
    if (!path) {
        slot->read_status = make_unexpected(path.error());
        return;
    }
    Expected<VirtualFileSystem::Resolution, Error> resolved = self.files->resolve(path.value());
    if (!resolved) {
        slot->read_status = make_unexpected(resolved.error());
        return;
    }

    PackageMount* package = resolved.value().source->as_package();
    if (package == nullptr) {
        // A loose file — a memory mount, the project directory, a remote host. There is no chunk
        // and no framing, so the decompress stage has nothing to do and says so.
        Array<u8> whole;
        Status read = self.files->read(path.value(), whole);
        if (!read) {
            slot->read_status = read;
            return;
        }
        slot->stored = std::move(whole);
        slot->entry = nullptr;
        slot->package = nullptr;
        slot->read_status = ok();
        return;
    }

    const PackageEntry* entry = package->entry_for(path.value());
    if (entry == nullptr || entry->is_deleted()) {
        slot->read_status = fail(ErrorCode::NotFound, "no such entry in the mounted packages");
        return;
    }
    slot->package = package;
    slot->entry = entry;

    if (Status recorded = slot->dependencies.resize(0); !recorded) {
        slot->read_status = recorded;
        return;
    }
    for (const cy::AssetId dependency : package->reader().dependencies(*entry)) {
        if (Status added = slot->dependencies.push_back(dependency); !added) {
            slot->read_status = added;
            return;
        }
    }

    const PackageChunk* chunk = package->reader().chunk_of(*entry);
    if (chunk == nullptr) {
        slot->read_status = fail(ErrorCode::NotFound,
                                 "the entry's payload is not in this package; it names a chunk "
                                 "another package holds");
        return;
    }

    // A stored, aligned chunk in a mapped package is served without a copy: the specification's
    // memory-mapped read. Nothing is read from disk here at all, which is the point of it.
    if (Expected<Span<const u8>, Error> view = package->reader().map_entry(*entry); view) {
        slot->mapped = view.value();
        slot->have_mapping = true;
        slot->read_status = ok();
        return;
    }

    slot->read_status = package->reader().read_chunk_stored(*chunk, slot->stored);
}

void AssetSystemImpl::process_job(const jobs::TaskContext& context, void* user) noexcept {
    (void)context;
    auto* slot = static_cast<AssetSlot*>(user);
    AssetSystemImpl& self = *slot->owner;

    // Stage boundary. Partial results — the bytes the read produced — are released here, which is
    // what "release any partial results" means for a load that got as far as reading.
    if (slot->cancellation.is_cancelled()) {
        std::lock_guard<std::mutex> guard(self.mutex);
        slot->stored.clear();
        self.complete_slot(*slot, LoadStage::Cancelled);
        (void)self.jobs->signal(slot->completion);
        return;
    }

    // Captured before the branches below: one of them moves the stored bytes into `payload`, and a
    // counter read after that would report zero for a loose file and the truth for a package entry.
    const usize stored_bytes = slot->stored.size();

    Array<u8> payload;
    Span<const u8> mapped;
    AssetKind kind = slot->expected_kind;
    bool placeholder = false;
    Status outcome = ok();

    if (!slot->read_status) {
        // `core-assets-and-io` — "Source deleted": a reference whose asset is gone yields a TYPED
        // placeholder and a diagnostic naming the referrer, rather than failing the whole load.
        if (slot->read_status.error().code == ErrorCode::NotFound) {
            placeholder = true;
            kind = slot->expected_kind;
            assets_log_missing_asset(slot->id, slot->referrer, kind);
        } else {
            outcome = slot->read_status;
        }
    } else if (slot->have_mapping) {
        mapped = slot->mapped;
        kind = slot->entry != nullptr ? slot->entry->kind : slot->expected_kind;
    } else if (slot->entry != nullptr) {
        {
            std::lock_guard<std::mutex> guard(self.mutex);
            slot->stage = LoadStage::Decompressing;
        }
        const PackageChunk* chunk = slot->package->reader().chunk_of(*slot->entry);
        if (chunk == nullptr) {
            outcome = fail(ErrorCode::Internal, "the entry's chunk vanished between stages");
        } else if (Status grown = payload.resize(static_cast<usize>(chunk->uncompressed_size));
                   !grown) {
            outcome = grown;
        } else if (!payload.empty()) {
            // The stored bytes are already in memory, so the "reader" the codec calls is a memcpy
            // out of them. Reading and decompressing are still two stages: this one runs on a
            // worker while the I/O thread has already moved on to the next request's read.
            struct Source {
                const Array<u8>* stored;
            } source{&slot->stored};
            outcome = decompress_range(
                chunk->method, slot->package->reader().frames_of(*chunk), chunk->frame_bytes,
                [](void* state, u64 offset, void* destination, usize size) noexcept -> Status {
                    const Array<u8>& bytes = *static_cast<Source*>(state)->stored;
                    if (offset + size > bytes.size()) {
                        return fail(ErrorCode::OutOfRange, "a frame lies past the stored payload");
                    }
                    std::memcpy(destination, bytes.data() + offset, size);
                    return ok();
                },
                &source, 0, payload.data(), payload.size(), nullptr);
        }
        kind = slot->entry->kind;
    } else {
        payload = std::move(slot->stored);
        kind = slot->expected_kind;
    }

    if (outcome) {
        std::lock_guard<std::mutex> guard(self.mutex);
        slot->stage = LoadStage::Deserializing;
        self.stats.bytes_read += stored_bytes;
        self.stats.bytes_decompressed += mapped.empty() ? payload.size() : 0;

        // Verification belongs here rather than at the read: it is CPU work over the decompressed
        // bytes, and putting it on the I/O thread would make every load wait for a hash.
        if (self.config.verify_content_hashes && slot->entry != nullptr && !placeholder) {
            const Span<const u8> bytes = mapped.empty() ? payload.span() : mapped;
            if (!(content_hash(bytes.data(), bytes.size()) == slot->entry->content)) {
                ++self.stats.integrity_failures;
                outcome = fail(ErrorCode::Io,
                               "the entry's payload does not match its recorded content hash");
            }
        }

        // The GPU upload stage. Counted and skipped: it needs a renderer, which is M3.
        ++self.stats.uploads_skipped;

        if (outcome) {
            outcome = self.publish(*slot, std::move(payload), mapped, kind, placeholder);
        }
    }

    std::vector<u32> dependencies;
    {
        std::lock_guard<std::mutex> guard(self.mutex);
        if (!outcome) {
            slot->error = outcome.error();
            self.complete_slot(*slot, LoadStage::Failed);
            (void)self.jobs->signal(slot->completion);
            return;
        }
        for (const cy::AssetId dependency : slot->dependencies) {
            LoadOptions options;
            options.referrer = slot->id;
            Expected<u32, Error> child = self.begin_load(dependency, options, true);
            if (child) {
                dependencies.push_back(child.value());
            }
        }
        slot->stored.clear();
    }

    if (dependencies.empty()) {
        std::lock_guard<std::mutex> guard(self.mutex);
        self.complete_slot(*slot, LoadStage::Complete);
        (void)self.jobs->signal(slot->completion);
        return;
    }

    // `core-assets-and-io` — "Dependencies load with the parent": this slot's completion is gated
    // on its children's, so the request reports complete only when the whole graph is resident.
    std::vector<jobs::JobHandle> handles;
    handles.reserve(dependencies.size());
    {
        std::lock_guard<std::mutex> guard(self.mutex);
        for (const u32 index : dependencies) {
            handles.push_back(self.slots[index]->completion);
        }
    }
    jobs::JobDesc desc;
    desc.body = &AssetSystemImpl::finish_job;
    desc.user = slot;
    desc.name = "assets.finish";
    desc.dependencies = handles.data();
    desc.dependency_count = static_cast<u32>(handles.size());
    if (!self.jobs->submit(desc)) {
        // The graph could not be extended, so the dependencies are awaited by nobody. Reporting
        // complete is still the honest answer for THIS asset, and the failure is counted.
        std::lock_guard<std::mutex> guard(self.mutex);
        self.complete_slot(*slot, LoadStage::Complete);
        (void)self.jobs->signal(slot->completion);
    }
}

void AssetSystemImpl::finish_job(const jobs::TaskContext& context, void* user) noexcept {
    (void)context;
    auto* slot = static_cast<AssetSlot*>(user);
    AssetSystemImpl& self = *slot->owner;
    {
        std::lock_guard<std::mutex> guard(self.mutex);
        self.complete_slot(*slot, LoadStage::Complete);
    }
    (void)self.jobs->signal(slot->completion);
}

Expected<u32, Error> AssetSystemImpl::begin_load(cy::AssetId id, const LoadOptions& options,
                                                 bool is_dependency) noexcept {
    if (id.is_nil()) {
        return fail(ErrorCode::InvalidArgument, "the nil asset id names no asset");
    }
    if (is_dependency) {
        ++stats.dependency_loads;
    }

    if (AssetSlot* existing = find_slot(id, options.variant); existing != nullptr) {
        // One load, two requesters. Both end up with the same slot and therefore the same `Ref`.
        ++stats.loads_coalesced;
        ++existing->request_refs;
        revive(*existing);
        return existing->index;
    }

    Expected<UniquePtr<AssetSlot>*, Error> added = slots.emplace_back();
    if (!added) {
        return make_unexpected(added.error());
    }
    Expected<UniquePtr<AssetSlot>, Error> created = make_unique<AssetSlot>(default_allocator());
    if (!created) {
        slots.pop_back();
        return make_unexpected(created.error());
    }
    *added.value() = std::move(created.value());

    AssetSlot& slot = **added.value();
    slot.index = static_cast<u32>(slots.size() - 1);
    slot.owner = this;
    slot.id = id;
    slot.variant = options.variant;
    slot.expected_kind = options.expected_kind;
    slot.referrer = options.referrer;
    slot.loading = true;
    slot.stage = LoadStage::Queued;
    slot.request_refs = 1;
    slot.last_use = use_clock++;

    Expected<jobs::CancellationSource, cy::Error> cancellation = jobs::CancellationSource::create();
    if (!cancellation) {
        slots.pop_back();
        return make_unexpected(cancellation.error());
    }
    slot.cancellation = std::move(cancellation.value());

    // The completion gate. Created before anything is submitted, so a caller can wait on it the
    // instant `begin_load` returns.
    jobs::JobDesc gate;
    gate.body = [](const jobs::TaskContext&, void*) noexcept {};
    gate.user = nullptr;
    gate.name = "assets.completion";
    gate.priority = options.priority;
    gate.gated = true;
    Expected<jobs::JobHandle, cy::Error> completion = jobs->submit(gate);
    if (!completion) {
        slots.pop_back();
        return make_unexpected(completion.error());
    }
    slot.completion = completion.value();

    // The read: on the dedicated service thread, which is the one place blocking is legal.
    Expected<jobs::JobHandle, cy::Error> read = async->submit_blocking(
        &AssetSystemImpl::read_operation, &slot, "assets.read", options.priority);
    if (!read) {
        (void)jobs->signal(slot.completion);
        slots.pop_back();
        return make_unexpected(read.error());
    }
    slot.read_handle = read.value();

    // The CPU half, gated on the read. Submitted now rather than by the reader, so that the graph
    // is complete before any of it runs and a cancellation has something to reach.
    jobs::JobDesc process;
    process.body = &AssetSystemImpl::process_job;
    process.user = &slot;
    process.name = "assets.process";
    process.priority = options.priority;
    // THE TOKEN IS DELIBERATELY NOT PUT ON THE JobDesc. A job the scheduler sees as cancelled never
    // runs its body at all — which would leave this slot's completion gate unsignalled and every
    // waiter on it stuck for ever. The body observes the token itself, at its own stage boundary,
    // and always signals the gate. That is what makes `cancel()` safe to call on a request somebody
    // is already waiting on.
    process.dependencies = &slot.read_handle;
    process.dependency_count = 1;
    if (Expected<jobs::JobHandle, cy::Error> submitted = jobs->submit(process); !submitted) {
        (void)jobs->signal(slot.completion);
        return make_unexpected(submitted.error());
    }

    ++stats.loads_started;
    return slot.index;
}

}  // namespace detail

// --- AssetSystem -----------------------------------------------------------------------------

AssetSystem::AssetSystem() noexcept = default;

AssetSystem::~AssetSystem() {
    shutdown();
}

Status AssetSystem::start(jobs::JobSystem& jobs, jobs::AsyncService& async,
                          VirtualFileSystem& files, const AssetSystemConfig& config) noexcept {
    if (impl_) {
        return fail(ErrorCode::AlreadyExists, "this asset system is already running");
    }
    if (!jobs.is_running()) {
        return fail(ErrorCode::Unavailable, "the job system is not running");
    }
    // The async service is NOT checked with `is_running()`. That predicate reports whether the
    // service THREAD has come up, which is a race against `start()` returning — a caller that did
    // everything right can still see false here. The condition that matters is that the service
    // exists, and `submit_blocking` reports Unavailable when it does not, so a service that was
    // never started fails the first read with a message rather than being accepted silently.

    Expected<UniquePtr<detail::AssetSystemImpl>, Error> created =
        make_unique<detail::AssetSystemImpl>(default_allocator());
    if (!created) {
        return make_unexpected(created.error());
    }
    impl_ = std::move(created.value());
    impl_->jobs = &jobs;
    impl_->async = &async;
    impl_->files = &files;
    impl_->config = config;
    impl_->running = true;
    return ok();
}

void AssetSystem::shutdown() noexcept {
    if (!impl_) {
        return;
    }
    detail::AssetSystemImpl& self = *impl_;

    // Cancel everything, then let the graph drain: a slot's state is written by jobs, so tearing
    // the table down while one is still running would be a use-after-free rather than a shutdown.
    {
        std::lock_guard<std::mutex> guard(self.mutex);
        for (UniquePtr<detail::AssetSlot>& slot : self.slots) {
            if (slot->loading && slot->cancellation.is_valid()) {
                slot->cancellation.cancel();
            }
        }
    }
    if (self.jobs != nullptr) {
        std::vector<jobs::JobHandle> pending;
        {
            std::lock_guard<std::mutex> guard(self.mutex);
            for (UniquePtr<detail::AssetSlot>& slot : self.slots) {
                pending.push_back(slot->completion);
            }
        }
        for (const jobs::JobHandle handle : pending) {
            self.jobs->wait(handle);
        }
    }

    std::vector<Ref<AssetData>> victims;
    {
        std::lock_guard<std::mutex> guard(self.mutex);
        // `running` first: with it false, the last release destroys rather than retiring, whatever
        // the retention policy is. Every reference is then MOVED out of the table so that the drop
        // happens below, outside the mutex the release path takes.
        self.running = false;
        for (UniquePtr<detail::AssetSlot>& slot : self.slots) {
            slot->evicting = true;
            if (slot->hold) {
                victims.push_back(std::move(slot->hold));
            }
            if (slot->pending) {
                victims.push_back(std::move(slot->pending));
            } else if (slot->retired && slot->data != nullptr) {
                // A retired asset's one reference is held by nobody — it was resurrected by the
                // retention policy. Adopting it puts it back under a Ref so it is released here.
                victims.push_back(Ref<AssetData>::adopt(slot->data));
            }
        }
        self.requests.clear();
    }
    detail::AssetSystemImpl::evict(victims);
    {
        std::lock_guard<std::mutex> guard(self.mutex);
        self.detach_survivors();
        self.slots.clear();
    }
    impl_.reset();
}

bool AssetSystem::is_running() const noexcept {
    return static_cast<bool>(impl_) && impl_->running;
}

Expected<LoadRequestId, Error> AssetSystem::load_batch(Span<const cy::AssetId> ids,
                                                       const LoadOptions& options) noexcept {
    if (!impl_) {
        return fail(ErrorCode::Unavailable, "the asset system is not running");
    }
    detail::AssetSystemImpl& self = *impl_;
    std::lock_guard<std::mutex> guard(self.mutex);

    if (self.requests.size() >= self.config.max_in_flight) {
        return fail(ErrorCode::Unavailable, "too many asset requests are in flight");
    }

    Expected<UniquePtr<detail::LoadRequestRecord>*, Error> added = self.requests.emplace_back();
    if (!added) {
        return make_unexpected(added.error());
    }
    Expected<UniquePtr<detail::LoadRequestRecord>, Error> created =
        make_unique<detail::LoadRequestRecord>(default_allocator());
    if (!created) {
        self.requests.pop_back();
        return make_unexpected(created.error());
    }
    *added.value() = std::move(created.value());
    detail::LoadRequestRecord& request = **added.value();
    request.id = self.next_request++;

    for (const cy::AssetId id : ids) {
        Expected<u32, Error> slot = self.begin_load(id, options, false);
        if (!slot) {
            return make_unexpected(slot.error());
        }
        if (Status recorded = request.slots.push_back(slot.value()); !recorded) {
            return make_unexpected(recorded.error());
        }
    }
    return request.id;
}

Expected<LoadRequestId, Error> AssetSystem::load_async(cy::AssetId id,
                                                       const LoadOptions& options) noexcept {
    return load_batch(Span<const cy::AssetId>(&id, 1), options);
}

Expected<Ref<AssetData>, Error> AssetSystem::load(cy::AssetId id,
                                                  const LoadOptions& options) noexcept {
    Expected<LoadRequestId, Error> request = load_async(id, options);
    if (!request) {
        return make_unexpected(request.error());
    }
    if (Status waited = wait(request.value()); !waited) {
        (void)forget(request.value());
        return make_unexpected(waited.error());
    }
    Expected<Ref<AssetData>, Error> asset = result(request.value());
    (void)forget(request.value());
    return asset;
}

LoadProgress AssetSystem::progress(LoadRequestId request) const noexcept {
    LoadProgress out;
    if (!impl_) {
        return out;
    }
    detail::AssetSystemImpl& self = *impl_;
    std::lock_guard<std::mutex> guard(self.mutex);

    const detail::LoadRequestRecord* record = self.find_request(request);
    if (record == nullptr) {
        out.done = true;
        return out;
    }
    out.total = static_cast<u32>(record->slots.size());
    out.cancelled = record->cancelled;
    LoadStage furthest = LoadStage::Complete;
    for (const u32 index : record->slots) {
        const detail::AssetSlot& slot = *self.slots[index];
        if (slot.stage == LoadStage::Complete) {
            ++out.completed;
        } else {
            if (slot.stage == LoadStage::Failed) {
                out.failed = true;
            }
            if (slot.stage == LoadStage::Cancelled) {
                out.cancelled = true;
            }
            // The least advanced stage is what a request as a whole has reached.
            if (static_cast<u8>(slot.stage) < static_cast<u8>(furthest)) {
                furthest = slot.stage;
            }
        }
    }
    out.stage = out.completed == out.total ? LoadStage::Complete : furthest;
    out.done = out.completed == out.total || out.failed || out.cancelled;
    return out;
}

bool AssetSystem::is_complete(LoadRequestId request) const noexcept {
    return progress(request).done;
}

Status AssetSystem::wait(LoadRequestId request) noexcept {
    if (!impl_) {
        return fail(ErrorCode::Unavailable, "the asset system is not running");
    }
    detail::AssetSystemImpl& self = *impl_;

    std::vector<jobs::JobHandle> handles;
    {
        std::lock_guard<std::mutex> guard(self.mutex);
        const detail::LoadRequestRecord* record = self.find_request(request);
        if (record == nullptr) {
            return fail(ErrorCode::NotFound, "no such asset request");
        }
        for (const u32 index : record->slots) {
            handles.push_back(self.slots[index]->completion);
        }
    }
    // Waits by RUNNING other ready tasks, never by sleeping: that is what makes this safe from
    // inside a job as well as from the main thread.
    for (const jobs::JobHandle handle : handles) {
        self.jobs->wait(handle);
    }
    return ok();
}

Expected<Ref<AssetData>, Error> AssetSystem::result_at(LoadRequestId request,
                                                       usize index) noexcept {
    if (!impl_) {
        return fail(ErrorCode::Unavailable, "the asset system is not running");
    }
    detail::AssetSystemImpl& self = *impl_;
    std::lock_guard<std::mutex> guard(self.mutex);

    const detail::LoadRequestRecord* record = self.find_request(request);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "no such asset request");
    }
    if (index >= record->slots.size()) {
        return fail(ErrorCode::OutOfRange, "that request did not ask for so many assets");
    }
    detail::AssetSlot& slot = *self.slots[record->slots[index]];
    if (slot.stage == LoadStage::Cancelled) {
        return fail(ErrorCode::Unavailable, "the load was cancelled");
    }
    if (slot.data == nullptr) {
        return slot.stage == LoadStage::Failed
                   ? make_unexpected(slot.error)
                   : fail(ErrorCode::Unavailable, "the load has not completed");
    }
    self.revive(slot);
    return Ref<AssetData>::retain(slot.data);
}

Expected<Ref<AssetData>, Error> AssetSystem::result(LoadRequestId request) noexcept {
    return result_at(request, 0);
}

Status AssetSystem::cancel(LoadRequestId request) noexcept {
    if (!impl_) {
        return fail(ErrorCode::Unavailable, "the asset system is not running");
    }
    detail::AssetSystemImpl& self = *impl_;
    std::lock_guard<std::mutex> guard(self.mutex);

    detail::LoadRequestRecord* record = self.find_request(request);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "no such asset request");
    }
    record->cancelled = true;
    for (const u32 index : record->slots) {
        detail::AssetSlot& slot = *self.slots[index];
        if (slot.request_refs > 0) {
            --slot.request_refs;
        }
        // Somebody else is still waiting on this load: cancelling your request must not cancel
        // theirs. This is what makes coalescing safe to rely on.
        if (slot.request_refs == 0 && slot.loading && slot.cancellation.is_valid()) {
            slot.cancellation.cancel();
        }
    }
    return ok();
}

Status AssetSystem::forget(LoadRequestId request) noexcept {
    if (!impl_) {
        return fail(ErrorCode::Unavailable, "the asset system is not running");
    }
    detail::AssetSystemImpl& self = *impl_;

    std::vector<Ref<AssetData>> victims;
    bool found = false;
    {
        std::lock_guard<std::mutex> guard(self.mutex);
        for (usize index = 0; index < self.requests.size(); ++index) {
            if (self.requests[index]->id != request) {
                continue;
            }
            for (const u32 slot_index : self.requests[index]->slots) {
                detail::AssetSlot& slot = *self.slots[slot_index];
                if (slot.request_refs > 0) {
                    --slot.request_refs;
                }
                if (slot.request_refs == 0 && slot.pending) {
                    // The system's own reference goes back. Moved out rather than reset, so the
                    // drop — and the retention decision it triggers — happens outside the mutex.
                    victims.push_back(std::move(slot.pending));
                }
            }
            self.requests.erase(index);
            found = true;
            break;
        }
    }
    detail::AssetSystemImpl::evict(victims);
    return found ? ok() : fail(ErrorCode::NotFound, "no such asset request");
}

Status AssetSystem::preload(Span<const cy::AssetId> ids, const LoadOptions& options) noexcept {
    Expected<LoadRequestId, Error> request = load_batch(ids, options);
    if (!request) {
        return make_unexpected(request.error());
    }
    if (Status waited = wait(request.value()); !waited) {
        return waited;
    }

    detail::AssetSystemImpl& self = *impl_;
    {
        std::lock_guard<std::mutex> guard(self.mutex);
        const detail::LoadRequestRecord* record = self.find_request(request.value());
        if (record != nullptr) {
            for (const u32 index : record->slots) {
                detail::AssetSlot& slot = *self.slots[index];
                if (slot.data != nullptr) {
                    slot.hold = Ref<AssetData>::retain(slot.data);
                }
            }
        }
    }
    return forget(request.value());
}

Status AssetSystem::release(Span<const cy::AssetId> ids, const LoadOptions& options) noexcept {
    if (!impl_) {
        return fail(ErrorCode::Unavailable, "the asset system is not running");
    }
    detail::AssetSystemImpl& self = *impl_;
    std::vector<Ref<AssetData>> victims;
    {
        std::lock_guard<std::mutex> guard(self.mutex);
        for (const cy::AssetId id : ids) {
            detail::AssetSlot* slot = self.find_slot(id, options.variant);
            if (slot == nullptr || !slot->hold) {
                continue;
            }
            victims.push_back(std::move(slot->hold));
        }
    }
    detail::AssetSystemImpl::evict(victims);
    return ok();
}

Expected<Ref<AssetData>, Error> AssetSystem::find_resident(cy::AssetId id,
                                                           VariantKey variant) noexcept {
    if (!impl_) {
        return fail(ErrorCode::Unavailable, "the asset system is not running");
    }
    detail::AssetSystemImpl& self = *impl_;
    std::lock_guard<std::mutex> guard(self.mutex);

    detail::AssetSlot* slot = self.find_slot(id, variant);
    if (slot == nullptr || slot->data == nullptr) {
        return fail(ErrorCode::NotFound, "that asset is not resident");
    }
    self.revive(*slot);
    return Ref<AssetData>::retain(slot->data);
}

Status AssetSystem::add_reload_observer(ReloadObserver observer, void* user) noexcept {
    if (!impl_) {
        return fail(ErrorCode::Unavailable, "the asset system is not running");
    }
    if (observer == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a null observer would be told nothing");
    }
    detail::AssetSystemImpl& self = *impl_;
    std::lock_guard<std::mutex> guard(self.mutex);
    for (const detail::AssetSystemImpl::ReloadListener& listener : self.reload_listeners) {
        if (listener.observer == observer && listener.user == user) {
            return ok();
        }
    }
    return self.reload_listeners.push_back(detail::AssetSystemImpl::ReloadListener{observer, user});
}

void AssetSystem::remove_reload_observer(ReloadObserver observer, void* user) noexcept {
    if (!impl_) {
        return;
    }
    detail::AssetSystemImpl& self = *impl_;
    std::lock_guard<std::mutex> guard(self.mutex);
    for (usize index = 0; index < self.reload_listeners.size(); ++index) {
        if (self.reload_listeners[index].observer == observer &&
            self.reload_listeners[index].user == user) {
            // erase(), not remove_unordered(): observers are told in registration order, and a
            // rebuild order that changed when an unrelated dependent unregistered would be a
            // different frame for a reason nobody could see.
            self.reload_listeners.erase(index);
            return;
        }
    }
}

Status AssetSystem::reload(cy::AssetId id, const LoadOptions& options) noexcept {
    if (!impl_) {
        return fail(ErrorCode::Unavailable, "the asset system is not running");
    }
    detail::AssetSystemImpl& self = *impl_;

    // Everything this call decides before it reads is decided under the lock, and the read itself
    // is not: reading is the slow part, every completing load takes this mutex, and holding it
    // across a file read would stall the loader for as long as the disk takes.
    {
        std::lock_guard<std::mutex> guard(self.mutex);
        if (!self.running) {
            return fail(ErrorCode::Unavailable, "the asset system is not running");
        }
        const detail::AssetSlot* slot = self.find_slot(id, options.variant);
        if (slot == nullptr || slot->data == nullptr) {
            return fail(ErrorCode::NotFound,
                        "that asset is not resident, and a reload does not start a load");
        }
        if (slot->loading) {
            return fail(ErrorCode::Unavailable,
                        "that asset is being loaded; reloading it would race the load that is "
                        "already producing its bytes");
        }
    }

    Expected<VirtualPath, Error> path = package_entry_path(id, options.variant);
    if (!path) {
        return make_unexpected(path.error());
    }
    Expected<VirtualFileSystem::Resolution, Error> resolved = self.files->resolve(path.value());
    if (!resolved) {
        ++self.stats.reloads_failed;
        return make_unexpected(resolved.error());
    }
    if (resolved.value().source->as_package() != nullptr) {
        return fail(ErrorCode::NotImplemented,
                    "this asset is served from a cooked package, and reloading one means "
                    "re-running the chunk framing, decompression and dependency pass that only the "
                    "load pipeline implements. Mount the loose file over the package to iterate on "
                    "it; see AssetSystem::reload's comment for what closing this needs.");
    }

    Array<u8> replacement(default_allocator());
    if (Status read = self.files->read(path.value(), replacement); !read) {
        // The specification's "malformed file mid-write" scenario. The old asset is untouched, and
        // it is untouched because nothing has been written yet rather than because something was
        // rolled back.
        ++self.stats.reloads_failed;
        return read;
    }

    ReloadEvent event;
    Array<detail::AssetSystemImpl::ReloadListener> listeners(default_allocator());
    {
        std::lock_guard<std::mutex> guard(self.mutex);
        detail::AssetSlot* slot = self.find_slot(id, options.variant);
        if (slot == nullptr || slot->data == nullptr) {
            // Evicted while the read was in flight. Not an error the caller can act on and not a
            // reload either: the next load will read the new bytes anyway.
            ++self.stats.reloads_failed;
            return fail(ErrorCode::NotFound, "the asset was released while it was being reloaded");
        }

        event.id = id;
        event.variant = options.variant;
        if (Status swapped = self.apply_reload(*slot, std::move(replacement), event); !swapped) {
            ++self.stats.reloads_failed;
            return swapped;
        }

        // Copied out so the observers are called with the mutex released: a dependent rebuilding a
        // material is entitled to ask this system for another asset, and doing that under the lock
        // it is already holding would deadlock.
        for (const detail::AssetSystemImpl::ReloadListener& listener : self.reload_listeners) {
            if (Status copied = listeners.push_back(listener); !copied) {
                break;
            }
        }
    }

    for (const detail::AssetSystemImpl::ReloadListener& listener : listeners) {
        listener.observer(listener.user, event);
    }
    return ok();
}

void AssetSystem::update() noexcept {
    if (!impl_) {
        return;
    }
    detail::AssetSystemImpl& self = *impl_;

    std::vector<Ref<AssetData>> victims;
    {
        std::lock_guard<std::mutex> guard(self.mutex);
        const i64 now = monotonic_now_ns();

        if (self.config.retention == RetentionPolicy::TimeDelayed) {
            for (UniquePtr<detail::AssetSlot>& slot : self.slots) {
                if (slot->retired && slot->data != nullptr &&
                    now - slot->retired_at_ns >= self.config.retention_delay_ns) {
                    slot->evicting = true;
                    victims.push_back(Ref<AssetData>::adopt(slot->data));
                }
            }
        } else if (self.config.retention == RetentionPolicy::BudgetBased) {
            // Least recently used first, until the resident total is inside the budget. The order
            // is the whole policy, so it is an explicit sort rather than table order.
            std::vector<detail::AssetSlot*> candidates;
            for (UniquePtr<detail::AssetSlot>& slot : self.slots) {
                if (slot->retired && slot->data != nullptr) {
                    candidates.push_back(slot.get());
                }
            }
            std::ranges::sort(candidates,
                              [](const detail::AssetSlot* a, const detail::AssetSlot* b) {
                                  return a->last_use < b->last_use;
                              });
            usize resident = self.stats.resident_bytes;
            for (detail::AssetSlot* slot : candidates) {
                if (resident <= self.config.residency_budget_bytes) {
                    break;
                }
                slot->evicting = true;
                resident -= slot->bytes;
                victims.push_back(Ref<AssetData>::adopt(slot->data));
            }
        }
    }
    detail::AssetSystemImpl::evict(victims);

    // The payloads reloads replaced. Freed here rather than at the swap, which is what makes
    // "re-acquire the span each frame" a sufficient rule for a reader — see AssetSystem::reload.
    {
        std::lock_guard<std::mutex> guard(self.mutex);
        self.retired_payloads.clear();
    }
}

AssetSystemStats AssetSystem::stats() const noexcept {
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> guard(impl_->mutex);
    AssetSystemStats out = impl_->stats;
    out.requests_in_flight = impl_->requests.size();
    return out;
}

void AssetSystem::reset_stats() noexcept {
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> guard(impl_->mutex);
    const usize resident_assets = impl_->stats.resident_assets;
    const usize resident_bytes = impl_->stats.resident_bytes;
    impl_->stats = AssetSystemStats{};
    impl_->stats.resident_assets = resident_assets;
    impl_->stats.resident_bytes = resident_bytes;
}

}  // namespace cy::assets
