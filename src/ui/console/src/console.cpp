// SPDX-License-Identifier: MIT
#include <cy/ui/console/console.h>

#include <algorithm>
#include <cstring>

namespace cy::ui {
namespace {

[[nodiscard]] std::string_view view_of(const char* text, u32 length) noexcept {
    return {text, length};
}

/// Copy at most `capacity` bytes of `source` into `destination` and return the length kept.
[[nodiscard]] u32 copy_cut(std::string_view source, char* destination, u32 capacity) noexcept {
    const auto length = static_cast<u32>(std::min<usize>(source.size(), capacity));
    if (length != 0U) {
        std::memcpy(destination, source.data(), length);
    }
    return length;
}

/// `name arguments` split at the first space.
void split_command(std::string_view line, std::string_view& name,
                   std::string_view& arguments) noexcept {
    const usize space = line.find(' ');
    if (space == std::string_view::npos) {
        name = line;
        arguments = {};
        return;
    }
    name = line.substr(0, space);
    arguments = line.substr(space + 1U);
}

ConsoleReply echo_command(std::string_view arguments, void* /*user*/) noexcept {
    return ConsoleReply{arguments, kConsoleInfo};
}

}  // namespace

DevConsole::DevConsole(Allocator& allocator) noexcept
    : rows_(allocator), lines_(allocator), commands_(allocator) {}

Expected<ElementId, Error> DevConsole::make_label(ElementId parent) noexcept {
    Expected<ElementId, Error> created = store_->create(parent, Name::intern("label"));
    if (!created.has_value()) {
        return created;
    }
    LayoutInput* input = store_->layout_input(*created);
    // A row is exactly one line tall whatever it holds, so a print is a repaint, never a relayout.
    input->preferred.y = text_->measure("M", style_.text).y;
    input->self_align = Align::Stretch;
    return created;
}

Status DevConsole::create(ElementStore& store, TextPainter& text, ElementId parent,
                          const ConsoleStyle& style) noexcept {
    if (store_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "console: already created");
    }
    if (style.visible_rows == 0 || style.history == 0) {
        return fail(ErrorCode::InvalidArgument, "console: it shows at least one row of one line");
    }
    store_ = &store;
    text_ = &text;
    style_ = style;
    if (Status sized = lines_.resize(style.history); !sized) {
        return sized;
    }

    Expected<ElementId, Error> panel = store.create(parent, Name::intern("console"));
    if (!panel.has_value()) {
        return make_unexpected(panel.error());
    }
    panel_ = *panel;
    LayoutInput* layout = store.layout_input(panel_);
    layout->model = LayoutModel::Flex;
    layout->direction = FlexDirection::Column;
    layout->anchor_min = style.anchor_min;
    layout->anchor_max = style.anchor_max;
    layout->offset_min = style.offset_min;
    layout->offset_max = style.offset_max;
    layout->padding = Insets{style.padding, style.padding, style.padding, style.padding};
    PaintData* paint = store.paint(panel_);
    paint->background = style.background;
    paint->border_colour = style.border;
    paint->border_width = 1.0F;
    if (Status flagged =
            store.set_flags(panel_, ElementFlags::Visible | ElementFlags::ClipsChildren);
        !flagged) {
        return flagged;
    }

    for (u32 row = 0; row < style.visible_rows; ++row) {
        Expected<ElementId, Error> label = make_label(panel_);
        if (!label.has_value()) {
            return make_unexpected(label.error());
        }
        if (Status pushed = rows_.push_back(*label); !pushed) {
            return pushed;
        }
    }
    Expected<ElementId, Error> input = make_label(panel_);
    if (!input.has_value()) {
        return make_unexpected(input.error());
    }
    input_line_ = *input;
    store.paint(input_line_)->background = style.input_background;

    if (Status added = add_command("echo", &echo_command, nullptr); !added) {
        return added;
    }
    return refresh();
}

Status DevConsole::add_command(std::string_view name, ConsoleCommandFn command,
                               void* user) noexcept {
    if (name.empty() || name.size() > sizeof(Command{}.name) || command == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "console: a command has a name of at most 32 bytes and a function");
    }
    for (Command& existing : commands_) {
        if (view_of(existing.name, existing.length) == name) {
            existing.function = command;
            existing.user = user;
            return ok();
        }
    }
    Command entry;
    entry.length = copy_cut(name, entry.name, sizeof(entry.name));
    entry.function = command;
    entry.user = user;
    return commands_.push_back(entry);
}

std::string_view DevConsole::line(u32 index) const noexcept {
    if (index >= count_) {
        return {};
    }
    const auto history = static_cast<u32>(lines_.size());
    const Line& entry = lines_[(head_ + history - count_ + index) % history];
    return view_of(entry.text, entry.length);
}

std::string_view DevConsole::input() const noexcept {
    return view_of(input_.text, input_.length);
}

Status DevConsole::print(std::string_view text, u32 colour) noexcept {
    if (store_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "console: not created");
    }
    const auto history = static_cast<u32>(lines_.size());
    Line& entry = lines_[head_];
    entry.length = copy_cut(text, entry.text, kConsoleLineCapacity);
    entry.colour = colour;
    head_ = (head_ + 1U) % history;
    count_ = std::min(count_ + 1U, history);
    scroll_ = 0;
    return refresh();
}

Status DevConsole::type(std::string_view characters) noexcept {
    // The input line keeps room for its prompt and caret within one scrollback line.
    constexpr u32 kInputCapacity = kConsoleLineCapacity - 4U;
    const u32 room = kInputCapacity - std::min(input_.length, kInputCapacity);
    input_.length += copy_cut(characters, input_.text + input_.length, room);
    return refresh();
}

Status DevConsole::backspace() noexcept {
    if (input_.length != 0U) {
        --input_.length;
    }
    return refresh();
}

Status DevConsole::scroll(i32 lines) noexcept {
    const u32 hidden = count_ > style_.visible_rows ? count_ - style_.visible_rows : 0U;
    const i64 wanted = static_cast<i64>(scroll_) + lines;
    scroll_ = static_cast<u32>(std::clamp<i64>(wanted, 0, hidden));
    return refresh();
}

Status DevConsole::clear() noexcept {
    head_ = 0;
    count_ = 0;
    scroll_ = 0;
    return refresh();
}

Status DevConsole::set_open(bool open) noexcept {
    if (store_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "console: not created");
    }
    open_ = open;
    const ElementFlags flags = store_->flags(panel_);
    const ElementFlags next =
        open ? without_flag(flags, ElementFlags::Collapsed) : (flags | ElementFlags::Collapsed);
    if (Status set = store_->set_flags(panel_, next); !set) {
        return set;
    }
    // Collapsing takes the panel out of layout, so its parent arranges again.
    store_->mark(panel_, Dirty::Measure | Dirty::Arrange);
    return ok();
}

Status DevConsole::help() noexcept {
    if (Status printed = print("commands: help clear", kConsoleInfo); !printed) {
        return printed;
    }
    for (const Command& command : commands_) {
        if (Status printed = print(view_of(command.name, command.length), kConsoleInfo); !printed) {
            return printed;
        }
    }
    return ok();
}

Status DevConsole::run(std::string_view name, std::string_view arguments) noexcept {
    if (name == "help") {
        return help();
    }
    if (name == "clear") {
        return clear();
    }
    for (const Command& command : commands_) {
        if (view_of(command.name, command.length) != name) {
            continue;
        }
        const ConsoleReply reply = command.function(arguments, command.user);
        return reply.text.empty() ? ok() : print(reply.text, reply.colour);
    }
    char message[kConsoleLineCapacity] = {};
    constexpr std::string_view kUnknown = "unknown command: ";
    u32 length = copy_cut(kUnknown, message, kConsoleLineCapacity);
    length += copy_cut(name, message + length, kConsoleLineCapacity - length);
    return print(view_of(message, length), kConsoleError);
}

Status DevConsole::submit() noexcept {
    if (store_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "console: not created");
    }
    // Copied first: printing the echo and running the command may both touch the input line.
    Line submitted = input_;
    input_.length = 0;
    const std::string_view entered = view_of(submitted.text, submitted.length);
    if (entered.empty()) {
        return refresh();
    }
    char echo[kConsoleLineCapacity] = {'>', ' '};
    const u32 echoed = 2U + copy_cut(entered, echo + 2, kConsoleLineCapacity - 2U);
    if (Status printed = print(view_of(echo, echoed), kConsoleEcho); !printed) {
        return printed;
    }
    std::string_view name;
    std::string_view arguments;
    split_command(entered, name, arguments);
    return run(name, arguments);
}

Status DevConsole::refresh() noexcept {
    const auto visible = static_cast<u32>(rows_.size());
    for (u32 row = 0; row < visible; ++row) {
        // The bottom row shows the newest line `scroll_` lines back; the rows above, older ones.
        const i64 index = static_cast<i64>(count_) - scroll_ - visible + row;
        TextStyle style = style_.text;
        std::string_view shown;
        if (index >= 0 && index < static_cast<i64>(count_)) {
            const auto history = static_cast<u32>(lines_.size());
            const Line& entry =
                lines_[(head_ + history - count_ + static_cast<u32>(index)) % history];
            shown = view_of(entry.text, entry.length);
            style.colour = entry.colour;
        }
        if (Status set = text_->set_text(rows_[row], shown, style); !set) {
            return set;
        }
        store_->mark(rows_[row], Dirty::Paint);
    }
    char prompt[kConsoleLineCapacity] = {'>', ' '};
    u32 length = 2U + copy_cut(input(), prompt + 2, kConsoleLineCapacity - 3U);
    prompt[length++] = '_';
    TextStyle style = style_.text;
    style.colour = style_.input_colour;
    if (Status set = text_->set_text(input_line_, view_of(prompt, length), style); !set) {
        return set;
    }
    store_->mark(input_line_, Dirty::Paint);
    return ok();
}

}  // namespace cy::ui
