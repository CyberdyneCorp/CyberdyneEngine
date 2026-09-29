// SPDX-License-Identifier: MIT
//! The audio assets the editor authors, and the engine's answers about them. Issue #29.
//!
//! Two project assets and one scene component:
//!
//! | What | Where | Read by |
//! |---|---|---|
//! | the bus graph | `audio/mixer.cymixer` (`cymixer 1`) | `cy::editor::parse_mixer` |
//! | a playable sound | `*.cycue` (`cycue 1`) | `cy::editor::parse_cue` |
//! | a sound in the world | `cy::audio::AudioSource` on an entity | `cy::editor::read_emitters` |
//!
//! The editor never mixes. It writes these, sends them over the backend service, and shows what
//! the engine's `AudioServer` answers — [`AudioState`] is decoded from the engine's reply, so a
//! bus gain in the panel is the gain the graph holds.
//!
//! THE TWO SIDES ARE HELD TO ONE ARTEFACT EACH. `src/editor_backend/tests/data/` carries the
//! mixer, cue and preview request this module encodes, which the engine's suite submits, and the
//! state and vocabulary replies the engine encodes, which this module's tests decode. A change to
//! either side fails one of the two suites.

use std::fmt::Write as _;
use std::path::{Component, Path};

use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::problem::{Problem, Result};

/// Where the project's mixer lives unless a command names another.
pub const DEFAULT_MIXER: &str = "audio/mixer.cymixer";
/// Transaction kind prefix for a saved audio asset; the rest is its project path.
pub const DOMAIN_PREFIX: &str = "audio_asset:";
/// The scene component a sound-emitting entity carries, matched by name on the engine side.
pub const SOURCE_COMPONENT: &str = "cy::audio::AudioSource";
/// The root of every mixer, which the engine's bus graph creates and never destroys.
pub const MASTER: &str = "Master";
/// Make a `cymixer 1` text the engine's bus graph.
pub const MIXER_APPLY: &str = "audio.mixer.apply";
/// Load a cue and play it once.
pub const CUE_PREVIEW: &str = "audio.cue.preview";
/// Stop every preview voice.
pub const PREVIEW_STOP: &str = "audio.preview.stop";
/// Read the engine's state, optionally advancing a null-backend mix first.
pub const STATE_GET: &str = "audio.state.get";

const MAX_BUSES: usize = 32;
const MAX_SENDS: usize = 4;
const MAX_EFFECTS: usize = 8;
const MAX_NAME: usize = 48;
const MAX_BUS_VOLUME: f32 = 4.0;
/// The effect kinds the engine's bus graph has, by the names `cy::audio::effect_kind_name` gives.
pub const EFFECT_KINDS: [&str; 4] = ["gain", "low-pass", "high-pass", "limiter"];
/// The attenuation models a source may name.
pub const ATTENUATION_MODELS: [&str; 4] = ["inverse", "inverse-square", "linear", "logarithmic"];

fn refuse(action: &str, because: impl Into<String>) -> Problem {
    Problem::new(action.to_owned(), because)
}

/// Refuse a path outside the project or without the asset's extension.
fn validate_reference(reference: &str, extension: &str, action: &str) -> Result<()> {
    let path = Path::new(reference);
    if path.extension().is_none_or(|found| found != extension)
        || !path
            .components()
            .all(|component| matches!(component, Component::Normal(_)))
    {
        return Err(refuse(
            action,
            format!("the path must be a project-relative .{extension} file"),
        ));
    }
    Ok(())
}

/// Refuse anything but a project-relative `.cymixer`.
pub fn validate_mixer_reference(reference: &str) -> Result<()> {
    validate_reference(reference, "cymixer", "access an audio mixer")
}

/// Refuse anything but a project-relative `.cycue`.
pub fn validate_cue_reference(reference: &str) -> Result<()> {
    validate_reference(reference, "cycue", "access an audio cue")
}

/// Whether a bus or cue-bus name is one the engine accepts.
#[must_use]
pub fn valid_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= MAX_NAME
        && name
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-' | b'.'))
}

fn finite_in(value: f32, low: f32, high: f32) -> bool {
    value.is_finite() && (low..=high).contains(&value)
}

/// One send of a bus.
#[derive(Clone, PartialEq, Debug)]
pub struct Send {
    /// The bus it sends into.
    pub target: String,
    /// Linear level, 0 to 1.
    pub level: f32,
}

/// One effect in a bus's chain, in the engine's own parameters.
#[derive(Clone, PartialEq, Debug)]
pub struct Effect {
    /// One of [`EFFECT_KINDS`].
    pub kind: String,
    /// Gain for `gain`, ceiling for `limiter`.
    pub a: f32,
    /// The one-pole coefficient for `low-pass` and `high-pass`.
    pub b: f32,
    /// Skipped by the mix while set.
    pub bypass: bool,
}

/// One bus.
#[derive(Clone, PartialEq, Debug)]
pub struct Bus {
    /// Unique within the mixer; what a cue and `Audio.bus` name it by.
    pub name: String,
    /// The bus it sums into, or `None` for Master.
    pub output: Option<String>,
    /// Linear gain, 0 to 4.
    pub volume: f32,
    /// Silenced.
    pub mute: bool,
    /// Everything that does not feed a soloed bus is silenced.
    pub solo: bool,
    /// The effect chain is skipped.
    pub bypass: bool,
    /// At most four.
    pub sends: Vec<Send>,
    /// At most eight, in order.
    pub effects: Vec<Effect>,
}

impl Bus {
    fn new(name: &str, output: Option<&str>) -> Self {
        Self {
            name: name.to_owned(),
            output: output.map(str::to_owned),
            volume: 1.0,
            mute: false,
            solo: false,
            bypass: false,
            sends: Vec::new(),
            effects: Vec::new(),
        }
    }
}

/// The bus graph a `.cymixer` asset holds.
#[derive(Clone, PartialEq, Debug)]
pub struct Mixer {
    /// Master first.
    pub buses: Vec<Bus>,
}

impl Default for Mixer {
    fn default() -> Self {
        Self {
            buses: vec![Bus::new(MASTER, None)],
        }
    }
}

impl Mixer {
    /// The bus of that name.
    #[must_use]
    pub fn bus(&self, name: &str) -> Option<&Bus> {
        self.buses.iter().find(|bus| bus.name == name)
    }

    fn bus_mut(&mut self, name: &str, action: &str) -> Result<&mut Bus> {
        self.buses
            .iter_mut()
            .find(|bus| bus.name == name)
            .ok_or_else(|| refuse(action, format!("the mixer has no bus named {name:?}")))
    }

    /// `cymixer 1` text: every bus, then every send, then every effect, each in bus order.
    #[must_use]
    pub fn encode(&self) -> String {
        let mut out = String::from("cymixer 1\n");
        for bus in &self.buses {
            let _ = writeln!(
                out,
                "bus {} {} {} {} {} {}",
                bus.name,
                bus.output.as_deref().unwrap_or("-"),
                bus.volume,
                u8::from(bus.mute),
                u8::from(bus.solo),
                u8::from(bus.bypass)
            );
        }
        for bus in &self.buses {
            for send in &bus.sends {
                let _ = writeln!(out, "send {} {} {}", bus.name, send.target, send.level);
            }
        }
        for bus in &self.buses {
            for effect in &bus.effects {
                let _ = writeln!(
                    out,
                    "effect {} {} {} {} {}",
                    bus.name,
                    effect.kind,
                    effect.a,
                    effect.b,
                    u8::from(effect.bypass)
                );
            }
        }
        out
    }

    /// Read `cymixer 1` text, refusing what the engine would refuse.
    pub fn decode(text: &str) -> Result<Self> {
        const ACTION: &str = "read an audio mixer";
        let mut lines = text.lines().filter(|line| !line.is_empty());
        if lines.next() != Some("cymixer 1") {
            return Err(refuse(ACTION, "expected `cymixer 1`"));
        }
        let mut mixer = Self { buses: Vec::new() };
        for line in lines {
            let words: Vec<&str> = line.split(' ').collect();
            match words.first().copied() {
                Some("bus") => mixer.decode_bus(&words)?,
                Some("send") => mixer.decode_send(&words)?,
                Some("effect") => mixer.decode_effect(&words)?,
                _ => return Err(refuse(ACTION, format!("unknown line {line:?}"))),
            }
        }
        mixer.validate()?;
        Ok(mixer)
    }

    fn decode_bus(&mut self, words: &[&str]) -> Result<()> {
        const ACTION: &str = "read an audio mixer bus";
        let [_, name, output, volume, mute, solo, bypass] = words else {
            return Err(refuse(ACTION, "a bus line has six fields"));
        };
        let mut bus = Bus::new(name, (*output != "-").then_some(*output));
        bus.volume = number(volume, ACTION)?;
        bus.mute = flag(mute, ACTION)?;
        bus.solo = flag(solo, ACTION)?;
        bus.bypass = flag(bypass, ACTION)?;
        self.buses.push(bus);
        Ok(())
    }

    fn decode_send(&mut self, words: &[&str]) -> Result<()> {
        const ACTION: &str = "read an audio mixer send";
        let [_, from, to, level] = words else {
            return Err(refuse(ACTION, "a send line has three fields"));
        };
        let level = number(level, ACTION)?;
        self.bus_mut(from, ACTION)?.sends.push(Send {
            target: (*to).to_owned(),
            level,
        });
        Ok(())
    }

    fn decode_effect(&mut self, words: &[&str]) -> Result<()> {
        const ACTION: &str = "read an audio mixer effect";
        let [_, bus, kind, a, b, bypass] = words else {
            return Err(refuse(ACTION, "an effect line has five fields"));
        };
        let effect = Effect {
            kind: (*kind).to_owned(),
            a: number(a, ACTION)?,
            b: number(b, ACTION)?,
            bypass: flag(bypass, ACTION)?,
        };
        self.bus_mut(bus, ACTION)?.effects.push(effect);
        Ok(())
    }

    /// Every rule the engine's `parse_mixer` applies, so an edit is refused before it is saved.
    pub fn validate(&self) -> Result<()> {
        const ACTION: &str = "validate the audio mixer";
        let Some(master) = self.buses.first() else {
            return Err(refuse(ACTION, "a mixer has a Master bus"));
        };
        if master.name != MASTER || master.output.is_some() || !master.sends.is_empty() {
            return Err(refuse(
                ACTION,
                "the first bus is Master, and Master routes and sends nowhere",
            ));
        }
        if self.buses.len() > MAX_BUSES {
            return Err(refuse(ACTION, "a mixer has at most 32 buses"));
        }
        for (index, bus) in self.buses.iter().enumerate() {
            self.validate_bus(index, bus)?;
        }
        if self.has_cycle() {
            return Err(refuse(
                ACTION,
                "the routing has a cycle, which the engine would never finish mixing",
            )
            .with_remedy("route or send into a bus that does not feed this one"));
        }
        Ok(())
    }

    fn validate_bus(&self, index: usize, bus: &Bus) -> Result<()> {
        let action = format!("validate audio bus {:?}", bus.name);
        if !valid_name(&bus.name) || self.buses[..index].iter().any(|b| b.name == bus.name) {
            return Err(refuse(&action, "bus names are unique letters, digits, _ - ."));
        }
        if index > 0 {
            let output = bus.output.as_deref().unwrap_or_default();
            if output == bus.name || self.bus(output).is_none() {
                return Err(refuse(&action, "it outputs into another bus of this mixer"));
            }
        }
        if !finite_in(bus.volume, 0.0, MAX_BUS_VOLUME) {
            return Err(refuse(&action, "a bus volume is from 0 to 4"));
        }
        if bus.sends.len() > MAX_SENDS || bus.effects.len() > MAX_EFFECTS {
            return Err(refuse(&action, "a bus has at most four sends and eight effects"));
        }
        for send in &bus.sends {
            if send.target == bus.name
                || self.bus(&send.target).is_none()
                || !finite_in(send.level, 0.0, 1.0)
            {
                return Err(refuse(&action, "a send targets another bus at 0 to 1"));
            }
        }
        for effect in &bus.effects {
            if !EFFECT_KINDS.contains(&effect.kind.as_str())
                || !effect.a.is_finite()
                || !effect.b.is_finite()
            {
                return Err(refuse(&action, "an effect is one the engine has"));
            }
        }
        Ok(())
    }

    fn edges(&self, index: usize) -> Vec<usize> {
        let bus = &self.buses[index];
        bus.output
            .iter()
            .chain(bus.sends.iter().map(|send| &send.target))
            .filter_map(|name| self.buses.iter().position(|other| &other.name == name))
            .collect()
    }

    fn has_cycle(&self) -> bool {
        // 0 unvisited, 1 on the stack, 2 finished.
        fn visit(mixer: &Mixer, index: usize, state: &mut [u8]) -> bool {
            match state[index] {
                1 => return true,
                2 => return false,
                _ => {}
            }
            state[index] = 1;
            if mixer
                .edges(index)
                .into_iter()
                .any(|next| visit(mixer, next, state))
            {
                return true;
            }
            state[index] = 2;
            false
        }
        let mut state = vec![0_u8; self.buses.len()];
        (0..self.buses.len()).any(|index| visit(self, index, &mut state))
    }

    /// Apply one edit and keep it only when the result validates.
    pub fn edited(&self, edit: impl FnOnce(&mut Self) -> Result<()>) -> Result<Self> {
        let mut next = self.clone();
        edit(&mut next)?;
        next.validate()?;
        Ok(next)
    }

    /// Add a bus routed into `output`.
    pub fn add_bus(&mut self, name: &str, output: &str) -> Result<()> {
        if self.bus(name).is_some() {
            return Err(refuse(
                "add an audio bus",
                format!("the mixer already has a bus named {name:?}"),
            ));
        }
        self.buses.push(Bus::new(name, Some(output)));
        Ok(())
    }

    /// Remove a bus nothing routes or sends into.
    pub fn remove_bus(&mut self, name: &str) -> Result<()> {
        const ACTION: &str = "remove an audio bus";
        if name == MASTER {
            return Err(refuse(ACTION, "Master is the root of every mixer"));
        }
        if let Some(user) = self.buses.iter().find(|bus| {
            bus.output.as_deref() == Some(name) || bus.sends.iter().any(|send| send.target == name)
        }) {
            return Err(
                refuse(ACTION, format!("{:?} still routes into {name:?}", user.name))
                    .with_remedy("route that bus elsewhere first"),
            );
        }
        let index = self
            .buses
            .iter()
            .position(|bus| bus.name == name)
            .ok_or_else(|| refuse(ACTION, format!("the mixer has no bus named {name:?}")))?;
        self.buses.remove(index);
        Ok(())
    }

    /// Set a bus's gain.
    pub fn set_volume(&mut self, name: &str, volume: f32) -> Result<()> {
        self.bus_mut(name, "set an audio bus volume")?.volume = volume;
        Ok(())
    }

    /// Set `mute`, `solo` or `bypass`.
    pub fn set_flag(&mut self, name: &str, which: &str, enabled: bool) -> Result<()> {
        let bus = self.bus_mut(name, "set an audio bus flag")?;
        match which {
            "mute" => bus.mute = enabled,
            "solo" => bus.solo = enabled,
            "bypass" => bus.bypass = enabled,
            _ => {
                return Err(refuse(
                    "set an audio bus flag",
                    "the flag is mute, solo or bypass",
                ));
            }
        }
        Ok(())
    }

    /// Route a bus's output.
    pub fn route(&mut self, name: &str, output: &str) -> Result<()> {
        const ACTION: &str = "route an audio bus";
        if name == MASTER {
            return Err(refuse(ACTION, "Master is the root and routes nowhere"));
        }
        self.bus_mut(name, ACTION)?.output = Some(output.to_owned());
        Ok(())
    }

    /// Set a send's level; zero removes it.
    pub fn set_send(&mut self, from: &str, to: &str, level: f32) -> Result<()> {
        let bus = self.bus_mut(from, "set an audio send")?;
        if let Some(index) = bus.sends.iter().position(|send| send.target == to) {
            if level == 0.0 {
                bus.sends.remove(index);
            } else {
                bus.sends[index].level = level;
            }
        } else if level != 0.0 {
            bus.sends.push(Send {
                target: to.to_owned(),
                level,
            });
        }
        Ok(())
    }

    /// Append an effect to a bus's chain.
    pub fn add_effect(&mut self, bus: &str, effect: Effect) -> Result<()> {
        self.bus_mut(bus, "add an audio effect")?
            .effects
            .push(effect);
        Ok(())
    }

    /// Change one effect of a bus's chain.
    pub fn set_effect(
        &mut self,
        bus: &str,
        index: usize,
        edit: impl FnOnce(&mut Effect),
    ) -> Result<()> {
        let effect = self
            .bus_mut(bus, "change an audio effect")?
            .effects
            .get_mut(index)
            .ok_or_else(|| refuse("change an audio effect", "no effect at that position"))?;
        edit(effect);
        Ok(())
    }

    /// Remove one effect of a bus's chain.
    pub fn remove_effect(&mut self, bus: &str, index: usize) -> Result<()> {
        let effects = &mut self.bus_mut(bus, "remove an audio effect")?.effects;
        if index >= effects.len() {
            return Err(refuse(
                "remove an audio effect",
                "no effect at that position",
            ));
        }
        effects.remove(index);
        Ok(())
    }
}

fn number(word: &str, action: &str) -> Result<f32> {
    word.parse::<f32>()
        .ok()
        .filter(|value| value.is_finite())
        .ok_or_else(|| refuse(action, format!("{word:?} is not a number")))
}

fn flag(word: &str, action: &str) -> Result<bool> {
    match word {
        "0" => Ok(false),
        "1" => Ok(true),
        _ => Err(refuse(action, format!("{word:?} is not 0 or 1"))),
    }
}

/// A `.cycue` asset.
#[derive(Clone, PartialEq, Debug)]
pub struct Cue {
    /// `tone:<hertz>:<seconds>`, or a project-relative 48 kHz `.wav`.
    pub clip: String,
    /// The mixer bus it plays on.
    pub bus: String,
    /// Linear gain, 0 to 4.
    pub volume: f32,
    /// Playback-rate ratio, 0.125 to 8.
    pub pitch: f32,
    /// Random gain range, 0 to 1.
    pub volume_variation: f32,
    /// Random rate range, 0 to 1.
    pub pitch_variation: f32,
    /// Loops until stopped.
    pub looping: bool,
}

impl Cue {
    /// A cue with the engine's defaults.
    #[must_use]
    pub fn new(clip: impl Into<String>) -> Self {
        Self {
            clip: clip.into(),
            bus: MASTER.to_owned(),
            volume: 1.0,
            pitch: 1.0,
            volume_variation: 0.0,
            pitch_variation: 0.0,
            looping: false,
        }
    }

    /// `cycue 1` text, every field written.
    #[must_use]
    pub fn encode(&self) -> String {
        format!(
            "cycue 1\nclip {}\nbus {}\nvolume {}\npitch {}\nvolume_variation {}\n\
             pitch_variation {}\nlooping {}\n",
            self.clip,
            self.bus,
            self.volume,
            self.pitch,
            self.volume_variation,
            self.pitch_variation,
            u8::from(self.looping)
        )
    }

    /// Read `cycue 1` text.
    pub fn decode(text: &str) -> Result<Self> {
        const ACTION: &str = "read an audio cue";
        let mut lines = text.lines().filter(|line| !line.is_empty());
        if lines.next() != Some("cycue 1") {
            return Err(refuse(ACTION, "expected `cycue 1`"));
        }
        let mut cue = Self::new("");
        for line in lines {
            let (key, value) = line
                .split_once(' ')
                .ok_or_else(|| refuse(ACTION, "a line is `<key> <value>`"))?;
            match key {
                "clip" => value.clone_into(&mut cue.clip),
                "bus" => value.clone_into(&mut cue.bus),
                "volume" => cue.volume = number(value, ACTION)?,
                "pitch" => cue.pitch = number(value, ACTION)?,
                "volume_variation" => cue.volume_variation = number(value, ACTION)?,
                "pitch_variation" => cue.pitch_variation = number(value, ACTION)?,
                "looping" => cue.looping = flag(value, ACTION)?,
                _ => return Err(refuse(ACTION, format!("unknown key {key:?}"))),
            }
        }
        cue.validate()?;
        Ok(cue)
    }

    /// The engine's ranges, checked before a save.
    pub fn validate(&self) -> Result<()> {
        const ACTION: &str = "validate an audio cue";
        let tone = self.clip.strip_prefix("tone:").and_then(|rest| {
            let (hertz, seconds) = rest.split_once(':')?;
            let hertz = hertz.parse::<f32>().ok()?;
            let seconds = seconds.parse::<f32>().ok()?;
            (finite_in(hertz, 20.0, 20_000.0) && finite_in(seconds, 0.01, 10.0)).then_some(())
        });
        let wav = Path::new(&self.clip)
            .extension()
            .is_some_and(|extension| extension == "wav")
            && Path::new(&self.clip)
                .components()
                .all(|component| matches!(component, Component::Normal(_)));
        if tone.is_none() && !wav {
            return Err(refuse(
                ACTION,
                "the clip is `tone:<hertz>:<seconds>` or a project-relative .wav",
            ));
        }
        if !valid_name(&self.bus) {
            return Err(refuse(ACTION, "the bus name is not one a mixer can hold"));
        }
        if !finite_in(self.volume, 0.0, 4.0)
            || !finite_in(self.pitch, 0.125, 8.0)
            || !finite_in(self.volume_variation, 0.0, 1.0)
            || !finite_in(self.pitch_variation, 0.0, 1.0)
        {
            return Err(refuse(
                ACTION,
                "volume is 0 to 4, pitch 0.125 to 8, variations 0 to 1",
            ));
        }
        Ok(())
    }
}

/// Where a preview is heard from.
#[derive(Clone, PartialEq, Debug)]
pub struct Placement {
    /// Spatialised at `position`, or played flat.
    pub spatial: bool,
    /// Where the source is.
    pub position: [f32; 3],
    /// Where the listener is: the editor camera.
    pub listener: [f32; 3],
    /// Which way the listener faces.
    pub forward: [f32; 3],
    /// Full volume inside this.
    pub min_distance: f32,
    /// Silent beyond this.
    pub max_distance: f32,
    /// One of [`ATTENUATION_MODELS`].
    pub model: String,
}

impl Placement {
    /// A flat, non-spatial preview.
    #[must_use]
    pub fn flat() -> Self {
        Self {
            spatial: false,
            position: [0.0; 3],
            listener: [0.0; 3],
            forward: [0.0, 0.0, -1.0],
            min_distance: 1.0,
            max_distance: 50.0,
            model: "inverse".into(),
        }
    }
}

/// The `audio.cue.preview` payload: the cue's name and text, then the placement.
#[must_use]
pub fn preview_payload(name: &str, cue: &str, placement: &Placement) -> Vec<u8> {
    let mut out = Writer::new();
    out.text(name);
    out.text(cue);
    out.u8(u8::from(placement.spatial));
    for lane in placement
        .position
        .iter()
        .chain(&placement.listener)
        .chain(&placement.forward)
    {
        out.f32(*lane);
    }
    out.f32(placement.min_distance);
    out.f32(placement.max_distance);
    out.text(&placement.model);
    out.finish()
}

/// One bus as the engine's graph holds it.
#[derive(Clone, PartialEq, Debug)]
pub struct BusState {
    /// Its name.
    pub name: String,
    /// The bus it sums into; empty for Master.
    pub output: String,
    /// The gain the graph applies.
    pub volume: f32,
    /// Muted.
    pub mute: bool,
    /// Soloed.
    pub solo: bool,
    /// Effects skipped.
    pub bypass: bool,
    /// Whether the mix can hear it, solo and mute considered.
    pub audible: bool,
    /// Its sends, by target name.
    pub sends: Vec<(String, f32)>,
    /// Its effect chain, by kind name.
    pub effects: Vec<String>,
    /// The last block's peak, after its gain.
    pub peak: f32,
    /// The last block's RMS, after its gain.
    pub rms: f32,
}

/// What the last preview started, as the engine measured it.
#[derive(Clone, PartialEq, Debug)]
pub struct PreviewState {
    /// The cue's project path.
    pub cue: String,
    /// Whether its voice is still in the mix.
    pub playing: bool,
    /// Metres from the listener.
    pub distance: f32,
    /// The engine's attenuation at that distance.
    pub gain: f32,
    /// Constant-power pan, left.
    pub left: f32,
    /// Constant-power pan, right.
    pub right: f32,
}

/// The engine's answer to every `audio.*` request.
#[derive(Clone, PartialEq, Debug)]
pub struct AudioState {
    /// `null`, or the output device's backend.
    pub backend: String,
    /// Frames per second.
    pub sample_rate: u32,
    /// Voices in the mix.
    pub active_voices: u32,
    /// Voices advanced but not mixed.
    pub virtual_voices: u32,
    /// Blocks mixed since start.
    pub blocks_mixed: u32,
    /// Whether editor Play is playing the world's sources.
    pub playing: bool,
    /// How many.
    pub play_voices: u32,
    /// The bus graph.
    pub buses: Vec<BusState>,
    /// The cues the server holds, by project path.
    pub cues: Vec<String>,
    /// The last preview.
    pub preview: Option<PreviewState>,
}

impl AudioState {
    /// Decode schema 1.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut reader = Reader::new(payload);
        if reader.u32()? != 1 {
            return Err(refuse(
                "read the engine's audio state",
                "unknown state version",
            ));
        }
        let mut state = Self {
            backend: reader.text()?,
            sample_rate: reader.u32()?,
            active_voices: reader.u32()?,
            virtual_voices: reader.u32()?,
            blocks_mixed: reader.u32()?,
            playing: reader.u8()? != 0,
            play_voices: reader.u32()?,
            buses: Vec::new(),
            cues: Vec::new(),
            preview: None,
        };
        for _ in 0..reader.u32()? {
            state.buses.push(decode_bus(&mut reader)?);
        }
        for _ in 0..reader.u32()? {
            state.cues.push(reader.text()?);
        }
        if reader.u8()? != 0 {
            state.preview = Some(PreviewState {
                cue: reader.text()?,
                playing: reader.u8()? != 0,
                distance: reader.f32()?,
                gain: reader.f32()?,
                left: reader.f32()?,
                right: reader.f32()?,
            });
        }
        if !reader.is_empty() {
            return Err(refuse(
                "read the engine's audio state",
                "trailing bytes after the state",
            ));
        }
        Ok(state)
    }

    /// The bus of that name.
    #[must_use]
    pub fn bus(&self, name: &str) -> Option<&BusState> {
        self.buses.iter().find(|bus| bus.name == name)
    }
}

fn decode_bus(reader: &mut Reader<'_>) -> Result<BusState> {
    let mut bus = BusState {
        name: reader.text()?,
        output: reader.text()?,
        volume: reader.f32()?,
        mute: reader.u8()? != 0,
        solo: reader.u8()? != 0,
        bypass: reader.u8()? != 0,
        audible: reader.u8()? != 0,
        sends: Vec::new(),
        effects: Vec::new(),
        peak: 0.0,
        rms: 0.0,
    };
    for _ in 0..reader.u32()? {
        bus.sends.push((reader.text()?, reader.f32()?));
    }
    for _ in 0..reader.u32()? {
        bus.effects.push(reader.text()?);
        let _ = (reader.f32()?, reader.f32()?, reader.u8()?);
    }
    bus.peak = reader.f32()?;
    bus.rms = reader.f32()?;
    Ok(bus)
}

/// One effect kind the engine offers, with what its two parameters mean.
#[derive(Clone, PartialEq, Debug)]
pub struct EffectVocabulary {
    /// Its name.
    pub kind: String,
    /// What `a` means, or empty when unused.
    pub label_a: String,
    /// What `b` means, or empty when unused.
    pub label_b: String,
    /// A new effect's `a`.
    pub default_a: f32,
    /// A new effect's `b`.
    pub default_b: f32,
}

/// `audio.capabilities.get`: the vocabulary the mixer editor offers, which is the engine's.
#[derive(Clone, PartialEq, Debug)]
pub struct AudioVocabulary {
    /// `null`, or the output device's backend.
    pub backend: String,
    /// Frames per second.
    pub sample_rate: u32,
    /// Most buses one mixer may have.
    pub max_buses: u32,
    /// Most sends per bus.
    pub max_sends: u32,
    /// Most effects per bus.
    pub max_effects: u32,
    /// The effect kinds.
    pub effects: Vec<EffectVocabulary>,
    /// The attenuation models a source may use.
    pub attenuation_models: Vec<String>,
}

impl AudioVocabulary {
    /// Decode schema 1.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut reader = Reader::new(payload);
        if reader.u32()? != 1 {
            return Err(refuse(
                "read the engine's audio vocabulary",
                "unknown version",
            ));
        }
        let mut vocabulary = Self {
            backend: reader.text()?,
            sample_rate: reader.u32()?,
            max_buses: reader.u32()?,
            max_sends: reader.u32()?,
            max_effects: reader.u32()?,
            effects: Vec::new(),
            attenuation_models: Vec::new(),
        };
        for _ in 0..reader.u32()? {
            vocabulary.effects.push(EffectVocabulary {
                kind: reader.text()?,
                label_a: reader.text()?,
                label_b: reader.text()?,
                default_a: reader.f32()?,
                default_b: reader.f32()?,
            });
        }
        for _ in 0..reader.u32()? {
            vocabulary.attenuation_models.push(reader.text()?);
        }
        if !reader.is_empty() || vocabulary.effects.is_empty() {
            return Err(refuse(
                "read the engine's audio vocabulary",
                "it is malformed or offers no effects",
            ));
        }
        Ok(vocabulary)
    }

    /// The kind of that name.
    #[must_use]
    pub fn effect(&self, kind: &str) -> Option<&EffectVocabulary> {
        self.effects.iter().find(|effect| effect.kind == kind)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The engine's fixtures, from this crate's own directory. Computed rather than passed in an
    /// environment variable, for the reason `tests/the_engines_gizmo_layout_decodes.rs` gives.
    fn engine_fixture(name: &str) -> std::path::PathBuf {
        std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../../src/editor_backend/tests/data")
            .join(name)
    }

    fn read_fixture(name: &str) -> Vec<u8> {
        std::fs::read(engine_fixture(name)).unwrap_or_else(|error| {
            panic!(
                "the engine's audio fixture {name} could not be read ({error}); it is written by \
                 `CY_UPDATE_AUDIO_WIRE=1 cy_test_integration_editor_backend_audio`"
            )
        })
    }

    /// The mixer the engine's suite applies.
    pub(crate) fn canonical_mixer() -> Mixer {
        let mut mixer = Mixer::default();
        mixer.add_bus("Music", MASTER).unwrap();
        mixer.add_bus("SFX", MASTER).unwrap();
        mixer.add_bus("Reverb", MASTER).unwrap();
        mixer.set_volume("Music", 0.5).unwrap();
        mixer.set_volume("SFX", 0.8).unwrap();
        mixer.set_volume("Reverb", 0.6).unwrap();
        mixer.set_send("SFX", "Reverb", 0.3).unwrap();
        mixer
            .add_effect(
                MASTER,
                Effect {
                    kind: "limiter".into(),
                    a: 0.95,
                    b: 0.0,
                    bypass: false,
                },
            )
            .unwrap();
        mixer
            .add_effect(
                "SFX",
                Effect {
                    kind: "low-pass".into(),
                    a: 0.0,
                    b: 0.5,
                    bypass: false,
                },
            )
            .unwrap();
        mixer
    }

    pub(crate) fn canonical_cue() -> Cue {
        let mut cue = Cue::new("tone:660:0.5");
        cue.bus = "SFX".into();
        cue
    }

    pub(crate) fn canonical_placement() -> Placement {
        Placement {
            spatial: true,
            position: [3.0, 0.0, 0.0],
            listener: [0.0, 0.0, 0.0],
            forward: [0.0, 0.0, -1.0],
            min_distance: 1.0,
            max_distance: 50.0,
            model: "inverse".into(),
        }
    }

    #[test]
    fn the_editors_mixer_is_the_text_the_engine_applies() {
        let committed = String::from_utf8(read_fixture("audio_mixer_v1.cymixer")).unwrap();
        assert_eq!(canonical_mixer().encode(), committed);
        assert_eq!(Mixer::decode(&committed).unwrap(), canonical_mixer());
    }

    #[test]
    fn the_editors_cue_and_preview_request_are_the_bytes_the_engine_reads() {
        let cue = String::from_utf8(read_fixture("audio_cue_v1.cycue")).unwrap();
        assert_eq!(canonical_cue().encode(), cue);
        assert_eq!(Cue::decode(&cue).unwrap(), canonical_cue());
        assert_eq!(
            preview_payload(
                "audio/cues/ping.cycue",
                &canonical_cue().encode(),
                &canonical_placement()
            ),
            read_fixture("audio_cue_preview_v1.wire")
        );
    }

    #[test]
    fn the_engines_state_reply_decodes_to_what_its_mixer_applied() {
        let state = AudioState::decode(&read_fixture("audio_state_v1.wire")).unwrap();
        assert_eq!(state.backend, "null");
        assert_eq!(state.sample_rate, 48_000);
        assert_eq!(state.active_voices, 1);
        let names: Vec<&str> = state.buses.iter().map(|bus| bus.name.as_str()).collect();
        assert_eq!(names, ["Master", "Music", "SFX", "Reverb"]);
        let music = state.bus("Music").unwrap();
        assert!((music.volume - 0.5).abs() < f32::EPSILON);
        assert_eq!(music.output, MASTER);
        let sfx = state.bus("SFX").unwrap();
        assert_eq!(sfx.sends, [("Reverb".to_owned(), 0.3)]);
        assert_eq!(sfx.effects, ["low-pass"]);
        assert!(sfx.peak > 0.0, "the engine mixed the preview through SFX");
        assert_eq!(state.bus(MASTER).unwrap().effects, ["limiter"]);
        let preview = state.preview.expect("the preview is reported");
        assert_eq!(preview.cue, "audio/cues/ping.cycue");
        assert!(preview.playing);
        assert!((preview.distance - 3.0).abs() < 1e-4);
        assert!((preview.gain - 1.0 / 3.0).abs() < 1e-4);
        assert!(preview.right > 0.99 && preview.left < 0.01);
    }

    #[test]
    fn the_engines_play_reply_reports_its_sources() {
        let state = AudioState::decode(&read_fixture("audio_state_play_v1.wire")).unwrap();
        assert!(state.playing);
        assert_eq!(state.play_voices, 1);
        assert_eq!(state.active_voices, 1);
        assert!(state.cues.iter().any(|cue| cue == "audio/cues/hum.cycue"));
        assert!(state.bus(MASTER).unwrap().peak > 0.0);
    }

    #[test]
    fn the_engines_vocabulary_is_the_editors_effect_and_model_list() {
        let vocabulary =
            AudioVocabulary::decode(&read_fixture("audio_capabilities_v1.wire")).unwrap();
        let kinds: Vec<&str> = vocabulary
            .effects
            .iter()
            .map(|effect| effect.kind.as_str())
            .collect();
        assert_eq!(kinds, EFFECT_KINDS);
        assert_eq!(vocabulary.attenuation_models, ATTENUATION_MODELS);
        assert_eq!(vocabulary.max_sends as usize, MAX_SENDS);
        assert_eq!(vocabulary.max_effects as usize, MAX_EFFECTS);
        assert_eq!(vocabulary.max_buses as usize, MAX_BUSES);
        assert_eq!(vocabulary.effect("limiter").unwrap().label_a, "ceiling");
    }

    #[test]
    fn an_edit_that_would_cycle_or_dangle_is_refused_and_leaves_the_mixer_alone() {
        let mixer = canonical_mixer();
        let cycle = mixer.edited(|mixer| mixer.set_send("Reverb", "SFX", 0.5));
        assert!(
            cycle.unwrap_err().to_string().contains("cycle"),
            "a send back into its own source would never finish mixing"
        );
        assert!(mixer.edited(|mixer| mixer.route("SFX", "Nowhere")).is_err());
        assert!(mixer.edited(|mixer| mixer.route(MASTER, "SFX")).is_err());
        assert!(mixer.edited(|mixer| mixer.set_volume("SFX", 5.0)).is_err());
        assert!(mixer.edited(|mixer| mixer.remove_bus("Reverb")).is_err());
        assert!(mixer.edited(|mixer| mixer.remove_bus(MASTER)).is_err());
        assert!(mixer.edited(|mixer| mixer.add_bus("Music", MASTER)).is_err());
        assert!(mixer.edited(|mixer| mixer.add_bus("bad name", MASTER)).is_err());
        let reverb_free = mixer
            .edited(|mixer| mixer.set_send("SFX", "Reverb", 0.0))
            .unwrap();
        assert!(reverb_free.bus("SFX").unwrap().sends.is_empty());
        assert!(reverb_free
            .edited(|mixer| mixer.remove_bus("Reverb"))
            .is_ok());
        assert_eq!(mixer, canonical_mixer(), "a refused edit changed nothing");
    }

    #[test]
    fn a_cue_out_of_range_or_outside_the_project_is_refused() {
        assert!(Cue::new("tone:5:1").validate().is_err());
        assert!(Cue::new("../outside.wav").validate().is_err());
        assert!(Cue::new("sounds/hit.wav").validate().is_ok());
        let mut loud = Cue::new("tone:440:1");
        loud.volume = 9.0;
        assert!(loud.validate().is_err());
        assert!(validate_cue_reference("audio/hit.cycue").is_ok());
        assert!(validate_cue_reference("/audio/hit.cycue").is_err());
        assert!(validate_mixer_reference("audio/../mixer.cymixer").is_err());
    }
}
