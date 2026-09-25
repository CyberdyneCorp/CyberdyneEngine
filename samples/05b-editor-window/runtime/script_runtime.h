// SPDX-License-Identifier: MIT
#pragma once

#include <cy/abi/host.h>
#include <cy/abi/module.h>
#include <cy/core/memory/ownership.h>
#include <cy/gameplay/play/session.h>
#include <cy/scene/serialization/worldfile.h>

#include <string>
#include <vector>

namespace cy::sample::editor_window {

/// Runs project Swift behaviours on authored nodes while the hosted Play session is active.
class ScriptRuntime {
public:
    ScriptRuntime(Allocator& allocator, const char* project,
                  const char* module_path = nullptr) noexcept;

    [[nodiscard]] Status start(gameplay::PlaySession& play,
                               const scene::serialization::World& authored) noexcept;
    void stop() noexcept;
    [[nodiscard]] Status tick(gameplay::PlaySession& play, f32 dt) noexcept;
    [[nodiscard]] Expected<abi::ReloadReport, Error> reload(const char* library) noexcept;
    [[nodiscard]] bool active() const noexcept { return static_cast<bool>(runtime_); }
    [[nodiscard]] u32 count() const noexcept { return static_cast<u32>(identities_.size()); }

private:
    Allocator* allocator_;
    std::string project_;
    std::string module_path_;
    std::string active_library_;
    abi::Host host_;
    UniquePtr<abi::World> binding_;
    UniquePtr<abi::BehaviourRuntime> runtime_;
    abi::ModuleManifest manifest_;
    std::vector<u64> identities_;
};

}  // namespace cy::sample::editor_window
