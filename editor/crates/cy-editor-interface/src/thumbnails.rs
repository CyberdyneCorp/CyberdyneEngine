//! Asset previews: a render where the engine can make one, and a **typed** placeholder until it can.
//!
//! `editor-visual-language`: "The content browser SHALL present assets as **useful previews** rather
//! than generic file icons wherever a visual representation is meaningful ... Thumbnails SHALL be
//! produced by the engine's own renderer, so that a preview and the shipping image are the same
//! path. Thumbnails SHALL be generated in the background, SHALL be cached, and SHALL degrade to a
//! **typed placeholder** rather than blocking the browser."
//!
//! The forbidden pattern this makes checkable is "a generic file icon where the engine could render
//! a preview". [`Thumbnail`] has no variant for one: the state before a render arrives is
//! [`Thumbnail::Pending`], which carries the asset's [`Kind`] and therefore draws as *a mesh that is
//! still rendering* rather than as a page with a corner turned down. There is nowhere to put a
//! generic icon.
//!
//! --- WHAT THIS MODULE IS NOT ------------------------------------------------------------------------
//!
//! It does not render. The requirement is that the engine's own renderer produces the image, which
//! makes a thumbnail a *request* the shell issues over the live bridge and an image that comes back
//! — the transport being `editor-viewport-and-gizmos` and `live-editing`'s business. What is here is
//! the browser's half: which asset needs one, what is shown meanwhile, what is cached, and the
//! guarantee that asking never blocks.

use std::collections::BTreeMap;

/// What an asset is, which is what a placeholder says while its render is being made.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum Kind {
    /// A mesh: rendered as itself.
    Mesh,
    /// A material: rendered on a representative surface.
    Material,
    /// A texture: shown as its own content.
    Texture,
    /// A prefab: rendered as a recognisable miniature.
    Prefab,
    /// A world.
    World,
    /// A script or a graph, which has no meaningful render.
    Script,
    /// Something the editor has no renderer for.
    Other,
}

impl Kind {
    /// The word a placeholder shows, so that a preview that is not ready still says what it is.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            Kind::Mesh => "Mesh",
            Kind::Material => "Material",
            Kind::Texture => "Texture",
            Kind::Prefab => "Prefab",
            Kind::World => "World",
            Kind::Script => "Script",
            Kind::Other => "Asset",
        }
    }

    /// Whether the engine can render a meaningful preview of this kind.
    ///
    /// A script cannot be rendered and does not pretend otherwise; everything visual can, which is
    /// what makes "wherever a visual representation is meaningful" a decision the type makes rather
    /// than a judgement each browser makes again.
    #[must_use]
    pub const fn is_renderable(self) -> bool {
        matches!(
            self,
            Kind::Mesh | Kind::Material | Kind::Texture | Kind::Prefab | Kind::World
        )
    }

    /// The kind an asset path implies, from its extension.
    #[must_use]
    pub fn of_path(path: &str) -> Self {
        match path.rsplit('.').next().unwrap_or_default() {
            "cymesh" | "gltf" | "glb" => Kind::Mesh,
            "cymat" => Kind::Material,
            "cytex" | "png" | "ktx2" => Kind::Texture,
            "cyprefab" => Kind::Prefab,
            "cyworld" => Kind::World,
            "swift" | "cygraph" => Kind::Script,
            _ => Kind::Other,
        }
    }
}

/// An image the engine produced, identified rather than held.
///
/// The bytes belong to whatever holds textures — which at M5 is not this crate and may never be —
/// so a thumbnail is a handle plus the revision it was made at. The revision is what makes a stale
/// preview detectable: an asset edited after its thumbnail was rendered has a newer one.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Rendered {
    /// The image, as whatever holds it knows it.
    pub image: u64,
    /// The asset revision it was rendered from.
    pub revision: u64,
}

/// What the browser draws for one asset.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Thumbnail {
    /// The engine's render.
    Render(Rendered),
    /// A render has been asked for and has not arrived. **Typed**, never generic.
    Pending(Kind),
    /// No render is possible for this kind, so the kind itself is what is shown.
    Typed(Kind),
}

impl Thumbnail {
    /// What kind of asset this is, whatever state the thumbnail is in.
    #[must_use]
    pub const fn kind(&self, known: Kind) -> Kind {
        match self {
            Thumbnail::Pending(kind) | Thumbnail::Typed(kind) => *kind,
            Thumbnail::Render(_) => known,
        }
    }

    /// Whether this is an image rather than a placeholder.
    #[must_use]
    pub const fn is_render(&self) -> bool {
        matches!(self, Thumbnail::Render(_))
    }
}

/// The browser's thumbnail cache.
///
/// Bounded, because a project browser scrolled through a hundred thousand assets would otherwise
/// hold a hundred thousand images. The eviction is the oldest request rather than a least-recently
/// used policy: scrolling is the access pattern, and under scrolling the two are the same.
#[derive(Debug)]
pub struct Thumbnails {
    entries: BTreeMap<String, Thumbnail>,
    order: Vec<String>,
    capacity: usize,
    requests: u64,
}

impl Default for Thumbnails {
    fn default() -> Self {
        Self::new(2_048)
    }
}

impl Thumbnails {
    /// A cache holding at most `capacity` thumbnails.
    #[must_use]
    pub fn new(capacity: usize) -> Self {
        Self {
            entries: BTreeMap::new(),
            order: Vec::new(),
            capacity: capacity.max(1),
            requests: 0,
        }
    }

    /// What to draw for an asset, asking for a render if none has been asked for.
    ///
    /// **Never blocks and never fails.** A browser calls this once per visible row per frame, and
    /// the answer for an asset nobody has rendered yet is a typed placeholder rather than a wait.
    pub fn thumbnail(&mut self, path: &str) -> Thumbnail {
        if let Some(held) = self.entries.get(path) {
            return held.clone();
        }
        let kind = Kind::of_path(path);
        let thumbnail = if kind.is_renderable() {
            self.requests += 1;
            Thumbnail::Pending(kind)
        } else {
            Thumbnail::Typed(kind)
        };
        self.insert(path.to_string(), thumbnail.clone());
        thumbnail
    }

    /// Record an image the engine produced.
    pub fn rendered(&mut self, path: &str, rendered: Rendered) {
        self.insert(path.to_string(), Thumbnail::Render(rendered));
    }

    /// Forget an asset's thumbnail, because the asset changed.
    pub fn invalidate(&mut self, path: &str) {
        self.entries.remove(path);
        self.order.retain(|held| held != path);
    }

    /// How many renders have been asked for.
    ///
    /// What a test counts to show that scrolling asks for what is visible rather than for the
    /// project.
    #[must_use]
    pub const fn requests(&self) -> u64 {
        self.requests
    }

    /// How many thumbnails are held.
    #[must_use]
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    /// Whether none is held.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    fn insert(&mut self, path: String, thumbnail: Thumbnail) {
        if !self.entries.contains_key(&path) {
            self.order.push(path.clone());
            if self.order.len() > self.capacity {
                let evicted = self.order.remove(0);
                self.entries.remove(&evicted);
            }
        }
        self.entries.insert(path, thumbnail);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::virtualise::{Viewport, Window};

    #[test]
    fn a_pending_thumbnail_says_what_the_asset_is_rather_than_showing_a_file_icon() {
        // The forbidden pattern: "a generic file icon where the engine could render a preview".
        // There is no variant for one, so the strongest statement a test can make is that the
        // placeholder carries the kind.
        let mut thumbnails = Thumbnails::default();
        let pending = thumbnails.thumbnail("meshes/rock.cymesh");
        assert_eq!(pending, Thumbnail::Pending(Kind::Mesh));
        assert_eq!(pending.kind(Kind::Mesh).label(), "Mesh");
    }

    #[test]
    fn browsing_never_blocks_and_asks_only_for_what_is_visible() {
        // "WHEN thumbnails are still generating THEN the browser SHALL remain navigable with
        // placeholders", and the cost of a hundred thousand assets is what is on screen.
        let assets: Vec<String> = (0..100_000)
            .map(|number| format!("assets/props/prop_{number:06}.cymesh"))
            .collect();
        let mut thumbnails = Thumbnails::default();

        let window = Window::of(assets.len(), Viewport::new(0.0, 600.0, 20.0));
        for asset in window.slice(&assets) {
            assert!(matches!(thumbnails.thumbnail(asset), Thumbnail::Pending(_)));
        }
        assert_eq!(thumbnails.requests(), window.count as u64);
        assert!(
            thumbnails.len() < 64,
            "browsing asked for {} thumbnails on one screen",
            thumbnails.len()
        );
    }

    #[test]
    fn a_render_replaces_the_placeholder_and_is_cached() {
        let mut thumbnails = Thumbnails::default();
        thumbnails.thumbnail("materials/brick.cymat");
        thumbnails.rendered(
            "materials/brick.cymat",
            Rendered {
                image: 7,
                revision: 3,
            },
        );

        let held = thumbnails.thumbnail("materials/brick.cymat");
        assert!(held.is_render());
        assert_eq!(
            thumbnails.requests(),
            1,
            "the second look asked for nothing"
        );
    }

    #[test]
    fn an_asset_that_changed_is_rendered_again() {
        let mut thumbnails = Thumbnails::default();
        thumbnails.rendered(
            "meshes/rock.cymesh",
            Rendered {
                image: 1,
                revision: 1,
            },
        );
        thumbnails.invalidate("meshes/rock.cymesh");
        assert_eq!(
            thumbnails.thumbnail("meshes/rock.cymesh"),
            Thumbnail::Pending(Kind::Mesh)
        );
    }

    #[test]
    fn a_kind_with_no_meaningful_render_is_shown_as_its_kind_and_asks_for_nothing() {
        let mut thumbnails = Thumbnails::default();
        assert_eq!(
            thumbnails.thumbnail("scripts/Harvester.swift"),
            Thumbnail::Typed(Kind::Script)
        );
        assert_eq!(thumbnails.requests(), 0, "nothing rendered a script");
    }

    #[test]
    fn the_cache_is_bounded_so_a_long_scroll_does_not_hold_the_project() {
        let mut thumbnails = Thumbnails::new(64);
        for number in 0..1_000 {
            thumbnails.thumbnail(&format!("assets/props/prop_{number:06}.cymesh"));
        }
        assert_eq!(thumbnails.len(), 64);
    }

    #[test]
    fn similar_units_are_distinguished_by_their_own_renders() {
        // "WHEN a project contains many visually similar unit assets THEN their thumbnails SHALL be
        // renders that distinguish them, not one shared type icon." The property a model can hold:
        // a render is per asset, so two assets never share one.
        let mut thumbnails = Thumbnails::default();
        thumbnails.rendered(
            "units/harvester_a.cymesh",
            Rendered {
                image: 1,
                revision: 1,
            },
        );
        thumbnails.rendered(
            "units/harvester_b.cymesh",
            Rendered {
                image: 2,
                revision: 1,
            },
        );
        assert_ne!(
            thumbnails.thumbnail("units/harvester_a.cymesh"),
            thumbnails.thumbnail("units/harvester_b.cymesh")
        );
    }
}
