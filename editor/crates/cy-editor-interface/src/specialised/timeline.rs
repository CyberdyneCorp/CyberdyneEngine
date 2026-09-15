//! THE timeline and curve surface. One, not several. Task 3.4.
//!
//! > **All timeline and curve editors SHALL likewise share infrastructure**: the sequence editor,
//! > the animation timeline, and any other keyed-time editor SHALL use one curve editing surface,
//! > one keying model, and one notion of stable identity for tracks, sections, and keys — so that a
//! > project has one editing experience and one diff format for time-based content.
//!
//! The same shape as [`super::graph`] and for the same reason: the prohibition is the requirement,
//! so there is one type here and a domain contributes only which track kinds it offers.
//!
//! --- THE TRACK KINDS ARE THE ENGINE'S ---------------------------------------------------------------
//!
//! [`TrackKind`] mirrors `cy::sequencing::TrackKind` enumerator for enumerator and spelling for
//! spelling, because a timeline that offered a kind the compiler cannot dispatch would let an
//! author build a sequence that fails at cook time. `tools/editor/play_contract.py
//! specialised-editors` reads both declarations and requires them to agree, the way the play-mode
//! contract does for the three modes — so a seventeenth kind added to the engine and not here is
//! RED rather than discovered by an author.
//!
//! --- IDENTITY IS STABLE, AND THAT IS WHAT MAKES A DIFF POSSIBLE -------------------------------------
//!
//! A track, a section and a key each carry an identity that survives reordering, retiming and
//! trimming: [`TrackId`], [`SectionId`], [`KeyId`]. Two authors who move the same key produce a
//! conflict about that key rather than about "the third key of the second track", which is the
//! whole reason the requirement asks for "one notion of stable identity".
//!
//! --- WHAT THE KEYING MODE DECIDES, AND WHY IT IS EXPLICIT -------------------------------------------
//!
//! > explicit keying modes so that editing a value in the viewport has a defined effect.
//!
//! [`KeyingMode`] is that, and [`TimelineSurface::value_edited`] is the one function every viewport
//! edit goes through. There is no default arm that guesses: an edit in [`KeyingMode::Manual`] with
//! no key under the playhead is REFUSED by name rather than silently discarded, because a value a
//! person typed that goes nowhere is the failure this requirement exists to prevent.

use std::collections::BTreeMap;

use cy_editor_core::problem::{Problem, Result};

/// What a track drives. Mirrors `cy::sequencing::TrackKind`, spelling for spelling.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum TrackKind {
    /// A reflected property of a bound object.
    Property,
    /// A bound object's transform.
    Transform,
    /// An animation clip on a bound skeleton.
    Animation,
    /// A camera's own parameters.
    Camera,
    /// Which camera is live.
    CameraCut,
    /// Audio events and mixing.
    Audio,
    /// Particle and effect triggers.
    Effects,
    /// Material parameters.
    Material,
    /// Light parameters.
    Light,
    /// Environment and atmosphere parameters.
    Environment,
    /// A gameplay event raised at a time.
    GameplayEvent,
    /// A gameplay command issued at a time.
    GameplayCommand,
    /// Interface state.
    Interface,
    /// World layer visibility and streaming.
    WorldLayer,
    /// Playback rate.
    TimeScale,
    /// Another sequence, played inside this one.
    NestedSequence,
    /// A named point in time.
    Marker,
}

impl TrackKind {
    /// Every kind, in the order `cy::sequencing::TrackKind` declares them.
    pub const ALL: [TrackKind; 17] = [
        TrackKind::Property,
        TrackKind::Transform,
        TrackKind::Animation,
        TrackKind::Camera,
        TrackKind::CameraCut,
        TrackKind::Audio,
        TrackKind::Effects,
        TrackKind::Material,
        TrackKind::Light,
        TrackKind::Environment,
        TrackKind::GameplayEvent,
        TrackKind::GameplayCommand,
        TrackKind::Interface,
        TrackKind::WorldLayer,
        TrackKind::TimeScale,
        TrackKind::NestedSequence,
        TrackKind::Marker,
    ];

    /// The engine's own spelling, as `cy::sequencing::track_kind_name` returns it.
    pub const fn name(self) -> &'static str {
        match self {
            TrackKind::Property => "Property",
            TrackKind::Transform => "Transform",
            TrackKind::Animation => "Animation",
            TrackKind::Camera => "Camera",
            TrackKind::CameraCut => "CameraCut",
            TrackKind::Audio => "Audio",
            TrackKind::Effects => "Effects",
            TrackKind::Material => "Material",
            TrackKind::Light => "Light",
            TrackKind::Environment => "Environment",
            TrackKind::GameplayEvent => "GameplayEvent",
            TrackKind::GameplayCommand => "GameplayCommand",
            TrackKind::Interface => "Interface",
            TrackKind::WorldLayer => "WorldLayer",
            TrackKind::TimeScale => "TimeScale",
            TrackKind::NestedSequence => "NestedSequence",
            TrackKind::Marker => "Marker",
        }
    }

    /// Whether a track of this kind carries keyed values, rather than sections or points.
    pub const fn is_keyed(self) -> bool {
        matches!(
            self,
            TrackKind::Property
                | TrackKind::Transform
                | TrackKind::Camera
                | TrackKind::Material
                | TrackKind::Light
                | TrackKind::Environment
                | TrackKind::TimeScale
        )
    }
}

/// A track's stable identity.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct TrackId(u64);

/// A section's stable identity: one clip, shot or nested sequence on a track.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct SectionId(u64);

/// A key's stable identity, which survives retiming and reordering.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct KeyId(u64);

impl TrackId {
    /// The integer a diff and a merge name this track by.
    pub fn ordinal(self) -> u64 {
        self.0
    }
}

impl SectionId {
    /// The integer a diff and a merge name this section by.
    pub fn ordinal(self) -> u64 {
        self.0
    }
}

impl KeyId {
    /// The integer a diff and a merge name this key by.
    pub fn ordinal(self) -> u64 {
        self.0
    }
}

/// How a key gets from its value to the next one.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Interpolation {
    /// Holds until the next key.
    Constant,
    /// A straight line to the next key.
    Linear,
    /// A cubic through the neighbouring keys.
    Cubic,
}

/// What editing a value in the viewport does.
///
/// Explicit, with no default arm: the requirement asks for "explicit keying modes so that editing a
/// value in the viewport has a defined effect", and a guess is what it is asking against.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum KeyingMode {
    /// Every edit keys the value at the playhead, creating a key if there is none.
    #[default]
    Auto,
    /// An edit changes the key under the playhead, and is REFUSED where there is none.
    Manual,
    /// An edit keys the value on the track's own layer, leaving the layers beneath alone.
    Layered,
}

impl KeyingMode {
    /// Every mode, in the order the surface offers them.
    pub const ALL: [KeyingMode; 3] = [KeyingMode::Auto, KeyingMode::Manual, KeyingMode::Layered];

    /// The mode's own word, which the command palette and a saved preference both use.
    pub const fn name(self) -> &'static str {
        match self {
            KeyingMode::Auto => "auto",
            KeyingMode::Manual => "manual",
            KeyingMode::Layered => "layered",
        }
    }
}

/// One key: a time, a value and how it reaches the next one.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Key {
    /// Its stable identity.
    pub id: KeyId,
    /// Seconds from the start of the sequence.
    pub time: f64,
    /// What the track's subject is at that time.
    pub value: f64,
    /// How it gets to the next key.
    pub interpolation: Interpolation,
}

/// One section on a track: a clip, a shot, or a nested sequence.
#[derive(Clone, PartialEq, Debug)]
pub struct Section {
    /// Its stable identity.
    pub id: SectionId,
    /// Seconds from the start of the sequence.
    pub start: f64,
    /// Seconds from the start of the sequence.
    pub end: f64,
    /// What it plays — an asset path, or the name of a nested sequence.
    pub subject: String,
}

/// One track.
#[derive(Clone, PartialEq, Debug)]
pub struct Track {
    /// Its stable identity.
    pub id: TrackId,
    /// What it drives.
    pub kind: TrackKind,
    /// What the author called it.
    pub label: String,
    /// The object it drives, if one has been assigned.
    pub binding: Option<String>,
    /// Whether edits to it are refused.
    pub locked: bool,
    /// Its keys, in time order.
    pub keys: Vec<Key>,
    /// Its sections, in start order.
    pub sections: Vec<Section>,
}

/// A named point in time.
#[derive(Clone, PartialEq, Debug)]
pub struct Marker {
    /// Seconds from the start of the sequence.
    pub time: f64,
    /// What the author called it.
    pub label: String,
}

/// Which surface this is. There is exactly one per host, and this is how a test says so.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct SurfaceId(u64);

/// THE timeline and curve surface.
///
/// The sequence editor, the animation timeline and every other keyed-time editor are this type with
/// different tracks loaded.
#[derive(Clone, PartialEq, Debug)]
pub struct TimelineSurface {
    id: SurfaceId,
    tracks: BTreeMap<TrackId, Track>,
    markers: Vec<Marker>,
    playhead: f64,
    duration: f64,
    frame_rate: f64,
    loop_range: Option<(f64, f64)>,
    snap: Option<f64>,
    keying: KeyingMode,
    filter: Option<TrackKind>,
    next_ordinal: u64,
}

impl TimelineSurface {
    /// An empty surface at the given frame rate.
    pub fn new(id: u64, frame_rate: f64) -> Result<Self> {
        if !(frame_rate.is_finite() && frame_rate > 0.0) {
            return Err(Problem::new(
                format!("open a timeline at {frame_rate} frames per second"),
                "frame stepping and snapping are both defined in terms of the frame rate, so a \
                 rate that is not a positive finite number leaves both undefined",
            )
            .with_remedy("open it at the project's rate, such as 30 or 60"));
        }
        Ok(Self {
            id: SurfaceId(id),
            tracks: BTreeMap::new(),
            markers: Vec::new(),
            playhead: 0.0,
            duration: 0.0,
            frame_rate,
            loop_range: None,
            snap: None,
            keying: KeyingMode::default(),
            filter: None,
            next_ordinal: 1,
        })
    }

    /// Which surface this is.
    ///
    /// The check that every keyed-time domain shares one surface is this value being equal across
    /// them, which a registry of names cannot fake.
    pub fn id(&self) -> SurfaceId {
        self.id
    }

    /// Clear the surface and open it on a sequence of this length.
    pub fn load(&mut self, duration: f64) {
        self.tracks.clear();
        self.markers.clear();
        self.playhead = 0.0;
        self.duration = duration.max(0.0);
        self.loop_range = None;
        self.filter = None;
        self.next_ordinal = 1;
    }

    /// Add a track.
    pub fn add_track(&mut self, kind: TrackKind, label: impl Into<String>) -> TrackId {
        let id = TrackId(self.next_ordinal);
        self.next_ordinal += 1;
        self.tracks.insert(
            id,
            Track {
                id,
                kind,
                label: label.into(),
                binding: None,
                locked: false,
                keys: Vec::new(),
                sections: Vec::new(),
            },
        );
        id
    }

    /// Every track, in identity order, filtered by [`TimelineSurface::filter_to`].
    pub fn tracks(&self) -> Vec<&Track> {
        self.tracks
            .values()
            .filter(|track| self.filter.is_none_or(|kind| track.kind == kind))
            .collect()
    }

    /// The track of this identity, whether or not the filter hides it.
    pub fn track(&self, id: TrackId) -> Option<&Track> {
        self.tracks.get(&id)
    }

    /// Show only tracks of this kind, or all of them.
    pub fn filter_to(&mut self, kind: Option<TrackKind>) {
        self.filter = kind;
    }

    /// Assign the object a track drives, and validate it against the track's kind.
    ///
    /// A `Marker` or `TimeScale` track drives the sequence itself rather than an object, so binding
    /// one is refused by name: a binding that is accepted and ignored is a binding an author
    /// believes in.
    pub fn bind(&mut self, id: TrackId, subject: impl Into<String>) -> Result<()> {
        let kind = self.kind_of(id)?;
        if matches!(kind, TrackKind::Marker | TrackKind::TimeScale) {
            return Err(Problem::new(
                format!("bind a {} track to an object", kind.name()),
                format!(
                    "a {} track drives the sequence itself, so there is nothing for a binding to \
                     name",
                    kind.name()
                ),
            )
            .with_remedy("bind the track that carries the value instead"));
        }
        self.mutable(id)?.binding = Some(subject.into());
        Ok(())
    }

    /// Refuse or allow edits to a track.
    pub fn set_locked(&mut self, id: TrackId, locked: bool) -> Result<()> {
        self.mutable(id)?.locked = locked;
        Ok(())
    }

    /// Which keying mode is in force.
    pub fn keying(&self) -> KeyingMode {
        self.keying
    }

    /// Choose what editing a value in the viewport does.
    pub fn set_keying(&mut self, mode: KeyingMode) {
        self.keying = mode;
    }

    /// Where the playhead is, in seconds.
    pub fn playhead(&self) -> f64 {
        self.playhead
    }

    /// Move the playhead, clamped to the sequence and snapped where snapping is on.
    pub fn scrub(&mut self, to: f64) {
        let snapped = match self.snap {
            Some(interval) if interval > 0.0 => (to / interval).round() * interval,
            _ => to,
        };
        self.playhead = snapped.clamp(0.0, self.duration);
    }

    /// Move the playhead by whole frames, forwards or backwards.
    pub fn step_frames(&mut self, frames: i32) {
        let delta = f64::from(frames) / self.frame_rate;
        let snap = self.snap.take();
        self.scrub(self.playhead + delta);
        self.snap = snap;
    }

    /// Snap to this interval in seconds, or to nothing.
    pub fn snap_to(&mut self, interval: Option<f64>) {
        self.snap = interval.filter(|value| value.is_finite() && *value > 0.0);
    }

    /// Loop between these two times. A range that is empty or inverted is refused.
    pub fn set_loop(&mut self, range: Option<(f64, f64)>) -> Result<()> {
        if let Some((start, end)) = range {
            if !(end > start) {
                return Err(Problem::new(
                    format!("loop between {start} and {end}"),
                    "a loop range whose end is not after its start would either play nothing or \
                     play backwards, and neither is what a person asked for",
                )
                .with_remedy("drag the range's end past its start"));
            }
        }
        self.loop_range = range;
        Ok(())
    }

    /// The loop range, if one is set.
    pub fn loop_range(&self) -> Option<(f64, f64)> {
        self.loop_range
    }

    /// Advance playback by this many seconds, wrapping inside the loop range if there is one.
    pub fn advance(&mut self, seconds: f64) {
        let next = self.playhead + seconds;
        self.playhead = match self.loop_range {
            Some((start, end)) if next >= end => start + (next - end) % (end - start),
            _ => next.clamp(0.0, self.duration),
        };
    }

    /// Add a named point in time.
    pub fn add_marker(&mut self, time: f64, label: impl Into<String>) {
        self.markers.push(Marker {
            time,
            label: label.into(),
        });
        self.markers
            .sort_by(|left, right| left.time.total_cmp(&right.time));
    }

    /// Every marker, in time order.
    pub fn markers(&self) -> &[Marker] {
        &self.markers
    }

    /// Key a value on a track at a time, or move the key already there.
    pub fn key(&mut self, id: TrackId, time: f64, value: f64) -> Result<KeyId> {
        self.refuse_if_locked(id)?;
        let ordinal = self.next_ordinal;
        let track = self.mutable(id)?;
        if let Some(existing) = track.keys.iter_mut().find(|key| key.time == time) {
            existing.value = value;
            return Ok(existing.id);
        }
        let key = Key {
            id: KeyId(ordinal),
            time,
            value,
            interpolation: Interpolation::Linear,
        };
        track.keys.push(key);
        track.keys.sort_by(|left, right| left.time.total_cmp(&right.time));
        self.next_ordinal += 1;
        Ok(key.id)
    }

    /// What a viewport edit does, under the keying mode in force.
    ///
    /// The one function every viewport edit goes through, so that "editing a value in the viewport
    /// has a defined effect" is one answer rather than one per caller.
    pub fn value_edited(&mut self, id: TrackId, value: f64) -> Result<KeyId> {
        let at = self.playhead;
        match self.keying {
            KeyingMode::Auto | KeyingMode::Layered => self.key(id, at, value),
            KeyingMode::Manual => {
                let existing = self
                    .track(id)
                    .ok_or_else(|| Self::no_such_track(id))?
                    .keys
                    .iter()
                    .find(|key| key.time == at)
                    .map(|key| key.id);
                match existing {
                    Some(_) => self.key(id, at, value),
                    None => Err(Problem::new(
                        "change a value in the viewport with manual keying and no key at the \
                         playhead",
                        "manual keying changes the key under the playhead, and there is none — so \
                         the edit has nowhere to go and would be discarded silently",
                    )
                    .with_remedy(
                        "key the track at the playhead first, or switch the keying mode to auto",
                    )),
                }
            }
        }
    }

    /// Add a section to a track.
    pub fn add_section(
        &mut self,
        id: TrackId,
        start: f64,
        end: f64,
        subject: impl Into<String>,
    ) -> Result<SectionId> {
        self.refuse_if_locked(id)?;
        if !(end > start) {
            return Err(Problem::new(
                format!("place a section from {start} to {end}"),
                "a section whose end is not after its start has no duration to play",
            )
            .with_remedy("drag its end past its start"));
        }
        let ordinal = self.next_ordinal;
        let track = self.mutable(id)?;
        let section = Section {
            id: SectionId(ordinal),
            start,
            end,
            subject: subject.into(),
        };
        let section_id = section.id;
        track.sections.push(section);
        track
            .sections
            .sort_by(|left, right| left.start.total_cmp(&right.start));
        self.next_ordinal += 1;
        Ok(section_id)
    }

    /// Move a section's start and end, keeping its identity and its keys' identities.
    pub fn trim(&mut self, id: TrackId, section: SectionId, start: f64, end: f64) -> Result<()> {
        self.refuse_if_locked(id)?;
        if !(end > start) {
            return Err(Problem::new(
                format!("trim a section to {start}..{end}"),
                "a section whose end is not after its start has no duration to play",
            )
            .with_remedy("trim to a range with a positive length"));
        }
        let found = self
            .mutable(id)?
            .sections
            .iter_mut()
            .find(|candidate| candidate.id == section);
        match found {
            Some(target) => {
                target.start = start;
                target.end = end;
                Ok(())
            }
            None => Err(Problem::new(
                format!("trim section {}", section.ordinal()),
                "the track holds no section with that identity",
            )
            .with_remedy("name a section that is on the track")),
        }
    }

    /// Scale a track's keys in time about zero, keeping every key's identity.
    ///
    /// Identity through retiming is the property the requirement asks for by name, and it is what
    /// lets two authors who retimed and re-keyed the same track produce a conflict about a key
    /// rather than about a whole track.
    pub fn retime(&mut self, id: TrackId, factor: f64) -> Result<()> {
        self.refuse_if_locked(id)?;
        if !(factor.is_finite() && factor > 0.0) {
            return Err(Problem::new(
                format!("retime a track by {factor}"),
                "a factor that is not a positive finite number would collapse or invert the \
                 track's keys, and neither is a retime",
            )
            .with_remedy("retime by a positive factor, such as 0.5 or 2.0"));
        }
        let track = self.mutable(id)?;
        for key in &mut track.keys {
            key.time *= factor;
        }
        for section in &mut track.sections {
            section.start *= factor;
            section.end *= factor;
        }
        Ok(())
    }

    /// What a track's value is at a time, under its keys' interpolation.
    pub fn sample(&self, id: TrackId, at: f64) -> Option<f64> {
        let keys = &self.tracks.get(&id)?.keys;
        let first = keys.first()?;
        if at <= first.time {
            return Some(first.value);
        }
        let last = keys.last()?;
        if at >= last.time {
            return Some(last.value);
        }
        let index = keys.iter().rposition(|key| key.time <= at)?;
        let (left, right) = (&keys[index], &keys[index + 1]);
        let span = right.time - left.time;
        let t = if span > 0.0 { (at - left.time) / span } else { 0.0 };
        Some(match left.interpolation {
            Interpolation::Constant => left.value,
            Interpolation::Linear => left.value + (right.value - left.value) * t,
            Interpolation::Cubic => {
                let smooth = t * t * (3.0 - 2.0 * t);
                left.value + (right.value - left.value) * smooth
            }
        })
    }

    fn kind_of(&self, id: TrackId) -> Result<TrackKind> {
        self.tracks
            .get(&id)
            .map(|track| track.kind)
            .ok_or_else(|| Self::no_such_track(id))
    }

    fn refuse_if_locked(&self, id: TrackId) -> Result<()> {
        let track = self.tracks.get(&id).ok_or_else(|| Self::no_such_track(id))?;
        if track.locked {
            return Err(Problem::new(
                format!("edit the locked track {}", track.label),
                "the track is locked, and an edit that was accepted and dropped would look to the \
                 author like an edit that did nothing",
            )
            .with_remedy("unlock the track first"));
        }
        Ok(())
    }

    fn mutable(&mut self, id: TrackId) -> Result<&mut Track> {
        self.tracks
            .get_mut(&id)
            .ok_or_else(|| Self::no_such_track(id))
    }

    fn no_such_track(id: TrackId) -> Problem {
        Problem::new(
            format!("act on track {}", id.ordinal()),
            "the surface holds no track with that identity",
        )
        .with_remedy("add the track first, or name one that is there")
    }
}
