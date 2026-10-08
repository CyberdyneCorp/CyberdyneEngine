// SPDX-License-Identifier: MIT
//! An animation graph on the one shared canvas, and a clip's events on the one shared timeline.
//! Issue #29, animation.
//!
//! The `.cyanimgraph` file is the document; the canvas is how its pose graph is drawn and edited,
//! through the same [`super::script::open`] and [`super::script::capture`] a gameplay graph uses,
//! because both are CyberGraph text. The vocabulary is the engine's (`animation.catalogue.get`):
//! clips, blends, states, and the transitions wired from one state's `state` output to another's.
//!
//! The timeline shows ONE clip: the `pose.clip` node being previewed, as an animation track with
//! the clip as its section, and one event track per event name with a key at each of that name's
//! times. A key's identity is not stored anywhere — the file holds `name@seconds` — so
//! [`clip_timeline`] answers, beside the surface, which event each key stands for.

use cy_editor_core::problem::{Problem, Result};
use cy_editor_services::animation_graph::{CLIP_NODE, ClipEvent, EVENTS, parse_events};
use cy_editor_services::script_graph::ScriptGraph;

use super::graph::Catalogue;
use super::material::catalogue_from_service;
use super::timeline::{KeyId, TimelineSurface, TrackId, TrackKind};

/// The prefix every pose graph node type carries.
pub const NODE_PREFIX: &str = "pose.";

/// The engine's pose vocabulary from an `animation.catalogue.get` reply.
///
/// # Errors
///
/// An unreadable catalogue, an empty one, or one with a node outside the pose vocabulary.
pub fn catalogue(payload: &[u8]) -> Result<Catalogue> {
    let nodes = catalogue_from_service(payload)?;
    if nodes.is_empty() || nodes.iter().any(|node| !node.name.starts_with(NODE_PREFIX)) {
        return Err(Problem::new(
            "load the animation graph catalogue",
            "the engine supplied no pose nodes, or a node outside the pose vocabulary",
        ));
    }
    Catalogue::new(nodes)
}

/// The clip a `pose.clip` node samples, and its events, read from the graph.
///
/// # Errors
///
/// When `node` is not a clip node of `graph`, or its events do not read.
pub fn clip_of(graph: &ScriptGraph, node: u64) -> Result<(String, Vec<ClipEvent>)> {
    let found = graph
        .nodes
        .get(&node)
        .filter(|found| found.type_name == CLIP_NODE)
        .ok_or_else(|| {
            Problem::new(
                format!("show node {node} on the timeline"),
                "it is not a pose.clip node of this graph",
            )
        })?;
    let text = |name: &str| {
        found
            .property(name)
            .map(|property| property.literal.display(&property.literal_type))
            .unwrap_or_default()
    };
    Ok((text("clip"), parse_events(&text(EVENTS))?))
}

/// The first `pose.clip` node of `graph`, by key: what the timeline shows when nothing is chosen.
#[must_use]
pub fn first_clip(graph: &ScriptGraph) -> Option<u64> {
    graph
        .nodes
        .values()
        .find(|node| node.type_name == CLIP_NODE)
        .map(|node| node.key)
}

/// What the timeline drew: the clip's track, and for each key the event it stands for.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct ClipTimeline {
    /// The animation track holding the clip as its one section.
    pub clip_track: Option<TrackId>,
    /// Each event track and the event name it carries.
    pub event_tracks: Vec<(TrackId, String)>,
    /// Each key and the event it stands for.
    pub keys: Vec<(KeyId, ClipEvent)>,
}

impl ClipTimeline {
    /// The event a key stands for.
    #[must_use]
    pub fn event(&self, key: KeyId) -> Option<&ClipEvent> {
        self.keys
            .iter()
            .find(|(candidate, _)| *candidate == key)
            .map(|(_, event)| event)
    }

    /// The event name a track carries.
    #[must_use]
    pub fn track_event(&self, track: TrackId) -> Option<&str> {
        self.event_tracks
            .iter()
            .find(|(candidate, _)| *candidate == track)
            .map(|(_, name)| name.as_str())
    }
}

/// Lay one clip and its events onto `surface`, which is cleared first: an animation track with the
/// clip over `[0, duration]`, then an event track per event name, in name order, with a key at each
/// time. Keeps the playhead where it was, inside the clip.
///
/// # Errors
///
/// When the surface refuses a section or a key.
pub fn clip_timeline(
    surface: &mut TimelineSurface,
    clip: &str,
    duration: f32,
    events: &[ClipEvent],
) -> Result<ClipTimeline> {
    let playhead = surface.playhead();
    surface.load(f64::from(duration.max(0.0)));
    let mut drawn = ClipTimeline::default();
    let track = surface.add_track(TrackKind::Animation, clip);
    // A clip the character does not have has no length to show: its track is empty.
    if duration > 0.0 {
        surface.add_section(track, 0.0, f64::from(duration), clip)?;
    }
    drawn.clip_track = Some(track);
    let mut names: Vec<&str> = events.iter().map(|event| event.name.as_str()).collect();
    names.sort_unstable();
    names.dedup();
    for name in names {
        let track = surface.add_track(TrackKind::GameplayEvent, name);
        drawn.event_tracks.push((track, name.to_owned()));
        for event in events.iter().filter(|event| event.name == name) {
            let key = surface.key(track, f64::from(event.time), 0.0)?;
            drawn.keys.push((key, event.clone()));
        }
    }
    surface.scrub(playhead);
    Ok(drawn)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn engine_fixture(name: &str) -> Vec<u8> {
        let path = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../../src/editor_backend/tests/data")
            .join(name);
        std::fs::read(&path).unwrap_or_else(|error| panic!("{}: {error}", path.display()))
    }

    #[test]
    fn the_engines_catalogue_opens_the_acceptance_graph_and_it_round_trips() {
        let catalogue = catalogue(&engine_fixture("animation_catalogue_v1.wire")).unwrap();
        let state = catalogue.get("pose.state").unwrap();
        assert!(
            state
                .pins
                .iter()
                .any(|pin| pin.name == "state" && pin.data_type == "state")
        );
        let source =
            String::from_utf8(engine_fixture("animation_locomotion_v1.cyanimgraph")).unwrap();
        let graph = ScriptGraph::decode(&source).unwrap();
        let canvas = super::super::script::canvas_for(catalogue, &graph).unwrap();
        assert_eq!(canvas.nodes().count(), 6);
        assert_eq!(canvas.links().count(), 6);
        assert_eq!(
            super::super::script::capture(&graph, &canvas)
                .unwrap()
                .encode(),
            source
        );
    }

    #[test]
    fn a_clips_events_are_one_track_per_name_and_a_key_per_time() {
        let source =
            String::from_utf8(engine_fixture("animation_locomotion_v1.cyanimgraph")).unwrap();
        let graph = ScriptGraph::decode(&source).unwrap();
        assert_eq!(first_clip(&graph), Some(1));
        let (clip, events) = clip_of(&graph, 3).unwrap();
        assert_eq!(clip, "walk");
        assert!(clip_of(&graph, 2).is_err(), "a state is not a clip");
        let mut surface = TimelineSurface::new(1, 30.0).unwrap();
        surface.load(2.0);
        surface.scrub(0.5);
        let drawn = clip_timeline(&mut surface, &clip, 1.0, &events).unwrap();
        assert_eq!(surface.tracks().len(), 2);
        assert_eq!(drawn.event_tracks.len(), 1);
        assert_eq!(drawn.keys.len(), 2);
        let (key, event) = &drawn.keys[1];
        assert_eq!(drawn.event(*key), Some(event));
        assert_eq!(event.time, 0.75);
        assert!(
            (surface.playhead() - 0.5).abs() < 1e-9,
            "the playhead stays put"
        );
        assert_eq!(surface.duration(), 1.0);
    }

    #[test]
    fn a_catalogue_outside_the_pose_vocabulary_is_refused() {
        assert!(catalogue(&engine_fixture("script_catalogue_v1.wire")).is_err());
    }
}
