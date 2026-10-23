// SPDX-License-Identifier: MIT
// A text field's model: caret, selection, composition, undo and the clipboard. See text_edit.h.

#include <cy/ui/toolkit/text_edit.h>

#include <cstring>

namespace cy::ui {
namespace {

[[nodiscard]] bool is_continuation(char byte) noexcept {
    return (static_cast<u8>(byte) & 0xC0U) == 0x80U;
}

[[nodiscard]] bool is_word(char byte) noexcept {
    const auto value = static_cast<u8>(byte);
    // Every byte of a multi-byte code point counts as a word character: a word of accented or
    // non-Latin letters is still one word to a caret.
    return value >= 0x80U || (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '_';
}

[[nodiscard]] bool is_control(char byte) noexcept {
    const auto value = static_cast<u8>(byte);
    return value < 0x20U || value == 0x7FU;
}

[[nodiscard]] std::string_view view_of(const Array<char>& bytes, u32 first, u32 length) noexcept {
    return {bytes.data() + first, length};
}

/// Insert `with` into `bytes` at `offset`, after removing `length` bytes there.
[[nodiscard]] Status splice(Array<char>& bytes, u32 offset, u32 length,
                            std::string_view with) noexcept {
    const usize old_size = bytes.size();
    const usize tail = old_size - offset - length;
    const usize new_size = old_size - length + with.size();
    if (new_size > old_size) {
        if (Status grown = bytes.resize(new_size); !grown) {
            return grown;
        }
    }
    char* data = bytes.data();
    if (tail > 0) {
        std::memmove(data + offset + with.size(), data + offset + length, tail);
    }
    if (!with.empty()) {
        std::memcpy(data + offset, with.data(), with.size());
    }
    if (new_size < old_size) {
        if (Status shrunk = bytes.resize(new_size); !shrunk) {
            return shrunk;
        }
    }
    return ok();
}

}  // namespace

u32 utf8_previous(std::string_view text, u32 offset) noexcept {
    if (offset == 0 || text.empty()) {
        return 0;
    }
    u32 at = (offset > text.size()) ? static_cast<u32>(text.size()) : offset;
    --at;
    while (at > 0 && is_continuation(text[at])) {
        --at;
    }
    return at;
}

u32 utf8_next(std::string_view text, u32 offset) noexcept {
    const auto size = static_cast<u32>(text.size());
    if (offset >= size) {
        return size;
    }
    u32 at = offset + 1;
    while (at < size && is_continuation(text[at])) {
        ++at;
    }
    return at;
}

const char* text_command_name(TextCommand command) noexcept {
    switch (command) {
        case TextCommand::MoveLeft:
            return "move-left";
        case TextCommand::MoveRight:
            return "move-right";
        case TextCommand::MoveWordLeft:
            return "move-word-left";
        case TextCommand::MoveWordRight:
            return "move-word-right";
        case TextCommand::MoveHome:
            return "move-home";
        case TextCommand::MoveEnd:
            return "move-end";
        case TextCommand::DeleteBackward:
            return "delete-backward";
        case TextCommand::DeleteForward:
            return "delete-forward";
        case TextCommand::SelectAll:
            return "select-all";
        case TextCommand::Copy:
            return "copy";
        case TextCommand::Cut:
            return "cut";
        case TextCommand::Paste:
            return "paste";
        case TextCommand::Undo:
            return "undo";
        case TextCommand::Redo:
            return "redo";
        case TextCommand::Count:
            break;
    }
    return "unknown";
}

Status MemoryClipboard::set_text(std::string_view text) noexcept {
    text_.clear();
    return text_.append(Span<const char>(text.data(), text.size()));
}

TextEditor::TextEditor(Allocator& allocator) noexcept
    : text_(allocator),
      composition_(allocator),
      history_(allocator),
      undo_(allocator),
      redo_(allocator) {}

Status TextEditor::set_text(std::string_view text) noexcept {
    text_.clear();
    composition_.clear();
    history_.clear();
    undo_.clear();
    redo_.clear();
    if (Status appended = text_.append(Span<const char>(text.data(), text.size())); !appended) {
        return appended;
    }
    caret_ = static_cast<u32>(text_.size());
    anchor_ = caret_;
    composition_caret_ = 0;
    return ok();
}

Status TextEditor::displayed(Array<char>& out) const noexcept {
    out.clear();
    if (Status head = out.append(Span<const char>(text_.data(), caret_)); !head) {
        return head;
    }
    if (Status middle = out.append(Span<const char>(composition_.data(), composition_.size()));
        !middle) {
        return middle;
    }
    return out.append(Span<const char>(text_.data() + caret_, text_.size() - caret_));
}

void TextEditor::set_caret(u32 offset, bool extend) noexcept {
    const std::string_view current = text();
    u32 at = (offset > current.size()) ? static_cast<u32>(current.size()) : offset;
    while (at > 0 && at < current.size() && is_continuation(current[at])) {
        --at;
    }
    caret_ = at;
    if (!extend) {
        anchor_ = at;
    }
}

bool TextEditor::accepts(std::string_view text) const noexcept {
    bool point = false;
    for (usize index = 0; index < text.size(); ++index) {
        const char byte = text[index];
        if (is_control(byte)) {
            return false;
        }
        const bool digit = byte >= '0' && byte <= '9';
        switch (mask_) {
            case TextMask::Any:
                break;
            case TextMask::Digits:
                if (!digit) {
                    return false;
                }
                break;
            case TextMask::Decimal: {
                // A minus only as the very first character of the field, and one point in it.
                const bool minus = byte == '-' && index == 0 && selection_begin() == 0;
                const bool decimal_point =
                    byte == '.' && !point && this->text().find('.') == std::string_view::npos;
                point = point || decimal_point;
                if (!digit && !minus && !decimal_point) {
                    return false;
                }
                break;
            }
        }
    }
    return true;
}

Status TextEditor::record(Array<Change>& stack, u32 offset, std::string_view removed,
                          std::string_view inserted) noexcept {
    Change change;
    change.offset = offset;
    change.caret_before = caret_;
    change.anchor_before = anchor_;
    change.removed_first = static_cast<u32>(history_.size());
    change.removed_length = static_cast<u32>(removed.size());
    if (Status kept = history_.append(Span<const char>(removed.data(), removed.size())); !kept) {
        return kept;
    }
    change.inserted_first = static_cast<u32>(history_.size());
    change.inserted_length = static_cast<u32>(inserted.size());
    if (Status kept = history_.append(Span<const char>(inserted.data(), inserted.size())); !kept) {
        return kept;
    }
    return stack.push_back(change);
}

Status TextEditor::replace(u32 offset, u32 length, std::string_view with, bool record_it) noexcept {
    if (record_it) {
        // The removed bytes are copied into the history BEFORE the splice moves them.
        if (Status recorded = record(undo_, offset, view_of(text_, offset, length), with);
            !recorded) {
            return recorded;
        }
        redo_.clear();
    }
    if (Status spliced = splice(text_, offset, length, with); !spliced) {
        return spliced;
    }
    caret_ = offset + static_cast<u32>(with.size());
    anchor_ = caret_;
    return ok();
}

Expected<bool, Error> TextEditor::insert(std::string_view text) noexcept {
    if (text.empty() || !accepts(text)) {
        return false;
    }
    const u32 begin = selection_begin();
    const u32 removed = selection_end() - begin;
    std::string_view accepted = text;
    if (max_length_ != 0) {
        const usize kept = text_.size() - removed;
        if (kept >= max_length_) {
            return false;
        }
        const usize room = max_length_ - kept;
        if (accepted.size() > room) {
            // Cut on a code point boundary, so the limit never leaves half a character.
            u32 cut = static_cast<u32>(room);
            while (cut > 0 && is_continuation(accepted[cut])) {
                --cut;
            }
            accepted = accepted.substr(0, cut);
        }
    }
    composition_.clear();
    if (Status replaced = replace(begin, removed, accepted, true); !replaced) {
        return make_unexpected(replaced.error());
    }
    return !accepted.empty() || removed > 0;
}

Status TextEditor::compose(std::string_view text, u32 caret) noexcept {
    if (!text.empty() && has_selection()) {
        // A composition replaces the selection the moment it starts, as a typed character would.
        if (Expected<bool, Error> erased = erase_selection(); !erased) {
            return make_unexpected(erased.error());
        }
    }
    composition_.clear();
    if (Status appended = composition_.append(Span<const char>(text.data(), text.size()));
        !appended) {
        return appended;
    }
    composition_caret_ = (caret > text.size()) ? static_cast<u32>(text.size()) : caret;
    return ok();
}

Expected<bool, Error> TextEditor::erase_selection() noexcept {
    if (!has_selection()) {
        return false;
    }
    const u32 begin = selection_begin();
    if (Status replaced = replace(begin, selection_end() - begin, {}, true); !replaced) {
        return make_unexpected(replaced.error());
    }
    return true;
}

Expected<bool, Error> TextEditor::delete_one(bool backward) noexcept {
    if (has_selection()) {
        return erase_selection();
    }
    const std::string_view current = text();
    if (backward) {
        if (caret_ == 0) {
            return false;
        }
        const u32 from = utf8_previous(current, caret_);
        if (Status replaced = replace(from, caret_ - from, {}, true); !replaced) {
            return make_unexpected(replaced.error());
        }
        return true;
    }
    if (caret_ >= current.size()) {
        return false;
    }
    const u32 to = utf8_next(current, caret_);
    const u32 at = caret_;
    if (Status replaced = replace(at, to - at, {}, true); !replaced) {
        return make_unexpected(replaced.error());
    }
    return true;
}

u32 TextEditor::move_target(TextCommand command) const noexcept {
    const std::string_view current = text();
    const auto size = static_cast<u32>(current.size());
    switch (command) {
        case TextCommand::MoveLeft:
            return utf8_previous(current, caret_);
        case TextCommand::MoveRight:
            return utf8_next(current, caret_);
        case TextCommand::MoveWordLeft: {
            // Skip the spaces before the caret, then the word: the platform convention everywhere.
            u32 at = caret_;
            while (at > 0 && !is_word(current[at - 1])) {
                --at;
            }
            while (at > 0 && is_word(current[at - 1])) {
                --at;
            }
            return at;
        }
        case TextCommand::MoveWordRight: {
            u32 at = caret_;
            while (at < size && !is_word(current[at])) {
                ++at;
            }
            while (at < size && is_word(current[at])) {
                ++at;
            }
            return at;
        }
        case TextCommand::MoveHome:
            return 0;
        case TextCommand::MoveEnd:
            return size;
        default:
            return caret_;
    }
}

Expected<bool, Error> TextEditor::step(Array<Change>& from, Array<Change>& to,
                                       bool undoing) noexcept {
    if (from.empty()) {
        return false;
    }
    const Change change = from[from.size() - 1];
    from.pop_back();
    const std::string_view removed = view_of(history_, change.removed_first, change.removed_length);
    const std::string_view inserted =
        view_of(history_, change.inserted_first, change.inserted_length);
    // Undoing puts back what the change removed; redoing does the change again. Either way the
    // inverse goes on the other stack, so undo and redo alternate for ever.
    const std::string_view put = undoing ? removed : inserted;
    const std::string_view take = undoing ? inserted : removed;
    Change inverse = change;
    inverse.caret_before = caret_;
    inverse.anchor_before = anchor_;
    if (Status pushed = to.push_back(inverse); !pushed) {
        return make_unexpected(pushed.error());
    }
    // `put` and `take` point into `history_`, which the splice of `text_` does not touch.
    if (Status spliced = splice(text_, change.offset, static_cast<u32>(take.size()), put);
        !spliced) {
        return make_unexpected(spliced.error());
    }
    if (undoing) {
        caret_ = change.caret_before;
        anchor_ = change.anchor_before;
    } else {
        caret_ = change.offset + static_cast<u32>(put.size());
        anchor_ = caret_;
    }
    return true;
}

Expected<bool, Error> TextEditor::clipboard_command(TextCommand command,
                                                    Clipboard* clipboard) noexcept {
    if (clipboard == nullptr) {
        return false;
    }
    if (command == TextCommand::Paste) {
        // Copied first: inserting may be the clipboard's own text, and a platform's buffer is not
        // ours to hold across an edit.
        Array<char> pasted(text_.allocator());
        const std::string_view source = clipboard->text();
        if (Status copied = pasted.append(Span<const char>(source.data(), source.size()));
            !copied) {
            return make_unexpected(copied.error());
        }
        return insert(std::string_view(pasted.data(), pasted.size()));
    }
    if (!has_selection()) {
        return false;
    }
    const u32 begin = selection_begin();
    if (Status copied = clipboard->set_text(view_of(text_, begin, selection_end() - begin));
        !copied) {
        return make_unexpected(copied.error());
    }
    return (command == TextCommand::Cut) ? erase_selection() : Expected<bool, Error>(false);
}

Expected<bool, Error> TextEditor::apply(TextCommand command, bool extend,
                                        Clipboard* clipboard) noexcept {
    switch (command) {
        case TextCommand::MoveLeft:
        case TextCommand::MoveRight:
            // Without `extend`, a movement out of a selection lands on its edge, not one past it.
            if (has_selection() && !extend) {
                const u32 edge =
                    (command == TextCommand::MoveLeft) ? selection_begin() : selection_end();
                set_caret(edge, false);
                return false;
            }
            set_caret(move_target(command), extend);
            return false;
        case TextCommand::MoveWordLeft:
        case TextCommand::MoveWordRight:
        case TextCommand::MoveHome:
        case TextCommand::MoveEnd:
            set_caret(move_target(command), extend);
            return false;
        case TextCommand::DeleteBackward:
            return delete_one(true);
        case TextCommand::DeleteForward:
            return delete_one(false);
        case TextCommand::SelectAll:
            anchor_ = 0;
            caret_ = static_cast<u32>(text_.size());
            return false;
        case TextCommand::Copy:
        case TextCommand::Cut:
        case TextCommand::Paste:
            return clipboard_command(command, clipboard);
        case TextCommand::Undo:
            return step(undo_, redo_, true);
        case TextCommand::Redo:
            return step(redo_, undo_, false);
        case TextCommand::Count:
            break;
    }
    return false;
}

}  // namespace cy::ui
