#pragma once
// What the runtime holds on the editor's behalf: which of its objects the editor's selection means,
// and what a transaction the editor committed does to them. M7 tasks 5b.1, 5b.3 and 5b.4.
//
// ================================================================================================
// WHY THERE IS AN ASSOCIATION HERE AT ALL, AND WHAT IT IS NOT
// ================================================================================================
//
// The editor's document and this runtime's scene are two worlds. `editor-viewport-and-gizmos`
// assigns gizmo geometry to the engine, so the engine has to answer "where is the pivot" — and the
// pivot is a property of the SELECTED OBJECT, which is a thing the document knows about and this
// scene does not.
//
// The complete answer is a shared world: the editor opens the engine's authoring document, the
// runtime loads the same one, and an identity means the same object on both sides. That is M7 task
// 5b.2 and `live-editing`'s at M8, and it is NOT what this file does.
//
// What this file does is the smallest honest thing that makes the interaction real: **the runtime
// associates each identity the editor names with one of its own objects, in first-seen order, and
// keeps that association for the session.** So the gizmo lands on a specific object, the same
// object every frame, and a drag moves that object — which is what the exit criterion is about.
// It is a stand-in for a shared world and it is named as one; a reader should not mistake it for
// one.
//
// ================================================================================================
// WHAT A TRANSACTION MEANS HERE
// ================================================================================================
//
// The editor sends `Message::Apply` carrying the bytes its journal carries — the same encoding, by
// requirement, so that "the journal SHALL be the same operation stream used by … live editing".
// This file decodes exactly one operation out of it, `SetField`, and exactly one shape of it: a
// `Vec3` before and a `Vec3` after.
//
// THE ONE THING IT CANNOT KNOW is which field that is. The identifiers in a transaction are the
// DOCUMENT's, assigned by `DocumentSchema::declare_type` in the order the manifest declared them —
// not the numbers in `types.cytypes`, and not anything this process can derive. So the rule is: a
// Vec3 field that changed is a translation **when the editor's last stated gizmo mode was
// Translate**, which is a signal the runtime legitimately has, and it is ignored otherwise. That
// is exact for a move and deliberately refuses to guess at a scale.
//
// When the worlds are shared this whole paragraph goes away, because the runtime will know the
// schema. Until then it is written down rather than discovered from an object that scaled when
// somebody dragged it.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/servers/render/gizmo.h>

namespace cy::sample::editor_window {

/// The identity the editor named, and the object this runtime gave it.
struct Association {
    u64 identity = 0;
    u32 object = 0;
};

/// What the editor has told this runtime about its selection and its manipulation.
class EditorSession {
public:
    /// The object an identity means, associating it with the next unused one when it is new.
    ///
    /// `object_count` is the scene's, so the association wraps rather than failing: an editor with
    /// more objects selected than the runtime has is a legitimate state, and the alternative — no
    /// gizmo at all — would be less informative than a gizmo on one of them.
    [[nodiscard]] u32 object_for(u64 identity, u32 object_count) noexcept;

    /// The object the gizmo is currently on, or `kNoObject`.
    [[nodiscard]] u32 anchored() const noexcept { return anchored_; }
    void anchor(u32 object) noexcept { anchored_ = object; }

    /// The mode the editor last asked for. What decides whether a changed Vec3 is a translation.
    [[nodiscard]] render::GizmoMode mode() const noexcept { return mode_; }
    void set_mode(render::GizmoMode mode) noexcept { mode_ = mode; }

    /// How many identities have been associated. For the report, and for a test.
    [[nodiscard]] usize associations() const noexcept { return associations_.size(); }

    static constexpr u32 kNoObject = 0xFFFF'FFFFU;

private:
    Array<Association> associations_;
    u32 anchored_ = kNoObject;
    render::GizmoMode mode_ = render::GizmoMode::Translate;
};

/// One translation the editor committed: which object, and by how much.
struct TranslationDelta {
    u64 identity = 0;
    Vec3 amount{0.0F, 0.0F, 0.0F};
};

/// Read the translations out of a transaction the editor applied.
///
/// Returns the number of operations the transaction held, so that a caller can say "twelve
/// operations, one of them a move" rather than silently ignoring eleven. A transaction this decoder
/// does not understand is not an error: the editor is entitled to send a create, a reparent or a
/// field this runtime has no object for, and refusing them would make an ordinary edit look like a
/// protocol failure.
[[nodiscard]] Expected<u32, Error> read_translations(Span<const u8> transaction,
                                                     Array<TranslationDelta>& out) noexcept;

}  // namespace cy::sample::editor_window
