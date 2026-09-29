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
    // One device at a time: the tests in this file run on parallel threads.
    static ONE_DEVICE: std::sync::Mutex<()> = std::sync::Mutex::new(());
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
