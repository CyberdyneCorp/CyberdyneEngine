//! The CyberEngine identity, in the header and on the window. Task 1.5b.
//!
//! --- WHAT THE IDENTITY IS, AND WHOSE IT IS --------------------------------------------------------
//!
//! **The product is CyberEngine. The publisher is Cyberdyne.** The lockup says so — `CYBERENGINE`
//! over `BY CYBERDYNE` — and interface text follows it: the application is CyberEngine everywhere a
//! user reads a name, and Cyberdyne appears where a publisher is named and nowhere else. The
//! repository directory keeps its historical name, which is a filesystem artefact rather than a
//! product name.
//!
//! --- THE ONE RULE THAT IS EASY TO GET WRONG -------------------------------------------------------
//!
//! `editor-visual-language`: *"The mark is rendered with metallic gradients and a blue emissive
//! core. Those belong to the logo, not to the interface. The chrome around it stays charcoal and
//! flat — a header that picks up the logo's gradients has misread it."*
//!
//! So this module draws the lockup **as an image, at its own size, once**, and the header around it
//! is `Surface::Window` like every other row of chrome. There is no accent taken from the mark, no
//! blue glow behind the header, and no gradient anywhere in this crate — `theme.rs` has no gradient
//! to offer even if a panel asked. [`tests::the_header_takes_no_colour_from_the_mark`] is the check.
//!
//! --- WHY THE ASSETS ARE DERIVED RATHER THAN REDRAWN ------------------------------------------------
//!
//! `docs/design/images/cyberengine-logo.png` is normative and it is one sheet. The editor needs the
//! marks on transparency so a lockup sits on charcoal rather than on a dark plate of its own, so
//! `editor/assets/identity/derive.py` crops the sheet and keys its backdrop out, and the results are
//! committed beside it. The script is committed too, because "where did this PNG come from" is a
//! question every asset in a repository eventually gets asked.
//!
//! The monochrome lockup is committed as a **mask** — one shape, pure white, with its alpha — and is
//! tinted at draw time with the theme's primary text colour. One file serves the dark theme and the
//! light one, and a monochrome lockup with a colour baked in would be neither.

use std::sync::Arc;

/// The horizontal lockup: the mark, `CYBERENGINE`, and `BY CYBERDYNE` on one line.
///
/// What sits in the application header. `editor-visual-language`: "The horizontal lockup sits in the
/// editor header. It identifies the product and occupies the header and no more."
const HORIZONTAL: &[u8] = include_bytes!("../../../assets/identity/cyberengine-horizontal.png");

/// The monochrome lockup, as a white mask to be tinted. Used where the colour lockup would not read
/// — the light theme's header, and any surface that is not charcoal.
const MONOCHROME: &[u8] = include_bytes!("../../../assets/identity/cyberengine-monochrome.png");

/// The mark alone, at window-icon size.
const MARK_256: &[u8] = include_bytes!("../../../assets/identity/cyberengine-mark-256.png");

/// The name of the product, as every piece of interface text spells it.
pub const PRODUCT: &str = "CyberEngine";

/// The name of the publisher, used only where a publisher is named.
pub const PUBLISHER: &str = "Cyberdyne";

/// The window title, and the name the desktop environment shows.
pub const WINDOW_TITLE: &str = "CyberEngine";

/// One decoded identity image.
pub struct Artwork {
    /// The pixels, as egui wants them.
    image: Arc<egui::ColorImage>,
    /// The handle, once it has been uploaded. `None` until the first frame that draws it.
    handle: Option<egui::TextureHandle>,
    /// The size in points at which the artwork is drawn at scale 1.0.
    natural: egui::Vec2,
}

impl Artwork {
    /// Decode a PNG.
    ///
    /// Panics on a malformed image, and that is correct: these are three files compiled into the
    /// binary, so a failure here is a build that shipped a corrupt asset rather than anything a
    /// user did, and continuing with a blank header would hide it.
    #[expect(
        clippy::cast_precision_loss,
        reason = "the identity assets are a few hundred pixels across; an f32 is exact there"
    )]
    fn decode(bytes: &[u8]) -> Self {
        let decoded = image::load_from_memory(bytes)
            .expect("an identity asset compiled into the binary decodes")
            .into_rgba8();
        let (width, height) = decoded.dimensions();
        let image = egui::ColorImage::from_rgba_unmultiplied(
            [width as usize, height as usize],
            decoded.as_raw(),
        );
        Self {
            image: Arc::new(image),
            handle: None,
            // Half the pixel size: the sheet is high-resolution artwork, and drawing it at 1:1
            // would put a 422-point lockup in a header row twenty points tall.
            natural: egui::vec2(width as f32 / 2.0, height as f32 / 2.0),
        }
    }

    /// The texture, uploading it on first use.
    fn texture(&mut self, ctx: &egui::Context, name: &str) -> egui::TextureHandle {
        self.handle
            .get_or_insert_with(|| {
                ctx.load_texture(name, Arc::clone(&self.image), egui::TextureOptions::LINEAR)
            })
            .clone()
    }

    /// The size the artwork occupies when scaled to a given height.
    fn at_height(&self, height: f32) -> egui::Vec2 {
        let scale = height / self.natural.y;
        egui::vec2(self.natural.x * scale, height)
    }
}

/// The identity's artwork, decoded once.
pub struct Identity {
    horizontal: Artwork,
    monochrome: Artwork,
}

impl Default for Identity {
    fn default() -> Self {
        Self::new()
    }
}

impl Identity {
    /// Decode the identity assets.
    #[must_use]
    pub fn new() -> Self {
        Self {
            horizontal: Artwork::decode(HORIZONTAL),
            monochrome: Artwork::decode(MONOCHROME),
        }
    }

    /// Draw the horizontal lockup at a given height, in the theme's own treatment.
    ///
    /// Dark theme: the colour lockup, with its metallic gradient and emissive core intact, because
    /// that is what the mark is. Light theme: the monochrome lockup tinted with the primary text
    /// colour, because a mark rendered for a near-black backdrop is invisible on a near-white one
    /// and scaling the colour one there would be a misuse of it rather than a compromise.
    pub fn lockup(&mut self, ui: &mut egui::Ui, theme: cy_editor_visual::Theme, height: f32) {
        let dark = theme.mode == cy_editor_visual::colour::Mode::Dark;
        let (artwork, name, tint) = if dark {
            (
                &mut self.horizontal,
                "identity-horizontal",
                egui::Color32::WHITE,
            )
        } else {
            (
                &mut self.monochrome,
                "identity-monochrome",
                crate::theme::role(theme, cy_editor_visual::Semantic::PrimaryText),
            )
        };
        let size = artwork.at_height(height);
        let texture = artwork.texture(ui.ctx(), name);
        let response = ui.add(
            egui::Image::new(&texture)
                .fit_to_exact_size(size)
                .tint(tint),
        );
        // The lockup is the product's name, so it is what an assistive technology should read there
        // — and eframe ships AccessKit, which was one of the two properties that decided the
        // toolkit. An image with no accessible name would report nothing at all.
        response.on_hover_text(format!("{PRODUCT} — by {PUBLISHER}"));
    }
}

/// The window and taskbar icon.
///
/// Returned as pixels rather than as a path: the icon has to exist before there is a window to put
/// it on, and a file read at start-up is a failure mode a compiled-in asset does not have.
#[must_use]
pub fn window_icon() -> egui::IconData {
    let decoded = image::load_from_memory(MARK_256)
        .expect("the window icon compiled into the binary decodes")
        .into_rgba8();
    let (width, height) = decoded.dimensions();
    egui::IconData {
        rgba: decoded.into_raw(),
        width,
        height,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use cy_editor_visual::colour::{Semantic, Surface, Theme};

    #[test]
    fn the_three_assets_decode_and_are_the_shapes_they_are_used_at() {
        let icon = window_icon();
        assert_eq!(icon.width, 256);
        assert_eq!(icon.height, 256);
        assert_eq!(icon.rgba.len(), 256 * 256 * 4);

        for bytes in [HORIZONTAL, MONOCHROME] {
            let decoded = image::load_from_memory(bytes)
                .expect("decodes")
                .into_rgba8();
            assert!(
                decoded.width() > decoded.height(),
                "a lockup is wider than tall"
            );
        }
    }

    #[test]
    fn the_lockups_are_transparent_at_their_corners_rather_than_carrying_a_plate() {
        // The property `derive.py` exists to produce. A lockup with an opaque backdrop reads as a
        // badge stuck onto the header, which is the "header that picks up the logo" failure in its
        // most literal form.
        for bytes in [HORIZONTAL, MONOCHROME] {
            let decoded = image::load_from_memory(bytes)
                .expect("decodes")
                .into_rgba8();
            let (width, height) = decoded.dimensions();
            for (x, y) in [
                (0, 0),
                (width - 1, 0),
                (0, height - 1),
                (width - 1, height - 1),
            ] {
                assert_eq!(
                    decoded.get_pixel(x, y).0[3],
                    0,
                    "the lockup is opaque at ({x}, {y}); it would draw a plate on the header"
                );
            }
        }
    }

    #[test]
    fn the_monochrome_lockup_is_a_mask_and_can_therefore_be_tinted_to_either_theme() {
        let decoded = image::load_from_memory(MONOCHROME)
            .expect("decodes")
            .into_rgba8();
        for pixel in decoded.pixels() {
            assert_eq!(
                [pixel.0[0], pixel.0[1], pixel.0[2]],
                [255, 255, 255],
                "the monochrome lockup carries a colour, so tinting it cannot reach the light theme"
            );
        }
    }

    #[test]
    fn the_header_takes_no_colour_from_the_mark() {
        // The rule stated as a check: the chrome around the lockup is a surface from the theme, and
        // the theme's surfaces are neutral charcoal. If a future header reached for the logo's blue
        // this fails, which is the whole point of writing it down.
        for theme in Theme::ALL {
            let header = theme.surface(Surface::Window);
            let neutral = theme.colour(Semantic::Neutral);
            let spread = |value: cy_editor_visual::Rgb| {
                let channels = [value.red, value.green, value.blue];
                u32::from(*channels.iter().max().expect("three channels"))
                    - u32::from(*channels.iter().min().expect("three channels"))
            };
            assert!(
                spread(header) <= 8,
                "{theme:?}'s header surface is not neutral: {header:?}"
            );
            assert!(spread(neutral) <= 8, "{theme:?}'s neutral is not neutral");
        }
    }

    #[test]
    fn interface_text_names_the_product_and_the_publisher_correctly() {
        assert_eq!(PRODUCT, "CyberEngine");
        assert_eq!(PUBLISHER, "Cyberdyne");
        assert_eq!(WINDOW_TITLE, PRODUCT);
    }
}
