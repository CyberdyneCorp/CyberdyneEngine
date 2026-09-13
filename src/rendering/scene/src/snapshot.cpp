// The snapshot's storage and the buffer swap at the commit boundary. Task 4.1.2.

#include <cy/rendering/scene/snapshot.h>

namespace cy::rendering {

SnapshotBuilder::SnapshotBuilder(Allocator& allocator) noexcept
    : published_(allocator), removed_(allocator), lights_(allocator), cameras_(allocator) {}

void SnapshotBuilder::reset() noexcept {
    published_.clear();
    removed_.clear();
    lights_.clear();
    cameras_.clear();
    environment_ = EnvironmentState{};
    alpha_ = determinism::Presentation<f32>(0.0f);
}

Status SnapshotBuilder::publish(u64 identity, const GpuInstance& instance) noexcept {
    return published_.push_back(InstanceUpdate{identity, instance});
}

Status SnapshotBuilder::remove(u64 identity) noexcept { return removed_.push_back(identity); }

Status SnapshotBuilder::add_light(const LightState& light) noexcept {
    return lights_.push_back(light);
}

Status SnapshotBuilder::add_camera(const CameraState& camera) noexcept {
    return cameras_.push_back(camera);
}

void SnapshotBuilder::set_environment(const EnvironmentState& environment) noexcept {
    environment_ = environment;
}

void SnapshotBuilder::set_interpolation_alpha(determinism::PresentationContext witness,
                                              f32 alpha) noexcept {
    alpha_.write(witness, alpha);
}

SnapshotPublisher::SnapshotPublisher(Allocator& allocator) noexcept
    : buffers_{SnapshotBuilder(allocator), SnapshotBuilder(allocator)},
      front_(&buffers_[0]),
      back_(&buffers_[1]) {}

Status SnapshotPublisher::on_commit(const determinism::CommitRecord& record) noexcept {
    // The swap is the publication. Everything the extract stage wrote into `back_` becomes the
    // readable snapshot, and the builder that was readable becomes the one the next extraction
    // fills — after being cleared, so a frame that publishes nothing publishes nothing rather than
    // republishing what was there two frames ago.
    SnapshotBuilder* published = back_;
    back_ = front_;
    front_ = published;
    back_->reset();

    readable_.state_version_ = record.state_version;
    readable_.point_ = record.point;
    readable_.alpha_ = published->alpha_;
    readable_.published_ = published->published_.span();
    readable_.removed_ = published->removed_.span();
    readable_.lights_ = published->lights_.span();
    readable_.cameras_ = published->cameras_.span();
    readable_.environment_ = published->environment_;
    ++published_frames_;
    return ok();
}

}  // namespace cy::rendering
