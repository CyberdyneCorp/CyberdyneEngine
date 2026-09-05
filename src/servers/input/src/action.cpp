// The action registry. Task 4.1.2.

#include <cy/servers/input/action.h>

namespace cy::input {

Expected<ActionId, Error> ActionRegistry::declare(const ActionDeclaration& declaration) noexcept {
    if (!declaration.stable_id.valid()) {
        return fail(ErrorCode::InvalidArgument,
                    "input: an action needs a stable identifier; zero is the null one");
    }
    if (find(declaration.stable_id) != kInvalidAction) {
        // Two actions with one identity would make a stored override ambiguous — the profile keys
        // on the stable id, and a duplicate is a save file that resolves differently depending on
        // registration order.
        return fail(ErrorCode::AlreadyExists, "input: that action's stable id is already declared");
    }
    const auto index = static_cast<ActionId>(declarations_.size());
    if (Status pushed = declarations_.push_back(declaration); !pushed) {
        return make_unexpected(pushed.error());
    }
    return index;
}

ActionId ActionRegistry::find(ActionStableId stable_id) const noexcept {
    for (usize index = 0; index < declarations_.size(); ++index) {
        if (declarations_[index].stable_id == stable_id) {
            return static_cast<ActionId>(index);
        }
    }
    return kInvalidAction;
}

ActionId ActionRegistry::find(Name name) const noexcept {
    for (usize index = 0; index < declarations_.size(); ++index) {
        if (declarations_[index].name == name) {
            return static_cast<ActionId>(index);
        }
    }
    return kInvalidAction;
}

}  // namespace cy::input
