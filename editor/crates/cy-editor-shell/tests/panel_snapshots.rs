// SPDX-License-Identifier: MIT
//! Offscreen PNGs of one dock panel, drawn by the shipped panel code and rendered by egui-wgpu.
//!
//! `editor:window?panel=<kind>` captures a panel from the live window, but only a panel that is the
//! front tab, and nothing selects a tab without pointer input. This renders the panel on its own
//! instead, through the same `Panels::ui` and the same egui-wgpu renderer the window presents with,
//! into an offscreen texture on whichever adapter wgpu finds. No window and no input.
//!
//! Ignored by default because it needs a GPU adapter (a software one works). To write the images:
//!
//! ```text
//! CY_PANEL_SNAPSHOTS=docs/design/images cargo test -p cy-editor-shell --test panel_snapshots \
//!     -- --ignored
//! ```

use std::future::Future;
use std::path::PathBuf;
use std::pin::pin;
use std::task::{Context, Poll, Waker};

use cy_editor_commands::{Arguments, Registry, Scope};
use cy_editor_core::Actor;
use cy_editor_interface::SpecialisedEditors;
use cy_editor_interface::panels::{PanelKey, PanelTitles};
use cy_editor_interface::shell::{Shell, panel_title};
use cy_editor_interface::thumbnails::Thumbnails;
use cy_editor_services::Editor;
use cy_editor_shell::panels::{Inputs, Intent, Panels};
use cy_editor_shell::viewport_link::ViewportLink;
use cy_editor_viewmodels::{
    AssetBrowserViewModel, DiffViewModel, HierarchyViewModel, HistoryViewModel, MergeViewModel,
    SettingsViewModel, SourceControlViewModel, SourceWorkspaceViewModel,
};
use egui_dock::TabViewer;

const SIZE: [u32; 2] = [960, 540];

/// One device at a time: the tests in this file run on parallel threads.
static ONE_DEVICE: std::sync::Mutex<()> = std::sync::Mutex::new(());

fn block_on<F: Future>(future: F) -> F::Output {
    let mut future = pin!(future);
    let mut context = Context::from_waker(Waker::noop());
    loop {
        if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
            return value;
        }
        std::thread::yield_now();
    }
}

/// Everything `Panels` borrows, owned for the length of a test.
struct Desk {
    editor: Editor,
    registry: Registry,
    scope: Scope,
    shell: Shell,
    specialised: SpecialisedEditors,
    hierarchy: HierarchyViewModel,
    history: HistoryViewModel,
    settings: SettingsViewModel,
    source_control: SourceControlViewModel,
    asset_browser: AssetBrowserViewModel,
    source_workspace: SourceWorkspaceViewModel,
    diff: DiffViewModel,
    merge_view: MergeViewModel,
    thumbnails: Thumbnails,
    titles: PanelTitles,
    link: ViewportLink,
    inputs: Inputs,
}

impl Desk {
    fn new() -> Self {
        let mut registry = Registry::new();
        cy_editor_services::builtin::register(&mut registry).expect("built-in commands");
        let shell = Shell::new(&registry).expect("shell");
        let mut titles = PanelTitles::new();
        for panel in shell.workspaces.current().panels() {
            titles.define(panel.as_str(), panel_title(&panel));
        }
        Self {
            editor: Editor::new(Actor::human("designer")),
            registry,
            scope: Scope::unrestricted(),
            shell,
            specialised: SpecialisedEditors::new().expect("specialised editors"),
            hierarchy: HierarchyViewModel::new(),
            history: HistoryViewModel::new(),
            settings: SettingsViewModel::new(),
            source_control: SourceControlViewModel::new(),
            asset_browser: AssetBrowserViewModel::new(),
            source_workspace: SourceWorkspaceViewModel::new(),
            diff: DiffViewModel::new(),
            merge_view: MergeViewModel::new(),
            thumbnails: Thumbnails::new(8),
            titles,
            link: ViewportLink::idle(),
            inputs: Inputs::default(),
        }
    }

    /// One frame of `panel`, answering what egui produced.
    fn frame(&mut self, ctx: &egui::Context, panel: &str) -> egui::FullOutput {
        let style = cy_editor_shell::theme::style(self.shell.theme, self.shell.metrics());
        ctx.all_styles_mut(|existing| *existing = style.clone());
        self.history.refresh(&self.editor);
        let raw = egui::RawInput {
            screen_rect: Some(egui::Rect::from_min_size(
                egui::Pos2::ZERO,
                egui::vec2(points(SIZE[0]), points(SIZE[1])),
            )),
            ..Default::default()
        };
        let Self {
            editor,
            registry,
            scope,
            shell,
            specialised,
            hierarchy,
            history,
            settings,
            source_control,
            asset_browser,
            source_workspace,
            diff,
            merge_view,
            thumbnails,
            titles,
            link,
            inputs,
        } = self;
        let mut intents: Vec<Intent> = Vec::new();
        ctx.run_ui(raw, |ui| {
            egui::CentralPanel::default().show(ui, |ui| {
                let mut panels = Panels {
                    editor,
                    registry,
                    scope,
                    shell,
                    specialised,
                    saved_vfx_document_reference: None,
                    saved_vfx_module_reference: None,
                    hierarchy,
                    history,
                    settings,
                    source_control,
                    asset_browser,
                    source_workspace,
                    agent: None,
                    diff,
                    merge: merge_view,
                    thumbnails,
                    link,
                    titles,
                    inputs,
                    intents: &mut intents,
                    tab_rects: Vec::new(),
                    panel_rects: Vec::new(),
                };
                let mut key = PanelKey::new(panel).expect("a built-in panel");
                panels.ui(ui, &mut key);
            });
        })
    }
}

#[expect(
    clippy::cast_precision_loss,
    reason = "a snapshot is a few hundred pixels across"
)]
fn points(pixels: u32) -> f32 {
    pixels as f32
}

/// A device and the egui renderer, kept across frames so texture deltas accumulate as in a window.
struct Gpu {
    device: wgpu::Device,
    queue: wgpu::Queue,
    renderer: egui_wgpu::Renderer,
}

const FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba8UnormSrgb;

impl Gpu {
    fn new() -> Self {
        let instance =
            wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
        let adapter = block_on(instance.request_adapter(&wgpu::RequestAdapterOptions::default()))
            .expect("a wgpu adapter; a software one is enough");
        let (device, queue) = block_on(adapter.request_device(&wgpu::DeviceDescriptor::default()))
            .expect("a wgpu device");
        let renderer =
            egui_wgpu::Renderer::new(&device, FORMAT, egui_wgpu::RendererOptions::default());
        Self {
            device,
            queue,
            renderer,
        }
    }

    fn textures(&mut self, delta: &egui::TexturesDelta) {
        for (id, images) in &delta.set {
            for image in images {
                self.renderer
                    .update_texture(&self.device, &self.queue, *id, image);
            }
        }
    }

    /// Render `shapes` offscreen and answer tightly packed RGBA8 rows.
    fn render(&mut self, ctx: &egui::Context, shapes: Vec<egui::epaint::ClippedShape>) -> Vec<u8> {
        let extent = wgpu::Extent3d {
            width: SIZE[0],
            height: SIZE[1],
            depth_or_array_layers: 1,
        };
        let target = self.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("panel snapshot"),
            size: extent,
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let view = target.create_view(&wgpu::TextureViewDescriptor::default());
        let screen = egui_wgpu::ScreenDescriptor {
            size_in_pixels: SIZE,
            pixels_per_point: 1.0,
        };
        let jobs = ctx.tessellate(shapes, 1.0);
        let mut encoder = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
        let mut buffers =
            self.renderer
                .update_buffers(&self.device, &self.queue, &mut encoder, &jobs, &screen);
        {
            let mut pass = encoder
                .begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some("panel snapshot"),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: &view,
                        depth_slice: None,
                        resolve_target: None,
                        ops: wgpu::Operations {
                            load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                            store: wgpu::StoreOp::Store,
                        },
                    })],
                    ..Default::default()
                })
                .forget_lifetime();
            self.renderer.render(&mut pass, &jobs, &screen);
        }
        let row = (SIZE[0] * 4).next_multiple_of(wgpu::COPY_BYTES_PER_ROW_ALIGNMENT);
        let readback = self.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("panel snapshot readback"),
            size: u64::from(row * SIZE[1]),
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        encoder.copy_texture_to_buffer(
            target.as_image_copy(),
            wgpu::TexelCopyBufferInfo {
                buffer: &readback,
                layout: wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(row),
                    rows_per_image: Some(SIZE[1]),
                },
            },
            extent,
        );
        buffers.push(encoder.finish());
        self.queue.submit(buffers);
        readback.slice(..).map_async(wgpu::MapMode::Read, |mapped| {
            mapped.expect("the readback maps");
        });
        self.device
            .poll(wgpu::PollType::wait_indefinitely())
            .expect("the GPU finishes");
        let mapped = readback
            .slice(..)
            .get_mapped_range()
            .expect("the readback is mapped");
        let width = usize::try_from(SIZE[0] * 4).expect("fits");
        mapped
            .chunks(usize::try_from(row).expect("fits"))
            .flat_map(|line| line[..width].iter().copied())
            .collect()
    }
}

fn snapshot(desk: &mut Desk, panel: &str, name: &str) {
    let Some(directory) = std::env::var_os("CY_PANEL_SNAPSHOTS").map(PathBuf::from) else {
        eprintln!("CY_PANEL_SNAPSHOTS is not set; not writing {name}");
        return;
    };
    let _device = ONE_DEVICE
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let ctx = egui::Context::default();
    let mut gpu = Gpu::new();
    // Two frames: the first lays out and measures, the second draws what a settled window shows.
    let mut first = desk.frame(&ctx, panel);
    gpu.textures(&first.textures_delta);
    first.textures_delta.clear();
    let mut output = desk.frame(&ctx, panel);
    gpu.textures(&output.textures_delta);
    output.textures_delta.clear();
    let pixels = gpu.render(&ctx, std::mem::take(&mut output.shapes));
    let path = directory.join(name);
    image::save_buffer(&path, &pixels, SIZE[0], SIZE[1], image::ColorType::Rgba8)
        .expect("the snapshot is written");
    eprintln!("wrote {}", path.display());
}

/// A world with one terrain root and one material layer, both through registered commands.
fn terrain_desk() -> Desk {
    let mut desk = Desk::new();
    desk.editor
        .open_document("worlds/terrain.cyworld")
        .expect("a world opens");
    let created = desk
        .registry
        .invoke(
            "terrain.create",
            &desk.scope,
            &mut desk.editor,
            &Arguments::new(),
        )
        .expect("terrain.create");
    let terrain = created.values["terrain"]
        .as_text()
        .expect("an id")
        .to_owned();
    desk.registry
        .invoke(
            "terrain.layer.add",
            &desk.scope,
            &mut desk.editor,
            &Arguments::new()
                .with("terrain", cy_editor_core::value::Value::Text(terrain))
                .with("name", cy_editor_core::value::Value::Text("Grass".into()))
                .with(
                    "material",
                    cy_editor_core::value::Value::Text("materials/grass.cymat".into()),
                ),
        )
        .expect("terrain.layer.add");
    desk
}

#[test]
#[ignore = "needs a GPU adapter; writes PNGs when CY_PANEL_SNAPSHOTS names a directory"]
fn terrain_panel_snapshots() {
    let mut desk = terrain_desk();
    snapshot(&mut desk, "editor-terrain", "editor-terrain-panel.png");
    desk.inputs.terrain_problem = Some("Paint requires a material layer.".into());
    snapshot(
        &mut desk,
        "editor-terrain",
        "editor-terrain-diagnostics.png",
    );
}

/// The engine fixtures the terrain snapshots are drawn from. The requests are what the editor sends
/// for [`terrain_tools_desk`]'s strokes; the reply is what `cy::editor-backend` answered for them,
/// written by `integration.editor_backend_terrain` and checked by it on every run.
const TERRAIN_FIXTURES: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/fixtures");

/// A request with its identities replaced by stable ones: the terrain is 0x29 and each modifier is
/// its position in the stack, plus one. Identities are random per document; the engine uses them
/// only to tell one modifier from another between evaluations.
fn stable_identities(request: &[u8]) -> Vec<u8> {
    let mut out = request.to_vec();
    out[4..20].copy_from_slice(&0x29_u128.to_le_bytes());
    let mut cursor = 4 + 16 + 12;
    let count = u32::from_le_bytes(out[cursor..cursor + 4].try_into().expect("four bytes"));
    cursor += 4;
    for index in 0..count {
        out[cursor..cursor + 16].copy_from_slice(&u128::from(index + 1).to_le_bytes());
        cursor += 16 + 3 + 12;
        let dabs = u32::from_le_bytes(out[cursor..cursor + 4].try_into().expect("four bytes"));
        cursor += 4 + 12 * usize::try_from(dabs).expect("fits");
    }
    assert_eq!(cursor, out.len(), "the request is read to its end");
    out
}

fn brush(
    desk: &mut Desk,
    terrain: &str,
    tool: &str,
    points: &str,
    extra: &[(&str, cy_editor_core::value::Value)],
) {
    let mut arguments = Arguments::new()
        .with(
            "terrain",
            cy_editor_core::value::Value::Text(terrain.into()),
        )
        .with("tool", cy_editor_core::value::Value::Text(tool.into()))
        .with("points", cy_editor_core::value::Value::Text(points.into()));
    for (name, value) in extra {
        arguments = arguments.with(*name, value.clone());
    }
    desk.registry
        .invoke(
            "terrain.brush.apply",
            &desk.scope,
            &mut desk.editor,
            &arguments,
        )
        .unwrap_or_else(|problem| panic!("{tool}: {problem}"));
}

/// Every tool but the hole, the last stroke [`terrain_tools_desk`] makes.
fn paint_and_sculpt(
    desk: &mut Desk,
    terrain: &str,
    grass: cy_editor_core::value::Value,
    rock: cy_editor_core::value::Value,
) {
    use cy_editor_core::value::Value;

    let firm = [
        ("radius", Value::Float(26.0)),
        ("strength", Value::Float(1.0)),
        ("falloff", Value::Float(0.9)),
    ];
    brush(
        desk,
        terrain,
        "raise",
        "0.35 0.4; 0.45 0.42; 0.55 0.45",
        &firm,
    );
    brush(desk, terrain, "raise", "0.3 0.45", &firm);
    brush(
        desk,
        terrain,
        "lower",
        "0.72 0.7; 0.8 0.62",
        &[
            ("radius", Value::Float(14.0)),
            ("strength", Value::Float(1.0)),
        ],
    );
    brush(
        desk,
        terrain,
        "smooth",
        "0.35 0.4; 0.5 0.44",
        &[
            ("radius", Value::Float(12.0)),
            ("strength", Value::Float(1.0)),
        ],
    );
    brush(
        desk,
        terrain,
        "paint",
        "0.2 0.75; 0.35 0.8; 0.5 0.82",
        &[
            ("layer", grass),
            ("radius", Value::Float(12.0)),
            ("strength", Value::Float(0.9)),
        ],
    );
    brush(
        desk,
        terrain,
        "paint",
        "0.42 0.38; 0.5 0.4",
        &[
            ("layer", rock),
            ("radius", Value::Float(10.0)),
            ("strength", Value::Float(0.8)),
        ],
    );
    brush(
        desk,
        terrain,
        "flatten",
        "0.18 0.2; 0.26 0.2",
        &[
            ("radius", Value::Float(9.0)),
            ("strength", Value::Float(1.0)),
            ("falloff", Value::Float(0.3)),
        ],
    );
}

/// A terrain sculpted, painted and cut through `terrain.brush.apply`, and the engine requests for
/// the stack before its last stroke and after it.
fn terrain_tools_desk() -> (Desk, Vec<u8>, Vec<u8>) {
    use cy_editor_core::value::Value;

    let mut desk = terrain_desk();
    let terrain = desk.editor.edited_terrain().expect("a terrain").to_string();
    let rock = desk
        .registry
        .invoke(
            "terrain.layer.add",
            &desk.scope,
            &mut desk.editor,
            &Arguments::new()
                .with("terrain", Value::Text(terrain.clone()))
                .with("name", Value::Text("Rock".into()))
                .with("material", Value::Text("materials/rock.cymat".into())),
        )
        .expect("a second layer")
        .values["layer"]
        .clone();
    let stack = |desk: &Desk| {
        cy_editor_services::terrain::TerrainStack::read(
            desk.editor
                .documents
                .get(desk.editor.workspace.active().unwrap())
                .unwrap(),
            desk.editor.edited_terrain().unwrap(),
        )
        .unwrap()
    };
    let grass = Value::Text(stack(&desk).layers[0].id.to_string());
    paint_and_sculpt(&mut desk, &terrain, grass, rock);
    let before = stable_identities(&desk.editor.edited_terrain_request().unwrap().1.unwrap());
    brush(
        &mut desk,
        &terrain,
        "hole",
        "0.62 0.3; 0.66 0.32",
        &[("radius", Value::Float(5.0))],
    );
    let after = stable_identities(&desk.editor.edited_terrain_request().unwrap().1.unwrap());
    (desk, before, after)
}

/// The committed engine requests are what the editor sends for the scripted strokes. With
/// `CY_TERRAIN_FIXTURE=write` this writes them instead, for `integration.editor_backend_terrain` to
/// answer.
#[test]
fn the_terrain_engine_fixture_requests_are_what_the_editor_sends() {
    let (_desk, before, after) = terrain_tools_desk();
    let directory = std::path::Path::new(TERRAIN_FIXTURES);
    if std::env::var("CY_TERRAIN_FIXTURE").as_deref() == Ok("write") {
        std::fs::create_dir_all(directory).expect("the fixture directory");
        std::fs::write(directory.join("terrain-tools-before.request"), &before).expect("written");
        std::fs::write(directory.join("terrain-tools.request"), &after).expect("written");
        return;
    }
    assert_eq!(
        std::fs::read(directory.join("terrain-tools-before.request")).expect("committed"),
        before
    );
    assert_eq!(
        std::fs::read(directory.join("terrain-tools.request")).expect("committed"),
        after
    );
}

/// Answer the editor's `terrain.evaluate` with the engine's committed reply, through the same
/// service request and event the hosted runtime exchanges.
fn install_engine_reply(desk: &mut Desk) {
    use cy_editor_protocol::{Message, ServiceEventKind, Session, read_frame, write_frame};

    let reply = std::fs::read(std::path::Path::new(TERRAIN_FIXTURES).join("terrain-tools.reply"))
        .expect("the engine reply is committed; see integration.editor_backend_terrain");
    let (editor_reader, mut runtime_writer) = std::io::pipe().expect("a pipe");
    let (mut runtime_reader, editor_writer) = std::io::pipe().expect("a pipe");
    desk.editor.runtime = cy_editor_services::runtime::RuntimeSession::over(Session::over(
        editor_reader,
        editor_writer,
    ));
    desk.editor.pump();
    let request = loop {
        let frame = read_frame(&mut runtime_reader)
            .expect("a frame")
            .expect("a frame");
        if let Message::ServiceRequest {
            request, operation, ..
        } = Message::decode(&frame).expect("a message")
            && operation == "terrain.evaluate"
        {
            break request;
        }
    };
    write_frame(
        &mut runtime_writer,
        &Message::ServiceEvent {
            request,
            kind: ServiceEventKind::Completed,
            schema_version: 1,
            payload: reply,
        }
        .encode(),
    )
    .expect("the reply is written");
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while desk.editor.terrain.evaluation().is_none() && std::time::Instant::now() < deadline {
        desk.editor.pump();
        std::thread::sleep(std::time::Duration::from_millis(1));
    }
    assert!(
        desk.editor.terrain.evaluation().is_some(),
        "the engine's reply arrives"
    );
    // Keep the pipes open for the snapshot frames: a closed runtime reads as a disconnect.
    std::mem::forget((runtime_reader, runtime_writer));
}

/// The finished terrain tools: the engine's sculpted, painted and holed surface in the brush field,
/// with the region the last stroke made stale for navigation outlined.
#[test]
#[ignore = "needs a GPU adapter; writes PNGs when CY_PANEL_SNAPSHOTS names a directory"]
fn terrain_tools_snapshots() {
    let (mut desk, _, _) = terrain_tools_desk();
    install_engine_reply(&mut desk);
    desk.inputs.terrain_tool = "hole".into();
    snapshot(&mut desk, "editor-terrain", "editor-terrain-tools.png");
    desk.inputs.terrain_tool = "paint".into();
    snapshot(&mut desk, "editor-terrain", "editor-terrain-paint.png");
}

fn catalogue(identity: u32, name: &str, pin_type: &str) -> Vec<u8> {
    let mut catalogue = cy_editor_core::codec::Writer::new();
    catalogue.u32(1);
    catalogue.u32(1);
    catalogue.u32(1);
    catalogue.u32(identity);
    catalogue.u32(1);
    catalogue.text(name);
    catalogue.u32(1);
    catalogue.u32(9);
    catalogue.u8(1);
    catalogue.text("out");
    catalogue.text(pin_type);
    catalogue.u32(0);
    catalogue.finish()
}

/// The two graph panels on the shared canvas, one node each: the pictures that show the canvas
/// extraction left them as they were.
#[test]
#[ignore = "needs a GPU adapter; writes PNGs when CY_PANEL_SNAPSHOTS names a directory"]
fn graph_panel_snapshots() {
    use cy_editor_interface::Domain;
    use cy_editor_interface::specialised::graph::Layout;
    use cy_editor_interface::specialised::vfx::{Emitter, SimulationPath, Stage, VfxDocument};

    let mut desk = Desk::new();
    desk.specialised
        .install_material_catalogue(&catalogue(42, "material.future", "value"))
        .expect("material catalogue");
    desk.specialised
        .open(Domain::Materials)
        .expect("materials open")
        .graph
        .expect("a graph domain")
        .add("material.future", Layout { x: 28.0, y: 34.0 })
        .expect("a node");
    snapshot(
        &mut desk,
        "editor-materials",
        "editor-material-graph-canvas.png",
    );

    let mut desk = Desk::new();
    desk.specialised
        .install_vfx_catalogue(&catalogue(1001, "vfx.constant", "float"))
        .expect("VFX catalogue");
    let mut document = VfxDocument::new("sparks").expect("a document");
    document.emitters.push(Emitter {
        name: "smoke".into(),
        path: SimulationPath::GpuPreferred,
        renderer: "Sprite".into(),
        stages: Vec::new(),
        modules: Vec::new(),
        interfaces: Vec::new(),
        capacity: 1024,
        attributes: Vec::new(),
    });
    desk.specialised
        .start_vfx_document(document)
        .expect("opens");
    desk.specialised
        .select_vfx_stage(0, Stage::Spawn)
        .expect("a stage");
    desk.specialised
        .open(Domain::VfxGraph)
        .expect("VFX opens")
        .graph
        .expect("a graph domain")
        .add("vfx.constant", Layout { x: 28.0, y: 34.0 })
        .expect("a node");
    snapshot(&mut desk, "editor-vfx-graph", "editor-vfx-graph-canvas.png");
}

/// A door hinged to a frame, both named, with the collider and joint layers asked for: what the
/// physics panel shows when a person selects the door.
fn physics_desk() -> Desk {
    use cy_editor_core::value::Value;

    let mut desk = Desk::new();
    desk.editor
        .open_document("worlds/door.cyworld")
        .expect("a world opens");
    let invoke = |desk: &mut Desk, command: &str, arguments: Arguments| {
        desk.registry
            .invoke(command, &desk.scope, &mut desk.editor, &arguments)
            .unwrap_or_else(|problem| panic!("{command}: {problem}"))
    };
    let mut nodes = Vec::new();
    for name in ["Door", "Frame"] {
        let created = invoke(&mut desk, "scene.create-entity", Arguments::new());
        let node = created.values["entity"]
            .as_text()
            .expect("an id")
            .to_owned();
        invoke(
            &mut desk,
            "scene.rename-entity",
            Arguments::new()
                .with("entity", Value::Text(node.clone()))
                .with("name", Value::Text(name.into())),
        );
        invoke(
            &mut desk,
            "scene.add-body",
            Arguments::new().with("entity", Value::Text(node.clone())),
        );
        nodes.push(node);
    }
    invoke(
        &mut desk,
        "physics.joint.add",
        Arguments::new()
            .with("entity", Value::Text(nodes[0].clone()))
            .with("kind", Value::Text("hinge".into()))
            .with("target", Value::Text(nodes[1].clone()))
            .with("anchor", Value::Vec3([-0.5, 0.0, 0.0]))
            .with("axis", Value::Vec3([0.0, 1.0, 0.0])),
    );
    for (field, value) in [
        ("limit_min", "-1.2"),
        ("limit_max", "1.2"),
        ("motor_max_force", "40"),
    ] {
        invoke(
            &mut desk,
            "physics.joint.set",
            Arguments::new()
                .with("entity", Value::Text(nodes[0].clone()))
                .with("field", Value::Text(field.into()))
                .with("value", Value::Text(value.into())),
        );
    }
    invoke(
        &mut desk,
        "edit.select",
        Arguments::new().with("entity", Value::Text(nodes[0].clone())),
    );
    for layer in ["colliders", "constraints"] {
        invoke(
            &mut desk,
            &format!("viewport.physics.{layer}"),
            Arguments::new().with("state", Value::Text("on".into())),
        );
    }
    desk
}

#[test]
#[ignore = "needs a GPU adapter; writes PNGs when CY_PANEL_SNAPSHOTS names a directory"]
fn physics_panel_snapshots() {
    let mut desk = physics_desk();
    snapshot(&mut desk, "physics", "editor-physics-joint.png");
    let mut empty = Desk::new();
    snapshot(&mut empty, "physics", "editor-physics-empty.png");
}
