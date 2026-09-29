// SPDX-License-Identifier: MIT
//! The physics debug view: which of the engine's physics layers a viewport asks to see. Issue #29.
//!
//! `editor-viewport-and-gizmos` — "View modes and debug visualisation" names **physics colliders**
//! among the engine debug views the editor exposes, and `physics` — "Physics debugging" names what
//! the engine can draw: "collider shapes, contact points and normals, constraint anchors and
//! limits, body sleep state, velocities, centres of mass, broad-phase bounds". The engine already
//! draws all of them into a sink (`cy::physics::PhysicsServer::debug_draw`, flags
//! `cy::physics::DebugDrawFlags`); the editor's part is to ask for them.
//!
//! --- A MIRROR, LIKE THE VIEW MODES, AND NOT A SIXTH RENDERER -----------------------------------------
//!
//! [`PhysicsLayer`] has exactly the engine's flags, with exactly its bits, because the number that
//! crosses the bridge is the engine's: [`PhysicsOverlays::bits`] goes out in the gizmo intent and
//! the runtime hands it to `debug_draw`. `tests::the_layers_are_the_engines` reads `debug.h` and
//! fails when the two lists drift, as `viewmode`'s test reads the render server's table.
//!
//! These are layers rather than a [`crate::viewmode::ViewMode`] because they compose: a collider
//! outline over the lit frame, over the wireframe, or with contacts on top. A view mode replaces
//! the shading; a physics layer is drawn over whatever the shading is.
//!
//! --- WHEN THERE IS SOMETHING TO DRAW -------------------------------------------------------------------
//!
//! The layers draw the SIMULATED world, so they appear while a world plays or is paused. While
//! authoring there is no physics world; the selected entity's authored joint is drawn by the engine
//! as a gizmo instead, through the same function that draws a simulated constraint.

/// One physics debug layer the engine draws over the frame.
///
/// The discriminants are `cy::physics::DebugDrawFlags`' bits. Do not renumber them.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum PhysicsLayer {
    /// Collider shapes at their simulated placement.
    Colliders = 1 << 0,
    /// Contact points and their normals.
    Contacts = 1 << 1,
    /// Joint anchors and limits.
    Constraints = 1 << 2,
    /// Which bodies are asleep.
    SleepState = 1 << 3,
    /// Linear and angular velocity.
    Velocities = 1 << 4,
    /// Each body's centre of mass.
    CentresOfMass = 1 << 5,
    /// The broad phase's bounding boxes.
    BroadPhaseBounds = 1 << 6,
}

impl PhysicsLayer {
    /// Every layer, in the engine's order.
    pub const ALL: [PhysicsLayer; 7] = [
        PhysicsLayer::Colliders,
        PhysicsLayer::Contacts,
        PhysicsLayer::Constraints,
        PhysicsLayer::SleepState,
        PhysicsLayer::Velocities,
        PhysicsLayer::CentresOfMass,
        PhysicsLayer::BroadPhaseBounds,
    ];

    /// The bit this layer is in the engine's flag set.
    #[must_use]
    pub const fn bit(self) -> u32 {
        self as u32
    }

    /// The engine's own spelling of the flag, as `debug.h` declares it.
    #[must_use]
    pub const fn engine_name(self) -> &'static str {
        match self {
            PhysicsLayer::Colliders => "Colliders",
            PhysicsLayer::Contacts => "Contacts",
            PhysicsLayer::Constraints => "Constraints",
            PhysicsLayer::SleepState => "SleepState",
            PhysicsLayer::Velocities => "Velocities",
            PhysicsLayer::CentresOfMass => "CentersOfMass",
            PhysicsLayer::BroadPhaseBounds => "BroadPhaseBounds",
        }
    }

    /// The word a command and a control take: `sleep-state`.
    #[must_use]
    pub const fn id(self) -> &'static str {
        match self {
            PhysicsLayer::Colliders => "colliders",
            PhysicsLayer::Contacts => "contacts",
            PhysicsLayer::Constraints => "constraints",
            PhysicsLayer::SleepState => "sleep-state",
            PhysicsLayer::Velocities => "velocities",
            PhysicsLayer::CentresOfMass => "centres-of-mass",
            PhysicsLayer::BroadPhaseBounds => "broad-phase-bounds",
        }
    }

    /// The layer a word names.
    #[must_use]
    pub fn of_id(id: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|layer| layer.id() == id)
    }

    /// The command that toggles it: `viewport.physics.colliders`.
    #[must_use]
    pub fn command_id(self) -> String {
        format!("viewport.physics.{}", self.id())
    }

    /// The viewport control that holds it: `physics-colliders`.
    #[must_use]
    pub const fn control(self) -> &'static str {
        match self {
            PhysicsLayer::Colliders => "physics-colliders",
            PhysicsLayer::Contacts => "physics-contacts",
            PhysicsLayer::Constraints => "physics-constraints",
            PhysicsLayer::SleepState => "physics-sleep-state",
            PhysicsLayer::Velocities => "physics-velocities",
            PhysicsLayer::CentresOfMass => "physics-centres-of-mass",
            PhysicsLayer::BroadPhaseBounds => "physics-broad-phase-bounds",
        }
    }

    /// The layer a control name holds.
    #[must_use]
    pub fn of_control(control: &str) -> Option<Self> {
        Self::ALL
            .into_iter()
            .find(|layer| layer.control() == control)
    }

    /// What a person reads in a menu.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            PhysicsLayer::Colliders => "Colliders",
            PhysicsLayer::Contacts => "Contacts",
            PhysicsLayer::Constraints => "Joints",
            PhysicsLayer::SleepState => "Sleep State",
            PhysicsLayer::Velocities => "Velocities",
            PhysicsLayer::CentresOfMass => "Centres of Mass",
            PhysicsLayer::BroadPhaseBounds => "Broad-Phase Bounds",
        }
    }

    /// What the layer shows, in one clause.
    #[must_use]
    pub const fn shows(self) -> &'static str {
        match self {
            PhysicsLayer::Colliders => {
                "every collider outlined at its simulated placement, coloured by motion: grey \
                 static, blue kinematic, green awake, dim asleep, orange trigger"
            }
            PhysicsLayer::Contacts => {
                "each contact point as a red cross, with its normal and its penetration depth"
            }
            PhysicsLayer::Constraints => {
                "each joint's two anchors, the line between them, and its limits in gold"
            }
            PhysicsLayer::SleepState => {
                "a marker at every body's origin, dim when the solver has put it to sleep"
            }
            PhysicsLayer::Velocities => "each body's linear velocity, and its angular velocity",
            PhysicsLayer::CentresOfMass => "a small marker at each body's centre of mass",
            PhysicsLayer::BroadPhaseBounds => "the box the broad phase tests each body with",
        }
    }

    /// How to read it: what a surprising picture means.
    #[must_use]
    pub const fn how_to_read(self) -> &'static str {
        match self {
            PhysicsLayer::Colliders => {
                "An outline that does not sit on its mesh is a collider authored in the wrong \
                 place; the physics uses the outline, not the mesh."
            }
            PhysicsLayer::Contacts => {
                "A long red stub is a deep overlap. Contacts flickering on a resting body mean it \
                 is not settling, usually a mass ratio or a collider that is too thin."
            }
            PhysicsLayer::Constraints => {
                "Two anchors drifting apart are a joint the solver cannot hold; lower the mass \
                 ratio between its bodies or raise the iterations."
            }
            PhysicsLayer::SleepState => {
                "A body that never dims never sleeps, which costs a solve every step; check that \
                 it is not being pushed or woken every frame."
            }
            PhysicsLayer::Velocities => {
                "A long line on a body that looks still is a body jittering faster than a frame \
                 shows."
            }
            PhysicsLayer::CentresOfMass => {
                "A centre of mass far from where the body looks balanced makes it tip; override it \
                 on the rigid body."
            }
            PhysicsLayer::BroadPhaseBounds => {
                "A box much larger than its body is a fast or rotating body the broad phase has \
                 to test against everything near it."
            }
        }
    }
}

/// The set of physics layers a viewport asks the engine to draw.
#[derive(Clone, Copy, PartialEq, Eq, Default, Debug)]
pub struct PhysicsOverlays {
    bits: u32,
}

impl PhysicsOverlays {
    /// No layers: the frame as the game draws it.
    pub const NONE: Self = Self { bits: 0 };

    /// Whether a layer is asked for.
    #[must_use]
    pub const fn contains(self, layer: PhysicsLayer) -> bool {
        self.bits & layer.bit() != 0
    }

    /// Ask for a layer, or stop asking.
    pub const fn set(&mut self, layer: PhysicsLayer, on: bool) {
        if on {
            self.bits |= layer.bit();
        } else {
            self.bits &= !layer.bit();
        }
    }

    /// The engine's flag bits, which is what crosses the bridge.
    #[must_use]
    pub const fn bits(self) -> u32 {
        self.bits
    }

    /// Whether nothing is asked for.
    #[must_use]
    pub const fn is_empty(self) -> bool {
        self.bits == 0
    }

    /// The layers asked for, in the engine's order.
    pub fn layers(self) -> impl Iterator<Item = PhysicsLayer> {
        PhysicsLayer::ALL
            .into_iter()
            .filter(move |layer| self.contains(*layer))
    }
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;
    use std::path::PathBuf;

    use super::*;

    /// `DebugDrawFlags`' enumerators and their bits, read out of the engine's header.
    fn engine_flags() -> Vec<(String, u32)> {
        let repository = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .ancestors()
            .nth(3)
            .expect("the crate is at editor/crates/<name>/")
            .to_path_buf();
        let header = repository.join("src/servers/physics/include/cy/servers/physics/debug.h");
        let text = std::fs::read_to_string(&header)
            .unwrap_or_else(|error| panic!("reading {}: {error}", header.display()));
        let body = text
            .split("enum class DebugDrawFlags : u32 {")
            .nth(1)
            .expect("the engine still declares DebugDrawFlags")
            .split("};")
            .next()
            .expect("the enumeration is brace-delimited");
        body.lines()
            .filter_map(|line| {
                let (name, value) = line.trim().trim_end_matches(',').split_once(" = ")?;
                let shift = value.strip_prefix("1U << ")?.trim_end_matches('U');
                Some((name.to_string(), 1 << shift.parse::<u32>().ok()?))
            })
            .collect()
    }

    #[test]
    fn the_layers_are_the_engines() {
        let engine = engine_flags();
        let editor: Vec<(String, u32)> = PhysicsLayer::ALL
            .iter()
            .map(|layer| (layer.engine_name().to_string(), layer.bit()))
            .collect();
        assert_eq!(
            editor, engine,
            "the editor's physics layers and cy::physics::DebugDrawFlags have drifted"
        );
    }

    #[test]
    fn physics_colliders_are_requestable_rather_than_planned() {
        assert!(
            !crate::viewmode::PLANNED_VIEWS
                .iter()
                .any(|(_, capability)| *capability == "physics"),
            "a physics view is listed as one the engine cannot draw"
        );
        assert!(PhysicsLayer::ALL.contains(&PhysicsLayer::Colliders));
    }

    #[test]
    fn a_set_holds_exactly_the_layers_asked_for() {
        let mut overlays = PhysicsOverlays::NONE;
        assert!(overlays.is_empty());
        overlays.set(PhysicsLayer::Colliders, true);
        overlays.set(PhysicsLayer::Constraints, true);
        assert_eq!(overlays.bits(), 0b101);
        assert!(overlays.contains(PhysicsLayer::Constraints));
        assert!(!overlays.contains(PhysicsLayer::Contacts));
        overlays.set(PhysicsLayer::Colliders, false);
        assert_eq!(
            overlays.layers().collect::<Vec<_>>(),
            vec![PhysicsLayer::Constraints]
        );
    }

    #[test]
    fn every_layer_says_what_it_shows_and_is_reachable_by_a_unique_command() {
        let commands: BTreeSet<String> = PhysicsLayer::ALL
            .iter()
            .map(|layer| layer.command_id())
            .collect();
        assert_eq!(commands.len(), PhysicsLayer::ALL.len());
        for layer in PhysicsLayer::ALL {
            assert!(layer.shows().len() > 20, "{layer:?}");
            assert!(layer.how_to_read().len() > 40, "{layer:?}");
            assert_eq!(PhysicsLayer::of_id(layer.id()), Some(layer));
            assert_eq!(PhysicsLayer::of_control(layer.control()), Some(layer));
            assert_eq!(layer.control(), format!("physics-{}", layer.id()));
        }
    }
}
