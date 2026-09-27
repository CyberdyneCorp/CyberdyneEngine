// SPDX-License-Identifier: MIT
#include <cy/rendering/selection/selection_component.h>

#include <cy/ecs/query.h>

#include <utility>

namespace cy::rendering::selection {
namespace {

[[nodiscard]] HighlightColour unpack(u32 packed) noexcept {
    return HighlightColour{static_cast<u8>(packed & 0xFFU), static_cast<u8>((packed >> 8U) & 0xFFU),
                           static_cast<u8>((packed >> 16U) & 0xFFU),
                           static_cast<u8>((packed >> 24U) & 0xFFU)};
}

}  // namespace

Expected<ecs::ComponentTypeId, Error> register_selection_highlight(ecs::World& world) noexcept {
    // Idempotent by name: a second view over the same world binds to the same id.
    if (const ecs::ComponentInfo* existing =
            world.components().find(kSelectionHighlightComponentName);
        existing != nullptr) {
        return existing->id;
    }
    return world.components().register_builtin(kSelectionHighlightComponentName,
                                               static_cast<u32>(sizeof(SelectionHighlight)),
                                               static_cast<u32>(alignof(SelectionHighlight)));
}

Expected<GatherReport, Error> gather_highlights(ecs::World& world, ecs::ComponentTypeId component,
                                                HighlightSet& out) noexcept {
    out.clear();
    ecs::QueryDesc desc(world.allocator());
    if (Status declared = desc.read(component); !declared) {
        return make_unexpected(declared.error());
    }
    ecs::Query query(world, std::move(desc));
    GatherReport report;
    Status failure = ok();
    Status iterated = query.for_each_chunk([&](ecs::QueryChunk& chunk) noexcept {
        if (!failure) {
            return;
        }
        const Span<const SelectionHighlight> marks = chunk.read<SelectionHighlight>(component);
        const Span<const ecs::Entity> entities = chunk.entities();
        for (u32 row = 0; row < chunk.count(); ++row) {
            const u32 kind = marks[row].kind;
            if (kind == 0) {
                continue;
            }
            if (kind != static_cast<u32>(HighlightKind::Selected) &&
                kind != static_cast<u32>(HighlightKind::Hovered)) {
                ++report.skipped;
                continue;
            }
            failure = out.mark(entities[row].bits(), static_cast<HighlightKind>(kind),
                               unpack(marks[row].colour));
            if (!failure) {
                return;
            }
            ++report.marked;
        }
    });
    if (!iterated) {
        return make_unexpected(iterated.error());
    }
    if (!failure) {
        return make_unexpected(failure.error());
    }
    return report;
}

}  // namespace cy::rendering::selection
