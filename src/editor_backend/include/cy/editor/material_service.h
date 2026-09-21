#pragma once

#include <cy/abi/host.h>
#include <cy/core/memory/allocator.h>

namespace cy::editor {

/// Engine-owned material authoring backend. Both C ABI and live protocol adapters submit the same
/// operation names and payload schemas to this object.
class MaterialService final : public abi::EditorServiceBackend {
public:
    explicit MaterialService(Allocator& allocator) noexcept : allocator_(&allocator) {}

    [[nodiscard]] CyResult open(CyServiceSession* out_session) noexcept override;
    void close(CyServiceSession session) noexcept override;
    [[nodiscard]] CyResult submit(CyServiceSession session,
                                  const CyServiceRequest& request) noexcept override;
    [[nodiscard]] CyResult cancel(CyServiceSession session, u64 request_id) noexcept override;
    [[nodiscard]] CyResult poll(CyServiceSession session, CyServiceEvent& out_event,
                                bool& out_has_event) noexcept override;

private:
    Allocator* allocator_;
};

}  // namespace cy::editor
