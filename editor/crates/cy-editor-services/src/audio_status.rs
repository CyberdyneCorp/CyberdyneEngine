// SPDX-License-Identifier: MIT
//! `audio.status`: the engine's last audio state as a command outcome. Issue #29.
//!
//! Every number here was decoded from an engine reply — none is the editor's own copy of what it
//! asked for — so an agent reading `bus.Music.volume` reads the gain the engine's graph holds.

use cy_editor_commands::Outcome;
use cy_editor_core::value::Value;

use crate::audio_requests::AudioRequests;

fn whole(value: u32) -> Value {
    Value::Int(i64::from(value))
}

/// Describe the engine's audio state for a person and for an agent.
#[must_use]
pub fn outcome(requests: &AudioRequests) -> Outcome {
    let mut outcome = Outcome::new(if requests.state().is_some() {
        "Engine audio state"
    } else {
        "The engine has not reported its audio state"
    })
    .with("pending", Value::Bool(requests.pending()));
    if let Some(problem) = requests.problem() {
        outcome = outcome.with("problem", Value::Text(problem.to_owned()));
    }
    let Some(state) = requests.state() else {
        return outcome;
    };
    outcome = outcome
        .with("backend", Value::Text(state.backend.clone()))
        .with("sample_rate", whole(state.sample_rate))
        .with("active_voices", whole(state.active_voices))
        .with("virtual_voices", whole(state.virtual_voices))
        .with("playing", Value::Bool(state.playing))
        .with("play_voices", whole(state.play_voices))
        .with(
            "buses",
            whole(u32::try_from(state.buses.len()).unwrap_or(u32::MAX)),
        )
        .with("cues", Value::Text(state.cues.join(", ")));
    for bus in &state.buses {
        let key = format!("bus.{}", bus.name);
        outcome = outcome
            .with(format!("{key}.volume"), Value::Float(bus.volume))
            .with(format!("{key}.peak"), Value::Float(bus.peak))
            .with(
                format!("{key}.route"),
                Value::Text(format!(
                    "output={} mute={} solo={} bypass={} audible={} sends=[{}] effects=[{}]",
                    if bus.output.is_empty() {
                        "-"
                    } else {
                        &bus.output
                    },
                    bus.mute,
                    bus.solo,
                    bus.bypass,
                    bus.audible,
                    bus.sends
                        .iter()
                        .map(|(target, level)| format!("{target}:{level}"))
                        .collect::<Vec<_>>()
                        .join(" "),
                    bus.effects.join(" ")
                )),
            );
    }
    if let Some(preview) = &state.preview {
        outcome = outcome
            .with("preview.cue", Value::Text(preview.cue.clone()))
            .with("preview.playing", Value::Bool(preview.playing))
            .with("preview.distance", Value::Float(preview.distance))
            .with("preview.gain", Value::Float(preview.gain))
            .with("preview.left", Value::Float(preview.left))
            .with("preview.right", Value::Float(preview.right));
    }
    outcome
}
