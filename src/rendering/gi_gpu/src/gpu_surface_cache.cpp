// SPDX-License-Identifier: MIT
#include <cy/rendering/gi_gpu/gpu_surface_cache.h>

#include <algorithm>
#include <cstring>

namespace cy::rendering::gi_gpu {
namespace {

using rhi::Access;
using rhi::QueueKind;

constexpr u64 kResultFloats = 12;

}  // namespace

void GpuSurfaceShading::bind(GpuGiScene& scene, const gi::DistanceField& field,
                             f32 lookup_radius) noexcept {
    scene_ = &scene;
    field_ = &field;
    lookup_radius_ = lookup_radius;
}

void GpuSurfaceShading::set_gather(const gi::CardGatherSettings& gather) noexcept {
    if (scene_ != nullptr) {
        scene_->set_gather(gather);
    }
}

u32 GpuSurfaceShading::submit(Span<const gi::SurfacePage> pages, Span<const u32> selected,
                              const gi::SurfaceUpdateContext& context) noexcept {
    submission_ = GpuSurfaceSubmission{};
    submission_.frame = context.frame;
    handles_.clear();
    revisions_.clear();
    declared_ = false;
    error_ = Error{};
    if (scene_ == nullptr || field_ == nullptr || scene_->device_ == nullptr) {
        error_ = Error{ErrorCode::InvalidArgument, "the device surface shading is not bound"};
        return 0;
    }

    // Stage 1 first: the scene representation the dispatch reads, brought up to date by what
    // moved. A frame in which nothing moved writes nothing here.
    Expected<FieldUploadReport, Error> field = scene_->upload_field(*field_);
    if (!field.has_value()) {
        error_ = field.error();
        return 0;
    }
    submission_.field = *field;
    Expected<CardUploadReport, Error> cards = scene_->upload_cards(pages, lookup_radius_);
    if (!cards.has_value()) {
        error_ = cards.error();
        return 0;
    }
    submission_.cards = *cards;
    if (Status lights = scene_->upload_lights(context.lights); !lights) {
        error_ = lights.error();
        return 0;
    }
    if (Status shadow = scene_->upload_shadow_map(shadow_map_); !shadow) {
        error_ = shadow.error();
        return 0;
    }

    // A PREFIX when the device was sized for fewer pages than the budget: the selection is in
    // priority order, so what is dropped is the least urgent work, exactly as a smaller budget
    // would have dropped it.
    const u32 accepted =
        static_cast<u32>(std::min<usize>(selected.size(), scene_->desc_.max_selection));
    auto* list = static_cast<u32*>(scene_->mapped(GpuGiScene::kSelection));
    for (u32 index = 0; index < accepted; ++index) {
        const u32 handle = selected[index];
        list[index] = handle;
        if (!handles_.push_back(handle) || !revisions_.push_back(pages[handle].revision)) {
            error_ = Error{ErrorCode::OutOfMemory, "the submission list could not grow"};
            handles_.clear();
            revisions_.clear();
            return 0;
        }
    }
    submission_.pages = accepted;
    return accepted;
}

Status GpuSurfaceShading::declare(RenderGraph& graph) noexcept {
    if (scene_ == nullptr || scene_->device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the device surface shading is not bound");
    }
    if (submission_.pages == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "nothing was submitted; a shade over an empty selection would report success "
                    "and write nothing");
    }
    GpuGiScene& scene = *scene_;
    using B = GpuGiScene;
    const ResourceId constants = scene.import(graph, B::kConstants);
    const ResourceId table = scene.import(graph, B::kPageTable);
    const ResourceId bricks = scene.import(graph, B::kBricks);
    const ResourceId cards = scene.import(graph, B::kCards);
    const ResourceId state = scene.import(graph, B::kCardState);
    const ResourceId ranges = scene.import(graph, B::kGridRanges);
    const ResourceId items = scene.import(graph, B::kGridItems);
    const ResourceId lights = scene.import(graph, B::kLights);
    const ResourceId shadow = scene.import(graph, B::kShadowDepths);
    const ResourceId selection = scene.import(graph, B::kSelection);
    const ResourceId results = scene.import(graph, B::kResults);
    const ResourceId host = scene.import(graph, B::kResultsReadback);

    const u32 frame = static_cast<u32>(submission_.frame & 0xFFFFFFFFULL);
    shade_ = B::Recording{&scene, B::kShade, B::Dispatch{submission_.pages, frame, {0, 0}}};
    commit_ = B::Recording{&scene, B::kCommit, B::Dispatch{submission_.pages, frame, {0, 0}}};

    // THE SHADE READS LAST FRAME'S STATE AND WRITES ONLY RESULTS; THE COMMIT WRITES THE STATE. The
    // graph's barrier between the two is what makes every gather in the shade see one frame.
    graph.add_pass("gi shade cards", QueueKind::Graphics)
        .read(constants, Access::ComputeStorageRead)
        .read(table, Access::ComputeStorageRead)
        .read(bricks, Access::ComputeStorageRead)
        .read(cards, Access::ComputeStorageRead)
        .read(state, Access::ComputeStorageRead)
        .read(ranges, Access::ComputeStorageRead)
        .read(items, Access::ComputeStorageRead)
        .read(lights, Access::ComputeStorageRead)
        .read(shadow, Access::ComputeStorageRead)
        .read(selection, Access::ComputeStorageRead)
        .write(results, Access::ComputeStorageWrite)
        .record(&GpuGiScene::record_dispatch, &shade_);
    graph.add_pass("gi commit cards", QueueKind::Graphics)
        .read(results, Access::ComputeStorageRead)
        .write(state, Access::ComputeStorageWrite)
        .record(&GpuGiScene::record_dispatch, &commit_);

    results_copy_ = B::Copy{&scene, B::kResults, B::kResultsReadback,
                            static_cast<u64>(submission_.pages) * kResultFloats * sizeof(f32)};
    graph.add_pass("gi results readback", QueueKind::Graphics)
        .read(results, Access::TransferRead)
        .write(host, Access::TransferWrite)
        .record(&GpuGiScene::record_copy, &results_copy_);
    graph.add_pass("gi results host", QueueKind::Graphics)
        .read(host, Access::HostRead)
        .side_effect();
    if (Status status = graph.status(); !status) {
        return status;
    }
    declared_ = true;
    return ok();
}

u32 GpuSurfaceShading::retire(Span<gi::SurfacePage> pages) noexcept {
    if (scene_ == nullptr || handles_.empty()) {
        return 0;
    }
    if (!declared_) {
        // Submitted and never declared: nothing ran, so there is nothing to fold back.
        handles_.clear();
        revisions_.clear();
        return 0;
    }
    const auto* results = static_cast<const f32*>(scene_->mapped(GpuGiScene::kResultsReadback));
    if (results == nullptr) {
        return 0;
    }
    u32 written = 0;
    for (usize index = 0; index < handles_.size(); ++index) {
        const u32 handle = handles_[index];
        if (handle >= pages.size()) {
            continue;
        }
        gi::SurfacePage& page = pages[handle];
        if (!page.live || page.revision != revisions_[index]) {
            continue;
        }
        const f32* record = results + (index * kResultFloats);
        page.direct = Vec3{record[0], record[1], record[2]};
        page.error = record[3];
        page.accumulated = Vec3{record[4], record[5], record[6]};
        page.valid = true;
        page.last_update_frame = submission_.frame;
        written += 1;
    }
    handles_.clear();
    revisions_.clear();
    declared_ = false;
    return written;
}

}  // namespace cy::rendering::gi_gpu
