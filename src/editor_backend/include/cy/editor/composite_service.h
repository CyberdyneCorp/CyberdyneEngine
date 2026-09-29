// SPDX-License-Identifier: MIT
#pragma once
// One editor-service binding over several backends. Issue #28, task 2.2 of
// `implement-issue-28-navigation-authoring`.
//
// `CyEngine_T::bind_editor_service` holds exactly one `EditorServiceBackend`, and the runtime
// forwards every editor request to it. `CompositeEditorService` is that one backend when more than
// one service exists: it routes each operation by the longest matching prefix, keeps one child
// session per backend, polls the children in turn, and answers `capabilities.get` with the union
// of every child's list. MaterialService and NavigationService stay unaware of each other.

#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>

#include <string_view>

namespace cy::editor {

/// Lets one editor-service binding serve several backends, routing each operation by the longest
/// matching prefix and merging their `capabilities.get` answers.
class CompositeEditorService final : public abi::EditorServiceBackend {
public:
    /// The most routes one composite holds.
    static constexpr u32 kMaxRoutes = 8;
    /// The longest route prefix, in bytes.
    static constexpr u32 kMaxPrefix = 47;

    explicit CompositeEditorService(Allocator& allocator) noexcept : allocator_(&allocator) {}

    /// Routes every operation starting with `prefix` to `backend`. One backend may serve several
    /// prefixes and still gets one child session. Refuses a ninth route, a duplicate or empty
    /// prefix, and any change once a session is open.
    [[nodiscard]] Status route(std::string_view prefix,
                               abi::EditorServiceBackend& backend) noexcept;

    [[nodiscard]] CyResult open(CyServiceSession* out_session) noexcept override;
    void close(CyServiceSession session) noexcept override;
    [[nodiscard]] CyResult submit(CyServiceSession session,
                                  const CyServiceRequest& request) noexcept override;
    [[nodiscard]] CyResult cancel(CyServiceSession session, u64 request_id) noexcept override;
    [[nodiscard]] CyResult poll(CyServiceSession session, CyServiceEvent& out_event,
                                bool& out_has_event) noexcept override;

    /// The child session `backend` was opened with inside `session`, so a host can reach a
    /// child's own accessors (`MaterialService::vfx_preview_world`, `NavigationService::mesh`).
    /// Null when `backend` is not routed.
    [[nodiscard]] CyServiceSession child_session(
        CyServiceSession session, const abi::EditorServiceBackend& backend) const noexcept;

private:
    /// The backend of the longest route `operation` starts with, or `kMaxRoutes`.
    [[nodiscard]] u32 backend_for(std::string_view operation) const noexcept;

    struct Route {
        char prefix[kMaxPrefix + 1] = {};
        u32 length = 0;
        u32 backend = 0;
    };

    Allocator* allocator_;
    Route routes_[kMaxRoutes] = {};
    abi::EditorServiceBackend* backends_[kMaxRoutes] = {};
    u32 route_count_ = 0;
    u32 backend_count_ = 0;
    u32 open_sessions_ = 0;
};

}  // namespace cy::editor
