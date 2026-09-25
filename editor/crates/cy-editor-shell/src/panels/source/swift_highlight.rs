// SPDX-License-Identifier: MIT
//! Lightweight Swift coloring for the editable code buffer.

use egui::{Color32, FontId, TextFormat, Ui, text::LayoutJob};

const KEYWORDS: &[&str] = &[
    "actor",
    "as",
    "associatedtype",
    "async",
    "await",
    "break",
    "case",
    "catch",
    "class",
    "continue",
    "default",
    "defer",
    "do",
    "else",
    "enum",
    "extension",
    "false",
    "for",
    "func",
    "guard",
    "if",
    "import",
    "in",
    "init",
    "inout",
    "internal",
    "is",
    "let",
    "nil",
    "open",
    "operator",
    "override",
    "private",
    "protocol",
    "public",
    "repeat",
    "return",
    "self",
    "static",
    "struct",
    "super",
    "switch",
    "throw",
    "throws",
    "true",
    "try",
    "typealias",
    "var",
    "where",
    "while",
];

pub(super) fn highlight(source: &str, ui: &Ui) -> LayoutJob {
    let mut job = LayoutJob::default();
    let font = FontId::monospace(ui.text_style_height(&egui::TextStyle::Monospace));
    let ordinary = ui.visuals().text_color();
    let bytes = source.as_bytes();
    let mut index = 0;
    while index < bytes.len() {
        let start = index;
        let color = if bytes[index..].starts_with(b"//") {
            index = source[index..]
                .find('\n')
                .map_or(bytes.len(), |end| index + end);
            Color32::from_rgb(125, 154, 132)
        } else if bytes[index] == b'"' {
            index += 1;
            while index < bytes.len() {
                if bytes[index] == b'\\' {
                    index = (index + 2).min(bytes.len());
                } else if bytes[index] == b'"' {
                    index += 1;
                    break;
                } else {
                    index += 1;
                }
            }
            Color32::from_rgb(215, 169, 120)
        } else if bytes[index] == b'@' || bytes[index].is_ascii_alphabetic() || bytes[index] == b'_'
        {
            index += 1;
            while index < bytes.len()
                && (bytes[index].is_ascii_alphanumeric() || bytes[index] == b'_')
            {
                index += 1;
            }
            let word = &source[start..index];
            if word.starts_with('@') {
                Color32::from_rgb(218, 171, 108)
            } else if KEYWORDS.contains(&word) {
                Color32::from_rgb(170, 145, 225)
            } else {
                ordinary
            }
        } else if bytes[index].is_ascii_digit() {
            index += 1;
            while index < bytes.len() && (bytes[index].is_ascii_digit() || bytes[index] == b'.') {
                index += 1;
            }
            Color32::from_rgb(145, 195, 220)
        } else {
            index += source[index..].chars().next().unwrap().len_utf8();
            ordinary
        };
        job.append(
            &source[start..index],
            0.0,
            TextFormat::simple(font.clone(), color),
        );
    }
    job
}
