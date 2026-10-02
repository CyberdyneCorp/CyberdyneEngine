// SPDX-License-Identifier: MIT
#ifndef CY_UI_CONSOLE_CONSOLE_H
#define CY_UI_CONSOLE_CONSOLE_H
// The developer console: CyberUI's first first-party consumer.
//
// --- WHY IT EXISTS -------------------------------------------------------------------------------
//
// `ui-system`'s "Forcing functions": the editor is not built on CyberUI, so the engine SHALL ship,
// on CyberUI, the tooling a game uses — a developer console first — "as shipping features usable in
// a game, not demonstrations". This is that console: elements in an `ElementStore`, laid out by
// `cy::ui::layout`, its text through `TextPainter`, flattened with every other interface in the
// frame and drawn by the one UI pass.
//
// --- THE SCROLLBACK IS VIRTUALISED ---------------------------------------------------------------
//
// A console prints at a high rate and keeps a long history. The history is a ring of lines held
// here, outside the store; the store holds only as many row elements as the panel shows, and
// scrolling rebinds which line each row draws. Ten thousand lines printed cost the same number of
// elements as ten, and a print repaints the rows — paint dirt only, never a relayout.
//
// --- WHAT IT DOES ------------------------------------------------------------------------------
//
//     console.create(store, text, parent, {});          // the panel, its rows and its input line
//     console.add_command("spawn", &spawn, &game);      // a command a game registers
//     console.type("spawn tank 3");                     // characters, as text input delivers them
//     console.submit();                                 // echoes, runs, prints what it returned
//     console.print("ready", kConsoleInfo);             // a line from anywhere
//
// Built in: `help` lists the commands, `clear` empties the scrollback, `echo` prints its argument.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/ui/store.h>
#include <cy/ui/text/text_painter.h>

#include <string_view>

namespace cy::ui {

/// Line colours, premultiplied ARGB.
inline constexpr u32 kConsoleInfo = 0xFFD8DEE6U;
inline constexpr u32 kConsoleEcho = 0xFF7FD4FFU;
inline constexpr u32 kConsoleWarning = 0xFFFFC857U;
inline constexpr u32 kConsoleError = 0xFFFF6B5EU;

/// The longest line the scrollback keeps, in bytes. A longer print is cut, not wrapped.
inline constexpr u32 kConsoleLineCapacity = 96;

struct ConsoleStyle {
    /// Where the panel sits in its parent, as `LayoutInput`'s absolute anchors and offsets — the
    /// parent must lay out its children with `LayoutModel::Absolute`.
    Vec2 anchor_min{0.0F, 0.0F};
    Vec2 anchor_max{1.0F, 0.0F};
    Vec2 offset_min{0.0F, 0.0F};
    Vec2 offset_max{0.0F, 120.0F};
    /// How many scrollback rows the panel shows. The store holds exactly this many row elements.
    u32 visible_rows = 6;
    /// How many lines the scrollback remembers.
    u32 history = 512;
    /// Premultiplied: a translucent dark panel and a slightly lighter input line.
    u32 background = 0xD0101418U;
    u32 input_background = 0xE01C232BU;
    u32 border = 0xFF3A4654U;
    u32 input_colour = 0xFFFFFFFFU;
    f32 padding = 4.0F;
    TextStyle text;
};

/// What a command answers with: printed as a line, unless empty.
struct ConsoleReply {
    /// Copied into the scrollback before the command returns control.
    std::string_view text;
    u32 colour = kConsoleInfo;
};

/// A command a game registers. `arguments` is what followed the name, without the separating space.
using ConsoleCommandFn = ConsoleReply (*)(std::string_view arguments, void* user) noexcept;

class DevConsole {
public:
    explicit DevConsole(Allocator& allocator) noexcept;

    DevConsole(const DevConsole&) = delete;
    DevConsole& operator=(const DevConsole&) = delete;
    DevConsole(DevConsole&&) = delete;
    DevConsole& operator=(DevConsole&&) = delete;

    /// Create the panel under `parent` (a root when invalid): a bordered panel, `visible_rows` row
    /// labels and the input line. The store and the painter must outlive the console.
    [[nodiscard]] Status create(ElementStore& store, TextPainter& text, ElementId parent,
                                const ConsoleStyle& style) noexcept;

    /// Register a command. Names are matched exactly; a second registration of a name replaces it.
    [[nodiscard]] Status add_command(std::string_view name, ConsoleCommandFn command,
                                     void* user) noexcept;

    /// Append a line to the scrollback, and follow it to the bottom.
    [[nodiscard]] Status print(std::string_view line, u32 colour = kConsoleInfo) noexcept;
    /// Characters at the caret, as a text input event delivers them.
    [[nodiscard]] Status type(std::string_view characters) noexcept;
    /// Delete the character before the caret.
    [[nodiscard]] Status backspace() noexcept;
    /// Echo the input line, run the command it names, print its reply, and clear the line.
    [[nodiscard]] Status submit() noexcept;
    /// Scroll the scrollback by `lines` — positive is back in time — clamped to the history.
    [[nodiscard]] Status scroll(i32 lines) noexcept;
    /// Empty the scrollback.
    [[nodiscard]] Status clear() noexcept;

    /// Show or hide the whole panel. Hidden is collapsed: no layout, no primitives.
    [[nodiscard]] Status set_open(bool open) noexcept;
    [[nodiscard]] bool is_open() const noexcept { return open_; }

    /// Lines held in the scrollback, at most `ConsoleStyle::history`.
    [[nodiscard]] u32 line_count() const noexcept { return count_; }
    /// The `index`th line, oldest first, or empty.
    [[nodiscard]] std::string_view line(u32 index) const noexcept;
    /// How many lines back from the newest the bottom row shows.
    [[nodiscard]] u32 scroll_offset() const noexcept { return scroll_; }
    [[nodiscard]] std::string_view input() const noexcept;

    [[nodiscard]] ElementId panel() const noexcept { return panel_; }
    /// The row elements, top to bottom. Exactly `visible_rows` of them, however long the history.
    [[nodiscard]] Span<const ElementId> rows() const noexcept { return rows_.span(); }
    [[nodiscard]] ElementId input_line() const noexcept { return input_line_; }

private:
    struct Line {
        char text[kConsoleLineCapacity] = {};
        u32 length = 0;
        u32 colour = kConsoleInfo;
    };
    struct Command {
        char name[32] = {};
        u32 length = 0;
        ConsoleCommandFn function = nullptr;
        void* user = nullptr;
    };

    [[nodiscard]] Expected<ElementId, Error> make_label(ElementId parent) noexcept;
    /// Rebind every row to the line it now shows, and the input line to the input.
    [[nodiscard]] Status refresh() noexcept;
    [[nodiscard]] Status run(std::string_view name, std::string_view arguments) noexcept;
    [[nodiscard]] Status help() noexcept;

    ElementStore* store_ = nullptr;
    TextPainter* text_ = nullptr;
    ConsoleStyle style_{};
    ElementId panel_;
    ElementId input_line_;
    Array<ElementId> rows_;
    Array<Line> lines_;
    Array<Command> commands_;
    /// The ring: `head_` is where the next line goes.
    u32 head_ = 0;
    u32 count_ = 0;
    u32 scroll_ = 0;
    Line input_{};
    bool open_ = true;
};

}  // namespace cy::ui

#endif  // CY_UI_CONSOLE_CONSOLE_H
