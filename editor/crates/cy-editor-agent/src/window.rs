// SPDX-License-Identifier: MIT
//! The editor window as an agent reads it: `editor:window` and `editor:window?panel=<kind>`.
//!
//! `viewport:` answers "what does the engine render"; this answers "what is on the person's
//! screen" — every panel, the palette, and the engine image *as the editor composited it*. The
//! pixels are the window's own presentation of one frame, handed over by the shell that drew it,
//! so nothing here renders, simulates, or reaches into a display server. See
//! `openspec/changes/add-mcp-editor-window-capture/design.md`.
//!
//! This crate names no toolkit (`cy-editor-app`'s containment test holds it to that), so the shell
//! converts its screenshot into a [`WindowFrame`] of plain RGBA and panel rectangles in points.

use cy_editor_core::problem::{Problem, Result};

/// The address of the whole window. `?panel=<kind>` narrows it to one dock panel.
pub const WINDOW_ADDRESS: &str = "editor:window";

/// What a window read asks for.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum WindowTarget {
    /// Every pixel of the window.
    Whole,
    /// One dock panel, by the kind its layout stores (`viewport`, `hierarchy`, …).
    Panel(String),
}

impl WindowTarget {
    /// Parse a resource address. `None` when the address is not a window address at all, so a
    /// caller can route on it without a second prefix check.
    #[must_use]
    pub fn parse(uri: &str) -> Option<Result<Self>> {
        let rest = uri.strip_prefix(WINDOW_ADDRESS)?;
        if rest.is_empty() {
            return Some(Ok(Self::Whole));
        }
        Some(match rest.strip_prefix("?panel=") {
            Some(kind) if !kind.is_empty() => Ok(Self::Panel(kind.to_string())),
            _ => Err(Problem::new(
                format!("read {uri:?}"),
                "the only query a window address takes is a non-empty panel kind",
            )
            .with_remedy(format!(
                "read {WINDOW_ADDRESS} for the whole window or {WINDOW_ADDRESS}?panel=viewport \
                 for one panel"
            ))),
        })
    }
}

/// Where one dock panel was drawn in a frame, in logical points from the window's top left.
#[derive(Clone, PartialEq, Debug)]
pub struct PanelRect {
    /// The panel's stable kind.
    pub kind: String,
    /// Top-left corner, in points.
    pub min: [f32; 2],
    /// Bottom-right corner, in points.
    pub max: [f32; 2],
}

/// One presented frame of the window, as the shell captured it.
#[derive(Clone, PartialEq, Debug)]
pub struct WindowFrame {
    /// The shell's capture counter, so a reply can name which frame it came from.
    pub sequence: u64,
    /// Width in physical pixels.
    pub width: u32,
    /// Height in physical pixels.
    pub height: u32,
    /// Physical pixels per logical point in that frame.
    pub pixels_per_point: f32,
    /// Unpremultiplied RGBA8, row-major, `width * height * 4` bytes.
    pub rgba: Vec<u8>,
    /// The panels drawn in that frame. A panel absent here was not visible.
    pub panels: Vec<PanelRect>,
    /// Every panel kind the editor defines, so an unknown name is told apart from a hidden one.
    pub kinds: Vec<String>,
}

/// A rectangle in physical pixels.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct PixelRect {
    /// Left edge.
    pub x: u32,
    /// Top edge.
    pub y: u32,
    /// Width.
    pub width: u32,
    /// Height.
    pub height: u32,
}

/// What a window read returns.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct WindowCapture {
    /// The pixels, encoded as PNG.
    pub png: Vec<u8>,
    /// The whole window's size in physical pixels.
    pub window_width: u32,
    /// See `window_width`.
    pub window_height: u32,
    /// The part of the window returned.
    pub rect: PixelRect,
    /// The panel it was cropped to, if any.
    pub panel: Option<String>,
    /// The shell's capture counter for the frame.
    pub sequence: u64,
}

impl WindowCapture {
    /// The address this capture answers.
    #[must_use]
    pub fn uri(&self) -> String {
        match &self.panel {
            Some(kind) => format!("{WINDOW_ADDRESS}?panel={kind}"),
            None => WINDOW_ADDRESS.to_string(),
        }
    }

    /// One line a caller reads. States that this is the editor's presentation, never the shipping
    /// frame, as `editor-agent-interface` requires every image to say what it is.
    #[must_use]
    pub fn describe(&self) -> String {
        let what = self.panel.as_ref().map_or_else(
            || "the editor window".to_string(),
            |kind| format!("the {kind} panel"),
        );
        format!(
            "{what} as presented in window frame {}: {}x{} at ({}, {}) of a {}x{} window; the \
             editor's composition, not the shipping frame",
            self.sequence,
            self.rect.width,
            self.rect.height,
            self.rect.x,
            self.rect.y,
            self.window_width,
            self.window_height
        )
    }
}

impl WindowFrame {
    /// Answer one read against this frame.
    pub fn capture(&self, target: &WindowTarget) -> Result<WindowCapture> {
        let expected = u64::from(self.width) * u64::from(self.height) * 4;
        if self.width == 0 || self.height == 0 || self.rgba.len() as u64 != expected {
            return Err(Problem::new(
                "capture the editor window",
                format!(
                    "the frame holds {} bytes for a {}x{} window",
                    self.rgba.len(),
                    self.width,
                    self.height
                ),
            ));
        }
        let (rect, panel) = match target {
            WindowTarget::Whole => (
                PixelRect {
                    x: 0,
                    y: 0,
                    width: self.width,
                    height: self.height,
                },
                None,
            ),
            WindowTarget::Panel(kind) => (self.panel_rect(kind)?, Some(kind.clone())),
        };
        Ok(WindowCapture {
            png: encode_png(&self.crop(rect), rect.width, rect.height)?,
            window_width: self.width,
            window_height: self.height,
            rect,
            panel,
            sequence: self.sequence,
        })
    }

    /// A panel's rectangle in pixels, clamped to the window; refused when unknown or not drawn.
    fn panel_rect(&self, kind: &str) -> Result<PixelRect> {
        let what = format!("capture the {kind} panel");
        if !self.kinds.iter().any(|known| known == kind) {
            return Err(
                Problem::new(what, format!("the editor has no panel kind {kind:?}"))
                    .with_remedy(format!("the kinds are: {}", self.kinds.join(", "))),
            );
        }
        let Some(panel) = self.panels.iter().find(|panel| panel.kind == kind) else {
            let shown: Vec<&str> = self
                .panels
                .iter()
                .map(|panel| panel.kind.as_str())
                .collect();
            return Err(Problem::new(
                what,
                format!("the {kind} panel was not drawn in the captured frame"),
            )
            .with_remedy(format!(
                "open it or select its tab (Workspace reset restores the default layout); the \
                 panels shown are: {}",
                shown.join(", ")
            )));
        };
        // In f64, where every u32 extent is exact, so clamping to the window loses nothing.
        let scale = f64::from(self.pixels_per_point);
        let edge = |points: f32, limit: u32, round: fn(f64) -> f64| -> u32 {
            let pixels = round(f64::from(points) * scale).clamp(0.0, f64::from(limit));
            #[allow(
                clippy::cast_possible_truncation,
                clippy::cast_sign_loss,
                reason = "the value is a whole number clamped to [0, a u32 window extent]"
            )]
            let whole = pixels as u32;
            whole
        };
        let left = edge(panel.min[0], self.width, f64::floor);
        let top = edge(panel.min[1], self.height, f64::floor);
        let right = edge(panel.max[0], self.width, f64::ceil);
        let bottom = edge(panel.max[1], self.height, f64::ceil);
        if right <= left || bottom <= top {
            return Err(Problem::new(
                what,
                "the panel has no area inside the window in the captured frame",
            )
            .with_remedy("enlarge the panel or the window"));
        }
        Ok(PixelRect {
            x: left,
            y: top,
            width: right - left,
            height: bottom - top,
        })
    }

    fn crop(&self, rect: PixelRect) -> Vec<u8> {
        let stride = self.width as usize * 4;
        let row = rect.width as usize * 4;
        let mut out = Vec::with_capacity(row * rect.height as usize);
        for y in rect.y..rect.y + rect.height {
            let start = y as usize * stride + rect.x as usize * 4;
            out.extend_from_slice(&self.rgba[start..start + row]);
        }
        out
    }
}

fn encode_png(rgba: &[u8], width: u32, height: u32) -> Result<Vec<u8>> {
    use image::ImageEncoder as _;
    let mut png = Vec::new();
    image::codecs::png::PngEncoder::new(&mut png)
        .write_image(rgba, width, height, image::ExtendedColorType::Rgba8)
        .map_err(|error| Problem::new("encode the editor window", error.to_string()))?;
    Ok(png)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A 4x4-point window at `scale`, left half red and right half blue, with a viewport panel
    /// on the right half and a hierarchy that exists but was not drawn.
    fn frame(scale: f32) -> WindowFrame {
        #[allow(
            clippy::cast_possible_truncation,
            clippy::cast_sign_loss,
            reason = "test scales are small positive integers"
        )]
        let side = (4.0 * scale) as u32;
        let mut rgba = Vec::new();
        for _y in 0..side {
            for x in 0..side {
                rgba.extend_from_slice(if x < side / 2 {
                    &[255, 0, 0, 255]
                } else {
                    &[0, 0, 255, 255]
                });
            }
        }
        WindowFrame {
            sequence: 7,
            width: side,
            height: side,
            pixels_per_point: scale,
            rgba,
            panels: vec![PanelRect {
                kind: "viewport".into(),
                min: [2.0, 0.0],
                max: [4.0, 4.0],
            }],
            kinds: vec!["viewport".into(), "hierarchy".into()],
        }
    }

    fn pixels(capture: &WindowCapture) -> Vec<u8> {
        image::load_from_memory(&capture.png)
            .expect("our own PNG")
            .to_rgba8()
            .into_raw()
    }

    #[test]
    fn addresses_parse_and_others_are_not_ours() {
        assert_eq!(
            WindowTarget::parse("editor:window").unwrap().unwrap(),
            WindowTarget::Whole
        );
        assert_eq!(
            WindowTarget::parse("editor:window?panel=viewport")
                .unwrap()
                .unwrap(),
            WindowTarget::Panel("viewport".into())
        );
        assert!(
            WindowTarget::parse("editor:window?panel=")
                .unwrap()
                .is_err()
        );
        assert!(
            WindowTarget::parse("editor:window?size=2")
                .unwrap()
                .is_err()
        );
        assert!(WindowTarget::parse("viewport:").is_none());
        assert!(WindowTarget::parse("hierarchy:").is_none());
    }

    #[test]
    fn the_whole_window_is_every_pixel() {
        let frame = frame(1.0);
        let capture = frame.capture(&WindowTarget::Whole).unwrap();
        assert_eq!((capture.rect.width, capture.rect.height), (4, 4));
        assert_eq!(pixels(&capture), frame.rgba);
        assert_eq!(capture.uri(), "editor:window");
        assert!(capture.describe().contains("not the shipping frame"));
    }

    #[test]
    fn a_panel_crop_scales_points_to_pixels() {
        for scale in [1.0, 2.0] {
            let capture = frame(scale)
                .capture(&WindowTarget::Panel("viewport".into()))
                .unwrap();
            #[allow(
                clippy::cast_possible_truncation,
                clippy::cast_sign_loss,
                reason = "test scales are small positive integers"
            )]
            let half = (2.0 * scale) as u32;
            assert_eq!(capture.rect.x, half, "at scale {scale}");
            assert_eq!((capture.rect.width, capture.rect.height), (half, half * 2));
            // Only the blue half: the crop took the viewport's pixels and nothing beside it.
            assert!(
                pixels(&capture)
                    .chunks(4)
                    .all(|pixel| pixel == [0, 0, 255, 255])
            );
            assert_eq!(capture.uri(), "editor:window?panel=viewport");
        }
    }

    #[test]
    fn a_rectangle_past_the_edge_is_clamped() {
        let mut frame = frame(1.0);
        frame.panels[0].max = [9.0, 9.0];
        frame.panels[0].min = [-3.0, 1.0];
        let capture = frame
            .capture(&WindowTarget::Panel("viewport".into()))
            .unwrap();
        assert_eq!(
            capture.rect,
            PixelRect {
                x: 0,
                y: 1,
                width: 4,
                height: 3
            }
        );
    }

    #[test]
    fn hidden_and_unknown_panels_are_refused_differently() {
        let frame = frame(1.0);
        let hidden = frame
            .capture(&WindowTarget::Panel("hierarchy".into()))
            .unwrap_err();
        assert!(hidden.because.contains("not drawn"), "{}", hidden.because);
        assert!(hidden.remedy.unwrap().contains("viewport"));
        let unknown = frame
            .capture(&WindowTarget::Panel("teapot".into()))
            .unwrap_err();
        assert!(
            unknown.because.contains("no panel kind"),
            "{}",
            unknown.because
        );
        assert!(unknown.remedy.unwrap().contains("hierarchy"));
    }

    #[test]
    fn a_frame_whose_bytes_disagree_with_its_size_is_refused() {
        let mut frame = frame(1.0);
        frame.rgba.pop();
        assert!(frame.capture(&WindowTarget::Whole).is_err());
    }
}
