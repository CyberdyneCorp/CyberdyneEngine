// SPDX-License-Identifier: MIT
#include "service_queue.h"

#include <algorithm>

namespace cy::sample::editor_window {

CyResult PendingServiceRequests::submit_one(const Waiting& waiting, ServiceSubmit submit,
                                            void* user) {
    const CyServiceRequest request{sizeof(CyServiceRequest), waiting.schema,
                                   waiting.request,          waiting.operation.c_str(),
                                   waiting.payload.data(),   waiting.payload.size()};
    return submit(user, request);
}

CyResult PendingServiceRequests::offer(u64 request, u32 schema, std::string_view operation,
                                       Span<const u8> payload, ServiceSubmit submit, void* user) {
    Waiting waiting{request, schema, std::string(operation),
                    std::vector<u8>(payload.begin(), payload.end())};
    // Behind one already waiting, whatever the service would say now: the editor's requests keep
    // the order they were sent in.
    if (!waiting_.empty()) {
        waiting_.push_back(std::move(waiting));
        return CY_RESULT_OK;
    }
    const CyResult result = submit_one(waiting, submit, user);
    if (result == CY_RESULT_ALREADY_EXISTS) {
        waiting_.push_back(std::move(waiting));
        return CY_RESULT_OK;
    }
    return result;
}

void PendingServiceRequests::retry(ServiceSubmit submit, void* user, std::vector<u64>& failed) {
    while (!waiting_.empty()) {
        const CyResult result = submit_one(waiting_.front(), submit, user);
        if (result == CY_RESULT_ALREADY_EXISTS) {
            return;
        }
        if (result != CY_RESULT_OK) {
            failed.push_back(waiting_.front().request);
        }
        waiting_.pop_front();
    }
}

bool PendingServiceRequests::cancel(u64 request) {
    const auto found = std::ranges::find_if(
        waiting_, [request](const Waiting& waiting) { return waiting.request == request; });
    if (found == waiting_.end()) {
        return false;
    }
    waiting_.erase(found);
    return true;
}

}  // namespace cy::sample::editor_window
