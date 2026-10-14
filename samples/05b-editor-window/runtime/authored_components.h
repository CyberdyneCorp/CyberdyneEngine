// SPDX-License-Identifier: MIT
#pragma once
// The authored world as the runtime host's seams read it: one component of one node by field name,
// and a mesh reference's triangles from its `.cyprim` source or its cooked asset. Shared by the
// navigation seam (nav_runtime.h) and the GI seam (gi_runtime.h), which read the same document.

#include <cy/core/base/expected.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/import/mesh.h>
#include <cy/scene/serialization/worldfile.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace cy::sample::editor_window {

namespace ser = scene::serialization;

/// One component of one node, read by field name. `present` is false when the node lacks it.
class ComponentReader {
public:
    ComponentReader(const ser::World& world, const ser::WorldNode& node,
                    std::string_view type_name) noexcept
        : world_(&world) {
        for (const ser::WorldComponent& component : node.components()) {
            const ser::WorldTypeDecl* type = world.type(component.file_type);
            if (type != nullptr && world.text(type->name) == type_name) {
                component_ = &component;
                type_ = type;
                return;
            }
        }
    }

    [[nodiscard]] bool present() const noexcept { return component_ != nullptr; }

    [[nodiscard]] const ser::WorldValue* value(std::string_view name) const noexcept {
        if (component_ == nullptr) {
            return nullptr;
        }
        for (const ser::WorldFieldDecl& declaration : type_->fields()) {
            if (world_->text(declaration.name) == name) {
                const ser::WorldField* field = component_->find(declaration.file_field);
                return field == nullptr ? nullptr : &field->value;
            }
        }
        return nullptr;
    }

    [[nodiscard]] f32 real(std::string_view name, f32 fallback) const noexcept {
        const ser::WorldValue* found = value(name);
        if (found == nullptr) {
            return fallback;
        }
        if (found->kind == ser::WorldValueKind::Float) {
            return found->lanes[0];
        }
        return found->kind == ser::WorldValueKind::Int ? static_cast<f32>(found->integer)
                                                       : fallback;
    }

    [[nodiscard]] i64 integer(std::string_view name, i64 fallback) const noexcept {
        const ser::WorldValue* found = value(name);
        return found != nullptr && found->kind == ser::WorldValueKind::Int ? found->integer
                                                                           : fallback;
    }

    [[nodiscard]] bool flag(std::string_view name, bool fallback) const noexcept {
        const ser::WorldValue* found = value(name);
        return found != nullptr && found->kind == ser::WorldValueKind::Bool ? found->integer != 0
                                                                            : fallback;
    }

    [[nodiscard]] Vec3 vector(std::string_view name, Vec3 fallback) const noexcept {
        const ser::WorldValue* found = value(name);
        return found != nullptr && found->kind == ser::WorldValueKind::Vec3
                   ? Vec3{found->lanes[0], found->lanes[1], found->lanes[2]}
                   : fallback;
    }

    [[nodiscard]] std::string text(std::string_view name) const {
        const ser::WorldValue* found = value(name);
        if (found == nullptr || found->kind != ser::WorldValueKind::Text) {
            return {};
        }
        const Span<const u8> bytes = world_->blob(*found);
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }

    /// A u64 recorded as an `int` (its bit pattern) or as decimal or `0x` text.
    [[nodiscard]] u64 word(std::string_view name) const {
        const ser::WorldValue* found = value(name);
        if (found != nullptr && found->kind == ser::WorldValueKind::Int) {
            u64 bits = 0;
            std::memcpy(&bits, &found->integer, sizeof(bits));
            return bits;
        }
        const std::string spelled = text(name);
        return spelled.empty() ? 0 : std::strtoull(spelled.c_str(), nullptr, 0);
    }

    /// The `world` field as a navigation world's id.
    [[nodiscard]] u32 world_id() const noexcept {
        return static_cast<u32>(integer("world", 0) & 0xFFFF'FFFF);
    }

private:
    const ser::World* world_;
    const ser::WorldComponent* component_ = nullptr;
    const ser::WorldTypeDecl* type_ = nullptr;
};

/// A `.cyprim` source's mesh, built by the importer's own primitive builder.
[[nodiscard]] Status read_primitive_mesh(const std::string& path, Allocator& allocator,
                                         import::MeshData& out) noexcept;

/// A cooked mesh asset's mesh.
[[nodiscard]] Status read_cooked_mesh_file(const std::string& path, Allocator& allocator,
                                           import::MeshData& out) noexcept;

/// The mesh a `MeshRenderer.mesh` reference names in `project`: its `.cyprim` beside the world, or
/// its cooked asset under `.cy/cooked/`. Validated.
[[nodiscard]] Status read_mesh_reference(const std::string& project, const std::string& reference,
                                         Allocator& allocator, import::MeshData& out) noexcept;

}  // namespace cy::sample::editor_window
