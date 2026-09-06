//! The engine's debug views, as things the editor can request and describe.
//!
//! `editor-viewport-and-gizmos` — "View modes and debug visualisation":
//!
//! > The editor SHALL expose the engine's debug views ... as **selectable view modes**. Every debug
//! > view SHALL state what it shows and how to read it, and SHALL be reachable from the command
//! > palette. Debug views SHALL be requestable per viewport, and SHALL be composable with visibility
//! > filters and isolation.
//!
//! And, from "The rendering responsibility split":
//!
//! > **WHEN** the editor offers an overdraw view **THEN** it SHALL be an engine debug view requested
//! > by the editor, not editor-side drawing.
//!
//! --- WHY THIS IS A MIRROR AND NOT A CATALOGUE OF ITS OWN --------------------------------------------
//!
//! [`ViewMode`] has exactly the discriminants `cy::render::DebugViewMode` has, in exactly its order,
//! because the number that crosses the bridge is the engine's. A view mode the editor could offer
//! and the engine could not draw would be an editor-side drawing waiting to happen — which is the
//! forbidden pattern, one refactor away.
//!
//! `tests::the_mode_list_matches_the_engines` reads `src/servers/render/src/types.cpp` and fails when
//! the two lists drift. That is the same technique `cy-editor-app`'s `tests/profiles.rs` uses on the
//! justfile's profile table, and it is here for the same reason: two lists that must agree, in two
//! languages, with no generator between them.
//!
//! --- WHAT THE SPECIFICATION NAMES AND THE ENGINE HAS NOT GOT YET ------------------------------------
//!
//! The requirement's list is longer than the engine's enum. Lightmap and GI probe visualisation,
//! virtual texture feedback and residency, virtual shadow page views, physics colliders, navigation
//! data, audio emitters and streaming region state are all named there and none of them exists in
//! `DebugViewMode` today, because the subsystems that would draw them arrive at M6 and later.
//!
//! They are **absent rather than stubbed**. An editor that offered "virtual texture feedback" and
//! showed an unlit frame would be worse than one that does not offer it: the user would report the
//! renderer. [`PLANNED_VIEWS`] names them with the capability that owns each, so the gap is a list
//! somebody can act on rather than a discovery.

/// A debug view the engine can draw.
///
/// The discriminants are `cy::render::DebugViewMode`'s. Do not reorder them.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
#[repr(u32)]
pub enum ViewMode {
    /// The shipping image. What the game will look like.
    #[default]
    Off = 0,
    /// Base colour with no lighting.
    Albedo = 1,
    /// Shading normals.
    Normals = 2,
    /// Surface roughness.
    Roughness = 3,
    /// Metalness.
    Metallic = 4,
    /// Ambient occlusion.
    AmbientOcclusion = 5,
    /// World-space position.
    WorldPosition = 6,
    /// Depth.
    Depth = 7,
    /// How many times each pixel was shaded.
    Overdraw = 8,
    /// Geometry as lines.
    Wireframe = 9,
    /// Shader cost per pixel.
    ShadingComplexity = 10,
    /// Lights affecting each pixel.
    LightComplexity = 11,
    /// Light cluster occupancy.
    ClusterOccupancy = 12,
    /// Which level of detail was chosen.
    LodLevel = 13,
    /// Which texture mip was sampled.
    MipLevel = 14,
    /// Per-pixel motion.
    MotionVectors = 15,
    /// The indirect lighting contribution alone.
    GiContribution = 16,
    /// Which shadow cascade shaded each pixel.
    ShadowCascades = 17,
    /// Instance bounding volumes.
    BoundingVolumes = 18,
}

/// Every mode, in the engine's order.
pub const ALL_VIEW_MODES: [ViewMode; 19] = [
    ViewMode::Off,
    ViewMode::Albedo,
    ViewMode::Normals,
    ViewMode::Roughness,
    ViewMode::Metallic,
    ViewMode::AmbientOcclusion,
    ViewMode::WorldPosition,
    ViewMode::Depth,
    ViewMode::Overdraw,
    ViewMode::Wireframe,
    ViewMode::ShadingComplexity,
    ViewMode::LightComplexity,
    ViewMode::ClusterOccupancy,
    ViewMode::LodLevel,
    ViewMode::MipLevel,
    ViewMode::MotionVectors,
    ViewMode::GiContribution,
    ViewMode::ShadowCascades,
    ViewMode::BoundingVolumes,
];

impl ViewMode {
    /// The mode with this discriminant, or `None` when a capture names one this build has not got.
    #[must_use]
    pub fn from_index(index: u32) -> Option<Self> {
        ALL_VIEW_MODES.get(index as usize).copied()
    }

    /// The engine's own spelling, as `debug_view_mode_name` produces it. What crosses the bridge in
    /// a diagnostic and what a console command accepts.
    #[must_use]
    pub const fn engine_name(self) -> &'static str {
        match self {
            ViewMode::Off => "Off",
            ViewMode::Albedo => "Albedo",
            ViewMode::Normals => "Normals",
            ViewMode::Roughness => "Roughness",
            ViewMode::Metallic => "Metallic",
            ViewMode::AmbientOcclusion => "AmbientOcclusion",
            ViewMode::WorldPosition => "WorldPosition",
            ViewMode::Depth => "Depth",
            ViewMode::Overdraw => "Overdraw",
            ViewMode::Wireframe => "Wireframe",
            ViewMode::ShadingComplexity => "ShadingComplexity",
            ViewMode::LightComplexity => "LightComplexity",
            ViewMode::ClusterOccupancy => "ClusterOccupancy",
            ViewMode::LodLevel => "LodLevel",
            ViewMode::MipLevel => "MipLevel",
            ViewMode::MotionVectors => "MotionVectors",
            ViewMode::GiContribution => "GiContribution",
            ViewMode::ShadowCascades => "ShadowCascades",
            ViewMode::BoundingVolumes => "BoundingVolumes",
        }
    }

    /// The identifier a command carries: `viewport.view-mode.overdraw`.
    #[must_use]
    pub fn command_id(self) -> String {
        let mut kebab = String::with_capacity(self.engine_name().len() + 4);
        for (index, character) in self.engine_name().char_indices() {
            if character.is_ascii_uppercase() && index > 0 {
                kebab.push('-');
            }
            kebab.push(character.to_ascii_lowercase());
        }
        format!("viewport.view-mode.{kebab}")
    }

    /// What a person sees in a menu.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            ViewMode::Off => "Lit",
            ViewMode::Albedo => "Base Colour",
            ViewMode::Normals => "Normals",
            ViewMode::Roughness => "Roughness",
            ViewMode::Metallic => "Metallic",
            ViewMode::AmbientOcclusion => "Ambient Occlusion",
            ViewMode::WorldPosition => "World Position",
            ViewMode::Depth => "Depth",
            ViewMode::Overdraw => "Overdraw",
            ViewMode::Wireframe => "Wireframe",
            ViewMode::ShadingComplexity => "Shading Complexity",
            ViewMode::LightComplexity => "Light Complexity",
            ViewMode::ClusterOccupancy => "Cluster Occupancy",
            ViewMode::LodLevel => "Level of Detail",
            ViewMode::MipLevel => "Texture Mip",
            ViewMode::MotionVectors => "Motion Vectors",
            ViewMode::GiContribution => "Indirect Light",
            ViewMode::ShadowCascades => "Shadow Cascades",
            ViewMode::BoundingVolumes => "Bounding Volumes",
        }
    }

    /// What the view shows, in one clause. The first half of the requirement.
    #[must_use]
    pub const fn shows(self) -> &'static str {
        match self {
            ViewMode::Off => "the frame the game will show, at the project's settings",
            ViewMode::Albedo => "each surface's base colour with no lighting applied",
            ViewMode::Normals => "the shading normal at each pixel, as a colour",
            ViewMode::Roughness => "how rough each surface is, from smooth to fully rough",
            ViewMode::Metallic => "which surfaces are metal and which are dielectric",
            ViewMode::AmbientOcclusion => "how occluded each pixel is from ambient light",
            ViewMode::WorldPosition => "each pixel's position in the world, as a colour",
            ViewMode::Depth => "distance from the camera",
            ViewMode::Overdraw => "how many times each pixel was shaded",
            ViewMode::Wireframe => "the geometry's edges, with no shading",
            ViewMode::ShadingComplexity => "the cost of the material that shaded each pixel",
            ViewMode::LightComplexity => "how many lights reached each pixel",
            ViewMode::ClusterOccupancy => "how full each light cluster is",
            ViewMode::LodLevel => "which level of detail the renderer chose for each object",
            ViewMode::MipLevel => "which texture mip level was sampled at each pixel",
            ViewMode::MotionVectors => "how far each pixel moved since the previous frame",
            ViewMode::GiContribution => "the indirect lighting alone, with direct light removed",
            ViewMode::ShadowCascades => "which shadow cascade shaded each pixel",
            ViewMode::BoundingVolumes => "the bounding volume culling and picking use",
        }
    }

    /// How to read it: what a bright pixel means, and what a user should do about one. The second
    /// half of the requirement, and the half that is usually left out.
    #[must_use]
    pub const fn how_to_read(self) -> &'static str {
        match self {
            ViewMode::Off => {
                "This is the reference. Every other view is a diagnostic, not an image \
                              to judge appearance from."
            }
            ViewMode::Albedo => {
                "Physically plausible base colours sit between about 0.04 and 0.9. \
                                 A pure black or pure white surface will look wrong under any \
                                 lighting."
            }
            ViewMode::Normals => {
                "Flat colour across a curved surface means the normals are not \
                                  being interpolated; a hard seam means split vertices."
            }
            ViewMode::Roughness => {
                "Black is mirror smooth, white is fully rough. A uniform grey \
                                    over a whole model usually means a missing texture."
            }
            ViewMode::Metallic => {
                "This should be nearly binary. Intermediate values are usually an \
                                   authoring mistake rather than a material."
            }
            ViewMode::AmbientOcclusion => {
                "Dark is occluded. Occlusion that follows a texture seam \
                                           rather than the geometry is a baking artefact."
            }
            ViewMode::WorldPosition => {
                "Bands rather than a smooth gradient mean depth precision is \
                                        being lost — check the near plane."
            }
            ViewMode::Depth => {
                "Near is bright. Banding close to the camera means the near plane is \
                                too small for the world's scale."
            }
            ViewMode::Overdraw => {
                "Brighter is more expensive. Stacked transparent surfaces are the \
                                   usual cause, and the fix is fewer layers rather than cheaper \
                                   ones."
            }
            ViewMode::Wireframe => {
                "Density is triangle count. A distant object that is still dense \
                                    is one whose level of detail is not being reduced."
            }
            ViewMode::ShadingComplexity => {
                "Brighter is a more expensive material. Compare against \
                                            the screen area it covers, not against other pixels."
            }
            ViewMode::LightComplexity => {
                "Brighter is more lights. A bright region far from any \
                                          visible light means a light's range is larger than its \
                                          visible effect."
            }
            ViewMode::ClusterOccupancy => {
                "A saturated cluster is dropping lights. Reduce light \
                                           ranges rather than light counts."
            }
            ViewMode::LodLevel => {
                "Each level has a colour. A distant object at level zero is a \
                                   missing LOD chain; a near object at the last level is a screen \
                                   coverage threshold set too high."
            }
            ViewMode::MipLevel => {
                "Blue is a fine mip, red is coarse. Red on a near surface means \
                                   the texture is under-sampled or the UVs are stretched."
            }
            ViewMode::MotionVectors => {
                "Colour is direction, brightness is speed. Motion on a still \
                                        object means its previous transform is not being kept."
            }
            ViewMode::GiContribution => {
                "A surface that is black here and lit in the reference is \
                                         lit only by direct light. This is the entry point to the \
                                         illumination capability's explanation of why."
            }
            ViewMode::ShadowCascades => {
                "Each cascade has a colour. A cascade boundary crossing an \
                                         object the camera is looking at is a distribution to \
                                         retune."
            }
            ViewMode::BoundingVolumes => {
                "A volume much larger than its object is why that object \
                                          is picked from far away and culled too late."
            }
        }
    }
}

/// A debug view the specification names that the engine cannot draw yet, with the capability that
/// owns it.
///
/// Present as data rather than as prose so that a test can assert it is not empty and a reader can
/// see what is missing without reading a header comment. Each of these becomes a [`ViewMode`] when
/// its capability arrives — and the entry is deleted from here in the same change, which is the
/// property that keeps this list from becoming a lie.
pub const PLANNED_VIEWS: [(&str, &str); 8] = [
    ("Lightmap density", "rendering-global-illumination"),
    ("GI probe placement", "rendering-global-illumination"),
    ("Virtual texture feedback", "virtual-texturing"),
    ("Virtual texture residency", "residency"),
    ("Virtual shadow pages", "virtual-shadows"),
    ("Physics colliders", "physics"),
    ("Navigation data", "navigation"),
    ("Streaming region state", "world-partition-and-streaming"),
];

/// Everything a command registry needs to make one view mode invokable.
///
/// `editor-agent-interface` requires a description "written for a caller that cannot see the
/// interface", and the two sentences below are exactly that: what it shows and how to read it,
/// which is also what the viewport requirement asks for. Registering these is three lines in
/// `cy_editor_services::builtin` — this crate is at layer 2 and cannot name the command registry,
/// which is the same layer.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct ViewModeCommand {
    /// The command's identifier: `viewport.view-mode.overdraw`.
    pub id: String,
    /// What a person reads in the palette.
    pub label: &'static str,
    /// The category every one of these shares.
    pub category: &'static str,
    /// What it shows and how to read it, in one paragraph.
    pub description: String,
    /// The mode the command selects.
    pub mode: ViewMode,
}

/// One command descriptor per debug view, for the palette.
#[must_use]
pub fn view_mode_commands() -> Vec<ViewModeCommand> {
    ALL_VIEW_MODES
        .iter()
        .map(|mode| ViewModeCommand {
            id: mode.command_id(),
            label: mode.label(),
            category: "Viewport",
            description: format!("Shows {}. {}", mode.shows(), mode.how_to_read()),
            mode: *mode,
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;
    use std::path::PathBuf;

    use super::*;

    /// The engine's name table, read out of the source rather than restated here.
    fn engine_names() -> Vec<String> {
        let repository = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .ancestors()
            .nth(3)
            .expect("the crate is at editor/crates/<name>/")
            .to_path_buf();
        let source = repository.join("src/servers/render/src/types.cpp");
        let text = std::fs::read_to_string(&source)
            .unwrap_or_else(|error| panic!("reading {}: {error}", source.display()));
        let table = text
            .split("kDebugViewModeNames[] = {")
            .nth(1)
            .expect("the engine still has a debug view name table")
            .split("};")
            .next()
            .expect("the table is brace-delimited");
        table
            .lines()
            .filter_map(|line| line.trim().strip_prefix('"'))
            .filter_map(|line| line.split('"').next())
            .map(str::to_string)
            .collect()
    }

    #[test]
    fn the_mode_list_matches_the_engines() {
        // Two lists in two languages with no generator between them. This is what keeps them
        // honest: a mode appended to `DebugViewMode` and not to `ViewMode` fails here rather than
        // becoming a view the editor can never select.
        let engine = engine_names();
        let editor: Vec<String> = ALL_VIEW_MODES
            .iter()
            .map(|mode| mode.engine_name().to_string())
            .collect();
        assert_eq!(
            editor, engine,
            "the editor's view modes and cy::render::DebugViewMode have drifted"
        );
    }

    #[test]
    fn the_discriminant_is_the_engines_and_survives_a_capture() {
        for (index, mode) in ALL_VIEW_MODES.iter().enumerate() {
            let index = u32::try_from(index).expect("nineteen modes");
            assert_eq!(*mode as u32, index);
            assert_eq!(ViewMode::from_index(index), Some(*mode));
        }
        assert_eq!(
            ViewMode::from_index(999),
            None,
            "a mode from a newer engine"
        );
    }

    #[test]
    fn every_view_says_what_it_shows_and_how_to_read_it() {
        // The requirement is two clauses and the second one is the one that gets skipped, so both
        // are checked. The length floor is what stops "n/a" from satisfying it.
        for mode in ALL_VIEW_MODES {
            assert!(
                mode.shows().len() > 20,
                "{mode:?} does not say what it shows"
            );
            assert!(
                mode.how_to_read().len() > 40,
                "{mode:?} does not say how to read it"
            );
            assert!(!mode.label().is_empty());
        }
    }

    #[test]
    fn every_view_is_reachable_by_a_unique_command_identifier() {
        let commands = view_mode_commands();
        assert_eq!(commands.len(), ALL_VIEW_MODES.len());
        let identifiers: BTreeSet<&str> =
            commands.iter().map(|command| command.id.as_str()).collect();
        assert_eq!(identifiers.len(), commands.len(), "identifiers collide");
        assert!(identifiers.contains("viewport.view-mode.overdraw"));
        assert!(identifiers.contains("viewport.view-mode.ambient-occlusion"));
        for command in &commands {
            assert!(
                command.description.len() > 60,
                "{} would tell a machine caller nothing",
                command.id
            );
        }
    }

    #[test]
    fn the_views_the_engine_cannot_draw_yet_are_named_rather_than_stubbed() {
        assert!(!PLANNED_VIEWS.is_empty());
        let offered: BTreeSet<&str> = ALL_VIEW_MODES.iter().map(|mode| mode.label()).collect();
        for (planned, capability) in PLANNED_VIEWS {
            assert!(
                !offered.contains(planned),
                "{planned} is offered and is also listed as unavailable"
            );
            assert!(!capability.is_empty());
        }
    }
}
