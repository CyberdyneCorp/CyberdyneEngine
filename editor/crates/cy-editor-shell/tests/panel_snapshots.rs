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

/// Draw `panel` in a fresh context and write it as `name`; answers its pixels when it was written.
fn snapshot(desk: &mut Desk, panel: &str, name: &str) -> Option<Vec<u8>> {
    let Some(directory) = std::env::var_os("CY_PANEL_SNAPSHOTS").map(PathBuf::from) else {
        eprintln!("CY_PANEL_SNAPSHOTS is not set; not writing {name}");
        return None;
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
    Some(pixels)
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
    let tools = snapshot(&mut desk, "editor-terrain", "editor-terrain-tools.png");
    // Each snapshot draws in a context of its own, which does not know the texture the panel
    // uploaded into the last one; a window keeps one context and never drops its cache.
    desk.inputs.terrain_surface = None;
    desk.inputs.terrain_tool = "paint".into();
    let paint = snapshot(&mut desk, "editor-terrain", "editor-terrain-paint.png");
    for pixels in [tools, paint].into_iter().flatten() {
        assert!(
            lit_field_pixels(&pixels) > 100_000,
            "the brush field shows the engine's surface, not an empty field"
        );
    }
}

/// Pixels in the terrain brush field lighter than the sunken background: the engine's grey
/// surface covers most of the field, the grid lines alone a few thousand pixels.
fn lit_field_pixels(pixels: &[u8]) -> usize {
    let width = usize::try_from(SIZE[0]).expect("fits");
    pixels
        .chunks_exact(4)
        .enumerate()
        .filter(|(at, _)| (260..940).contains(&(at % width)) && (100..495).contains(&(at / width)))
        .filter(|(_, rgba)| rgba[..3].iter().map(|&c| u32::from(c)).sum::<u32>() > 300)
        .count()
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

/// The audio mixer over the engine's own fixtures: the canonical mixer, a cue, the engine's
/// vocabulary and the state it reported after previewing that cue through SFX. #29.
#[test]
#[ignore = "needs a GPU adapter; writes PNGs when CY_PANEL_SNAPSHOTS names a directory"]
fn audio_panel_snapshots() {
    use cy_editor_protocol::{Message, ServiceEventKind, Session, write_frame};

    let fixture = |name: &str| {
        std::fs::read(
            PathBuf::from(env!("CARGO_MANIFEST_DIR"))
                .join("../../../src/editor_backend/tests/data")
                .join(name),
        )
        .expect("the engine's audio fixture")
    };
    let project = std::env::temp_dir().join(format!("cy-audio-snapshot-{}", std::process::id()));
    std::fs::create_dir_all(project.join("audio/cues")).unwrap();
    std::fs::create_dir_all(project.join("game/audio")).unwrap();
    std::fs::write(
        project.join("game/audio/mixer.cymixer"),
        fixture("audio_mixer_v1.cymixer"),
    )
    .unwrap();
    std::fs::write(
        project.join("audio/cues/ping.cycue"),
        fixture("audio_cue_v1.cycue"),
    )
    .unwrap();
    std::fs::write(
        project.join("audio/cues/hum.cycue"),
        "cycue 1\nclip tone:220:0.5\nbus Music\nlooping 1\n",
    )
    .unwrap();

    let mut desk = Desk::new();
    desk.editor = Editor::new(Actor::human("sound-designer"))
        .with_project(cy_editor_services::ProjectService::new(&project));
    desk.editor.open_document("worlds/audio.cyworld").unwrap();
    desk.specialised.install_audio_vocabulary(
        cy_editor_services::audio::AudioVocabulary::decode(&fixture("audio_capabilities_v1.wire"))
            .unwrap(),
    );
    let (editor_reader, mut runtime_writer) = std::io::pipe().unwrap();
    let (_runtime_reader, editor_writer) = std::io::pipe().unwrap();
    desk.editor.runtime =
        cy_editor_services::RuntimeSession::over(Session::over(editor_reader, editor_writer));
    let request = desk
        .editor
        .backend
        .audio
        .request(&desk.editor.runtime, "audio.state.get", Vec::new())
        .unwrap()
        .unwrap();
    write_frame(
        &mut runtime_writer,
        &Message::ServiceEvent {
            request,
            kind: ServiceEventKind::Completed,
            schema_version: 1,
            payload: fixture("audio_state_v1.wire"),
        }
        .encode(),
    )
    .unwrap();
    let mut notifications = cy_editor_services::NotificationService::new();
    while desk.editor.backend.audio.pending() {
        for message in desk.editor.runtime.pump(&mut notifications) {
            let _ = desk.editor.backend.accept(&message);
        }
        std::thread::yield_now();
    }
    desk.inputs.audio.bus = Some("SFX".into());
    desk.inputs.audio.cue = Some("audio/cues/ping.cycue".into());
    snapshot(
        &mut desk,
        "editor-audio-buses-and-mixing",
        "editor-audio-mixer.png",
    );

    let created = desk
        .registry
        .invoke(
            "audio.source.create",
            &desk.scope,
            &mut desk.editor,
            &Arguments::new()
                .with(
                    "cue",
                    cy_editor_core::value::Value::Text("audio/cues/ping.cycue".into()),
                )
                .with("at", cy_editor_core::value::Value::Vec3([3.0, 0.0, 0.0])),
        )
        .expect("audio.source.create");
    assert!(created.values.contains_key("entity"));
    desk.inputs.audio.bus = None;
    snapshot(
        &mut desk,
        "editor-audio-buses-and-mixing",
        "editor-audio-source-range.png",
    );
    let _ = std::fs::remove_dir_all(&project);
}

/// The gameplay graph editor over the engine's own fixtures (#29, visual scripting): the
/// acceptance graph as the engine compiled it, the same graph with a misspelled function and the
/// engine's diagnostic on that node, and Play's state after the unit arrived.
#[test]
#[ignore = "needs a GPU adapter; writes PNGs when CY_PANEL_SNAPSHOTS names a directory"]
fn gameplay_graph_snapshots() {
    use cy_editor_protocol::{Message, ServiceEventKind, Session, write_frame};

    let fixture = |name: &str| {
        std::fs::read(
            PathBuf::from(env!("CARGO_MANIFEST_DIR"))
                .join("../../../src/editor_backend/tests/data")
                .join(name),
        )
        .expect("the engine's gameplay graph fixture")
    };
    let reference = "game/scripts/unit_command.cyscript";
    let source = String::from_utf8(fixture("script_unit_command_v1.cyscript")).unwrap();
    let project = std::env::temp_dir().join(format!("cy-graph-snapshot-{}", std::process::id()));
    std::fs::create_dir_all(project.join("game/scripts")).unwrap();
    let mut desk = Desk::new();
    desk.editor = Editor::new(Actor::human("designer"))
        .with_project(cy_editor_services::ProjectService::new(&project));
    desk.editor.open_document("worlds/units.cyworld").unwrap();
    desk.specialised
        .install_script_catalogue(&fixture("script_catalogue_v1.wire"))
        .unwrap();
    let answer = |desk: &mut Desk, send: &dyn Fn(&mut Editor) -> cy_editor_protocol::RequestId, reply: Vec<u8>| {
        let (editor_reader, mut runtime_writer) = std::io::pipe().unwrap();
        let (_runtime_reader, editor_writer) = std::io::pipe().unwrap();
        desk.editor.runtime =
            cy_editor_services::RuntimeSession::over(Session::over(editor_reader, editor_writer));
        let request = send(&mut desk.editor);
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
        .unwrap();
        let mut notifications = cy_editor_services::NotificationService::new();
        while desk.editor.backend.script.pending() {
            for message in desk.editor.runtime.pump(&mut notifications) {
                let _ = desk.editor.backend.accept(&message);
            }
            std::thread::yield_now();
        }
    };
    let compile = |source: String| {
        move |editor: &mut Editor| {
            editor
                .backend
                .script
                .compile(&editor.runtime, reference, &source)
                .unwrap()
                .unwrap()
        }
    };

    std::fs::write(project.join(reference), &source).unwrap();
    answer(&mut desk, &compile(source.clone()), fixture("script_compile_v1.wire"));
    snapshot(
        &mut desk,
        "editor-gameplay-and-utility-graphs",
        "editor-gameplay-graph.png",
    );

    let misspelled = source.replace("unit.move_to", "unit.mvoe_to");
    std::fs::write(project.join(reference), &misspelled).unwrap();
    answer(
        &mut desk,
        &compile(misspelled.clone()),
        fixture("script_compile_error_v1.wire"),
    );
    snapshot(
        &mut desk,
        "editor-gameplay-and-utility-graphs",
        "editor-gameplay-graph-diagnostic.png",
    );

    std::fs::write(project.join(reference), &source).unwrap();
    answer(&mut desk, &compile(source.clone()), fixture("script_compile_v1.wire"));
    answer(
        &mut desk,
        &|editor: &mut Editor| {
            editor
                .backend
                .script
                .refresh(&editor.runtime)
                .unwrap()
                .unwrap()
        },
        fixture("script_state_play_v1.wire"),
    );
    snapshot(
        &mut desk,
        "editor-gameplay-and-utility-graphs",
        "editor-gameplay-graph-play.png",
    );
    let _ = std::fs::remove_dir_all(&project);
}

/// The room [`lighting_desk`] bakes, authored through the registered commands.
fn author_room(desk: &mut Desk) {
    use cy_editor_core::value::Value;

    let invoke = |desk: &mut Desk, id: &str, arguments: Arguments| {
        desk.registry
            .invoke(id, &desk.scope, &mut desk.editor, &arguments)
            .unwrap_or_else(|problem| panic!("{id}: {problem}"))
    };
    let floor = invoke(
        desk,
        "scene.create-primitive",
        Arguments::new()
            .with("shape", Value::Text("plane".into()))
            .with("name", Value::Text("Floor".into()))
            .with("extent", Value::Vec3([4.0, 1.0, 4.0])),
    )
    .values["entity"]
        .as_text()
        .unwrap()
        .to_owned();
    invoke(
        desk,
        "lighting.object.set-resolution",
        Arguments::new()
            .with("entity", Value::Text(floor))
            .with("scale", Value::Float(2.0)),
    );
    invoke(
        desk,
        "scene.create-primitive",
        Arguments::new()
            .with("shape", Value::Text("box".into()))
            .with("name", Value::Text("Crate".into()))
            .with("origin", Value::Text("base".into()))
            .with("at", Value::Vec3([1.0, 0.0, 0.5])),
    );
    let lamp = invoke(
        desk,
        "scene.create-light",
        Arguments::new()
            .with("kind", Value::Text("point".into()))
            .with("at", Value::Vec3([0.0, 2.0, 0.0])),
    )
    .values["entity"]
        .as_text()
        .unwrap()
        .to_owned();
    invoke(
        desk,
        "lighting.light.set-mobility",
        Arguments::new()
            .with("entity", Value::Text(lamp))
            .with("mobility", Value::Text("static".into())),
    );
    invoke(
        desk,
        "lighting.volume.create",
        Arguments::new()
            .with("at", Value::Vec3([-1.5, 0.5, -1.5]))
            .with("count_x", Value::Int(4))
            .with("count_y", Value::Int(2))
            .with("count_z", Value::Int(4))
            .with("rays", Value::Int(64)),
    );
}

/// A room authored through the registered commands — a floor at twice the level's density, a crate
/// that only occludes, a static lamp and an irradiance volume — in a project of its own, baked by
/// the real `cy_build` the tree built. `None` when no `cy_build` is built near this crate.
fn lighting_desk(project: &std::path::Path) -> Option<Desk> {
    use cy_editor_core::progress::OperationState;
    use cy_editor_core::value::Value;
    use cy_editor_services::lightmaps::{CliLightmapBaker, LightmapBakeService, LightmapBaker};

    let baker = CliLightmapBaker::found_near(std::path::Path::new(env!("CARGO_MANIFEST_DIR")));
    if baker.describe().starts_with("no cy_build") {
        eprintln!("no cy_build built near this crate; not drawing the lighting panel");
        return None;
    }
    std::fs::create_dir_all(project).unwrap();
    std::fs::write(project.join("project.json"), "{}").unwrap();
    let mut desk = Desk::new();
    desk.editor = Editor::new(Actor::human("designer"))
        .with_project(cy_editor_services::project::ProjectService::new(project));
    desk.editor.lightmaps = LightmapBakeService::with_baker(project, std::sync::Arc::new(baker));
    // A world whose schema carries a Transform, as an opened `.cyworld` does.
    let mut document = cy_editor_documents::Document::new("worlds/room.cyworld");
    let transform = document.schema_mut().declare_type("Transform", false);
    for (name, kind) in [
        ("translation", cy_editor_core::value::ValueKind::Vec3),
        ("rotation", cy_editor_core::value::ValueKind::Quat),
        ("scale", cy_editor_core::value::ValueKind::Vec3),
    ] {
        document
            .schema_mut()
            .declare_field(transform, name, kind, "part of a transform")
            .expect("a fresh schema");
    }
    let world = desk.editor.documents.insert(document);
    desk.editor.workspace.opened(world);
    author_room(&mut desk);
    let invoke = |desk: &mut Desk, id: &str, arguments: Arguments| {
        desk.registry
            .invoke(id, &desk.scope, &mut desk.editor, &arguments)
            .unwrap_or_else(|problem| panic!("{id}: {problem}"))
    };
    let started = invoke(
        &mut desk,
        "lighting.bake-lightmaps",
        Arguments::new()
            .with("mode", Value::Text("irradiance".into()))
            .with("samples", Value::Int(16))
            .with("density", Value::Float(8.0)),
    );
    let Some(Value::Int(request)) = started.values.get("request").cloned() else {
        panic!("{started:?}");
    };
    let operation = desk
        .editor
        .operations
        .all()
        .iter()
        .find(|operation| operation.id() == u64::try_from(request).unwrap())
        .cloned()
        .unwrap();
    assert_eq!(
        operation.block_until_settled(std::time::Duration::from_mins(2)),
        OperationState::Completed
    );
    Some(desk)
}

#[test]
#[ignore = "needs a GPU adapter and a built cy_build; writes PNGs when CY_PANEL_SNAPSHOTS names a directory"]
fn lighting_panel_snapshots() {
    let project = std::env::temp_dir().join(format!("cy-lighting-snapshot-{}", std::process::id()));
    let Some(mut desk) = lighting_desk(&project) else {
        return;
    };
    snapshot(
        &mut desk,
        "editor-lighting-and-lightmap-baking",
        "editor-lighting-baked.png",
    );
    let light = cy_editor_services::lighting::LightingScene::read(
        desk.editor
            .documents
            .get(desk.editor.workspace.active().unwrap())
            .unwrap(),
    )
    .lights[0]
        .id;
    let mut selection = cy_editor_documents::selection::Selection::new();
    selection.add_node(light);
    desk.editor.selection.set(selection);
    // The window describes the Inspector from the open world's schema every frame; do it here.
    let document = desk
        .editor
        .documents
        .get(desk.editor.workspace.active().unwrap())
        .unwrap();
    desk.shell
        .describe_with(cy_editor_reflection::Catalogue::of_document(
            document.schema(),
        ));
    desk.shell.inspector.refresh(&desk.editor);
    snapshot(&mut desk, "inspector", "editor-lighting-inspector.png");
    std::fs::remove_dir_all(&project).ok();
}
