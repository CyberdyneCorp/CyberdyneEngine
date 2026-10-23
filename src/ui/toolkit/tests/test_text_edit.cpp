// SPDX-License-Identifier: MIT
// `unit.ui_toolkit`: the text field's model and the text field — caret, selection, composition,
// undo, the clipboard, masks — and the field on screen.

#include "fixture.h"

#include <cy/ui/toolkit/text_edit.h>

#include <string_view>

using namespace cy;
using namespace cy::ui;
using namespace cy::ui::toolkit_test;

namespace {

[[nodiscard]] std::string_view shown(const TextEditor& editor, Array<char>& scratch) noexcept {
    (void)editor.displayed(scratch);
    return {scratch.data(), scratch.size()};
}

}  // namespace

CY_TEST_CASE("ui_text_edit: typing, moving and deleting step over whole code points") {
    TextEditor editor(allocator());
    CY_REQUIRE(editor.set_text("pur\xC3\xA9").has_value());  // "puré", the é two bytes
    CY_CHECK_EQ(editor.caret(), 5U);
    CY_REQUIRE(editor.apply(TextCommand::MoveLeft, false, nullptr).has_value());
    CY_CHECK_EQ(editor.caret(), 3U);  // before the é, not inside it
    CY_REQUIRE(editor.apply(TextCommand::DeleteForward, false, nullptr).has_value());
    CY_CHECK_EQ(editor.text(), std::string_view("pur"));
    CY_REQUIRE(editor.insert("\xC3\xA8s").has_value());  // "ès"
    CY_CHECK_EQ(editor.text(), std::string_view("pur\xC3\xA8s"));
    CY_REQUIRE(editor.apply(TextCommand::DeleteBackward, false, nullptr).has_value());
    CY_REQUIRE(editor.apply(TextCommand::DeleteBackward, false, nullptr).has_value());
    CY_CHECK_EQ(editor.text(), std::string_view("pur"));
    // A caret placed inside a code point snaps to its start.
    CY_REQUIRE(editor.set_text("\xC3\xA9t\xC3\xA9").has_value());
    editor.set_caret(1, false);
    CY_CHECK_EQ(editor.caret(), 0U);
}

CY_TEST_CASE("ui_text_edit: words, home and end, and selections that extend and collapse") {
    TextEditor editor(allocator());
    CY_REQUIRE(editor.set_text("open the gate").has_value());
    CY_REQUIRE(editor.apply(TextCommand::MoveWordLeft, false, nullptr).has_value());
    CY_CHECK_EQ(editor.caret(), 9U);
    CY_REQUIRE(editor.apply(TextCommand::MoveWordLeft, true, nullptr).has_value());
    CY_CHECK_EQ(editor.caret(), 5U);
    CY_CHECK_EQ(editor.anchor(), 9U);
    CY_CHECK(editor.has_selection());
    // Moving right without extending lands on the selection's far edge, not one past it.
    CY_REQUIRE(editor.apply(TextCommand::MoveRight, false, nullptr).has_value());
    CY_CHECK_EQ(editor.caret(), 9U);
    CY_CHECK_FALSE(editor.has_selection());
    CY_REQUIRE(editor.apply(TextCommand::MoveHome, true, nullptr).has_value());
    CY_CHECK_EQ(editor.selection_begin(), 0U);
    CY_CHECK_EQ(editor.selection_end(), 9U);
    // Typing replaces the selection.
    CY_REQUIRE(editor.insert("shut ").has_value());
    CY_CHECK_EQ(editor.text(), std::string_view("shut gate"));
    CY_REQUIRE(editor.apply(TextCommand::MoveWordRight, false, nullptr).has_value());
    CY_CHECK_EQ(editor.caret(), 9U);
}

CY_TEST_CASE("ui_text_edit: undo and redo walk the edits back and forth, caret and all") {
    TextEditor editor(allocator());
    CY_REQUIRE(editor.set_text("").has_value());
    CY_REQUIRE(editor.insert("ab").has_value());
    CY_REQUIRE(editor.insert("cd").has_value());
    CY_REQUIRE(editor.apply(TextCommand::SelectAll, false, nullptr).has_value());
    CY_REQUIRE(editor.apply(TextCommand::DeleteBackward, false, nullptr).has_value());
    CY_CHECK_EQ(editor.text(), std::string_view(""));
    CY_CHECK_EQ(editor.undo_depth(), 3U);
    CY_REQUIRE(editor.apply(TextCommand::Undo, false, nullptr).has_value());
    CY_CHECK_EQ(editor.text(), std::string_view("abcd"));
    CY_CHECK_EQ(editor.selection_begin(), 0U);  // the selection the delete took is back
    CY_CHECK_EQ(editor.selection_end(), 4U);
    CY_REQUIRE(editor.apply(TextCommand::Undo, false, nullptr).has_value());
    CY_CHECK_EQ(editor.text(), std::string_view("ab"));
    CY_REQUIRE(editor.apply(TextCommand::Redo, false, nullptr).has_value());
    CY_CHECK_EQ(editor.text(), std::string_view("abcd"));
    CY_CHECK_EQ(editor.caret(), 4U);
    CY_REQUIRE(editor.apply(TextCommand::Redo, false, nullptr).has_value());
    CY_CHECK_EQ(editor.text(), std::string_view(""));
    // A new edit after an undo forgets the redo.
    CY_REQUIRE(editor.apply(TextCommand::Undo, false, nullptr).has_value());
    CY_REQUIRE(editor.insert("!").has_value());
    CY_CHECK_EQ(editor.redo_depth(), 0U);
    Expected<bool, Error> nothing = editor.apply(TextCommand::Redo, false, nullptr);
    CY_REQUIRE(nothing.has_value());
    CY_CHECK_FALSE(*nothing);
}

CY_TEST_CASE("ui_text_edit: copy, cut and paste go through the clipboard") {
    TextEditor editor(allocator());
    MemoryClipboard clipboard(allocator());
    CY_REQUIRE(editor.set_text("level 7").has_value());
    CY_REQUIRE(editor.apply(TextCommand::MoveWordLeft, true, &clipboard).has_value());
    CY_REQUIRE(editor.apply(TextCommand::Copy, false, &clipboard).has_value());
    CY_CHECK_EQ(clipboard.text(), std::string_view("7"));
    CY_CHECK_EQ(editor.text(), std::string_view("level 7"));
    CY_REQUIRE(editor.apply(TextCommand::Cut, false, &clipboard).has_value());
    CY_CHECK_EQ(editor.text(), std::string_view("level "));
    CY_REQUIRE(editor.apply(TextCommand::MoveHome, false, &clipboard).has_value());
    CY_REQUIRE(editor.apply(TextCommand::Paste, false, &clipboard).has_value());
    CY_CHECK_EQ(editor.text(), std::string_view("7level "));
    CY_REQUIRE(editor.apply(TextCommand::Undo, false, &clipboard).has_value());
    CY_CHECK_EQ(editor.text(), std::string_view("level "));
}

CY_TEST_CASE("ui_text_edit: an input method's composition is shown at the caret, not stored") {
    TextEditor editor(allocator());
    Array<char> scratch(allocator());
    CY_REQUIRE(editor.set_text("ab").has_value());
    editor.set_caret(1, false);
    CY_REQUIRE(editor.compose("\xE3\x81\x8B", 3).has_value());  // か, being composed
    CY_CHECK(editor.composing());
    CY_CHECK_EQ(editor.text(), std::string_view("ab"));
    CY_CHECK_EQ(shown(editor, scratch), std::string_view("a\xE3\x81\x8B"
                                                         "b"));
    CY_CHECK_EQ(editor.undo_depth(), 0U);
    // Committing replaces the composition with the text the IME chose, as one undoable step.
    CY_REQUIRE(editor.compose({}, 0).has_value());
    CY_REQUIRE(editor.insert("\xE6\xBC\xA2").has_value());  // 漢
    CY_CHECK_FALSE(editor.composing());
    CY_CHECK_EQ(editor.text(), std::string_view("a\xE6\xBC\xA2"
                                                "b"));
    CY_CHECK_EQ(editor.undo_depth(), 1U);
}

CY_TEST_CASE("ui_text_edit: masks and a length limit refuse what they should, whole") {
    TextEditor digits(allocator());
    digits.set_mask(TextMask::Digits);
    CY_REQUIRE(digits.set_text("").has_value());
    Expected<bool, Error> refused = digits.insert("12a");
    CY_REQUIRE(refused.has_value());
    CY_CHECK_FALSE(*refused);
    CY_REQUIRE(digits.insert("123").has_value());
    CY_CHECK_EQ(digits.text(), std::string_view("123"));

    TextEditor decimal(allocator());
    decimal.set_mask(TextMask::Decimal);
    CY_REQUIRE(decimal.set_text("").has_value());
    CY_CHECK(*decimal.insert("-1.5"));
    CY_CHECK_FALSE(*decimal.insert("."));  // a second point
    CY_CHECK_FALSE(*decimal.insert("-"));  // a minus not at the start

    TextEditor limited(allocator());
    limited.set_max_length(4);
    CY_REQUIRE(limited.set_text("").has_value());
    CY_REQUIRE(limited.insert("ab\xC3\xA9z").has_value());  // 5 bytes: the cut keeps whole points
    CY_CHECK_EQ(limited.text(), std::string_view("ab\xC3\xA9"));
    CY_CHECK_FALSE(*limited.insert("x"));
    // Control characters are never text.
    TextEditor any(allocator());
    CY_REQUIRE(any.set_text("").has_value());
    CY_CHECK_FALSE(*any.insert("a\tb"));
}

CY_TEST_CASE("ui_toolkit: a text field edits through text input and commands, and shows it") {
    Kit kit;
    CY_REQUIRE(kit.started);
    WidgetDesc desc;
    desc.kind = WidgetKind::TextField;
    desc.id = "name";
    desc.placeholder = "Your name";
    auto field = kit.toolkit.create(kNoElement, desc);
    CY_REQUIRE(field.has_value());
    CY_REQUIRE(kit.toolkit.set_anchors(*field, Vec2{0, 0}, Vec2{0, 0}, Vec2{10, 10}, Vec2{210, 34})
                   .has_value());
    CY_REQUIRE(kit.frame());
    const ElementId placeholder = kit.toolkit.part(*field, "text-placeholder");
    const ElementId caret = kit.toolkit.part(*field, "text-caret");
    const ElementId value = kit.toolkit.part(*field, "text-value");
    CY_CHECK(has_flag(kit.store.flags(placeholder), ElementFlags::Visible));
    CY_CHECK_FALSE(kit.toolkit.wants_text_input());
    CY_CHECK_FALSE(kit.toolkit.text_input(TextInput{"x", false, 0}));  // not focused: refused

    // A click focuses it — and puts the caret where it landed.
    kit.click(Vec2{40.0F, 22.0F});
    CY_REQUIRE(kit.frame());
    CY_CHECK(kit.toolkit.wants_text_input());
    CY_CHECK(kit.toolkit.captures_input());
    CY_CHECK(kit.toolkit.text_input(TextInput{"Ada", false, 0}));
    CY_CHECK_EQ(kit.toolkit.text(*field), std::string_view("Ada"));
    CY_CHECK_EQ(kit.count_events(WidgetEventKind::Changed, *field), 1U);
    CY_REQUIRE(kit.frame());
    CY_CHECK_FALSE(has_flag(kit.store.flags(placeholder), ElementFlags::Visible));
    // The caret is after the third character: three cells past the text's origin.
    CY_CHECK_NEAR(kit.rect(caret).x, kit.rect(value).x + (3.0F * kCharWidth), 0.01F);

    // Commands: select the last word and cut it; the field is empty again.
    CY_CHECK(kit.toolkit.edit(TextCommand::MoveWordLeft, true));
    CY_CHECK(kit.toolkit.edit(TextCommand::Cut, false));
    CY_CHECK_EQ(kit.toolkit.text(*field), std::string_view(""));
    CY_CHECK(kit.toolkit.edit(TextCommand::Paste, false));
    CY_CHECK_EQ(kit.toolkit.text(*field), std::string_view("Ada"));

    // IME: the composition is drawn and the candidate window goes under the caret.
    CY_CHECK(kit.toolkit.text_input(TextInput{"\xE3\x81\x8B", true, 3}));
    CY_REQUIRE(kit.frame());
    const ElementId underline = kit.toolkit.part(*field, "text-composition");
    CY_CHECK(has_flag(kit.store.flags(underline), ElementFlags::Visible));
    ui::Rect candidate;
    CY_REQUIRE(kit.toolkit.text_input_rect(candidate));
    CY_CHECK_EQ(candidate.x, kit.rect(caret).x);
    CY_CHECK_EQ(kit.toolkit.text(*field), std::string_view("Ada"));

    // Enter submits.
    CY_CHECK(kit.toolkit.action(UiAction::Accept));
    CY_CHECK_EQ(kit.count_events(WidgetEventKind::Submitted, *field), 1U);
}

CY_TEST_CASE("ui_toolkit: a long text scrolls under the caret instead of running out") {
    Kit kit;
    CY_REQUIRE(kit.started);
    WidgetDesc desc;
    desc.kind = WidgetKind::TextField;
    auto field = kit.toolkit.create(kNoElement, desc);
    CY_REQUIRE(field.has_value());
    CY_REQUIRE(kit.toolkit.set_anchors(*field, Vec2{0, 0}, Vec2{0, 0}, Vec2{0, 0}, Vec2{72, 24})
                   .has_value());
    // The theme's minimum width would hold it at 180; this field is narrower on purpose.
    CY_REQUIRE(kit.toolkit.set_inline_style(*field, "min-width: 0").has_value());
    CY_REQUIRE(kit.toolkit.focus(*field).has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK(kit.toolkit.text_input(TextInput{"abcdefghijklmnopqrst", false, 0}));
    CY_REQUIRE(kit.frame());
    const ui::Rect box = kit.rect(*field);
    const ui::Rect caret = kit.rect(kit.toolkit.part(*field, "text-caret"));
    CY_CHECK_LE(caret.right(), box.right());
    CY_CHECK_GE(caret.x, box.x);
    // The text's start has scrolled out to the left.
    CY_CHECK_LT(kit.rect(kit.toolkit.part(*field, "text-value")).x, box.x);
}
