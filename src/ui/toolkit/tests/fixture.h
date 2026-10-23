// SPDX-License-Identifier: MIT
// The toolkit suites' shared fixture: a store, a text server on the built-in font, and a started
// toolkit. No device, no file, no window — the built-in font's cells are six by thirteen units, so
// every size a case reads is arithmetic.
#pragma once

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/ui/text/builtin_font.h>
#include <cy/ui/text/text_painter.h>
#include <cy/ui/toolkit/toolkit.h>

#include <string_view>

namespace cy::ui::toolkit_test {

inline Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Engine);
}

/// One character of the built-in font, and one line of it.
inline constexpr f32 kCharWidth = 6.0F;
inline constexpr f32 kLineHeight = 13.0F;

struct Kit {
    Kit() noexcept {
        cy::text::TextServerConfig config;
        config.atlas.initial_extent = 256;
        config.atlas.maximum_extent = 256;
        started = server.start(config).has_value() &&
                  text.start(server, builtin_font(), 1).has_value() && toolkit.start().has_value();
        settings.mode = ScaleMode::FixedPixel;
        settings.reference = Vec2{800.0F, 600.0F};
    }

    /// One frame of `seconds`.
    [[nodiscard]] bool frame(f32 seconds = 1.0F / 60.0F) noexcept {
        return toolkit.update(seconds, settings, viewport, &report).has_value();
    }

    [[nodiscard]] WidgetId make(WidgetId parent, WidgetKind kind, std::string_view id = {},
                                std::string_view caption = {}) noexcept {
        WidgetDesc desc;
        desc.kind = kind;
        desc.id = id;
        desc.text = caption;
        auto made = toolkit.create(parent, desc);
        return made.has_value() ? *made : kNoElement;
    }

    [[nodiscard]] Rect rect(ElementId element) const noexcept {
        const LayoutOutput* output = store.layout_output(element);
        return output != nullptr ? output->rect : Rect{};
    }

    [[nodiscard]] Vec2 centre(ElementId element) const noexcept {
        const Rect r = rect(element);
        return Vec2{r.x + (r.width * 0.5F), r.y + (r.height * 0.5F)};
    }

    /// Press and release the pointer at `point`: a click.
    void click(Vec2 point) noexcept {
        (void)toolkit.pointer(PointerInput{point, true, {}, 0});
        (void)toolkit.pointer(PointerInput{point, false, {}, 0});
    }

    [[nodiscard]] u32 count_events(WidgetEventKind kind,
                                   WidgetId widget = kNoElement) const noexcept {
        u32 count = 0;
        for (const WidgetEvent& event : toolkit.events()) {
            if (event.kind == kind && (!widget.is_valid() || event.widget == widget)) {
                ++count;
            }
        }
        return count;
    }

    ElementStore store{allocator()};
    cy::text::TextServer server;
    TextPainter text{allocator()};
    Toolkit toolkit{allocator(), store, text};
    ScaleSettings settings;
    Vec2 viewport{800.0F, 600.0F};
    ToolkitReport report;
    bool started = false;
};

}  // namespace cy::ui::toolkit_test
