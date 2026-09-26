// SPDX-License-Identifier: MIT
//! The window half of `editor:window`: ask egui for a screenshot of the frame just drawn, and hand
//! it to the agent host when it arrives.
//!
//! egui-wgpu renders every frame into its own capture texture before presenting it, so the image is
//! the window's framebuffer — independent of other windows on the desktop, of the compositor and of
//! the display server. It arrives on a later frame as `egui::Event::Screenshot`, which is why the
//! agent host parks window reads until [`AgentWindow::receive`] supplies one. See
//! `openspec/changes/add-mcp-editor-window-capture/design.md`, D1 and D3.

use std::time::{Duration, Instant};

use cy_editor_agent::{DesktopAgentHost, PanelRect, WindowFrame};
use cy_editor_interface::shell::BUILT_IN_PANEL_KINDS;

/// How long a requested screenshot may take before it is forgotten and asked for again. Longer than
/// the host's own wait, so a read is refused by the host rather than left behind by this.
const FORGET_AFTER: Duration = Duration::from_secs(4);

/// One screenshot asked for and not yet delivered.
struct Requested {
    sequence: u64,
    at: Instant,
    pixels_per_point: f32,
    panels: Vec<PanelRect>,
}

/// The screenshot requests the window has made on behalf of its agent.
#[derive(Default)]
pub struct AgentWindow {
    sequence: u64,
    requested: Option<Requested>,
}

impl AgentWindow {
    /// At the start of a frame: give a delivered screenshot to the agent host.
    pub fn receive(&mut self, ctx: &egui::Context, agent: &mut DesktopAgentHost) {
        let Some(requested) = &self.requested else {
            return;
        };
        let sequence = requested.sequence;
        let delivered = ctx.input(|input| {
            input.events.iter().find_map(|event| match event {
                egui::Event::Screenshot {
                    user_data, image, ..
                } if user_data
                    .data
                    .as_ref()
                    .and_then(|data| data.downcast_ref::<u64>())
                    == Some(&sequence) =>
                {
                    Some(image.clone())
                }
                _ => None,
            })
        });
        match delivered {
            Some(image) => {
                let requested = self.requested.take().expect("checked above");
                agent.provide_window_capture(window_frame(&image, requested));
            }
            None if requested.at.elapsed() > FORGET_AFTER => self.requested = None,
            None => {}
        }
    }

    /// After the frame is drawn: ask for a screenshot of it when an agent is waiting for one.
    ///
    /// `panels` are the rectangles the dock drew in this same frame, kept with the request so a crop
    /// and the pixels it cuts come from one layout.
    pub fn request(
        &mut self,
        ctx: &egui::Context,
        agent: &DesktopAgentHost,
        panels: &[(String, egui::Rect)],
    ) {
        if self.requested.is_some() || !agent.wants_window_image() {
            return;
        }
        self.sequence += 1;
        ctx.send_viewport_cmd(egui::ViewportCommand::Screenshot(egui::UserData::new(
            self.sequence,
        )));
        self.requested = Some(Requested {
            sequence: self.sequence,
            at: Instant::now(),
            pixels_per_point: ctx.pixels_per_point(),
            panels: panels
                .iter()
                .map(|(kind, rect)| PanelRect {
                    kind: kind.clone(),
                    min: [rect.min.x, rect.min.y],
                    max: [rect.max.x, rect.max.y],
                })
                .collect(),
        });
        // The screenshot is taken when this frame is painted and delivered on a later one.
        ctx.request_repaint();
    }
}

/// The screenshot as the agent crate's toolkit-free frame.
fn window_frame(image: &egui::ColorImage, requested: Requested) -> WindowFrame {
    let rgba = image
        .pixels
        .iter()
        .flat_map(egui::Color32::to_srgba_unmultiplied)
        .collect();
    let mut kinds: Vec<String> = BUILT_IN_PANEL_KINDS
        .iter()
        .map(ToString::to_string)
        .collect();
    for panel in &requested.panels {
        // A plugin's panel is a kind the editor has even though the built-in table does not name it.
        if !kinds.contains(&panel.kind) {
            kinds.push(panel.kind.clone());
        }
    }
    WindowFrame {
        sequence: requested.sequence,
        width: u32::try_from(image.size[0]).unwrap_or(u32::MAX),
        height: u32::try_from(image.size[1]).unwrap_or(u32::MAX),
        pixels_per_point: requested.pixels_per_point,
        rgba,
        panels: requested.panels,
        kinds,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_screenshot_becomes_a_frame_with_its_panels_and_every_kind() {
        let image = egui::ColorImage::new([3, 2], vec![egui::Color32::from_rgb(10, 20, 30); 6]);
        let frame = window_frame(
            &image,
            Requested {
                sequence: 5,
                at: Instant::now(),
                pixels_per_point: 2.0,
                panels: vec![PanelRect {
                    kind: "plugin-panel".into(),
                    min: [0.0, 0.0],
                    max: [1.0, 1.0],
                }],
            },
        );
        assert_eq!((frame.width, frame.height, frame.sequence), (3, 2, 5));
        assert_eq!(frame.rgba.len(), 3 * 2 * 4);
        assert_eq!(&frame.rgba[..4], &[10, 20, 30, 255]);
        assert!(frame.kinds.iter().any(|kind| kind == "viewport"));
        assert!(frame.kinds.iter().any(|kind| kind == "plugin-panel"));
    }
}
