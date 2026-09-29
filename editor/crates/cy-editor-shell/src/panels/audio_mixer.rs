// SPDX-License-Identifier: MIT
//! The audio bus and mixer editor, cue preview and the selected source's range. Issue #29.
//!
//! A table, which is the surface `Domain::AudioBusesAndMixing` declares: one row per bus, with its
//! output, gain, mute, solo and bypass, and the level the ENGINE measured for it on the last block
//! it mixed. Everything the panel changes is one registered command — one undoable transaction, and
//! an MCP tool of the same name — and it reads back only what the engine answered, so a bus that
//! the engine refused shows the engine's graph and the refusal, not the value the author typed.

use cy_editor_commands::Arguments;
use cy_editor_core::value::Value;
use cy_editor_interface::Domain;
use cy_editor_interface::shell::Shell;
use cy_editor_interface::specialised::Session;
use cy_editor_services::audio::{
    ATTENUATION_MODELS, AudioState, AudioVocabulary, Bus, EFFECT_KINDS, MASTER, Mixer,
};
use cy_editor_services::audio_commands::AudioSource;
use cy_editor_visual::colour::{Semantic, Surface};

use super::specialised::{SpecialisedTool, ToolDiagnostic, ToolFrame};
use super::{Inputs, Intent, Panels, heading, nothing_here, numeric, secondary};
use crate::theme;

/// The panel's own presentation state: the bus a person selected and what they are typing.
#[derive(Clone, Debug)]
pub struct AudioInputs {
    /// Whether the panel drew this frame; the window keeps the engine's meters live while it does.
    pub seen: bool,
    /// The bus whose sends and effects are shown.
    pub bus: Option<String>,
    /// A new bus's name.
    pub new_bus: String,
    /// A drag in progress: which control, and the value it has reached. One command on release.
    pub drag: Option<(String, f32)>,
    /// A new cue's project path.
    pub cue_reference: String,
    /// A new cue's clip.
    pub cue_clip: String,
    /// A new cue's bus.
    pub cue_bus: String,
    /// The cue a new source plays and a preview plays.
    pub cue: Option<String>,
}

impl Default for AudioInputs {
    fn default() -> Self {
        Self {
            seen: false,
            bus: None,
            new_bus: String::new(),
            drag: None,
            cue_reference: "audio/cues/new.cycue".into(),
            cue_clip: "tone:440:0.5".into(),
            cue_bus: MASTER.into(),
            cue: None,
        }
    }
}

/// What the panel edits this frame.
pub(crate) struct Target {
    mixer: Mixer,
    state: Option<AudioState>,
    vocabulary: Option<AudioVocabulary>,
    cues: Vec<String>,
    source: Option<AudioSource>,
    problem: Option<String>,
}

/// The audio mixer, drawn in the specialised-editor frame.
pub(crate) struct AudioMixerTool;

impl SpecialisedTool for AudioMixerTool {
    const DOMAIN: Domain = Domain::AudioBusesAndMixing;
    const TITLE: &'static str = "Audio Mixer";
    const COMMANDS: &'static [&'static str] = &[
        "audio.mixer.create",
        "audio.bus.add",
        "audio.bus.remove",
        "audio.bus.volume",
        "audio.bus.flag",
        "audio.bus.route",
        "audio.bus.send",
        "audio.bus.effect.add",
        "audio.bus.effect.set",
        "audio.bus.effect.remove",
        "audio.cue.save",
        "audio.cue.preview",
        "audio.preview.stop",
        "audio.source.create",
        "audio.source.range",
        "audio.source.preview",
    ];

    type Target = Target;

    fn target(panels: &mut Panels<'_>, ui: &mut egui::Ui) -> Option<Self::Target> {
        panels.inputs.audio.seen = true;
        let Some(document_id) = panels.editor.workspace.active() else {
            nothing_here(
                ui,
                panels.shell,
                "No world is open.",
                "Open a world; audio edits undo in its history.",
            );
            return None;
        };
        let project = &panels.editor.project;
        let Some(mixer) = read_mixer(project) else {
            ui.label(secondary(
                panels.shell,
                "This project has no mixer yet. Everything plays through Master until it has one.",
            ));
            if ui.button("Create mixer").clicked() {
                panels
                    .intents
                    .push(Intent::Invoke("audio.mixer.create".into(), Arguments::new()));
            }
            return None;
        };
        let cues = project
            .source_paths()
            .into_iter()
            .filter(|path| path.ends_with(".cycue"))
            .collect();
        let selected: Vec<_> = panels.editor.selection.get().nodes().collect();
        let source = panels
            .editor
            .documents
            .get(document_id)
            .and_then(|document| {
                selected
                    .iter()
                    .find_map(|node| AudioSource::read(document, *node))
            });
        let audio = &panels.editor.backend.audio;
        Some(Target {
            mixer,
            state: audio.state().cloned(),
            vocabulary: audio.vocabulary().cloned(),
            cues,
            source,
            problem: audio.problem().map(str::to_owned),
        })
    }

    fn diagnostics(inputs: &Inputs) -> Vec<ToolDiagnostic> {
        let _ = inputs;
        Vec::new()
    }

    fn body(frame: &mut ToolFrame<'_>, _session: Session<'_>, target: Target, ui: &mut egui::Ui) {
        if let Some(problem) = &target.problem {
            super::status(ui, frame.shell, Semantic::Error, problem);
        }
        engine_line(frame.shell, ui, target.state.as_ref());
        egui::ScrollArea::vertical().show(ui, |ui| {
            bus_table(frame, ui, &target);
            selected_bus(frame, ui, &target);
            add_bus(frame, ui, &target.mixer);
            cues(frame, ui, &target);
            if let Some(source) = &target.source {
                source_range(frame, ui, source);
            }
        });
    }
}

fn read_mixer(project: &cy_editor_services::ProjectService) -> Option<Mixer> {
    let reference = cy_editor_services::audio::DEFAULT_MIXER;
    if !project.source_exists(reference) {
        return None;
    }
    project
        .read_source(reference)
        .ok()
        .and_then(|source| Mixer::decode(&source).ok())
}

fn invoke(frame: &mut ToolFrame<'_>, command: &str, arguments: Arguments) {
    frame
        .intents
        .push(Intent::Invoke(command.to_owned(), arguments));
}

fn text(value: &str) -> Value {
    Value::Text(value.to_owned())
}

/// One line on what is mixing, so the null backend is never mistaken for a silent device.
fn engine_line(shell: &Shell, ui: &mut egui::Ui, state: Option<&AudioState>) {
    let line = state.map_or_else(
        || "The engine has not reported its mixer. Attach a runtime to hear and meter it.".into(),
        |state| {
            format!(
                "Engine mixer: {} backend · {} Hz · {} voice(s) · Play {}",
                state.backend,
                state.sample_rate,
                state.active_voices,
                if state.playing {
                    format!("sounding {} source(s)", state.play_voices)
                } else {
                    "stopped".into()
                }
            )
        },
    );
    ui.label(secondary(shell, line));
}

/// A drag that becomes one command when it ends. Answers the value to commit, once.
fn committed_drag(
    ui: &mut egui::Ui,
    drag: &mut Option<(String, f32)>,
    key: &str,
    current: f32,
    range: std::ops::RangeInclusive<f32>,
) -> Option<f32> {
    let mut value = match drag {
        Some((dragging, value)) if dragging == key => *value,
        _ => current,
    };
    let response = ui.add(
        egui::DragValue::new(&mut value)
            .range(range)
            .speed(0.01)
            .max_decimals(3),
    );
    if response.dragged() {
        *drag = Some((key.to_owned(), value));
        return None;
    }
    let dragged_here = drag.as_ref().is_some_and(|(dragging, _)| dragging == key);
    if response.drag_stopped() || dragged_here {
        *drag = None;
        return Some(value);
    }
    (response.changed() && value.total_cmp(&current).is_ne()).then_some(value)
}

fn bus_table(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target) {
    heading(ui, frame.shell, "Buses");
    egui::Grid::new("audio-mixer-buses")
        .striped(true)
        .num_columns(8)
        .show(ui, |ui| {
            for title in ["Bus", "Output", "Volume", "Mute", "Solo", "Bypass", "Level", ""] {
                ui.label(secondary(frame.shell, title));
            }
            ui.end_row();
            for bus in &target.mixer.buses {
                bus_row(frame, ui, target, bus);
                ui.end_row();
            }
        });
}

fn bus_row(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target, bus: &Bus) {
    let selected = frame.inputs.audio.bus.as_deref() == Some(bus.name.as_str());
    if ui.selectable_label(selected, &bus.name).clicked() {
        frame.inputs.audio.bus = Some(bus.name.clone());
    }
    output_choice(frame, ui, &target.mixer, bus);
    if let Some(volume) = committed_drag(
        ui,
        &mut frame.inputs.audio.drag,
        &format!("volume:{}", bus.name),
        bus.volume,
        0.0..=4.0,
    ) {
        invoke(
            frame,
            "audio.bus.volume",
            Arguments::new()
                .with("name", text(&bus.name))
                .with("volume", Value::Float(volume)),
        );
    }
    for (which, set) in [("mute", bus.mute), ("solo", bus.solo), ("bypass", bus.bypass)] {
        let mut enabled = set;
        let name = format!("{which} {}", bus.name);
        let response = ui.checkbox(&mut enabled, "").on_hover_text(&name);
        response.widget_info(|| {
            egui::WidgetInfo::selected(egui::WidgetType::Checkbox, true, enabled, &name)
        });
        if response.changed() {
            invoke(
                frame,
                "audio.bus.flag",
                Arguments::new()
                    .with("name", text(&bus.name))
                    .with("flag", text(which))
                    .with("enabled", Value::Bool(enabled)),
            );
        }
    }
    let engine = target.state.as_ref().and_then(|state| state.bus(&bus.name));
    meter(frame.shell, ui, &bus.name, engine);
    if bus.name != MASTER
        && ui
            .small_button("Remove")
            .on_hover_text(format!("Remove {}", bus.name))
            .clicked()
    {
        invoke(
            frame,
            "audio.bus.remove",
            Arguments::new().with("name", text(&bus.name)),
        );
    }
}

fn output_choice(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, mixer: &Mixer, bus: &Bus) {
    let Some(output) = bus.output.as_deref() else {
        ui.label(secondary(frame.shell, "—"));
        return;
    };
    let mut chosen = output.to_owned();
    egui::ComboBox::from_id_salt(("audio-output", &bus.name))
        .selected_text(output)
        .show_ui(ui, |ui| {
            for candidate in mixer.buses.iter().filter(|other| other.name != bus.name) {
                ui.selectable_value(&mut chosen, candidate.name.clone(), &candidate.name);
            }
        });
    if chosen != output {
        invoke(
            frame,
            "audio.bus.route",
            Arguments::new()
                .with("name", text(&bus.name))
                .with("output", text(&chosen)),
        );
    }
}

/// The engine's peak for a bus: a thin bar and the number, so a meter is readable without colour.
fn meter(
    shell: &Shell,
    ui: &mut egui::Ui,
    name: &str,
    engine: Option<&cy_editor_services::audio::BusState>,
) {
    let peak = engine.map_or(0.0, |bus| bus.peak);
    let (rect, response) = ui.allocate_exact_size(egui::vec2(64.0, 8.0), egui::Sense::hover());
    let painter = ui.painter_at(rect);
    painter.rect_filled(rect, 1.0, theme::surface(shell.theme, Surface::Sunken));
    let lit = rect.width() * peak.clamp(0.0, 1.0);
    let role = if peak >= 1.0 {
        Semantic::Error
    } else {
        Semantic::Live
    };
    painter.rect_filled(
        egui::Rect::from_min_size(rect.min, egui::vec2(lit, rect.height())),
        1.0,
        theme::role(shell.theme, role),
    );
    let label = match engine {
        None => "no engine level".to_owned(),
        Some(_) if peak <= 1e-5 => "silent".to_owned(),
        Some(_) => format!("{:.1} dB", 20.0 * peak.log10()),
    };
    response.widget_info(|| {
        egui::WidgetInfo::labeled(egui::WidgetType::ProgressIndicator, true, format!("{name} level"))
    });
    ui.label(numeric(shell, label));
}

fn selected_bus(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target) {
    let Some(bus) = frame
        .inputs
        .audio
        .bus
        .clone()
        .and_then(|name| target.mixer.bus(&name).cloned())
    else {
        return;
    };
    heading(ui, frame.shell, &format!("{} sends", bus.name));
    sends(frame, ui, &target.mixer, &bus);
    heading(ui, frame.shell, &format!("{} effect chain", bus.name));
    effects(frame, ui, target.vocabulary.as_ref(), &bus);
}

fn sends(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, mixer: &Mixer, bus: &Bus) {
    for send in &bus.sends {
        ui.horizontal(|ui| {
            ui.label(format!("→ {}", send.target));
            if let Some(level) = committed_drag(
                ui,
                &mut frame.inputs.audio.drag,
                &format!("send:{}:{}", bus.name, send.target),
                send.level,
                0.0..=1.0,
            ) {
                invoke(frame, "audio.bus.send", send_arguments(bus, &send.target, level));
            }
            if ui.small_button("Remove send").clicked() {
                invoke(frame, "audio.bus.send", send_arguments(bus, &send.target, 0.0));
            }
        });
    }
    if bus.name == MASTER {
        return;
    }
    ui.menu_button("Add send", |ui| {
        for candidate in mixer.buses.iter().filter(|other| {
            other.name != bus.name && !bus.sends.iter().any(|send| send.target == other.name)
        }) {
            if ui.button(&candidate.name).clicked() {
                invoke(frame, "audio.bus.send", send_arguments(bus, &candidate.name, 0.5));
                ui.close();
            }
        }
    });
}

fn send_arguments(bus: &Bus, to: &str, level: f32) -> Arguments {
    Arguments::new()
        .with("from", text(&bus.name))
        .with("to", text(to))
        .with("level", Value::Float(level))
}

fn effects(
    frame: &mut ToolFrame<'_>,
    ui: &mut egui::Ui,
    vocabulary: Option<&AudioVocabulary>,
    bus: &Bus,
) {
    for (index, effect) in bus.effects.iter().enumerate() {
        ui.horizontal(|ui| {
            ui.label(&effect.kind);
            let labels = vocabulary.and_then(|vocabulary| vocabulary.effect(&effect.kind));
            let mut edited = (effect.a, effect.b, effect.bypass);
            let key = format!("effect:{}:{index}", bus.name);
            if labels.is_none_or(|labels| !labels.label_a.is_empty())
                && let Some(a) =
                    committed_drag(ui, &mut frame.inputs.audio.drag, &format!("{key}:a"), effect.a, 0.0..=4.0)
            {
                edited.0 = a;
            }
            if labels.is_none_or(|labels| !labels.label_b.is_empty())
                && let Some(b) = committed_drag(
                    ui,
                    &mut frame.inputs.audio.drag,
                    &format!("{key}:b"),
                    effect.b,
                    0.0..=0.9999,
                )
            {
                edited.1 = b;
            }
            ui.checkbox(&mut edited.2, "Bypass");
            if edited != (effect.a, effect.b, effect.bypass) {
                invoke(frame, "audio.bus.effect.set", effect_arguments(bus, index, edited));
            }
            if ui.small_button("Remove effect").clicked() {
                invoke(
                    frame,
                    "audio.bus.effect.remove",
                    Arguments::new()
                        .with("bus", text(&bus.name))
                        .with("index", Value::Int(i64::try_from(index).unwrap_or(i64::MAX))),
                );
            }
        });
    }
    ui.menu_button("Add effect", |ui| {
        let kinds: Vec<(String, f32, f32)> = vocabulary.map_or_else(
            || EFFECT_KINDS.iter().map(|kind| ((*kind).to_owned(), 1.0, 0.5)).collect(),
            |vocabulary| {
                vocabulary
                    .effects
                    .iter()
                    .map(|effect| (effect.kind.clone(), effect.default_a, effect.default_b))
                    .collect()
            },
        );
        for (kind, a, b) in kinds {
            if ui.button(&kind).clicked() {
                invoke(
                    frame,
                    "audio.bus.effect.add",
                    Arguments::new()
                        .with("bus", text(&bus.name))
                        .with("kind", text(&kind))
                        .with("a", Value::Float(a))
                        .with("b", Value::Float(b)),
                );
                ui.close();
            }
        }
    });
}

fn effect_arguments(bus: &Bus, index: usize, (a, b, bypass): (f32, f32, bool)) -> Arguments {
    Arguments::new()
        .with("bus", text(&bus.name))
        .with("index", Value::Int(i64::try_from(index).unwrap_or(i64::MAX)))
        .with("a", Value::Float(a))
        .with("b", Value::Float(b))
        .with("bypass", Value::Bool(bypass))
}

fn add_bus(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, mixer: &Mixer) {
    ui.horizontal(|ui| {
        ui.add(egui::TextEdit::singleline(&mut frame.inputs.audio.new_bus).hint_text("Bus name"));
        let name = frame.inputs.audio.new_bus.trim().to_owned();
        let valid = cy_editor_services::audio::valid_name(&name) && mixer.bus(&name).is_none();
        if ui.add_enabled(valid, egui::Button::new("Add bus")).clicked() {
            invoke(
                frame,
                "audio.bus.add",
                Arguments::new()
                    .with("name", text(&name))
                    .with("output", text(MASTER)),
            );
            frame.inputs.audio.new_bus.clear();
        }
    });
}

fn cues(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target) {
    heading(ui, frame.shell, "Cues");
    if target.cues.is_empty() {
        ui.label(secondary(frame.shell, "No cues yet. Save one below to preview it."));
    }
    for cue in &target.cues {
        ui.horizontal(|ui| {
            let chosen = frame.inputs.audio.cue.as_deref() == Some(cue.as_str());
            if ui.selectable_label(chosen, cue).clicked() {
                frame.inputs.audio.cue = Some(cue.clone());
            }
            if ui.small_button("Preview").on_hover_text(format!("Play {cue}")).clicked() {
                invoke(
                    frame,
                    "audio.cue.preview",
                    Arguments::new().with("reference", text(cue)),
                );
            }
        });
    }
    ui.horizontal(|ui| {
        if ui.button("Stop preview").clicked() {
            invoke(frame, "audio.preview.stop", Arguments::new());
        }
        let cue = frame.inputs.audio.cue.clone();
        if ui
            .add_enabled(cue.is_some(), egui::Button::new("Place source"))
            .on_hover_text("Create an entity that plays the chosen cue at the origin")
            .clicked()
            && let Some(cue) = cue
        {
            invoke(
                frame,
                "audio.source.create",
                Arguments::new().with("cue", text(&cue)),
            );
        }
    });
    new_cue(frame, ui, &target.mixer);
}

fn new_cue(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, mixer: &Mixer) {
    let audio = &mut frame.inputs.audio;
    ui.horizontal(|ui| {
        ui.add(egui::TextEdit::singleline(&mut audio.cue_reference).hint_text("audio/cues/hit.cycue"));
        ui.add(egui::TextEdit::singleline(&mut audio.cue_clip).hint_text("tone:440:0.5 or a .wav"));
        egui::ComboBox::from_id_salt("audio-cue-bus")
            .selected_text(audio.cue_bus.clone())
            .show_ui(ui, |ui| {
                for bus in &mixer.buses {
                    ui.selectable_value(&mut audio.cue_bus, bus.name.clone(), &bus.name);
                }
            });
    });
    if ui.button("Save cue").clicked() {
        let arguments = Arguments::new()
            .with("reference", text(&frame.inputs.audio.cue_reference))
            .with("clip", text(&frame.inputs.audio.cue_clip))
            .with("bus", text(&frame.inputs.audio.cue_bus));
        invoke(frame, "audio.cue.save", arguments);
    }
}

fn source_range(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, source: &AudioSource) {
    heading(ui, frame.shell, "Selected audio source");
    ui.label(secondary(
        frame.shell,
        format!(
            "{} · full volume within the solid ring, silent past the dashed one",
            source.cue
        ),
    ));
    let entity = source.entity.to_string();
    let mut range = (source.min_distance, source.max_distance, source.attenuation.clone());
    ui.horizontal(|ui| {
        ui.label("Full volume within");
        if let Some(min) = committed_drag(
            ui,
            &mut frame.inputs.audio.drag,
            &format!("min:{entity}"),
            source.min_distance,
            0.01..=10_000.0,
        ) {
            range.0 = min;
        }
        ui.label("m, silent past");
        if let Some(max) = committed_drag(
            ui,
            &mut frame.inputs.audio.drag,
            &format!("max:{entity}"),
            source.max_distance,
            0.02..=10_000.0,
        ) {
            range.1 = max;
        }
        ui.label("m");
        egui::ComboBox::from_id_salt("audio-source-attenuation")
            .selected_text(range.2.clone())
            .show_ui(ui, |ui| {
                for model in ATTENUATION_MODELS {
                    ui.selectable_value(&mut range.2, model.to_owned(), model);
                }
            });
    });
    if (range.0, range.1) != (source.min_distance, source.max_distance)
        || range.2 != source.attenuation
    {
        invoke(
            frame,
            "audio.source.range",
            Arguments::new()
                .with("entity", text(&entity))
                .with("min_distance", Value::Float(range.0))
                .with("max_distance", Value::Float(range.1))
                .with("attenuation", text(&range.2)),
        );
    }
    if ui
        .button("Preview from the camera")
        .on_hover_text("Play it in the engine where it is, heard from the viewport camera")
        .clicked()
    {
        invoke(
            frame,
            "audio.source.preview",
            Arguments::new().with("entity", text(&entity)),
        );
    }
}
