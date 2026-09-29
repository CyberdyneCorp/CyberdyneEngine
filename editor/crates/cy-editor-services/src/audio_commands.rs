// SPDX-License-Identifier: MIT
//! The audio authoring commands the mixer panel, scripts and MCP share. Issue #29.
//!
//! Every mixer edit reads the `.cymixer`, applies one change, validates the whole graph the way
//! the engine will, and saves it as one undoable project transaction — which also sends it to the
//! engine. A cue is saved the same way. A source is a scene entity created in one document
//! transaction. Previews and state reads change no document and are `Read`, as the VFX preview's
//! are: they play a sound; they do not author one.

use cy_editor_commands::{
    Arguments, Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, ProjectHost,
    Registry,
};
use cy_editor_core::ids::NodeId;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::Document;
use cy_editor_documents::selection::Selection;
use cy_editor_viewport::gizmo::TransformBinding;

use crate::audio::{
    ATTENUATION_MODELS, CUE_PREVIEW, Cue, DEFAULT_MIXER, EFFECT_KINDS, Effect, MIXER_APPLY, Mixer,
    PREVIEW_STOP, Placement, SOURCE_COMPONENT, STATE_GET, preview_payload, validate_cue_reference,
    validate_mixer_reference,
};
use crate::authoring::within_scope;

const CATEGORY: &str = "Audio";

/// Register every audio command.
pub fn register(registry: &mut Registry) -> Result<()> {
    for command in [
        mixer_read(),
        mixer_create(),
        mixer_apply(),
        bus_add(),
        bus_remove(),
        bus_volume(),
        bus_flag(),
        bus_route(),
        bus_send(),
        effect_add(),
        effect_set(),
        effect_remove(),
        cue_save(),
        cue_read(),
        cue_preview(),
        preview_stop(),
        refresh(),
        status(),
        source_create(),
        source_range(),
        source_preview(),
    ] {
        registry.register(command)?;
    }
    Ok(())
}

fn host(context: &mut dyn CommandContext) -> Result<&mut dyn ProjectHost> {
    context
        .project()
        .ok_or_else(|| Problem::new("author audio", "no project is open"))
}

fn text<'a>(arguments: &'a Arguments, name: &str) -> &'a str {
    arguments.text(name).unwrap_or_default()
}

fn float(arguments: &Arguments, name: &str) -> f32 {
    arguments
        .get(name)
        .and_then(Value::as_float)
        .unwrap_or_default()
}

fn flag(arguments: &Arguments, name: &str) -> bool {
    matches!(arguments.get(name), Some(Value::Bool(true)))
}

fn index(arguments: &Arguments, name: &str) -> Result<usize> {
    arguments
        .get(name)
        .and_then(Value::as_int)
        .and_then(|value| usize::try_from(value).ok())
        .ok_or_else(|| Problem::new("address an audio effect", "the index is a whole number"))
}

fn mixer_reference() -> ParameterSpec {
    ParameterSpec::optional(
        "reference",
        ValueKind::Text,
        "Project-relative .cymixer asset; the project's game/audio/mixer.cymixer when omitted.",
        Value::Text(DEFAULT_MIXER.into()),
    )
}

fn mutation(id: &'static str, label: &'static str, description: &'static str) -> Metadata {
    Metadata::new(
        id,
        label,
        CATEGORY,
        description,
        EffectClass::ReversibleMutation,
    )
    .with(mixer_reference())
}

fn read_class(id: &'static str, label: &'static str, description: &'static str) -> Metadata {
    Metadata::new(id, label, CATEGORY, description, EffectClass::Read)
}

/// The mixer at `reference`, or a Master-only one where there is none yet.
fn current_mixer(host: &dyn ProjectHost, reference: &str) -> Result<Mixer> {
    if host.source_exists(reference) {
        Mixer::decode(&host.read_source(reference)?)
    } else {
        Ok(Mixer::default())
    }
}

/// Read, change, validate and save one mixer as one undoable transaction.
fn edit_mixer(
    context: &mut dyn CommandContext,
    arguments: &Arguments,
    summary: String,
    edit: impl FnOnce(&mut Mixer) -> Result<()>,
) -> Result<Outcome> {
    let reference = text(arguments, "reference");
    let reference = if reference.is_empty() {
        DEFAULT_MIXER
    } else {
        reference
    };
    validate_mixer_reference(reference)?;
    within_scope(context, reference)?;
    let host = host(context)?;
    let next = current_mixer(host, reference)?.edited(edit)?;
    let source = next.encode();
    host.audio_asset_save(reference, &source)?;
    Ok(Outcome::new(summary)
        .with("reference", Value::Text(reference.to_owned()))
        .with("source", Value::Text(source)))
}

fn mixer_read() -> Command {
    Command::new(
        read_class(
            "audio.mixer.read",
            "Read Audio Mixer",
            "Returns the project's mixer as cymixer text, and every bus with its output and gain.",
        )
        .with(mixer_reference()),
        |context, arguments| {
            let reference = text(arguments, "reference");
            validate_mixer_reference(reference)?;
            let mixer = current_mixer(host(context)?, reference)?;
            let mut outcome = Outcome::new(format!("{} bus(es)", mixer.buses.len()))
                .with("source", Value::Text(mixer.encode()));
            for bus in &mixer.buses {
                outcome = outcome.with(
                    format!("bus.{}", bus.name),
                    Value::Text(format!(
                        "output={} volume={} mute={} solo={} bypass={} sends={} effects={}",
                        bus.output.as_deref().unwrap_or("-"),
                        bus.volume,
                        bus.mute,
                        bus.solo,
                        bus.bypass,
                        bus.sends.len(),
                        bus.effects.len()
                    )),
                );
            }
            Ok(outcome)
        },
    )
}

fn mixer_create() -> Command {
    Command::new(
        mutation(
            "audio.mixer.create",
            "Create Audio Mixer",
            "Creates the project's mixer with only its Master bus, as one undoable transaction.",
        ),
        |context, arguments| {
            let reference = text(arguments, "reference");
            if context
                .project()
                .is_some_and(|host| host.source_exists(reference))
            {
                return Err(Problem::new(
                    "create an audio mixer",
                    format!("{reference} already exists"),
                ));
            }
            edit_mixer(context, arguments, "Created the audio mixer".into(), |_| {
                Ok(())
            })
        },
    )
}

fn mixer_apply() -> Command {
    Command::new(
        read_class(
            "audio.mixer.apply",
            "Apply Audio Mixer",
            "Sends the saved mixer to the engine's audio server; the engine keeps each bus that \
             keeps its name. Changes no document.",
        )
        .with(mixer_reference()),
        |context, arguments| {
            let reference = text(arguments, "reference");
            validate_mixer_reference(reference)?;
            let host = host(context)?;
            let source = current_mixer(host, reference)?.encode();
            let request = host.audio_request(MIXER_APPLY, source.into_bytes())?;
            Ok(Outcome::new("Sent the mixer to the engine")
                .with("request", Value::Text(request.to_string())))
        },
    )
}

fn bus_name() -> ParameterSpec {
    ParameterSpec::required(
        "name",
        ValueKind::Text,
        "Bus name: letters, digits, underscore, hyphen or dot.",
    )
}

fn bus_add() -> Command {
    Command::new(
        mutation(
            "audio.bus.add",
            "Add Audio Bus",
            "Adds a bus at unit gain routed into another bus, as one undoable transaction.",
        )
        .with(bus_name())
        .with(ParameterSpec::optional(
            "output",
            ValueKind::Text,
            "The bus it sums into; Master when omitted.",
            Value::Text(crate::audio::MASTER.into()),
        )),
        |context, arguments| {
            let name = text(arguments, "name").to_owned();
            let output = text(arguments, "output").to_owned();
            edit_mixer(
                context,
                arguments,
                format!("Added audio bus {name}"),
                |mixer| mixer.add_bus(&name, &output),
            )
        },
    )
}

fn bus_remove() -> Command {
    Command::new(
        mutation(
            "audio.bus.remove",
            "Remove Audio Bus",
            "Removes a bus nothing routes or sends into, as one undoable transaction.",
        )
        .with(bus_name()),
        |context, arguments| {
            let name = text(arguments, "name").to_owned();
            edit_mixer(
                context,
                arguments,
                format!("Removed audio bus {name}"),
                |mixer| mixer.remove_bus(&name),
            )
        },
    )
}

fn bus_volume() -> Command {
    Command::new(
        mutation(
            "audio.bus.volume",
            "Set Audio Bus Volume",
            "Sets a bus's linear gain, 0 to 4, as one undoable transaction.",
        )
        .with(bus_name())
        .with(ParameterSpec::required(
            "volume",
            ValueKind::Float,
            "Linear gain from 0 (silent) to 4; 1 is unity.",
        )),
        |context, arguments| {
            let name = text(arguments, "name").to_owned();
            let volume = float(arguments, "volume");
            edit_mixer(
                context,
                arguments,
                format!("Set {name} to {volume}"),
                |mixer| mixer.set_volume(&name, volume),
            )
        },
    )
}

fn bus_flag() -> Command {
    Command::new(
        mutation(
            "audio.bus.flag",
            "Set Audio Bus Flag",
            "Sets a bus's mute, solo or bypass flag, as one undoable transaction.",
        )
        .with(bus_name())
        .with(ParameterSpec::required(
            "flag",
            ValueKind::Text,
            "One of mute, solo or bypass.",
        ))
        .with(ParameterSpec::required(
            "enabled",
            ValueKind::Bool,
            "Whether the flag is set.",
        )),
        |context, arguments| {
            let name = text(arguments, "name").to_owned();
            let which = text(arguments, "flag").to_owned();
            let enabled = flag(arguments, "enabled");
            edit_mixer(
                context,
                arguments,
                format!("Set {name} {which} {enabled}"),
                |mixer| mixer.set_flag(&name, &which, enabled),
            )
        },
    )
}

fn bus_route() -> Command {
    Command::new(
        mutation(
            "audio.bus.route",
            "Route Audio Bus",
            "Routes a bus's output into another bus; a route that closes a cycle is refused.",
        )
        .with(bus_name())
        .with(ParameterSpec::required(
            "output",
            ValueKind::Text,
            "The bus it sums into.",
        )),
        |context, arguments| {
            let name = text(arguments, "name").to_owned();
            let output = text(arguments, "output").to_owned();
            edit_mixer(
                context,
                arguments,
                format!("Routed {name} into {output}"),
                |mixer| mixer.route(&name, &output),
            )
        },
    )
}

fn bus_send() -> Command {
    Command::new(
        mutation(
            "audio.bus.send",
            "Set Audio Send",
            "Sets a send's level from one bus into another, 0 removing it; a send that closes a \
             cycle is refused.",
        )
        .with(ParameterSpec::required(
            "from",
            ValueKind::Text,
            "The bus whose signal is sent.",
        ))
        .with(ParameterSpec::required(
            "to",
            ValueKind::Text,
            "The bus it sends into.",
        ))
        .with(ParameterSpec::required(
            "level",
            ValueKind::Float,
            "Linear send level from 0 to 1; 0 removes the send.",
        )),
        |context, arguments| {
            let from = text(arguments, "from").to_owned();
            let to = text(arguments, "to").to_owned();
            let level = float(arguments, "level");
            edit_mixer(
                context,
                arguments,
                format!("Sent {from} into {to} at {level}"),
                |mixer| mixer.set_send(&from, &to, level),
            )
        },
    )
}

fn effect_bus() -> ParameterSpec {
    ParameterSpec::required(
        "bus",
        ValueKind::Text,
        "The bus whose effect chain changes.",
    )
}

fn effect_index() -> ParameterSpec {
    ParameterSpec::required(
        "index",
        ValueKind::Int,
        "Zero-based position in the bus's effect chain.",
    )
}

fn effect_add() -> Command {
    Command::new(
        mutation(
            "audio.bus.effect.add",
            "Add Audio Effect",
            "Appends an effect the engine has — gain, low-pass, high-pass or limiter — to a bus's \
             chain, as one undoable transaction.",
        )
        .with(effect_bus())
        .with(ParameterSpec::required(
            "kind",
            ValueKind::Text,
            "gain, low-pass, high-pass or limiter.",
        ))
        .with(ParameterSpec::optional(
            "a",
            ValueKind::Float,
            "Gain for gain, ceiling for limiter; unused otherwise.",
            Value::Float(1.0),
        ))
        .with(ParameterSpec::optional(
            "b",
            ValueKind::Float,
            "One-pole coefficient 0 to 0.9999 for low-pass and high-pass; unused otherwise.",
            Value::Float(0.5),
        )),
        |context, arguments| {
            let bus = text(arguments, "bus").to_owned();
            let kind = text(arguments, "kind").to_owned();
            if !EFFECT_KINDS.contains(&kind.as_str()) {
                return Err(Problem::new(
                    "add an audio effect",
                    format!("the engine has no {kind:?} effect"),
                )
                .with_remedy(format!("use one of: {}", EFFECT_KINDS.join(", "))));
            }
            let effect = Effect {
                kind: kind.clone(),
                a: float(arguments, "a"),
                b: float(arguments, "b"),
                bypass: false,
            };
            edit_mixer(
                context,
                arguments,
                format!("Added {kind} to {bus}"),
                |mixer| mixer.add_effect(&bus, effect),
            )
        },
    )
}

fn effect_set() -> Command {
    Command::new(
        mutation(
            "audio.bus.effect.set",
            "Set Audio Effect",
            "Changes one effect's parameters and bypass in a bus's chain, as one undoable \
             transaction.",
        )
        .with(effect_bus())
        .with(effect_index())
        .with(ParameterSpec::required(
            "a",
            ValueKind::Float,
            "Gain for gain, ceiling for limiter; unused otherwise.",
        ))
        .with(ParameterSpec::required(
            "b",
            ValueKind::Float,
            "One-pole coefficient for low-pass and high-pass; unused otherwise.",
        ))
        .with(ParameterSpec::optional(
            "bypass",
            ValueKind::Bool,
            "Skip this effect in the mix.",
            Value::Bool(false),
        )),
        |context, arguments| {
            let bus = text(arguments, "bus").to_owned();
            let position = index(arguments, "index")?;
            let (a, b, bypass) = (
                float(arguments, "a"),
                float(arguments, "b"),
                flag(arguments, "bypass"),
            );
            edit_mixer(
                context,
                arguments,
                format!("Changed effect {position} of {bus}"),
                |mixer| {
                    mixer.set_effect(&bus, position, |effect| {
                        effect.a = a;
                        effect.b = b;
                        effect.bypass = bypass;
                    })
                },
            )
        },
    )
}

fn effect_remove() -> Command {
    Command::new(
        mutation(
            "audio.bus.effect.remove",
            "Remove Audio Effect",
            "Removes one effect from a bus's chain, as one undoable transaction.",
        )
        .with(effect_bus())
        .with(effect_index()),
        |context, arguments| {
            let bus = text(arguments, "bus").to_owned();
            let position = index(arguments, "index")?;
            edit_mixer(
                context,
                arguments,
                format!("Removed effect {position} of {bus}"),
                |mixer| mixer.remove_effect(&bus, position),
            )
        },
    )
}

fn cue_reference() -> ParameterSpec {
    ParameterSpec::required(
        "reference",
        ValueKind::Text,
        "Project-relative .cycue asset path.",
    )
}

fn cue_save() -> Command {
    Command::new(
        Metadata::new(
            "audio.cue.save",
            "Save Audio Cue",
            CATEGORY,
            "Saves a playable cue — clip, bus, gain, pitch, variation and looping — as one undoable \
             project transaction.",
            EffectClass::ReversibleMutation,
        )
        .with(cue_reference())
        .with(ParameterSpec::required(
            "clip",
            ValueKind::Text,
            "tone:<hertz>:<seconds>, or a project-relative 16-bit or float 48 kHz .wav.",
        ))
        .with(ParameterSpec::optional(
            "bus",
            ValueKind::Text,
            "The mixer bus it plays on; Master when omitted.",
            Value::Text(crate::audio::MASTER.into()),
        ))
        .with(ParameterSpec::optional(
            "volume",
            ValueKind::Float,
            "Linear gain from 0 to 4.",
            Value::Float(1.0),
        ))
        .with(ParameterSpec::optional(
            "pitch",
            ValueKind::Float,
            "Playback-rate ratio from 0.125 to 8.",
            Value::Float(1.0),
        ))
        .with(ParameterSpec::optional(
            "volume_variation",
            ValueKind::Float,
            "Random gain range from 0 to 1.",
            Value::Float(0.0),
        ))
        .with(ParameterSpec::optional(
            "pitch_variation",
            ValueKind::Float,
            "Random rate range from 0 to 1.",
            Value::Float(0.0),
        ))
        .with(ParameterSpec::optional(
            "looping",
            ValueKind::Bool,
            "Loop the clip until the voice is stopped.",
            Value::Bool(false),
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            validate_cue_reference(reference)?;
            within_scope(context, reference)?;
            let cue = Cue {
                clip: text(arguments, "clip").to_owned(),
                bus: text(arguments, "bus").to_owned(),
                volume: float(arguments, "volume"),
                pitch: float(arguments, "pitch"),
                volume_variation: float(arguments, "volume_variation"),
                pitch_variation: float(arguments, "pitch_variation"),
                looping: flag(arguments, "looping"),
            };
            cue.validate()?;
            let source = cue.encode();
            host(context)?.audio_asset_save(reference, &source)?;
            Ok(Outcome::new(format!("Saved audio cue {reference}"))
                .with("source", Value::Text(source)))
        },
    )
}

fn cue_read() -> Command {
    Command::new(
        read_class(
            "audio.cue.read",
            "Read Audio Cue",
            "Returns a cue's cycue text.",
        )
        .with(cue_reference()),
        |context, arguments| {
            let reference = text(arguments, "reference");
            validate_cue_reference(reference)?;
            let source = host(context)?.read_source(reference)?;
            Cue::decode(&source)?;
            Ok(Outcome::new(format!("Read audio cue {reference}"))
                .with("source", Value::Text(source)))
        },
    )
}

/// Load and play a saved cue in the engine, flat or placed.
fn send_preview(
    context: &mut dyn CommandContext,
    reference: &str,
    placement: &Placement,
) -> Result<Outcome> {
    validate_cue_reference(reference)?;
    let host = host(context)?;
    let source = host.read_source(reference)?;
    Cue::decode(&source)?;
    let request =
        host.audio_request(CUE_PREVIEW, preview_payload(reference, &source, placement))?;
    Ok(
        Outcome::new(format!("Previewing {reference} in the engine"))
            .with("request", Value::Text(request.to_string())),
    )
}

fn cue_preview() -> Command {
    Command::new(
        read_class(
            "audio.cue.preview",
            "Preview Audio Cue",
            "Plays a saved cue once through the engine's mixer, unspatialised. Read audio.status \
             for the voice the engine started.",
        )
        .with(cue_reference()),
        |context, arguments| {
            send_preview(context, text(arguments, "reference"), &Placement::flat())
        },
    )
}

fn preview_stop() -> Command {
    Command::new(
        read_class(
            "audio.preview.stop",
            "Stop Audio Preview",
            "Stops every voice an audio preview started in the engine.",
        ),
        |context, _| {
            let request = host(context)?.audio_request(PREVIEW_STOP, Vec::new())?;
            Ok(Outcome::new("Stopping the audio preview")
                .with("request", Value::Text(request.to_string())))
        },
    )
}

fn refresh() -> Command {
    Command::new(
        read_class(
            "audio.refresh",
            "Refresh Audio State",
            "Asks the engine for its bus gains, levels and voices; read audio.status once it \
             answers. With no output device, advances the engine's mix by `seconds` first.",
        )
        .with(ParameterSpec::optional(
            "seconds",
            ValueKind::Float,
            "Seconds of mix to advance on the null backend, 0 to 0.25.",
            Value::Float(0.0),
        )),
        |context, arguments| {
            let seconds = float(arguments, "seconds");
            let payload = if seconds > 0.0 {
                crate::audio_requests::advance_payload(seconds)
            } else {
                Vec::new()
            };
            let request = host(context)?.audio_request(STATE_GET, payload)?;
            Ok(Outcome::new("Asked the engine for its audio state")
                .with("request", Value::Text(request.to_string())))
        },
    )
}

fn status() -> Command {
    Command::new(
        read_class(
            "audio.status",
            "Audio Status",
            "Reports the engine's last audio state: backend, voices, Play, every bus as the \
             engine's graph holds it with its level, and the last preview.",
        ),
        |context, _| Ok(host(context)?.audio_status()),
    )
}

fn source_create() -> Command {
    Command::new(
        Metadata::new(
            "audio.source.create",
            "Create Audio Source",
            CATEGORY,
            "Creates an entity that plays a cue at its position, with the radius it is heard at \
             full volume and the radius it falls silent at, as one undoable transaction.",
            EffectClass::ReversibleMutation,
        )
        .with(ParameterSpec::required(
            "cue",
            ValueKind::Text,
            "Project-relative .cycue the source plays.",
        ))
        .with(ParameterSpec::optional(
            "at",
            ValueKind::Vec3,
            "World position in metres; the origin when omitted.",
            Value::Vec3([0.0, 0.0, 0.0]),
        ))
        .with(ParameterSpec::optional(
            "min_distance",
            ValueKind::Float,
            "Metres within which it plays at full volume.",
            Value::Float(1.0),
        ))
        .with(ParameterSpec::optional(
            "max_distance",
            ValueKind::Float,
            "Metres beyond which it is silent.",
            Value::Float(30.0),
        ))
        .with(ParameterSpec::optional(
            "attenuation",
            ValueKind::Text,
            "inverse, inverse-square, linear or logarithmic.",
            Value::Text("inverse".into()),
        ))
        .with(ParameterSpec::optional(
            "autoplay",
            ValueKind::Bool,
            "Start when Play starts.",
            Value::Bool(true),
        )),
        create_source,
    )
}

fn check_range(min: f32, max: f32, attenuation: &str) -> Result<()> {
    if !(min.is_finite() && max.is_finite() && min > 0.0 && max > min) {
        return Err(Problem::new(
            "shape an audio source",
            "its distances need 0 < min_distance < max_distance",
        ));
    }
    if !ATTENUATION_MODELS.contains(&attenuation) {
        return Err(Problem::new(
            "shape an audio source",
            format!("attenuation is one of: {}", ATTENUATION_MODELS.join(", ")),
        ));
    }
    Ok(())
}

fn create_source(context: &mut dyn CommandContext, arguments: &Arguments) -> Result<Outcome> {
    let cue = text(arguments, "cue").to_owned();
    validate_cue_reference(&cue)?;
    let (min, max) = (
        float(arguments, "min_distance"),
        float(arguments, "max_distance"),
    );
    let attenuation = text(arguments, "attenuation").to_owned();
    check_range(min, max, &attenuation)?;
    let at = arguments
        .get("at")
        .and_then(Value::as_vec3)
        .unwrap_or_default();
    let autoplay = flag(arguments, "autoplay");
    let document_id = context
        .active_document()
        .ok_or_else(|| Problem::new("create an audio source", "no world is open"))?;
    let actor = context.actor();
    let name = std::path::Path::new(&cue).file_stem().map_or_else(
        || "Audio Source".to_owned(),
        |stem| stem.to_string_lossy().into_owned(),
    );
    let document = context
        .document_mut(document_id)
        .ok_or_else(|| Problem::not_found("the open world"))?;
    let node = document.with_transaction("Create Audio Source", actor, |document| {
        let node = document.create_node(None)?;
        document.set_name(node, &name)?;
        crate::primitives::add_transform(document, node, at)?;
        add_source(
            document,
            node,
            &[
                ("cue", Value::Text(cue.clone())),
                ("min_distance", Value::Float(min)),
                ("max_distance", Value::Float(max)),
                ("attenuation", Value::Text(attenuation.clone())),
                ("autoplay", Value::Bool(autoplay)),
                ("enabled", Value::Bool(true)),
            ],
        )?;
        Ok(node)
    })?;
    let mut selection = Selection::new();
    selection.add_node(node);
    context.set_selection(selection);
    Ok(Outcome::new(format!("Created audio source {name}"))
        .with("entity", Value::Text(node.to_string())))
}

fn add_source(document: &mut Document, node: NodeId, fields: &[(&str, Value)]) -> Result<()> {
    let schema = document.schema_mut();
    let component = match schema.type_named(SOURCE_COMPONENT) {
        Some(definition) => definition.id,
        None => schema.declare_type(SOURCE_COMPONENT, false),
    };
    let mut values = Vec::new();
    for (name, value) in fields {
        let id = match schema
            .type_of(component)
            .and_then(|definition| definition.field_named(name))
        {
            Some(field) => field.id,
            None => schema.declare_field(
                component,
                *name,
                value.kind(),
                format!("The audio source's {name}."),
            )?,
        };
        values.push((id, value.clone()));
    }
    document.add_component(node, component, values)
}

/// An authored source, read back out of the document.
#[derive(Clone, PartialEq, Debug)]
pub struct AudioSource {
    /// Its entity.
    pub entity: NodeId,
    /// The cue it plays.
    pub cue: String,
    /// Where it is.
    pub position: [f32; 3],
    /// Full volume inside this.
    pub min_distance: f32,
    /// Silent beyond this.
    pub max_distance: f32,
    /// Its curve between the two.
    pub attenuation: String,
}

impl AudioSource {
    /// The source on `entity`, when it has one.
    #[must_use]
    pub fn read(document: &Document, entity: NodeId) -> Option<Self> {
        let schema = document.schema();
        let component = schema.type_named(SOURCE_COMPONENT)?;
        let field = |name: &str| {
            let id = component.field_named(name)?.id;
            document.content().field(entity, component.id, id)
        };
        let (
            Some(Value::Text(cue)),
            Some(Value::Float(min_distance)),
            Some(Value::Float(max_distance)),
            Some(Value::Text(attenuation)),
        ) = (
            field("cue"),
            field("min_distance"),
            field("max_distance"),
            field("attenuation"),
        )
        else {
            return None;
        };
        let transform = TransformBinding::of_schema(schema)?;
        let position =
            match document
                .content()
                .field(entity, transform.component, transform.translation)
            {
                Some(Value::Vec3(position)) => *position,
                _ => [0.0; 3],
            };
        Some(Self {
            entity,
            cue: cue.clone(),
            position,
            min_distance: *min_distance,
            max_distance: *max_distance,
            attenuation: attenuation.clone(),
        })
    }
}

fn entity_argument(arguments: &Arguments) -> Result<NodeId> {
    let entity = text(arguments, "entity");
    u128::from_str_radix(entity, 16)
        .map(NodeId::from_u128)
        .map_err(|_| {
            Problem::new(
                format!("find the audio source {entity:?}"),
                "it is not an entity identity",
            )
        })
}

fn entity_parameter() -> ParameterSpec {
    ParameterSpec::required(
        "entity",
        ValueKind::Text,
        "Hexadecimal identity of the audio source entity.",
    )
}

fn source_range() -> Command {
    Command::new(
        Metadata::new(
            "audio.source.range",
            "Set Audio Source Range",
            CATEGORY,
            "Sets the distance a source plays at full volume and the distance it falls silent at, \
             and its curve between, as one undoable transaction.",
            EffectClass::ReversibleMutation,
        )
        .with(entity_parameter())
        .with(ParameterSpec::required(
            "min_distance",
            ValueKind::Float,
            "Metres within which it plays at full volume.",
        ))
        .with(ParameterSpec::required(
            "max_distance",
            ValueKind::Float,
            "Metres beyond which it is silent.",
        ))
        .with(ParameterSpec::optional(
            "attenuation",
            ValueKind::Text,
            "inverse, inverse-square, linear or logarithmic.",
            Value::Text("inverse".into()),
        )),
        |context, arguments| {
            let entity = entity_argument(arguments)?;
            let (min, max) = (
                float(arguments, "min_distance"),
                float(arguments, "max_distance"),
            );
            let attenuation = text(arguments, "attenuation").to_owned();
            check_range(min, max, &attenuation)?;
            let document_id = context
                .active_document()
                .ok_or_else(|| Problem::new("shape an audio source", "no world is open"))?;
            let actor = context.actor();
            let document = context
                .document_mut(document_id)
                .ok_or_else(|| Problem::not_found("the open world"))?;
            if AudioSource::read(document, entity).is_none() {
                return Err(Problem::new(
                    "shape an audio source",
                    format!("{entity} has no {SOURCE_COMPONENT}"),
                ));
            }
            let component = document
                .schema()
                .type_named(SOURCE_COMPONENT)
                .expect("read above")
                .clone();
            let id = |name: &str| component.field_named(name).expect("read above").id;
            let (min_id, max_id, curve_id) =
                (id("min_distance"), id("max_distance"), id("attenuation"));
            document.with_transaction("Set Audio Source Range", actor, |document| {
                document.set_field(entity, component.id, min_id, Value::Float(min))?;
                document.set_field(entity, component.id, max_id, Value::Float(max))?;
                document.set_field(
                    entity,
                    component.id,
                    curve_id,
                    Value::Text(attenuation.clone()),
                )
            })?;
            Ok(Outcome::new(format!("Heard from {min} m to {max} m")))
        },
    )
}

fn source_preview() -> Command {
    Command::new(
        read_class(
            "audio.source.preview",
            "Preview Audio Source",
            "Plays an authored source's cue in the engine at the source's position, heard from \
             the viewport camera, with its attenuation. Read audio.status for the gain and pan the \
             engine applied.",
        )
        .with(entity_parameter()),
        |context, arguments| {
            let entity = entity_argument(arguments)?;
            let document_id = context
                .active_document()
                .ok_or_else(|| Problem::new("preview an audio source", "no world is open"))?;
            let source = context
                .document(document_id)
                .and_then(|document| AudioSource::read(document, entity))
                .ok_or_else(|| {
                    Problem::new(
                        "preview an audio source",
                        format!("{entity} has no {SOURCE_COMPONENT}"),
                    )
                })?;
            let (listener, forward) = host(context)?.audio_listener();
            let placement = Placement {
                spatial: true,
                position: source.position,
                listener,
                forward,
                min_distance: source.min_distance,
                max_distance: source.max_distance,
                model: source.attenuation.clone(),
            };
            send_preview(context, &source.cue, &placement)
        },
    )
}
