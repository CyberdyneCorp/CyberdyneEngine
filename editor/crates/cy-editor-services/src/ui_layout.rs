// SPDX-License-Identifier: MIT
//! The wire of the engine's `ui.*` operations, the UI layout editor's (issue #29, "UI layout"):
//! the widget vocabulary the engine declares, the preview request the editor sends, and the image,
//! rectangles and diagnostics the engine answers with.
//!
//! The payloads are specified in `src/editor_backend/include/cy/editor/ui_layout_service.h`.
//! `src/editor_backend/tests/data/ui_catalogue_v1.wire` and `ui_preview_request_v1.wire` hold them
//! from both sides: the engine's suite writes the catalogue and decodes the request this module
//! encodes, and the tests below decode the catalogue and encode the request byte for byte.

use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::problem::{Problem, Result};

/// The engine's widget vocabulary.
pub const CATALOGUE: &str = "ui.catalogue.get";
/// Make a document into widgets and draw it.
pub const PREVIEW: &str = "ui.preview.render";
const FORMAT: u32 = 1;

/// What a property's value is: the engine's `UiPropertyType`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum PropertyType {
    /// A number.
    Number,
    /// Quoted text.
    Text,
    /// A bare word.
    Word,
    /// `true` or `false`.
    Bool,
    /// A list of texts: `items`.
    TextList,
    /// A list of numbers: `anchor`, `offset`.
    NumberList,
}

impl PropertyType {
    fn from_wire(byte: u8) -> Result<Self> {
        Ok(match byte {
            0 => Self::Number,
            1 => Self::Text,
            2 => Self::Word,
            3 => Self::Bool,
            4 => Self::TextList,
            5 => Self::NumberList,
            _ => {
                return Err(Problem::new(
                    "read the engine's widget vocabulary",
                    format!("it names a property type {byte} this editor does not know"),
                ));
            }
        })
    }
}

/// One widget kind an author can place.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct WidgetKind {
    /// Its name in a document (`slider`).
    pub name: String,
    /// It holds other widgets.
    pub container: bool,
    /// It takes focus.
    pub focusable: bool,
    /// Its properties, the inspector's rows, in order.
    pub properties: Vec<(String, PropertyType)>,
}

/// The engine's widget vocabulary: `ui.catalogue.get`'s answer.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct UiCatalogue {
    /// Every kind a palette offers.
    pub kinds: Vec<WidgetKind>,
    /// Every `.cyss` property, which a line may also carry as an inline declaration.
    pub style_properties: Vec<String>,
    /// The states a preview can put a widget into: hover, pressed, focused, disabled, open.
    pub states: Vec<String>,
    /// `one-way`, `two-way`.
    pub binding_modes: Vec<String>,
}

impl UiCatalogue {
    /// Decode the engine's answer.
    ///
    /// # Errors
    ///
    /// When it is cut short, padded, or in a format this editor does not read.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut reader = Reader::new(payload);
        if reader.u32()? != FORMAT {
            return Err(Problem::new(
                "read the engine's widget vocabulary",
                "it is in a format this editor does not read",
            ));
        }
        let kinds = (0..reader.u32()?)
            .map(|_| {
                let name = reader.text()?;
                let flags = reader.u8()?;
                let properties = (0..reader.u32()?)
                    .map(|_| Ok((reader.text()?, PropertyType::from_wire(reader.u8()?)?)))
                    .collect::<Result<Vec<_>>>()?;
                Ok(WidgetKind {
                    name,
                    container: flags & 1 != 0,
                    focusable: flags & 2 != 0,
                    properties,
                })
            })
            .collect::<Result<Vec<_>>>()?;
        let texts = |reader: &mut Reader<'_>| -> Result<Vec<String>> {
            (0..reader.u32()?).map(|_| reader.text()).collect()
        };
        let style_properties = texts(&mut reader)?;
        let states = texts(&mut reader)?;
        let binding_modes = texts(&mut reader)?;
        finished(&reader, "the engine's widget vocabulary")?;
        Ok(Self {
            kinds,
            style_properties,
            states,
            binding_modes,
        })
    }

    /// A kind by name.
    #[must_use]
    pub fn kind(&self, name: &str) -> Option<&WidgetKind> {
        self.kinds.iter().find(|kind| kind.name == name)
    }
}

fn finished(reader: &Reader<'_>, what: &str) -> Result<()> {
    if reader.remaining() == 0 {
        Ok(())
    } else {
        Err(Problem::new(
            format!("read {what}"),
            "there are bytes after its end",
        ))
    }
}

/// How the document is scaled to the target: the engine's `ui::ScaleMode`.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum ScaleMode {
    /// One reference unit is one pixel.
    FixedPixel,
    /// Follow the width.
    ScaleWithWidth,
    /// Follow the height.
    ScaleWithHeight,
    /// The smaller ratio: everything fits.
    #[default]
    ScaleWithSmaller,
    /// The larger ratio.
    ScaleWithLarger,
}

impl ScaleMode {
    /// Every mode, in the engine's order.
    pub const ALL: [Self; 5] = [
        Self::FixedPixel,
        Self::ScaleWithWidth,
        Self::ScaleWithHeight,
        Self::ScaleWithSmaller,
        Self::ScaleWithLarger,
    ];

    /// The name a command takes.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::FixedPixel => "fixed",
            Self::ScaleWithWidth => "width",
            Self::ScaleWithHeight => "height",
            Self::ScaleWithSmaller => "smaller",
            Self::ScaleWithLarger => "larger",
        }
    }

    /// The mode a name spells.
    #[must_use]
    pub fn named(name: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|mode| mode.name() == name)
    }

    const fn wire(self) -> u8 {
        match self {
            Self::FixedPixel => 0,
            Self::ScaleWithWidth => 1,
            Self::ScaleWithHeight => 2,
            Self::ScaleWithSmaller => 3,
            Self::ScaleWithLarger => 4,
        }
    }
}

/// The state a preview puts one widget into.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum PreviewState {
    /// At rest.
    #[default]
    None,
    /// The pointer over it.
    Hover,
    /// Pressed on.
    Pressed,
    /// Focused by the keyboard or a gamepad: the ring is drawn.
    Focused,
    /// Disabled by the game.
    Disabled,
    /// A drop-down's list or a dialog, open.
    Open,
}

impl PreviewState {
    /// Every state, in the engine's order.
    pub const ALL: [Self; 6] = [
        Self::None,
        Self::Hover,
        Self::Pressed,
        Self::Focused,
        Self::Disabled,
        Self::Open,
    ];

    /// The engine's name for it.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::None => "none",
            Self::Hover => "hover",
            Self::Pressed => "pressed",
            Self::Focused => "focused",
            Self::Disabled => "disabled",
            Self::Open => "open",
        }
    }

    /// The state a name spells.
    #[must_use]
    pub fn named(name: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|state| state.name() == name)
    }

    const fn wire(self) -> u8 {
        match self {
            Self::None => 0,
            Self::Hover => 1,
            Self::Pressed => 2,
            Self::Focused => 3,
            Self::Disabled => 4,
            Self::Open => 5,
        }
    }
}

/// What the author previews: the target, its DPI, how the document scales, the theme, and one
/// widget's state.
#[derive(Clone, PartialEq, Debug)]
pub struct PreviewSettings {
    /// The target's pixels.
    pub width: u32,
    /// The target's pixels.
    pub height: u32,
    /// The platform's DPI scale.
    pub dpi: f32,
    /// How the document scales to the target.
    pub scale_mode: ScaleMode,
    /// The resolution the document was designed at.
    pub reference: [f32; 2],
    /// The project theme (`.cyss`) the preview is drawn in; empty for the built-in theme.
    pub theme: String,
    /// The widget, by index, put into `state`.
    pub state_widget: Option<usize>,
    /// The state.
    pub state: PreviewState,
    /// The largest image the editor wants back, per axis.
    pub image_max: [u32; 2],
}

impl Default for PreviewSettings {
    fn default() -> Self {
        Self {
            width: 1920,
            height: 1080,
            dpi: 1.0,
            scale_mode: ScaleMode::ScaleWithSmaller,
            reference: [1920.0, 1080.0],
            theme: String::new(),
            state_widget: None,
            state: PreviewState::None,
            image_max: [1280, 1280],
        }
    }
}

/// The resolutions and aspect ratios the toolbar offers.
pub const RESOLUTIONS: &[(&str, u32, u32)] = &[
    ("1280x720 (16:9)", 1280, 720),
    ("1920x1080 (16:9)", 1920, 1080),
    ("2560x1440 (16:9)", 2560, 1440),
    ("3840x2160 (16:9)", 3840, 2160),
    ("1024x768 (4:3)", 1024, 768),
    ("1920x1200 (16:10)", 1920, 1200),
    ("2560x1080 (21:9)", 2560, 1080),
    ("1080x1920 (portrait)", 1080, 1920),
];

/// The DPI scales the toolbar offers.
pub const DPI_SCALES: &[f32] = &[1.0, 1.25, 1.5, 2.0];

/// One file a previewed document reaches, as read from the project.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct PreviewFile {
    /// The path the document names it by.
    pub path: String,
    /// Its text.
    pub bytes: String,
}

/// The `ui.preview.render` payload.
#[must_use]
pub fn preview_payload(
    document: &str,
    files: &[PreviewFile],
    theme: &str,
    settings: &PreviewSettings,
    state_line: u32,
) -> Vec<u8> {
    let mut writer = Writer::new();
    writer.u32(FORMAT);
    writer.text(document);
    writer.u32(u32::try_from(files.len()).unwrap_or(u32::MAX));
    for file in files {
        writer.text(&file.path);
        writer.text(&file.bytes);
    }
    writer.text(theme);
    writer.u32(settings.width);
    writer.u32(settings.height);
    writer.f32(settings.dpi);
    writer.u8(settings.scale_mode.wire());
    writer.f32(settings.reference[0]);
    writer.f32(settings.reference[1]);
    writer.f32(0.5);
    writer.u32(settings.image_max[0]);
    writer.u32(settings.image_max[1]);
    writer.u32(state_line);
    writer.u8(settings.state.wire());
    writer.finish()
}

/// Where the engine laid out one line of the document.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct NodeRect {
    /// The line, from one.
    pub line: u32,
    /// Whether it made a widget.
    pub made: bool,
    /// The rectangle, in reference units: x, y, width, height.
    pub rect: [f32; 4],
}

/// A diagnostic the engine named.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct PreviewDiagnostic {
    /// The line, from one; zero for a sheet's or the theme's.
    pub line: u32,
    /// The column.
    pub column: u32,
    /// What is wrong.
    pub message: String,
    /// What it is about: a kind, a parameter, `sheet` or `theme`.
    pub detail: String,
}

/// The engine's preview: the image, every line's rectangle, what it named, and what it cost.
#[derive(Clone, PartialEq, Debug)]
pub struct UiPreview {
    /// The image's size.
    pub width: u32,
    /// The image's size.
    pub height: u32,
    /// Pixels per reference unit at the target.
    pub scale: f32,
    /// Image pixels per target pixel.
    pub image_factor: f32,
    /// RGBA8, top row first.
    pub pixels: Vec<u8>,
    /// Each element line's rectangle, in document order.
    pub nodes: Vec<NodeRect>,
    /// What the engine named.
    pub diagnostics: Vec<PreviewDiagnostic>,
    /// Every class the theme and the sheets select.
    pub classes: Vec<String>,
    /// Widgets made.
    pub widgets: u32,
    /// Primitives flattened.
    pub primitives: u32,
    /// Draws the renderer would record.
    pub draws: u32,
    /// Milliseconds to make and lay out.
    pub layout_ms: f64,
    /// Milliseconds to flatten and draw.
    pub draw_ms: f64,
}

impl UiPreview {
    /// Decode the engine's answer.
    ///
    /// # Errors
    ///
    /// When it is cut short, padded, or its image is not the size it says.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut reader = Reader::new(payload);
        if reader.u32()? != FORMAT {
            return Err(Problem::new(
                "read the engine's preview",
                "it is in a format this editor does not read",
            ));
        }
        let width = reader.u32()?;
        let height = reader.u32()?;
        let scale = reader.f32()?;
        let image_factor = reader.f32()?;
        let pixels = reader.bytes()?;
        if pixels.len() != (width as usize) * (height as usize) * 4 {
            return Err(Problem::new(
                "read the engine's preview",
                format!(
                    "its image is {} bytes, not {width}x{height} texels",
                    pixels.len()
                ),
            ));
        }
        let nodes = (0..reader.u32()?)
            .map(|_| {
                Ok(NodeRect {
                    line: reader.u32()?,
                    made: reader.u8()? != 0,
                    rect: [reader.f32()?, reader.f32()?, reader.f32()?, reader.f32()?],
                })
            })
            .collect::<Result<Vec<_>>>()?;
        let diagnostics = (0..reader.u32()?)
            .map(|_| {
                Ok(PreviewDiagnostic {
                    line: reader.u32()?,
                    column: reader.u32()?,
                    message: reader.text()?,
                    detail: reader.text()?,
                })
            })
            .collect::<Result<Vec<_>>>()?;
        let classes = (0..reader.u32()?)
            .map(|_| reader.text())
            .collect::<Result<Vec<_>>>()?;
        let preview = Self {
            width,
            height,
            scale,
            image_factor,
            pixels,
            nodes,
            diagnostics,
            classes,
            widgets: reader.u32()?,
            primitives: reader.u32()?,
            draws: reader.u32()?,
            layout_ms: reader.f64()?,
            draw_ms: reader.f64()?,
        };
        finished(&reader, "the engine's preview")?;
        Ok(preview)
    }

    /// Image pixels per reference unit: where a rectangle lands on the image.
    #[must_use]
    pub fn pixels_per_unit(&self) -> f32 {
        self.scale * self.image_factor
    }

    /// The rectangle of the widget on `line`, when the engine made one.
    #[must_use]
    pub fn rect_of_line(&self, line: u32) -> Option<[f32; 4]> {
        self.nodes
            .iter()
            .find(|node| node.line == line && node.made)
            .map(|node| node.rect)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn engine_fixture(name: &str) -> Vec<u8> {
        let path = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../../src/editor_backend/tests/data")
            .join(name);
        std::fs::read(&path).unwrap_or_else(|error| panic!("{}: {error}", path.display()))
    }

    /// `kGallery` in `test_ui_layout_service.cpp`.
    const GALLERY: &str = r#"cyui 1
panel #menu .panel anchor=[0.5, 0.5, 0.5, 0.5] offset=[-150, -120, 150, 120]
  column #body gap=8 anchor=[0, 0, 1, 1] offset=[10, 10, -10, -10]
    label #title text="Options"
    slider #volume min=0 max=10 step=1 value=4
    dropdown #quality items=["Low", "Medium", "High"] selected=1
    row #buttons gap=6
      button #back text="Back"
      button #play .primary text="Play"
"#;

    #[test]
    fn the_engines_vocabulary_decodes_with_every_placeable_kind() {
        let catalogue = UiCatalogue::decode(&engine_fixture("ui_catalogue_v1.wire")).unwrap();
        assert_eq!(catalogue.kinds.len(), 20);
        let slider = catalogue.kind("slider").unwrap();
        assert!(slider.focusable && !slider.container);
        assert!(
            slider
                .properties
                .iter()
                .any(|(key, kind)| key == "value" && *kind == PropertyType::Number)
        );
        let panel = catalogue.kind("panel").unwrap();
        assert!(panel.container);
        assert!(
            panel
                .properties
                .iter()
                .any(|(key, kind)| key == "anchor" && *kind == PropertyType::NumberList)
        );
        assert!(
            catalogue
                .style_properties
                .iter()
                .any(|name| name == "background-color")
        );
        assert_eq!(
            catalogue.states,
            vec!["hover", "pressed", "focused", "disabled", "open"]
        );
        assert_eq!(catalogue.binding_modes, vec!["one-way", "two-way"]);
        let mut padded = engine_fixture("ui_catalogue_v1.wire");
        padded.push(0);
        assert!(UiCatalogue::decode(&padded).is_err());
    }

    #[test]
    fn the_preview_request_is_the_bytes_the_engines_suite_decodes() {
        let settings = PreviewSettings {
            width: 640,
            height: 360,
            dpi: 1.0,
            scale_mode: ScaleMode::FixedPixel,
            reference: [1920.0, 1080.0],
            theme: String::new(),
            state_widget: None,
            state: PreviewState::None,
            image_max: [640, 360],
        };
        assert_eq!(
            preview_payload(GALLERY, &[], "", &settings, 0),
            engine_fixture("ui_preview_request_v1.wire")
        );
    }

    #[test]
    fn a_preview_decodes_and_an_image_of_the_wrong_size_is_refused() {
        let mut writer = Writer::new();
        writer.u32(1);
        writer.u32(2);
        writer.u32(1);
        writer.f32(2.0);
        writer.f32(0.5);
        writer.bytes(&[1, 2, 3, 4, 5, 6, 7, 8]);
        writer.u32(1);
        writer.u32(2);
        writer.u8(1);
        for value in [10.0, 20.0, 30.0, 40.0] {
            writer.f32(value);
        }
        writer.u32(1);
        writer.u32(3);
        writer.u32(4);
        writer.text("no such widget kind");
        writer.text("sliderr");
        writer.u32(1);
        writer.text("primary");
        writer.u32(9);
        writer.u32(40);
        writer.u32(6);
        writer.f64(1.5);
        writer.f64(2.5);
        let bytes = writer.finish();
        let preview = UiPreview::decode(&bytes).unwrap();
        assert_eq!((preview.width, preview.height), (2, 1));
        assert!((preview.pixels_per_unit() - 1.0).abs() < f32::EPSILON);
        assert_eq!(preview.rect_of_line(2), Some([10.0, 20.0, 30.0, 40.0]));
        assert_eq!(preview.rect_of_line(3), None);
        assert_eq!(preview.diagnostics[0].detail, "sliderr");
        assert_eq!(preview.classes, vec!["primary"]);
        assert_eq!(
            (preview.widgets, preview.primitives, preview.draws),
            (9, 40, 6)
        );
        // The image says 2x1 and carries 8 bytes; one more is refused.
        let mut wrong = bytes[..20].to_vec();
        wrong[16] = 9;
        assert!(UiPreview::decode(&wrong).is_err());
        assert!(UiPreview::decode(&bytes[..bytes.len() - 1]).is_err());
    }

    #[test]
    fn states_and_scale_modes_are_named_as_commands_take_them() {
        for state in PreviewState::ALL {
            assert_eq!(PreviewState::named(state.name()), Some(state));
        }
        for mode in ScaleMode::ALL {
            assert_eq!(ScaleMode::named(mode.name()), Some(mode));
        }
        assert_eq!(PreviewState::named("active"), None);
    }
}
