//! Editable VFX system hierarchy. Stage sources are snapshots of the shared graph canvas;
//! compilation and canonical graph writing belong to the engine service.

use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::problem::{Problem, Result};

use super::graph::GraphCanvas;
use super::material::{graph_canvas_interchange, load_graph_canvas_interchange};

const VERSION: u32 = 1;
const MAX_ITEMS: u32 = 4096;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
/// An engine VFX stage, in the engine's declared order.
pub enum Stage {
    /// Determines spawn count.
    Spawn,
    /// Initializes new particles.
    Initialise,
    /// Advances live particles.
    Update,
    /// Handles received events.
    Event,
    /// Produces renderer inputs.
    Render,
    /// Runs emitter reductions or grids.
    Compute,
}

impl Stage {
    /// All stages declared by the engine asset model.
    pub const ALL: [Self; 6] = [
        Self::Spawn,
        Self::Initialise,
        Self::Update,
        Self::Event,
        Self::Render,
        Self::Compute,
    ];

    /// Author-facing stage name.
    pub const fn label(self) -> &'static str {
        match self {
            Self::Spawn => "Spawn",
            Self::Initialise => "Initialise",
            Self::Update => "Update",
            Self::Event => "Event",
            Self::Render => "Render",
            Self::Compute => "Compute",
        }
    }

    fn code(self) -> u8 {
        self as u8
    }

    fn from_code(code: u8) -> Result<Self> {
        match code {
            0 => Ok(Self::Spawn),
            1 => Ok(Self::Initialise),
            2 => Ok(Self::Update),
            3 => Ok(Self::Event),
            4 => Ok(Self::Render),
            5 => Ok(Self::Compute),
            _ => Err(invalid("unknown VFX stage")),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
/// The path required by an emitter.
pub enum SimulationPath {
    /// Use the GPU when the device supports it.
    GpuPreferred,
    /// Run on the CPU for synchronous gameplay reads.
    CpuRequired,
}

/// One editable stage canvas snapshot.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StageGraph {
    /// Stage receiving this graph.
    pub stage: Stage,
    /// Versioned shared-canvas interchange text.
    pub canvas: String,
}

/// An authored emitter and its stage graph references.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Emitter {
    /// Stable author-facing name.
    pub name: String,
    /// Requested simulation path.
    pub path: SimulationPath,
    /// Backend renderer kind name.
    pub renderer: String,
    /// Editable stage snapshots.
    pub stages: Vec<StageGraph>,
    /// Reusable module asset names.
    pub modules: Vec<String>,
    /// Bound data-interface names.
    pub interfaces: Vec<String>,
}

/// A typed parameter exposed to gameplay or folded by the compiler.
#[derive(Clone, Debug, PartialEq)]
pub struct Parameter {
    /// Parameter name.
    pub name: String,
    /// Engine numeric type name.
    pub kind: String,
    /// Up to four components of the authored value.
    pub value: [f32; 4],
    /// Whether runtime updates may change this parameter.
    pub exposed: bool,
}

/// Versioned editable hierarchy for one VFX system.
#[derive(Clone, Debug, PartialEq)]
pub struct VfxDocument {
    /// System name.
    pub name: String,
    /// Emitters in authoring order.
    pub emitters: Vec<Emitter>,
    /// System parameters.
    pub parameters: Vec<Parameter>,
}

impl VfxDocument {
    /// Create an empty system with a valid engine name.
    pub fn new(name: impl Into<String>) -> Result<Self> {
        let name = name.into();
        identifier(&name)?;
        Ok(Self {
            name,
            emitters: Vec::new(),
            parameters: Vec::new(),
        })
    }

    /// Replace one stage snapshot after the shared canvas has been edited.
    pub fn set_stage(&mut self, emitter: usize, stage: Stage, canvas: String) -> Result<()> {
        let emitter = self
            .emitters
            .get_mut(emitter)
            .ok_or_else(|| invalid("unknown emitter"))?;
        if let Some(existing) = emitter.stages.iter_mut().find(|entry| entry.stage == stage) {
            existing.canvas = canvas;
        } else {
            emitter.stages.push(StageGraph { stage, canvas });
        }
        Ok(())
    }

    /// Snapshot the active shared canvas into one emitter stage.
    pub fn capture_stage(
        &mut self,
        emitter: usize,
        stage: Stage,
        canvas: &GraphCanvas,
    ) -> Result<()> {
        let name = self
            .emitters
            .get(emitter)
            .ok_or_else(|| invalid("unknown emitter"))?
            .name
            .clone();
        let source = graph_canvas_interchange(&name, canvas, "cyvfxcanvas", "emitter")?;
        self.set_stage(emitter, stage, source)
    }

    /// Replace the active shared canvas with an emitter's saved stage.
    pub fn open_stage(&self, emitter: usize, stage: Stage, canvas: &mut GraphCanvas) -> Result<()> {
        let emitter = self
            .emitters
            .get(emitter)
            .ok_or_else(|| invalid("unknown emitter"))?;
        let Some(source) = emitter
            .stages
            .iter()
            .find(|entry| entry.stage == stage)
            .map(|entry| &entry.canvas)
        else {
            canvas.load(canvas.catalogue().clone());
            return Ok(());
        };
        let mut loaded = canvas.clone();
        let name = load_graph_canvas_interchange(source, &mut loaded, "cyvfxcanvas", "emitter")?;
        if name != emitter.name {
            return Err(invalid("stage canvas belongs to a different emitter"));
        }
        *canvas = loaded;
        Ok(())
    }

    /// Versioned authoring payload; the engine still owns canonical `.cygraph` output.
    pub fn encode(&self) -> Result<Vec<u8>> {
        self.validate()?;
        let mut out = Writer::new();
        out.u32(VERSION);
        out.text(&self.name);
        out.u32(count(self.emitters.len())?);
        for emitter in &self.emitters {
            out.text(&emitter.name);
            out.u8(u8::from(emitter.path == SimulationPath::CpuRequired));
            out.text(&emitter.renderer);
            out.u32(count(emitter.stages.len())?);
            for stage in &emitter.stages {
                out.u8(stage.stage.code());
                out.text(&stage.canvas);
            }
            write_names(&mut out, &emitter.modules)?;
            write_names(&mut out, &emitter.interfaces)?;
        }
        out.u32(count(self.parameters.len())?);
        for parameter in &self.parameters {
            out.text(&parameter.name);
            out.text(&parameter.kind);
            for value in parameter.value {
                out.u32(value.to_bits());
            }
            out.u8(u8::from(parameter.exposed));
        }
        Ok(out.finish())
    }

    /// Text envelope used by project save commands and source control.
    pub fn encode_text(&self) -> Result<String> {
        use std::fmt::Write as _;

        let bytes = self.encode()?;
        let mut source = String::with_capacity(11 + bytes.len() * 2);
        source.push_str("cyvfxdoc 1\n");
        for byte in bytes {
            let _ = write!(source, "{byte:02x}");
        }
        Ok(source)
    }

    /// Reopen a project document without trusting its envelope or payload.
    pub fn decode_text(source: &str) -> Result<Self> {
        let payload = source
            .strip_prefix("cyvfxdoc 1\n")
            .ok_or_else(|| invalid("unsupported VFX document text version"))?;
        if payload.is_empty() || payload.len() % 2 != 0 {
            return Err(invalid("invalid hexadecimal VFX document payload"));
        }
        let mut bytes = Vec::with_capacity(payload.len() / 2);
        for pair in payload.as_bytes().chunks_exact(2) {
            let digits = std::str::from_utf8(pair)
                .map_err(|_| invalid("invalid hexadecimal VFX document payload"))?;
            bytes.push(
                u8::from_str_radix(digits, 16)
                    .map_err(|_| invalid("invalid hexadecimal VFX document payload"))?,
            );
        }
        Self::decode(&bytes)
    }

    /// Read a versioned authoring payload, rejecting malformed or unsupported entries.
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        let mut input = Reader::new(bytes);
        if input.u32()? != VERSION {
            return Err(invalid("unsupported VFX document version"));
        }
        let mut document = Self::new(input.text()?)?;
        for _ in 0..read_count(&mut input)? {
            let name = input.text()?;
            let path = match input.u8()? {
                0 => SimulationPath::GpuPreferred,
                1 => SimulationPath::CpuRequired,
                _ => return Err(invalid("unknown simulation path")),
            };
            let renderer = input.text()?;
            let mut stages = Vec::new();
            for _ in 0..read_count(&mut input)? {
                stages.push(StageGraph {
                    stage: Stage::from_code(input.u8()?)?,
                    canvas: input.text()?,
                });
            }
            document.emitters.push(Emitter {
                name,
                path,
                renderer,
                stages,
                modules: read_names(&mut input)?,
                interfaces: read_names(&mut input)?,
            });
        }
        for _ in 0..read_count(&mut input)? {
            let name = input.text()?;
            let kind = input.text()?;
            let mut value = [0.0; 4];
            for component in &mut value {
                *component = f32::from_bits(input.u32()?);
            }
            let exposed = match input.u8()? {
                0 => false,
                1 => true,
                _ => return Err(invalid("invalid parameter exposure")),
            };
            document.parameters.push(Parameter {
                name,
                kind,
                value,
                exposed,
            });
        }
        if input.remaining() != 0 {
            return Err(invalid("trailing VFX document data"));
        }
        document.validate()?;
        Ok(document)
    }

    fn validate(&self) -> Result<()> {
        identifier(&self.name)?;
        count(self.emitters.len())?;
        count(self.parameters.len())?;
        for emitter in &self.emitters {
            identifier(&emitter.name)?;
            identifier(&emitter.renderer)?;
            count(emitter.stages.len())?;
            for (index, stage) in emitter.stages.iter().enumerate() {
                if emitter.stages[..index]
                    .iter()
                    .any(|prior| prior.stage == stage.stage)
                {
                    return Err(invalid("duplicate emitter stage"));
                }
            }
            for name in emitter.modules.iter().chain(&emitter.interfaces) {
                identifier(name)?;
            }
        }
        for parameter in &self.parameters {
            identifier(&parameter.name)?;
            if !matches!(
                parameter.kind.as_str(),
                "float" | "vec2" | "vec3" | "vec4" | "int" | "bool"
            ) || parameter.value.iter().any(|value| !value.is_finite())
            {
                return Err(invalid("invalid typed VFX parameter"));
            }
        }
        Ok(())
    }
}

fn invalid(reason: &str) -> Problem {
    Problem::new("read or write a VFX document", reason)
}

fn identifier(value: &str) -> Result<()> {
    if value.is_empty()
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
    {
        return Err(invalid("expected an ASCII identifier"));
    }
    Ok(())
}

fn count(length: usize) -> Result<u32> {
    u32::try_from(length)
        .ok()
        .filter(|count| *count <= MAX_ITEMS)
        .ok_or_else(|| invalid("too many VFX document entries"))
}

fn read_count(input: &mut Reader<'_>) -> Result<u32> {
    let value = input.u32()?;
    if value > MAX_ITEMS {
        return Err(invalid("too many VFX document entries"));
    }
    Ok(value)
}

fn write_names(out: &mut Writer, names: &[String]) -> Result<()> {
    out.u32(count(names.len())?);
    for name in names {
        out.text(name);
    }
    Ok(())
}

fn read_names(input: &mut Reader<'_>) -> Result<Vec<String>> {
    let mut names = Vec::new();
    for _ in 0..read_count(input)? {
        names.push(input.text()?);
    }
    Ok(names)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::specialised::graph::{Catalogue, Layout, NodeType};

    fn canvas() -> GraphCanvas {
        let mut canvas = GraphCanvas::new(1);
        canvas.load(Catalogue::new(vec![NodeType::new("vfx.constant", Vec::new())]).unwrap());
        canvas
    }

    #[test]
    fn stage_switch_restores_each_canvas_from_the_same_catalogue() {
        let mut document = VfxDocument::new("sparks").unwrap();
        document.emitters.push(Emitter {
            name: "smoke".into(),
            path: SimulationPath::GpuPreferred,
            renderer: "Sprite".into(),
            stages: Vec::new(),
            modules: Vec::new(),
            interfaces: Vec::new(),
        });
        let mut canvas = canvas();
        canvas
            .add("vfx.constant", Layout { x: 12.0, y: 34.0 })
            .unwrap();
        document.capture_stage(0, Stage::Spawn, &canvas).unwrap();
        document.open_stage(0, Stage::Update, &mut canvas).unwrap();
        assert_eq!(canvas.nodes().count(), 0);
        canvas
            .add("vfx.constant", Layout { x: 99.0, y: 101.0 })
            .unwrap();
        document.capture_stage(0, Stage::Update, &canvas).unwrap();

        let reopened = VfxDocument::decode(&document.encode().unwrap()).unwrap();
        reopened.open_stage(0, Stage::Spawn, &mut canvas).unwrap();
        let node = canvas.nodes().next().unwrap();
        assert_eq!(
            canvas.layout_of(node.key),
            Some(Layout { x: 12.0, y: 34.0 })
        );
        reopened.open_stage(0, Stage::Update, &mut canvas).unwrap();
        let node = canvas.nodes().next().unwrap();
        assert_eq!(
            canvas.layout_of(node.key),
            Some(Layout { x: 99.0, y: 101.0 })
        );
    }

    #[test]
    fn cpu_and_gpu_emitters_round_trip_with_distinct_stage_graphs() {
        let mut document = VfxDocument::new("sparks").unwrap();
        for (name, path) in [
            ("cpu", SimulationPath::CpuRequired),
            ("gpu", SimulationPath::GpuPreferred),
        ] {
            document.emitters.push(Emitter {
                name: name.into(),
                path,
                renderer: "Sprite".into(),
                stages: Vec::new(),
                modules: vec!["shared_drag".into()],
                interfaces: vec!["scene_depth".into()],
            });
        }
        document
            .set_stage(0, Stage::Spawn, "cyvfxcanvas 1\nemitter cpu\n".into())
            .unwrap();
        document
            .set_stage(1, Stage::Update, "cyvfxcanvas 1\nemitter gpu\n".into())
            .unwrap();
        document.parameters.push(Parameter {
            name: "speed".into(),
            kind: "float".into(),
            value: [2.0, 0.0, 0.0, 0.0],
            exposed: true,
        });
        assert_eq!(
            VfxDocument::decode(&document.encode().unwrap()).unwrap(),
            document
        );
        assert_eq!(
            VfxDocument::decode_text(&document.encode_text().unwrap()).unwrap(),
            document
        );
        cy_editor_services::vfx_document::validate_source(&document.encode_text().unwrap())
            .unwrap();
    }

    #[test]
    fn duplicate_stage_and_trailing_data_are_rejected() {
        let mut document = VfxDocument::new("sparks").unwrap();
        document.emitters.push(Emitter {
            name: "smoke".into(),
            path: SimulationPath::GpuPreferred,
            renderer: "Sprite".into(),
            stages: vec![
                StageGraph {
                    stage: Stage::Spawn,
                    canvas: String::new(),
                },
                StageGraph {
                    stage: Stage::Spawn,
                    canvas: String::new(),
                },
            ],
            modules: Vec::new(),
            interfaces: Vec::new(),
        });
        assert!(document.encode().is_err());
        document.emitters[0].stages.pop();
        let mut bytes = document.encode().unwrap();
        bytes.push(0);
        assert!(VfxDocument::decode(&bytes).is_err());
    }
}
