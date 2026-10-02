// SPDX-License-Identifier: MIT
#include "hud.h"

#include <cy/ui/layout.h>

#include <algorithm>
#include <cstdio>

namespace cy::sample::rts {
namespace {

using ui::Align;
using ui::Dirty;
using ui::ElementFlags;
using ui::ElementId;
using ui::FlexDirection;
using ui::Insets;
using ui::LayoutInput;
using ui::LayoutModel;
using ui::PaintData;

// Premultiplied 0xAARRGGBB, as every interface colour is.
constexpr u32 kPanel = 0xC80E141CU;
constexpr u32 kPanelBorder = 0xFF3C5064U;
constexpr u32 kText = 0xFFE6EBF0U;
constexpr u32 kDim = 0xFF8C98A6U;
constexpr u32 kGold = 0xFFFFD34DU;
constexpr u32 kWood = 0xFF9BD46AU;
constexpr u32 kFood = 0xFFF0A070U;
constexpr u32 kMapGround = 0xFF1E3A24U;
constexpr u32 kMapWater = 0xFF1F4E79U;
constexpr u32 kMapBorder = 0xFF8FA3B5U;
constexpr u32 kCamera = 0xFFFFFFFFU;
constexpr u32 kHealthBack = 0xFF3A1E1EU;
constexpr u32 kHealth = 0xFF4CC34CU;
constexpr u32 kTeamColours[3] = {0xFF288CFFU, 0xFFFF6040U, 0xFFC8C8C8U};

void place(LayoutInput& input, Vec2 anchor_min, Vec2 anchor_max, Vec2 offset_min,
           Vec2 offset_max) noexcept {
    input.anchor_min = anchor_min;
    input.anchor_max = anchor_max;
    input.offset_min = offset_min;
    input.offset_max = offset_max;
}

void fill(PaintData& paint, u32 background, u32 border = 0, f32 radius = 0.0F) noexcept {
    paint.background = background;
    paint.border_colour = border;
    paint.border_width = border != 0U ? 1.0F : 0.0F;
    paint.corner_radius = radius;
}

}  // namespace

Expected<ElementId, Error> Hud::element(ElementId parent, const char* type) noexcept {
    return store_->create(parent, Name::intern(type));
}

Status Hud::set_text(ElementId target, const char* text, u32 colour) noexcept {
    ui::TextStyle style;
    style.colour = colour;
    if (Status set = text_->set_text(target, text, style); !set) {
        return set;
    }
    // A different string may be a different width: measure again, and repaint.
    store_->mark(target, Dirty::Measure | Dirty::Paint);
    return ok();
}

Expected<ElementId, Error> Hud::label(ElementId parent, const char* text, u32 colour) noexcept {
    Expected<ElementId, Error> made = element(parent, "label");
    if (!made.has_value()) {
        return made;
    }
    if (Status set = set_text(*made, text, colour); !set) {
        return make_unexpected(set.error());
    }
    return made;
}

Status Hud::create(ui::ElementStore& store, ui::TextPainter& text, ElementId root) noexcept {
    if (store_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "hud: already created");
    }
    store_ = &store;
    text_ = &text;
    if (Status made = create_bar(root); !made) {
        return made;
    }
    if (Status made = create_minimap(root); !made) {
        return made;
    }
    return create_selection(root);
}

Status Hud::create_bar(ElementId root) noexcept {
    // THE RESOURCE BAR: a strip across the top, a row of icon-and-count pairs.
    Expected<ElementId, Error> bar = element(root, "resource-bar");
    if (!bar.has_value()) {
        return make_unexpected(bar.error());
    }
    bar_ = *bar;
    LayoutInput& input = *store_->layout_input(bar_);
    place(input, Vec2{0.0F, 0.0F}, Vec2{1.0F, 0.0F}, Vec2{0.0F, 0.0F}, Vec2{0.0F, 17.0F});
    input.model = LayoutModel::Flex;
    input.direction = FlexDirection::Row;
    input.align = Align::Centre;
    input.gap = 4.0F;
    input.padding = Insets{6.0F, 2.0F, 6.0F, 2.0F};
    fill(*store_->paint(bar_), kPanel, 0);

    struct Item {
        u32 colour;
        ElementId* count;
    };
    const Item items[] = {{kGold, &gold_}, {kWood, &wood_}, {kFood, &food_}};
    bool first = true;
    for (const Item& item : items) {
        Expected<ElementId, Error> icon = element(bar_, "icon");
        if (!icon.has_value()) {
            return make_unexpected(icon.error());
        }
        LayoutInput& icon_input = *store_->layout_input(*icon);
        icon_input.preferred = Vec2{7.0F, 7.0F};
        icon_input.margin.left = first ? 0.0F : 10.0F;
        fill(*store_->paint(*icon), item.colour, 0, 2.0F);
        Expected<ElementId, Error> count = label(bar_, "0", item.colour);
        if (!count.has_value()) {
            return make_unexpected(count.error());
        }
        *item.count = *count;
        first = false;
    }
    return ok();
}

Status Hud::create_minimap(ElementId root) noexcept {
    // THE MINIMAP: anchored to the bottom-right corner, clipping what it holds — the camera's
    // rectangle runs off its edge when the camera looks past the map's.
    Expected<ElementId, Error> map = element(root, "minimap");
    if (!map.has_value()) {
        return make_unexpected(map.error());
    }
    minimap_ = *map;
    LayoutInput& input = *store_->layout_input(minimap_);
    place(input, Vec2{1.0F, 1.0F}, Vec2{1.0F, 1.0F}, Vec2{-108.0F, -80.0F}, Vec2{-6.0F, -6.0F});
    input.model = LayoutModel::Absolute;
    fill(*store_->paint(minimap_), kMapGround, kMapBorder);
    if (Status flagged =
            store_->set_flags(minimap_, ElementFlags::Visible | ElementFlags::ClipsChildren);
        !flagged) {
        return flagged;
    }

    Expected<ElementId, Error> lake = element(minimap_, "water");
    if (!lake.has_value()) {
        return make_unexpected(lake.error());
    }
    place(*store_->layout_input(*lake), Vec2{0.55F, 0.12F}, Vec2{0.9F, 0.42F}, Vec2{0.0F, 0.0F},
          Vec2{0.0F, 0.0F});
    fill(*store_->paint(*lake), kMapWater, 0, 6.0F);

    for (ElementId& dot : dots_) {
        Expected<ElementId, Error> made = element(minimap_, "unit");
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        dot = *made;
        if (Status hidden = store_->set_flags(dot, ElementFlags::Collapsed); !hidden) {
            return hidden;
        }
    }

    Expected<ElementId, Error> camera = element(minimap_, "camera");
    if (!camera.has_value()) {
        return make_unexpected(camera.error());
    }
    camera_ = *camera;
    // A frame and nothing inside it: no background, a one-unit border.
    fill(*store_->paint(camera_), 0, kCamera);
    return ok();
}

Status Hud::create_row(Row& row) noexcept {
    Expected<ElementId, Error> made = element(selection_, "unit-row");
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    row.row = *made;
    LayoutInput& input = *store_->layout_input(row.row);
    input.model = LayoutModel::Flex;
    input.direction = FlexDirection::Row;
    input.align = Align::Centre;
    input.gap = 6.0F;
    input.preferred.y = 13.0F;

    made = label(row.row, "", kText);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    row.name = *made;
    store_->layout_input(row.name)->preferred.x = 54.0F;

    made = element(row.row, "health-bar");
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    row.bar = *made;
    LayoutInput& bar = *store_->layout_input(row.bar);
    bar.model = LayoutModel::Absolute;
    bar.preferred = Vec2{50.0F, 5.0F};
    fill(*store_->paint(row.bar), kHealthBack, 0, 1.0F);

    made = element(row.bar, "health");
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    row.fill = *made;
    fill(*store_->paint(row.fill), kHealth, 0, 1.0F);

    made = label(row.row, "", kDim);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    row.health = *made;
    return ok();
}

Status Hud::create_selection(ElementId root) noexcept {
    // THE SELECTION PANEL: bottom-left, a title and one row per selected unit.
    Expected<ElementId, Error> panel = element(root, "selection");
    if (!panel.has_value()) {
        return make_unexpected(panel.error());
    }
    selection_ = *panel;
    LayoutInput& input = *store_->layout_input(selection_);
    place(input, Vec2{0.0F, 1.0F}, Vec2{0.0F, 1.0F}, Vec2{6.0F, -64.0F}, Vec2{186.0F, -6.0F});
    input.model = LayoutModel::Flex;
    input.direction = FlexDirection::Column;
    input.padding = Insets{5.0F, 3.0F, 5.0F, 3.0F};
    fill(*store_->paint(selection_), kPanel, kPanelBorder, 3.0F);
    if (Status flagged =
            store_->set_flags(selection_, ElementFlags::Visible | ElementFlags::ClipsChildren);
        !flagged) {
        return flagged;
    }
    Expected<ElementId, Error> title = label(selection_, "Nothing selected", kDim);
    if (!title.has_value()) {
        return make_unexpected(title.error());
    }
    title_ = *title;
    for (Row& row : rows_) {
        if (Status made = create_row(row); !made) {
            return made;
        }
        if (Status hidden = store_->set_flags(row.row, ElementFlags::Collapsed); !hidden) {
            return hidden;
        }
    }
    return ok();
}

Status Hud::set_resources(const Resources& resources) noexcept {
    char text[32];
    (void)std::snprintf(text, sizeof(text), "%u", resources.gold);
    if (Status set = set_text(gold_, text, kGold); !set) {
        return set;
    }
    (void)std::snprintf(text, sizeof(text), "%u", resources.wood);
    if (Status set = set_text(wood_, text, kWood); !set) {
        return set;
    }
    (void)std::snprintf(text, sizeof(text), "%u/%u", resources.food, resources.food_cap);
    return set_text(food_, text, kFood);
}

Status Hud::set_selection(Span<const SelectedUnit> units) noexcept {
    char text[48];
    if (units.empty()) {
        (void)std::snprintf(text, sizeof(text), "Nothing selected");
    } else if (units.size() > kSelectionRows) {
        (void)std::snprintf(text, sizeof(text), "Selected: %u units (%u more)",
                            static_cast<u32>(units.size()),
                            static_cast<u32>(units.size()) - kSelectionRows);
    } else {
        (void)std::snprintf(text, sizeof(text), "Selected: %u unit%s",
                            static_cast<u32>(units.size()), units.size() == 1 ? "" : "s");
    }
    if (Status set = set_text(title_, text, units.empty() ? kDim : kText); !set) {
        return set;
    }
    for (u32 index = 0; index < kSelectionRows; ++index) {
        Row& row = rows_[index];
        const bool shown = index < units.size();
        if (Status flagged =
                store_->set_flags(row.row, shown ? ElementFlags::Visible : ElementFlags::Collapsed);
            !flagged) {
            return flagged;
        }
        store_->mark(row.row, Dirty::Measure | Dirty::Arrange);
        if (!shown) {
            continue;
        }
        const SelectedUnit& unit = units[index];
        if (Status set = set_text(row.name, unit.name, kText); !set) {
            return set;
        }
        (void)std::snprintf(text, sizeof(text), "%u/%u", unit.health, unit.max_health);
        if (Status set = set_text(row.health, text, kDim); !set) {
            return set;
        }
        const f32 ratio =
            unit.max_health == 0U
                ? 0.0F
                : std::min(1.0F, static_cast<f32>(unit.health) / static_cast<f32>(unit.max_health));
        place(*store_->layout_input(row.fill), Vec2{0.0F, 0.0F}, Vec2{ratio, 1.0F},
              Vec2{0.0F, 0.0F}, Vec2{0.0F, 0.0F});
        store_->mark(row.fill, Dirty::Arrange);
    }
    store_->mark(selection_, Dirty::Measure | Dirty::Arrange);
    return ok();
}

Status Hud::set_minimap(Span<const MinimapDot> dots, ui::Rect camera) noexcept {
    for (u32 index = 0; index < kMinimapDots; ++index) {
        const ElementId dot = dots_[index];
        const bool shown = index < dots.size();
        if (Status flagged =
                store_->set_flags(dot, shown ? ElementFlags::Visible : ElementFlags::Collapsed);
            !flagged) {
            return flagged;
        }
        if (shown) {
            const MinimapDot& where = dots[index];
            place(*store_->layout_input(dot), Vec2{where.x, where.y}, Vec2{where.x, where.y},
                  Vec2{-1.5F, -1.5F}, Vec2{1.5F, 1.5F});
            fill(*store_->paint(dot), kTeamColours[static_cast<u32>(where.team)], 0, 0.0F);
        }
        store_->mark(dot, Dirty::Arrange | Dirty::Paint);
    }
    place(*store_->layout_input(camera_), Vec2{camera.x, camera.y},
          Vec2{camera.right(), camera.bottom()}, Vec2{0.0F, 0.0F}, Vec2{0.0F, 0.0F});
    store_->mark(camera_, Dirty::Arrange | Dirty::Paint);
    store_->mark(minimap_, Dirty::Arrange);
    return ok();
}

}  // namespace cy::sample::rts
