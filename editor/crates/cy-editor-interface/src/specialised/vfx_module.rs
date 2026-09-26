//! Editable source for a reusable VFX stage graph. Cooking and dependency resolution belong to
//! the engine; this model only preserves the authored module and its typed interface.

use std::fmt::Write as _;

use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::problem::{Problem, Result};

use super::graph::GraphCanvas;
use super::material::{graph_canvas_interchange, load_graph_canvas_interchange};
use super::vfx::Stage;

const VERSION: u32 = 1;
const MAX_ITEMS: usize = 4096;
const ENVELOPE: &str = "cyvfxmodule 1\n";

/// One typed attribute a module reads from its host emitter.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ModuleInput {
    /// Attribute name used by `vfx.attribute` nodes in the graph.
    pub name: String,
    /// Engine VFX numeric type.
    pub kind: String,
}

/// One separately saved stage graph and its declared host inputs.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VfxModule {
    /// Stable module identifier, resolved by the project asset layer.
    pub name: String,
    /// Stage in which this graph can run.
    pub stage: Stage,
    /// Host attributes the graph may read.
    pub inputs: Vec<ModuleInput>,
    /// Other module identifiers this module depends on.
    pub dependencies: Vec<String>,
    /// Versioned shared-canvas source, not a cooked graph.
    pub canvas: String,
}

impl VfxModule {
    /// Start a module with an empty graph for one engine stage.
    pub fn new(name: impl Into<String>, stage: Stage) -> Result<Self> {
        let name = name.into();
        identifier(&name)?;
        Ok(Self {
            canvas: format!("cyvfxcanvas 1\nmodule {name}\n"),
            name,
            stage,
            inputs: Vec::new(),
            dependencies: Vec::new(),
        })
    }

    /// Capture the shared canvas without changing the graph being edited.
    pub fn capture(&mut self, canvas: &GraphCanvas) -> Result<()> {
        self.canvas = graph_canvas_interchange(&self.name, canvas, "cyvfxcanvas", "module")?;
        Ok(())
    }

    /// Open the saved graph on the shared canvas, keeping the previous canvas on refusal.
    pub fn open(&self, canvas: &mut GraphCanvas) -> Result<()> {
        let mut loaded = canvas.clone();
        let name =
            load_graph_canvas_interchange(&self.canvas, &mut loaded, "cyvfxcanvas", "module")?;
        if name != self.name {
            return Err(invalid("module canvas belongs to another module"));
        }
        *canvas = loaded;
        Ok(())
    }

    /// Encode a versioned authoring payload for project save commands.
    pub fn encode(&self) -> Result<Vec<u8>> {
        self.validate()?;
        let mut out = Writer::new();
        out.u32(VERSION);
        out.text(&self.name);
        out.u8(self.stage as u8);
        out.u32(count(self.inputs.len())?);
        for input in &self.inputs {
            out.text(&input.name);
            out.text(&input.kind);
        }
        out.u32(count(self.dependencies.len())?);
        for dependency in &self.dependencies {
            out.text(dependency);
        }
        out.text(&self.canvas);
        Ok(out.finish())
    }

    /// Encode source-control-friendly project text.
    pub fn encode_text(&self) -> Result<String> {
        let bytes = self.encode()?;
        let mut source = String::with_capacity(ENVELOPE.len() + bytes.len() * 2);
        source.push_str(ENVELOPE);
        for byte in bytes {
            let _ = write!(source, "{byte:02x}");
        }
        Ok(source)
    }

    /// Reopen a saved module, rejecting malformed envelopes and payloads.
    pub fn decode_text(source: &str) -> Result<Self> {
        let payload = source
            .strip_prefix(ENVELOPE)
            .ok_or_else(|| invalid("unsupported VFX module text version"))?;
        if payload.is_empty() || payload.len() % 2 != 0 {
            return Err(invalid("invalid hexadecimal VFX module payload"));
        }
        let mut bytes = Vec::with_capacity(payload.len() / 2);
        for pair in payload.as_bytes().chunks_exact(2) {
            let digits = std::str::from_utf8(pair)
                .map_err(|_| invalid("invalid hexadecimal VFX module payload"))?;
            bytes.push(
                u8::from_str_radix(digits, 16)
                    .map_err(|_| invalid("invalid hexadecimal VFX module payload"))?,
            );
        }
        Self::decode(&bytes)
    }

    /// Decode the versioned module payload.
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        let mut input = Reader::new(bytes);
        if input.u32()? != VERSION {
            return Err(invalid("unsupported VFX module version"));
        }
        let name = input.text()?;
        let stage = *Stage::ALL
            .get(usize::from(input.u8()?))
            .ok_or_else(|| invalid("unknown VFX module stage"))?;
        let mut module = Self::new(name, stage)?;
        for _ in 0..read_count(&mut input)? {
            module.inputs.push(ModuleInput {
                name: input.text()?,
                kind: input.text()?,
            });
        }
        for _ in 0..read_count(&mut input)? {
            module.dependencies.push(input.text()?);
        }
        module.canvas = input.text()?;
        if input.remaining() != 0 {
            return Err(invalid("trailing VFX module data"));
        }
        module.validate()?;
        Ok(module)
    }

    fn validate(&self) -> Result<()> {
        identifier(&self.name)?;
        count(self.inputs.len())?;
        count(self.dependencies.len())?;
        let header = format!("cyvfxcanvas 1\nmodule {}\n", self.name);
        if !self.canvas.starts_with(&header) {
            return Err(invalid("module canvas belongs to another module"));
        }
        for (index, input) in self.inputs.iter().enumerate() {
            identifier(&input.name)?;
            if !matches!(
                input.kind.as_str(),
                "float" | "vec2" | "vec3" | "vec4" | "int" | "bool"
            ) || self.inputs[..index]
                .iter()
                .any(|prior| prior.name == input.name)
            {
                return Err(invalid("invalid or duplicate VFX module input"));
            }
        }
        for (index, dependency) in self.dependencies.iter().enumerate() {
            identifier(dependency)?;
            if dependency == &self.name || self.dependencies[..index].contains(dependency) {
                return Err(invalid("self or duplicate VFX module dependency"));
            }
        }
        Ok(())
    }
}

fn invalid(reason: &str) -> Problem {
    Problem::new("read a VFX module", reason)
}

fn identifier(value: &str) -> Result<()> {
    if value.is_empty()
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
    {
        return Err(invalid("invalid VFX module identifier"));
    }
    Ok(())
}

fn count(value: usize) -> Result<u32> {
    if value > MAX_ITEMS {
        return Err(invalid("too many VFX module entries"));
    }
    u32::try_from(value).map_err(|_| invalid("too many VFX module entries"))
}

fn read_count(input: &mut Reader<'_>) -> Result<usize> {
    let value =
        usize::try_from(input.u32()?).map_err(|_| invalid("too many VFX module entries"))?;
    count(value)?;
    Ok(value)
}

#[cfg(test)]
mod tests {
    use super::super::graph::{Catalogue, Layout, NodeType};
    use super::*;

    #[test]
    fn module_graph_and_typed_inputs_round_trip() {
        let mut canvas = GraphCanvas::new(1);
        canvas.load(Catalogue::new(vec![NodeType::new("vfx.constant", Vec::new())]).unwrap());
        let node = canvas
            .add("vfx.constant", Layout { x: 12.0, y: 34.0 })
            .unwrap();
        let mut module = VfxModule::new("shared_drag", Stage::Update).unwrap();
        module.inputs.push(ModuleInput {
            name: "velocity".into(),
            kind: "vec3".into(),
        });
        module.dependencies.push("shared_noise".into());
        module.capture(&canvas).unwrap();

        let reopened = VfxModule::decode_text(&module.encode_text().unwrap()).unwrap();
        assert_eq!(reopened, module);
        canvas.remove(node).unwrap();
        reopened.open(&mut canvas).unwrap();
        assert!(canvas.node(node).is_some());
        assert_eq!(canvas.layout_of(node), Some(Layout { x: 12.0, y: 34.0 }));
    }

    #[test]
    fn module_rejects_duplicate_inputs_and_self_dependency() {
        let mut module = VfxModule::new("shared_drag", Stage::Update).unwrap();
        module.inputs = vec![
            ModuleInput {
                name: "velocity".into(),
                kind: "vec3".into(),
            },
            ModuleInput {
                name: "velocity".into(),
                kind: "float".into(),
            },
        ];
        assert!(module.encode_text().is_err());
        module.inputs.pop();
        module.dependencies.push("shared_drag".into());
        assert!(module.encode_text().is_err());
    }

    #[test]
    fn committed_module_fixture_reopens_with_its_typed_interface() {
        let source = include_str!(
            "../../../../../samples/05b-editor-window/project/effects/shared_drag.cyvfxmodule"
        );
        let module = VfxModule::decode_text(source).unwrap();
        assert_eq!(module.name, "shared_drag");
        assert_eq!(module.stage, Stage::Update);
        assert_eq!(module.inputs[0].name, "velocity");
        assert_eq!(module.inputs[0].kind, "vec3");
        assert_eq!(module.encode_text().unwrap(), source);
    }
}
