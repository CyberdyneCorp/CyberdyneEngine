// SPDX-License-Identifier: MIT
#ifndef CY_UI_TOOLKIT_TEXT_EDIT_H
#define CY_UI_TOOLKIT_TEXT_EDIT_H
// A text field's model: UTF-8 text, a caret, a selection, an input method's composition, undo and
// redo, and the clipboard — everything a text field is except its pixels. Issue #91, stage 4.
//
// --- WHY IT IS ITS OWN CLASS ---------------------------------------------------------------------
//
// `ui-system`: "Text input SHALL support: selection, clipboard, undo and redo, IME composition,
// ... and platform text-editing keyboard conventions." None of that needs an element, a font or a
// frame, so none of it is written against one: `TextEditor` is a value a suite drives with strings
// and reads back with strings, and `Toolkit`'s text field is this plus three elements (the text,
// the selection and the caret) that show it.
//
// --- WHERE TEXT COMES FROM -----------------------------------------------------------------------
//
// `ui-system` and `interaction.h` both say it: "Text entry SHALL use the platform text input path,
// not interface actions or key interpretation." So COMMITTED TEXT and the IME's COMPOSITION arrive
// as `TextInput` (UTF-8 the platform composed), and the editing keys — the caret, the selection,
// the clipboard, undo — arrive as `TextCommand`s, which a platform maps from its own conventions
// (Ctrl+C here, Cmd+C on macOS). Neither is a `UiAction`: a text field that moved its caret on
// `UI.NavigateLeft` would move it when a gamepad's stick was nudged.
//
// --- OFFSETS ARE BYTES, MOVEMENT IS BY CODE POINT
// -------------------------------------------------
//
// The caret and the selection are byte offsets into the UTF-8 text, always on a code point
// boundary: a caret move steps over a whole code point, a backspace removes one, and nothing here
// can leave half a character behind.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

#include <string_view>

namespace cy::ui {

/// The editing commands a text field takes. A platform maps its keys onto these; the field never
/// sees a key.
enum class TextCommand : u8 {
    MoveLeft = 0,
    MoveRight,
    /// To the start or end of the previous or next word.
    MoveWordLeft,
    MoveWordRight,
    MoveHome,
    MoveEnd,
    /// Delete the selection, or the code point before (Backspace) or after (Delete) the caret.
    DeleteBackward,
    DeleteForward,
    SelectAll,
    Copy,
    Cut,
    Paste,
    Undo,
    Redo,
    Count,
};

[[nodiscard]] const char* text_command_name(TextCommand command) noexcept;

/// What the clipboard is: the platform's, through this. `MemoryClipboard` is one a suite and a
/// build with no platform clipboard use.
class Clipboard {
public:
    Clipboard() = default;
    virtual ~Clipboard() = default;
    Clipboard(const Clipboard&) = delete;
    Clipboard& operator=(const Clipboard&) = delete;
    Clipboard(Clipboard&&) = delete;
    Clipboard& operator=(Clipboard&&) = delete;

    [[nodiscard]] virtual Status set_text(std::string_view text) noexcept = 0;
    /// The clipboard's text, valid until the next `set_text`.
    [[nodiscard]] virtual std::string_view text() const noexcept = 0;
};

/// A clipboard in memory.
class MemoryClipboard final : public Clipboard {
public:
    explicit MemoryClipboard(Allocator& allocator) noexcept : text_(allocator) {}
    [[nodiscard]] Status set_text(std::string_view text) noexcept override;
    [[nodiscard]] std::string_view text() const noexcept override {
        return {text_.data(), text_.size()};
    }

private:
    Array<char> text_;
};

/// Which characters a field accepts — `ui-system`'s "input masks and validation".
enum class TextMask : u8 {
    /// Anything printable.
    Any = 0,
    /// Digits only.
    Digits,
    /// A signed decimal number: digits, one point, a leading minus.
    Decimal,
};

/// A text field's text and everything that edits it.
class TextEditor {
public:
    explicit TextEditor(Allocator& allocator) noexcept;

    TextEditor(const TextEditor&) = delete;
    TextEditor& operator=(const TextEditor&) = delete;
    TextEditor(TextEditor&&) noexcept = default;
    TextEditor& operator=(TextEditor&&) noexcept = default;
    ~TextEditor() = default;

    /// Replace the text, put the caret at its end and forget undo. What a binding or the game does;
    /// an edit is `insert`.
    [[nodiscard]] Status set_text(std::string_view text) noexcept;
    [[nodiscard]] std::string_view text() const noexcept { return {text_.data(), text_.size()}; }
    /// The text as shown: the composition, while there is one, sits at the caret.
    [[nodiscard]] Status displayed(Array<char>& out) const noexcept;

    [[nodiscard]] u32 caret() const noexcept { return caret_; }
    /// The selection's other end. Equal to the caret when nothing is selected.
    [[nodiscard]] u32 anchor() const noexcept { return anchor_; }
    [[nodiscard]] bool has_selection() const noexcept { return caret_ != anchor_; }
    [[nodiscard]] u32 selection_begin() const noexcept {
        return caret_ < anchor_ ? caret_ : anchor_;
    }
    [[nodiscard]] u32 selection_end() const noexcept { return caret_ < anchor_ ? anchor_ : caret_; }
    /// Place the caret at a byte offset, snapped to the code point it falls in; `extend` keeps the
    /// anchor, which is a drag selecting.
    void set_caret(u32 offset, bool extend) noexcept;

    /// Insert committed text at the caret, replacing the selection, as one undoable step. Text the
    /// mask refuses, and text past the length limit, is dropped; returns whether anything changed.
    [[nodiscard]] Expected<bool, Error> insert(std::string_view text) noexcept;
    /// The input method's in-progress composition, shown at the caret and not yet part of the
    /// text or its undo; an empty one ends it. `caret` is the IME's own caret within it, in bytes.
    [[nodiscard]] Status compose(std::string_view text, u32 caret) noexcept;
    [[nodiscard]] std::string_view composition() const noexcept {
        return {composition_.data(), composition_.size()};
    }
    [[nodiscard]] bool composing() const noexcept { return !composition_.empty(); }
    /// The composition's caret, in bytes into `composition()`.
    [[nodiscard]] u32 composition_caret() const noexcept { return composition_caret_; }

    /// Apply an editing command; `extend` turns a movement into a selection. `clipboard` may be
    /// null, and then Copy, Cut and Paste do nothing. Returns whether the TEXT changed.
    [[nodiscard]] Expected<bool, Error> apply(TextCommand command, bool extend,
                                              Clipboard* clipboard) noexcept;

    void set_mask(TextMask mask) noexcept { mask_ = mask; }
    [[nodiscard]] TextMask mask() const noexcept { return mask_; }
    /// The most bytes the text may hold; zero is no limit.
    void set_max_length(u32 bytes) noexcept { max_length_ = bytes; }

    [[nodiscard]] usize undo_depth() const noexcept { return undo_.size(); }
    [[nodiscard]] usize redo_depth() const noexcept { return redo_.size(); }

private:
    /// One undoable change: the bytes removed and inserted at an offset, and the caret either side.
    struct Change {
        u32 offset = 0;
        u32 removed_first = 0;
        u32 removed_length = 0;
        u32 inserted_first = 0;
        u32 inserted_length = 0;
        u32 caret_before = 0;
        u32 anchor_before = 0;
    };

    [[nodiscard]] Status replace(u32 offset, u32 length, std::string_view with,
                                 bool record) noexcept;
    [[nodiscard]] Status record(Array<Change>& stack, u32 offset, std::string_view removed,
                                std::string_view inserted) noexcept;
    [[nodiscard]] Expected<bool, Error> step(Array<Change>& from, Array<Change>& to,
                                             bool undoing) noexcept;
    [[nodiscard]] Expected<bool, Error> erase_selection() noexcept;
    [[nodiscard]] Expected<bool, Error> delete_one(bool backward) noexcept;
    [[nodiscard]] Expected<bool, Error> clipboard_command(TextCommand command,
                                                          Clipboard* clipboard) noexcept;
    [[nodiscard]] u32 move_target(TextCommand command) const noexcept;
    [[nodiscard]] bool accepts(std::string_view text) const noexcept;

    Array<char> text_;
    Array<char> composition_;
    /// The bytes every recorded change removed and inserted, by offset into this pool.
    Array<char> history_;
    Array<Change> undo_;
    Array<Change> redo_;
    u32 caret_ = 0;
    u32 anchor_ = 0;
    u32 composition_caret_ = 0;
    u32 max_length_ = 0;
    TextMask mask_ = TextMask::Any;
};

/// The byte offset of the code point before `offset`, or zero.
[[nodiscard]] u32 utf8_previous(std::string_view text, u32 offset) noexcept;
/// The byte offset of the code point after the one at `offset`, or the text's length.
[[nodiscard]] u32 utf8_next(std::string_view text, u32 offset) noexcept;

}  // namespace cy::ui

#endif  // CY_UI_TOOLKIT_TEXT_EDIT_H
