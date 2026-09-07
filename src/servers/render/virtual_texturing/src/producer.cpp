#include <cy/servers/render/virtual_texturing/producer.h>

namespace cy::render::vt {

const char* persistence_class_name(PersistenceClass persistence) noexcept {
    switch (persistence) {
        case PersistenceClass::Transient:
            return "transient";
        case PersistenceClass::Derived:
            return "derived";
        case PersistenceClass::SaveGame:
            return "save-game";
        case PersistenceClass::Replicated:
            return "replicated";
        case PersistenceClass::Count:
            break;
    }
    return "unknown";
}

Status ProducerRegistry::register_producer(u32 texture, PageProducer& producer) noexcept {
    if (texture >= kMaxVirtualTextures) {
        return make_unexpected(
            Error{ErrorCode::OutOfRange, "virtual texturing: texture id exceeds 20 bits"});
    }
    if (auto placed = producers_.insert(texture, &producer); !placed) {
        return make_unexpected(placed.error());
    }
    return ok();
}

bool ProducerRegistry::unregister_producer(u32 texture) noexcept {
    return producers_.remove(texture);
}

PageProducer* ProducerRegistry::find(u32 texture) const noexcept {
    PageProducer* const* found = producers_.find(texture);
    return (found == nullptr) ? nullptr : *found;
}

}  // namespace cy::render::vt
