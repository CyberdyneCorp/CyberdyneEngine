// SPDX-License-Identifier: MIT
#pragma once
// Editor service requests the runtime could not submit yet. Issue #29, found by the animation
// panel.
//
// THE SERVICE TAKES ONE REQUEST AT A TIME PER BACKEND. `MaterialService::submit` refuses a second
// request while one is pending (`CY_RESULT_ALREADY_EXISTS`), and `CompositeEditorService` passes
// that refusal on. The editor keeps one request in flight per kind of request — material, VFX,
// audio, scripts, animation — so two kinds that reach the same backend in one frame are two
// requests at once, and the runtime used to ignore the second submission's result: the request was
// dropped, no event ever answered it, and that editor request manager waited for it forever. The
// animation panel's catalogue and the VFX catalogue are routed to one backend and asked for in the
// same frame.
//
// So a refused request waits here, in arrival order, and is submitted as the service frees up; a
// request that can never be submitted (anything but busy) is handed back for the host to fail, so
// every editor request still ends in exactly one terminal event.

#include <cy/abi/cy_abi.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace cy::sample::editor_window {

/// Submits one request to the service, answering the service's result.
using ServiceSubmit = CyResult (*)(void* user, const CyServiceRequest& request) noexcept;

class PendingServiceRequests {
public:
    /// Submit a request now, or keep it when the service is busy or an earlier one still waits.
    /// Answers `CY_RESULT_OK` when it was submitted or kept, and the service's refusal when it can
    /// never be submitted, which the host answers with a failed event.
    [[nodiscard]] CyResult offer(u64 request, u32 schema, std::string_view operation,
                                 Span<const u8> payload, ServiceSubmit submit, void* user);

    /// Submit what waits, oldest first, until the service is busy again. A request the service
    /// refuses for another reason is dropped from the queue and its identity is appended to
    /// `failed` for the host to answer.
    void retry(ServiceSubmit submit, void* user, std::vector<u64>& failed);

    /// Forget a waiting request the editor cancelled. False when none waits under that identity.
    [[nodiscard]] bool cancel(u64 request);

    [[nodiscard]] usize waiting() const noexcept { return waiting_.size(); }

private:
    struct Waiting {
        u64 request = 0;
        u32 schema = 0;
        std::string operation;
        std::vector<u8> payload;
    };

    [[nodiscard]] static CyResult submit_one(const Waiting& waiting, ServiceSubmit submit,
                                             void* user);

    std::deque<Waiting> waiting_;
};

}  // namespace cy::sample::editor_window
