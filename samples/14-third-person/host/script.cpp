// SPDX-License-Identifier: MIT
// The scripted player. See script.h.

#include "script.h"

namespace sample::tps {
namespace {

using cy::input::Key;

/// One key edge on one frame.
struct Edge {
    cy::u64 frame;
    Key key;
    bool down;
};

/// The timeline script.h describes, in frame order. A tap is both edges inside one frame: the
/// input server reports the press to the tick that follows even though the key is already up.
constexpr Edge kEdges[] = {
    {45, Key::W, true},       {380, Key::Space, true},       {380, Key::Space, false},
    {520, Key::W, false},     {540, Key::Right, true},       {736, Key::Right, false},
    {750, Key::W, true},      {800, Key::LeftShift, true},   {960, Key::Space, true},
    {960, Key::Space, false}, {1040, Key::LeftShift, false}, {1130, Key::W, false},
    {1135, Key::Left, true},  {1199, Key::Left, false},
};
}  // namespace

cy::Status Player::before_frame(cy::u64 frame) noexcept {
    for (const Edge& edge : kEdges) {
        if (edge.frame != frame) {
            continue;
        }
        if (cy::Status pressed = host_->press_key(edge.key, edge.down); !pressed) {
            return pressed;
        }
    }
    return cy::ok();
}

}  // namespace sample::tps
