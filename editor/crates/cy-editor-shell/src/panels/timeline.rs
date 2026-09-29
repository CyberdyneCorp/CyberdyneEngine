// SPDX-License-Identifier: MIT
//! The one timeline widget: tracks, keys and clips, a ruler with the playhead, zoom and selection.
//!
//! `cy_editor_interface::specialised::timeline::TimelineSurface` is the one keyed-time model every
//! time-based editor shares (animation clips, the sequencer, audio cues). This is its one view, so
//! those editors share an editing experience as well as a model.
//!
//! The widget reads the surface and never writes it. What a person does comes back as a
//! [`TimelineResponse`]: a playhead position to scrub to, and [`TimelineEdit`]s. Each edit is one
//! completed gesture — a key drag is one [`TimelineEdit::MoveKey`] on release, not one per frame —
//! and [`TimelineEdit::apply`] answers its exact inverse, which is what a host records to make the
//! gesture one undoable transaction (and what an MCP command replays). Escape during a drag cancels
//! it and records nothing. Zoom, scroll and selection
//! are presentation state in [`TimelineView`] and are never transactions.

use std::collections::BTreeSet;

use cy_editor_core::problem::{Problem, Result};
use cy_editor_interface::shell::Shell;
use cy_editor_interface::specialised::timeline::{
    Key, KeyId, SectionId, TimelineSurface, Track, TrackId,
};
use cy_editor_visual::colour::{Semantic, Surface};
use cy_editor_visual::density::TextRole;

use crate::theme;

/// Width of the track-label column, in points.
pub(super) const LABEL_WIDTH: f32 = 140.0;
/// Height of the ruler the playhead is scrubbed on.
pub(super) const RULER_HEIGHT: f32 = 22.0;
/// Height of one track row.
pub(super) const ROW_HEIGHT: f32 = 24.0;
/// The zoom range, in points per second.
pub(super) const ZOOM_RANGE: (f32, f32) = (8.0, 2000.0);
const KEY_RADIUS: f32 = 5.0;
const EDGE_GRAB: f32 = 6.0;
/// Ruler tick spacings, in seconds; the first that leaves this many points between ticks is used.
const TICK_STEPS: [f64; 10] = [0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0];
const MIN_TICK_SPACING: f32 = 56.0;

/// One authoring change on a timeline. Each variant is one gesture and one transaction.
#[derive(Clone, Copy, PartialEq, Debug)]
pub(super) enum TimelineEdit {
    /// Key a value at an empty time on a track.
    AddKey {
        /// The track keyed.
        track: TrackId,
        /// Seconds from the start.
        time: f64,
        /// The value keyed.
        value: f64,
    },
    /// Remove one key.
    RemoveKey {
        /// The key's track.
        track: TrackId,
        /// The key removed.
        key: KeyId,
    },
    /// Put a removed key back with its identity: the inverse of [`TimelineEdit::RemoveKey`].
    RestoreKey {
        /// The key's track.
        track: TrackId,
        /// The key, whole.
        key: Key,
    },
    /// Move one key in time.
    MoveKey {
        /// The key's track.
        track: TrackId,
        /// The key moved.
        key: KeyId,
        /// Where it goes, in seconds.
        to: f64,
    },
    /// Move a clip's start and end.
    TrimSection {
        /// The clip's track.
        track: TrackId,
        /// The clip.
        section: SectionId,
        /// Its new start, in seconds.
        start: f64,
        /// Its new end, in seconds.
        end: f64,
    },
}

impl TimelineEdit {
    /// Apply the edit and answer the edit that undoes it. A refused edit changes nothing.
    pub(super) fn apply(self, surface: &mut TimelineSurface) -> Result<TimelineEdit> {
        match self {
            TimelineEdit::AddKey { track, time, value } => {
                if track_of(surface, track)?
                    .keys
                    .iter()
                    .any(|key| key.time.total_cmp(&time).is_eq())
                {
                    return Err(Problem::new(
                        format!("add a key at {time}"),
                        "the track already has a key at that time, and adding one would silently \
                         overwrite it",
                    )
                    .with_remedy("move or edit the key that is there"));
                }
                let key = surface.key(track, time, value)?;
                Ok(TimelineEdit::RemoveKey { track, key })
            }
            TimelineEdit::RemoveKey { track, key } => {
                let key = surface.remove_key(track, key)?;
                Ok(TimelineEdit::RestoreKey { track, key })
            }
            TimelineEdit::RestoreKey { track, key } => {
                surface.restore_key(track, key)?;
                Ok(TimelineEdit::RemoveKey { track, key: key.id })
            }
            TimelineEdit::MoveKey { track, key, to } => {
                let from = surface.move_key(track, key, to)?;
                Ok(TimelineEdit::MoveKey {
                    track,
                    key,
                    to: from,
                })
            }
            TimelineEdit::TrimSection {
                track,
                section,
                start,
                end,
            } => {
                let before = track_of(surface, track)?
                    .sections
                    .iter()
                    .find(|candidate| candidate.id == section)
                    .map(|candidate| (candidate.start, candidate.end));
                surface.trim(track, section, start, end)?;
                let (start, end) = before.expect("trim refuses a section the track lacks");
                Ok(TimelineEdit::TrimSection {
                    track,
                    section,
                    start,
                    end,
                })
            }
        }
    }
}

fn track_of(surface: &TimelineSurface, id: TrackId) -> Result<&Track> {
    surface.track(id).ok_or_else(|| {
        Problem::new(
            format!("edit track {}", id.ordinal()),
            "the timeline holds no track with that identity",
        )
    })
}

/// Something selected on the timeline.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub(super) enum Selected {
    /// A key, by its track and its own identity.
    Key(TrackId, KeyId),
    /// A clip, by its track and its own identity.
    Section(TrackId, SectionId),
}

#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
enum Edge {
    Start,
    End,
}

#[derive(Clone, Copy, PartialEq, Debug)]
enum Drag {
    Key {
        track: TrackId,
        key: KeyId,
        from: f64,
        to: f64,
    },
    Edge {
        track: TrackId,
        section: SectionId,
        edge: Edge,
        start: f64,
        end: f64,
    },
}

/// The widget's presentation state: zoom, scroll, selection and the gesture in progress.
#[derive(Clone, PartialEq, Debug)]
pub(super) struct TimelineView {
    /// Horizontal zoom, in points per second.
    pub pixels_per_second: f32,
    /// The time at the left edge of the lanes, in seconds.
    pub scroll: f64,
    /// What is selected.
    pub selection: BTreeSet<Selected>,
    drag: Option<Drag>,
}

impl Default for TimelineView {
    fn default() -> Self {
        Self {
            pixels_per_second: 100.0,
            scroll: 0.0,
            selection: BTreeSet::new(),
            drag: None,
        }
    }
}

impl TimelineView {
    /// The x coordinate of a time, for lanes starting at `left`.
    pub(super) fn x_of(&self, time: f64, left: f32) -> f32 {
        left + points((time - self.scroll) * f64::from(self.pixels_per_second))
    }

    /// The time under an x coordinate, for lanes starting at `left`.
    pub(super) fn time_at(&self, x: f32, left: f32) -> f64 {
        self.scroll + f64::from(x - left) / f64::from(self.pixels_per_second)
    }

    /// Zoom by `factor`, keeping the time under `anchor_x` where it is.
    pub(super) fn zoom_about(&mut self, factor: f32, anchor_x: f32, left: f32) {
        let anchor = self.time_at(anchor_x, left);
        self.pixels_per_second =
            (self.pixels_per_second * factor).clamp(ZOOM_RANGE.0, ZOOM_RANGE.1);
        self.scroll = anchor - f64::from(anchor_x - left) / f64::from(self.pixels_per_second);
    }

    fn select(&mut self, item: Selected, additive: bool) {
        if !additive {
            self.selection.clear();
            self.selection.insert(item);
        } else if !self.selection.remove(&item) {
            self.selection.insert(item);
        }
    }
}

/// What one frame of the widget asks its host to do.
#[derive(Clone, Default, PartialEq, Debug)]
pub(super) struct TimelineResponse {
    /// Move the playhead here. Preview state, not a transaction.
    pub scrub: Option<f64>,
    /// Completed gestures, each one transaction.
    pub edits: Vec<TimelineEdit>,
}

#[expect(
    clippy::cast_possible_truncation,
    reason = "a timeline coordinate is a screen position; f32 holds any a display can show"
)]
fn points(value: f64) -> f32 {
    value as f32
}

/// Where each part of the widget is this frame.
struct Layout {
    ruler: egui::Rect,
    lanes_left: f32,
    right: f32,
    rows_top: f32,
}

impl Layout {
    fn row(&self, index: usize) -> egui::Rect {
        let top = self.rows_top + theme::points(index) * ROW_HEIGHT;
        egui::Rect::from_min_max(
            egui::pos2(self.lanes_left, top),
            egui::pos2(self.right, top + ROW_HEIGHT),
        )
    }
}

/// Draw the timeline and answer what the person did.
pub(super) fn show(
    ui: &mut egui::Ui,
    shell: &Shell,
    surface: &TimelineSurface,
    view: &mut TimelineView,
) -> TimelineResponse {
    let tracks = surface.tracks();
    let height = RULER_HEIGHT + theme::points(tracks.len().max(1)) * ROW_HEIGHT;
    let (rect, _) = ui.allocate_exact_size(
        egui::vec2(ui.available_width(), height),
        egui::Sense::hover(),
    );
    let layout = Layout {
        ruler: egui::Rect::from_min_max(
            egui::pos2(rect.left() + LABEL_WIDTH, rect.top()),
            egui::pos2(rect.right(), rect.top() + RULER_HEIGHT),
        ),
        lanes_left: rect.left() + LABEL_WIDTH,
        right: rect.right(),
        rows_top: rect.top() + RULER_HEIGHT,
    };
    let painter = ui.painter_at(rect);
    painter.rect_filled(rect, 2.0, theme::surface(shell.theme, Surface::Sunken));
    painter.rect_filled(
        layout.ruler,
        0.0,
        theme::surface(shell.theme, Surface::Raised),
    );

    let mut response = TimelineResponse::default();
    if ui.input(|input| input.key_pressed(egui::Key::Escape)) {
        // A cancelled gesture records nothing: the release that follows finds no drag to finish.
        view.drag = None;
    }
    navigate(ui, rect, &layout, view);
    response.scrub = ruler(ui, shell, &painter, &layout, surface, view);
    for (index, track) in tracks.iter().enumerate() {
        track_row(
            ui,
            shell,
            &painter,
            &layout,
            index,
            track,
            surface,
            view,
            &mut response,
        );
    }
    if ui.rect_contains_pointer(rect) && ui.input(|input| input.key_pressed(egui::Key::Delete)) {
        response
            .edits
            .extend(view.selection.iter().filter_map(|item| match *item {
                Selected::Key(track, key) => Some(TimelineEdit::RemoveKey { track, key }),
                Selected::Section(..) => None,
            }));
    }
    let playhead = view.x_of(surface.playhead(), layout.lanes_left);
    if playhead >= layout.lanes_left {
        painter.line_segment(
            [
                egui::pos2(playhead, rect.top()),
                egui::pos2(playhead, rect.bottom()),
            ],
            egui::Stroke::new(1.5, theme::role(shell.theme, Semantic::Live)),
        );
    }
    response
}

/// Zoom with a pinch or Ctrl+scroll about the pointer; pan with a plain horizontal scroll.
fn navigate(ui: &egui::Ui, rect: egui::Rect, layout: &Layout, view: &mut TimelineView) {
    let Some(pointer) = ui.input(|input| input.pointer.hover_pos()) else {
        return;
    };
    if !rect.contains(pointer) {
        return;
    }
    let (zoom, pan) = ui.input(|input| (input.zoom_delta(), input.smooth_scroll_delta.x));
    if (zoom - 1.0).abs() > f32::EPSILON {
        view.zoom_about(zoom, pointer.x.max(layout.lanes_left), layout.lanes_left);
    } else if pan.abs() > f32::EPSILON {
        view.scroll -= f64::from(pan) / f64::from(view.pixels_per_second);
    }
}

fn ruler(
    ui: &mut egui::Ui,
    shell: &Shell,
    painter: &egui::Painter,
    layout: &Layout,
    surface: &TimelineSurface,
    view: &TimelineView,
) -> Option<f64> {
    let step = TICK_STEPS
        .into_iter()
        .find(|step| points(step * f64::from(view.pixels_per_second)) >= MIN_TICK_SPACING)
        .unwrap_or(TICK_STEPS[TICK_STEPS.len() - 1]);
    let text = theme::role(shell.theme, Semantic::SecondaryText);
    let mut tick = (view.scroll / step).ceil() * step;
    while view.x_of(tick, layout.lanes_left) <= layout.right {
        let x = view.x_of(tick, layout.lanes_left);
        painter.line_segment(
            [
                egui::pos2(x, layout.ruler.bottom() - 6.0),
                egui::pos2(x, layout.ruler.bottom()),
            ],
            egui::Stroke::new(1.0, text),
        );
        painter.text(
            egui::pos2(x + 3.0, layout.ruler.top() + 2.0),
            egui::Align2::LEFT_TOP,
            format!("{tick:.2}s"),
            egui::FontId::monospace(shell.metrics().text(TextRole::Secondary)),
            text,
        );
        tick += step;
    }
    for marker in surface.markers() {
        let x = view.x_of(marker.time, layout.lanes_left);
        painter.line_segment(
            [
                egui::pos2(x, layout.ruler.top()),
                egui::pos2(x, layout.ruler.bottom()),
            ],
            egui::Stroke::new(1.0, theme::role(shell.theme, Semantic::Warning)),
        );
    }
    let scrub = ui.interact(
        layout.ruler,
        ui.id().with("timeline-ruler"),
        egui::Sense::click_and_drag(),
    );
    if scrub.clicked() || scrub.dragged() {
        let x = scrub.interact_pointer_pos()?.x;
        return Some(
            view.time_at(x, layout.lanes_left)
                .clamp(0.0, surface.duration()),
        );
    }
    None
}

#[expect(
    clippy::too_many_arguments,
    reason = "one row reads the whole frame's context; bundling it would only rename the list"
)]
fn track_row(
    ui: &mut egui::Ui,
    shell: &Shell,
    painter: &egui::Painter,
    layout: &Layout,
    index: usize,
    track: &Track,
    surface: &TimelineSurface,
    view: &mut TimelineView,
    response: &mut TimelineResponse,
) {
    let row = layout.row(index);
    painter.line_segment(
        [
            egui::pos2(row.left() - LABEL_WIDTH, row.bottom()),
            egui::pos2(row.right(), row.bottom()),
        ],
        egui::Stroke::new(0.5, theme::role(shell.theme, Semantic::Neutral)),
    );
    painter.text(
        egui::pos2(row.left() - LABEL_WIDTH + 8.0, row.center().y),
        egui::Align2::LEFT_CENTER,
        format!("{} · {}", track.label, track.kind.name()),
        egui::FontId::proportional(shell.metrics().text(TextRole::Secondary)),
        theme::role(
            shell.theme,
            if track.locked {
                Semantic::SecondaryText
            } else {
                Semantic::PrimaryText
            },
        ),
    );
    let lane = ui.interact(
        row,
        ui.id().with(("timeline-lane", track.id.ordinal())),
        egui::Sense::click(),
    );
    let additive = ui.input(|input| input.modifiers.shift);
    if lane.double_clicked()
        && track.kind.is_keyed()
        && let Some(position) = lane.interact_pointer_pos()
    {
        let time = view
            .time_at(position.x, layout.lanes_left)
            .clamp(0.0, surface.duration());
        let value = surface.sample(track.id, time).unwrap_or(0.0);
        response.edits.push(TimelineEdit::AddKey {
            track: track.id,
            time,
            value,
        });
    } else if lane.clicked() && !additive {
        view.selection.clear();
    }
    for section in &track.sections {
        clip(
            ui, shell, painter, layout, row, track.id, section, view, response,
        );
    }
    for key in &track.keys {
        key_diamond(
            ui, shell, painter, layout, row, track.id, key, view, response,
        );
    }
}

#[expect(
    clippy::too_many_arguments,
    reason = "one clip reads the whole frame's context; bundling it would only rename the list"
)]
fn clip(
    ui: &mut egui::Ui,
    shell: &Shell,
    painter: &egui::Painter,
    layout: &Layout,
    row: egui::Rect,
    track: TrackId,
    section: &cy_editor_interface::specialised::timeline::Section,
    view: &mut TimelineView,
    response: &mut TimelineResponse,
) {
    let (start, end) = match view.drag {
        Some(Drag::Edge {
            section: dragged,
            start,
            end,
            ..
        }) if dragged == section.id => (start, end),
        _ => (section.start, section.end),
    };
    let body = egui::Rect::from_min_max(
        egui::pos2(view.x_of(start, layout.lanes_left), row.top() + 4.0),
        egui::pos2(view.x_of(end, layout.lanes_left), row.bottom() - 4.0),
    );
    if body.right() < layout.lanes_left || body.left() > layout.right {
        return;
    }
    let item = Selected::Section(track, section.id);
    let ordinal = section.id.ordinal();
    let hit = ui.interact(
        body.shrink2(egui::vec2(EDGE_GRAB, 0.0)),
        ui.id().with(("timeline-clip", ordinal)),
        egui::Sense::click(),
    );
    if hit.clicked() {
        view.select(item, ui.input(|input| input.modifiers.shift));
    }
    for (edge, x) in [(Edge::Start, body.left()), (Edge::End, body.right())] {
        let grip = ui.interact(
            egui::Rect::from_center_size(
                egui::pos2(x, body.center().y),
                egui::vec2(EDGE_GRAB * 2.0, body.height()),
            ),
            ui.id().with(("timeline-clip-edge", ordinal, edge)),
            egui::Sense::drag(),
        );
        trim_gesture(&grip, edge, track, section, layout, view, response);
    }
    let lane = layout.row(0).x_range();
    let visible = egui::Rect::from_x_y_ranges(
        body.left().max(lane.min)..=body.right().min(lane.max),
        body.y_range(),
    );
    painter.rect_filled(
        visible,
        2.0,
        theme::lifted(shell.theme, Surface::Raised, 0.04),
    );
    painter.rect_stroke(
        visible,
        2.0,
        egui::Stroke::new(
            1.0,
            theme::role(
                shell.theme,
                if view.selection.contains(&item) {
                    Semantic::Selection
                } else {
                    Semantic::Active
                },
            ),
        ),
        egui::StrokeKind::Inside,
    );
    painter.with_clip_rect(visible).text(
        visible.left_center() + egui::vec2(6.0, 0.0),
        egui::Align2::LEFT_CENTER,
        &section.subject,
        egui::FontId::proportional(shell.metrics().text(TextRole::Secondary)),
        theme::role(shell.theme, Semantic::PrimaryText),
    );
}

/// One clip-edge drag: the edge follows the pointer, and release is one trim.
fn trim_gesture(
    grip: &egui::Response,
    edge: Edge,
    track: TrackId,
    section: &cy_editor_interface::specialised::timeline::Section,
    layout: &Layout,
    view: &mut TimelineView,
    response: &mut TimelineResponse,
) {
    if grip.drag_started() {
        view.drag = Some(Drag::Edge {
            track,
            section: section.id,
            edge,
            start: section.start,
            end: section.end,
        });
    }
    if grip.dragged()
        && let Some(position) = grip.interact_pointer_pos()
    {
        let time = view.time_at(position.x, layout.lanes_left).max(0.0);
        if let Some(Drag::Edge {
            section: dragged,
            edge: dragging,
            start,
            end,
            ..
        }) = view.drag.as_mut()
            && *dragged == section.id
            && *dragging == edge
        {
            match edge {
                Edge::Start => *start = time,
                Edge::End => *end = time,
            }
        }
    }
    if grip.drag_stopped()
        && let Some(Drag::Edge {
            track, start, end, ..
        }) = view.drag.take()
        && (start.total_cmp(&section.start).is_ne() || end.total_cmp(&section.end).is_ne())
    {
        response.edits.push(TimelineEdit::TrimSection {
            track,
            section: section.id,
            start,
            end,
        });
    }
}

#[expect(
    clippy::too_many_arguments,
    reason = "one key reads the whole frame's context; bundling it would only rename the list"
)]
fn key_diamond(
    ui: &mut egui::Ui,
    shell: &Shell,
    painter: &egui::Painter,
    layout: &Layout,
    row: egui::Rect,
    track: TrackId,
    key: &Key,
    view: &mut TimelineView,
    response: &mut TimelineResponse,
) {
    let time = match view.drag {
        Some(Drag::Key {
            key: dragged, to, ..
        }) if dragged == key.id => to,
        _ => key.time,
    };
    let centre = egui::pos2(view.x_of(time, layout.lanes_left), row.center().y);
    if centre.x < layout.lanes_left || centre.x > layout.right {
        return;
    }
    let hit = ui.interact(
        egui::Rect::from_center_size(centre, egui::Vec2::splat(KEY_RADIUS * 2.0 + 4.0)),
        ui.id().with(("timeline-key", key.id.ordinal())),
        egui::Sense::click_and_drag(),
    );
    let item = Selected::Key(track, key.id);
    if hit.clicked() {
        view.select(item, ui.input(|input| input.modifiers.shift));
    }
    if hit.drag_started() {
        view.drag = Some(Drag::Key {
            track,
            key: key.id,
            from: key.time,
            to: key.time,
        });
    }
    if hit.dragged()
        && let Some(position) = hit.interact_pointer_pos()
    {
        let time = view.time_at(position.x, layout.lanes_left).max(0.0);
        if let Some(Drag::Key { to, .. }) = view.drag.as_mut() {
            *to = time;
        }
    }
    if hit.drag_stopped()
        && let Some(Drag::Key {
            track,
            key,
            from,
            to,
        }) = view.drag.take()
        && from.total_cmp(&to).is_ne()
    {
        response
            .edits
            .push(TimelineEdit::MoveKey { track, key, to });
    }
    let selected = view.selection.contains(&item);
    let role = if selected {
        Semantic::Selection
    } else {
        Semantic::Active
    };
    let colour = theme::role(shell.theme, role);
    let diamond = vec![
        centre + egui::vec2(0.0, -KEY_RADIUS),
        centre + egui::vec2(KEY_RADIUS, 0.0),
        centre + egui::vec2(0.0, KEY_RADIUS),
        centre + egui::vec2(-KEY_RADIUS, 0.0),
    ];
    painter.add(egui::Shape::convex_polygon(
        diamond,
        colour,
        egui::Stroke::new(1.0, theme::role(shell.theme, Semantic::PrimaryText)),
    ));
}

#[cfg(test)]
mod tests {
    use cy_editor_commands::Registry;
    use cy_editor_interface::specialised::timeline::TrackKind;

    use super::*;

    fn surface() -> (TimelineSurface, TrackId, KeyId, TrackId, SectionId) {
        let mut surface = TimelineSurface::new(1, 30.0).unwrap();
        surface.load(10.0);
        let curve = surface.add_track(TrackKind::Property, "intensity");
        let key = surface.key(curve, 1.0, 0.5).unwrap();
        surface.key(curve, 3.0, 1.0).unwrap();
        let clips = surface.add_track(TrackKind::Animation, "walk");
        let clip = surface
            .add_section(clips, 2.0, 4.0, "clips/walk.cyanim")
            .unwrap();
        (surface, curve, key, clips, clip)
    }

    // --- The view's arithmetic ------------------------------------------------------------------

    #[test]
    fn time_and_x_are_inverse_at_any_zoom_and_scroll() {
        let mut view = TimelineView {
            scroll: 1.5,
            ..TimelineView::default()
        };
        for pps in [ZOOM_RANGE.0, 100.0, ZOOM_RANGE.1] {
            view.pixels_per_second = pps;
            for time in [0.0, 1.5, 2.25, 9.0] {
                let x = view.x_of(time, 140.0);
                assert!((view.time_at(x, 140.0) - time).abs() < 1e-3, "{pps} {time}");
            }
        }
        view.pixels_per_second = 100.0;
        assert!((view.x_of(2.5, 140.0) - 240.0).abs() < 1e-3);
    }

    #[test]
    fn zoom_keeps_the_time_under_the_pointer_and_stays_in_range() {
        let mut view = TimelineView::default();
        let anchor_x = 400.0;
        let before = view.time_at(anchor_x, 140.0);
        view.zoom_about(2.0, anchor_x, 140.0);
        assert!((view.pixels_per_second - 200.0).abs() < 1e-3);
        assert!((view.time_at(anchor_x, 140.0) - before).abs() < 1e-6);
        view.zoom_about(1.0e6, anchor_x, 140.0);
        assert!((view.pixels_per_second - ZOOM_RANGE.1).abs() < f32::EPSILON);
        view.zoom_about(1.0e-6, anchor_x, 140.0);
        assert!((view.pixels_per_second - ZOOM_RANGE.0).abs() < f32::EPSILON);
    }

    // --- Edits are transactions: each answers its exact inverse --------------------------------

    #[test]
    fn every_edit_is_undone_exactly_by_the_edit_it_answers() {
        let (original, curve, key, clips, clip) = surface();
        let edits = [
            TimelineEdit::AddKey {
                track: curve,
                time: 2.0,
                value: 0.75,
            },
            TimelineEdit::RemoveKey { track: curve, key },
            TimelineEdit::MoveKey {
                track: curve,
                key,
                to: 5.0,
            },
            TimelineEdit::TrimSection {
                track: clips,
                section: clip,
                start: 1.0,
                end: 6.0,
            },
        ];
        // What an author sees: every track, key and clip with its identity. The surface's
        // identity counter is deliberately not compared — an undone key's identity is not reused.
        let content = |surface: &TimelineSurface| -> Vec<Track> {
            surface.tracks().into_iter().cloned().collect()
        };
        for edit in edits {
            let mut surface = original.clone();
            let inverse = edit.apply(&mut surface).expect("the edit applies");
            let applied = content(&surface);
            assert_ne!(applied, content(&original), "{edit:?} changed nothing");
            let redo = inverse.apply(&mut surface).expect("its inverse applies");
            assert_eq!(
                content(&surface),
                content(&original),
                "{edit:?} was not undone by {inverse:?}"
            );
            redo.apply(&mut surface).expect("and redoes");
            assert_eq!(content(&surface), applied, "{edit:?} did not redo");
        }
    }

    #[test]
    fn a_refused_edit_changes_nothing() {
        let (original, curve, key, clips, clip) = surface();
        let mut surface = original.clone();
        for edit in [
            TimelineEdit::AddKey {
                track: curve,
                time: 3.0,
                value: 9.0,
            },
            TimelineEdit::MoveKey {
                track: curve,
                key,
                to: 3.0,
            },
            TimelineEdit::TrimSection {
                track: clips,
                section: clip,
                start: 4.0,
                end: 2.0,
            },
        ] {
            assert!(edit.apply(&mut surface).is_err(), "{edit:?} was accepted");
            assert_eq!(surface, original, "{edit:?} was refused but left a change");
        }
        surface.set_locked(curve, true).unwrap();
        let locked = surface.clone();
        let edit = TimelineEdit::RemoveKey { track: curve, key };
        assert!(edit.apply(&mut surface).is_err());
        assert_eq!(surface, locked);
    }

    // --- The widget driven through real egui frames -------------------------------------------

    struct Frames {
        ctx: egui::Context,
        shell: Shell,
        time: f64,
    }

    impl Frames {
        fn new() -> Self {
            Self {
                ctx: egui::Context::default(),
                shell: Shell::new(&Registry::new()).expect("shell"),
                time: 0.0,
            }
        }

        /// One frame; answers the widget's response and its lanes' left edge and top.
        fn run(
            &mut self,
            events: Vec<egui::Event>,
            surface: &TimelineSurface,
            view: &mut TimelineView,
        ) -> (TimelineResponse, egui::Pos2) {
            self.time += 1.0 / 30.0;
            let raw = egui::RawInput {
                screen_rect: Some(egui::Rect::from_min_size(
                    egui::Pos2::ZERO,
                    egui::vec2(1200.0, 400.0),
                )),
                time: Some(self.time),
                events,
                ..Default::default()
            };
            let mut answer = TimelineResponse::default();
            let mut origin = egui::Pos2::ZERO;
            let mut output = self.ctx.run_ui(raw, |ui| {
                egui::CentralPanel::default().show(ui, |ui| {
                    origin = ui.available_rect_before_wrap().min;
                    answer = show(ui, &self.shell, surface, view);
                });
            });
            output.textures_delta.clear();
            (answer, origin + egui::vec2(LABEL_WIDTH, RULER_HEIGHT))
        }

        fn quiet(
            &mut self,
            surface: &TimelineSurface,
            view: &mut TimelineView,
        ) -> (TimelineResponse, egui::Pos2) {
            self.run(Vec::new(), surface, view)
        }

        fn pointer(
            &mut self,
            at: egui::Pos2,
            pressed: Option<bool>,
            surface: &TimelineSurface,
            view: &mut TimelineView,
        ) -> TimelineResponse {
            let mut events = vec![egui::Event::PointerMoved(at)];
            if let Some(pressed) = pressed {
                events.push(egui::Event::PointerButton {
                    pos: at,
                    button: egui::PointerButton::Primary,
                    pressed,
                    modifiers: egui::Modifiers::NONE,
                });
            }
            self.run(events, surface, view).0
        }

        fn click(
            &mut self,
            at: egui::Pos2,
            surface: &TimelineSurface,
            view: &mut TimelineView,
        ) -> TimelineResponse {
            let pressed = self.pointer(at, Some(true), surface, view);
            let released = self.pointer(at, Some(false), surface, view);
            merge(pressed, released)
        }
    }

    fn merge(mut first: TimelineResponse, second: TimelineResponse) -> TimelineResponse {
        first.scrub = second.scrub.or(first.scrub);
        first.edits.extend(second.edits);
        first
    }

    /// Where a time is on row `row`, for lanes whose top-left is `lanes`.
    fn at(view: &TimelineView, lanes: egui::Pos2, time: f64, row: usize) -> egui::Pos2 {
        egui::pos2(
            view.x_of(time, lanes.x),
            lanes.y + (theme::points(row) + 0.5) * ROW_HEIGHT,
        )
    }

    #[test]
    fn clicking_the_ruler_scrubs_without_an_edit() {
        let (surface, ..) = surface();
        let mut view = TimelineView::default();
        let mut frames = Frames::new();
        let (_, lanes) = frames.quiet(&surface, &mut view);
        let ruler = egui::pos2(view.x_of(2.5, lanes.x), lanes.y - RULER_HEIGHT * 0.5);
        let response = frames.click(ruler, &surface, &mut view);
        let scrub = response.scrub.expect("the ruler scrubs");
        assert!((scrub - 2.5).abs() < 0.02, "{scrub}");
        assert!(response.edits.is_empty(), "scrubbing is not a transaction");
    }

    #[test]
    fn dragging_a_key_is_one_move_on_release_and_leaves_the_surface_alone() {
        let (surface, curve, key, ..) = surface();
        let before = surface.clone();
        let mut view = TimelineView::default();
        let mut frames = Frames::new();
        let (_, lanes) = frames.quiet(&surface, &mut view);
        let grab = at(&view, lanes, 1.0, 0);
        let mut edits = Vec::new();
        edits.extend(frames.pointer(grab, Some(true), &surface, &mut view).edits);
        for step in 1..=4 {
            let to = grab + egui::vec2(25.0 * theme::points(step), 0.0);
            edits.extend(frames.pointer(to, None, &surface, &mut view).edits);
        }
        let drop = grab + egui::vec2(100.0, 0.0);
        edits.extend(frames.pointer(drop, Some(false), &surface, &mut view).edits);
        edits.extend(frames.quiet(&surface, &mut view).0.edits);

        assert_eq!(surface, before, "the widget never writes the surface");
        let [
            TimelineEdit::MoveKey {
                track,
                key: moved,
                to,
            },
        ] = edits.as_slice()
        else {
            panic!("one release is one move: {edits:?}");
        };
        assert_eq!((*track, *moved), (curve, key));
        assert!(
            (to - 2.0).abs() < 0.02,
            "a hundred points at 100/s is a second: {to}"
        );
    }

    #[test]
    fn escape_mid_drag_cancels_the_gesture_and_records_nothing() {
        let (surface, ..) = surface();
        let mut view = TimelineView::default();
        let mut frames = Frames::new();
        let (_, lanes) = frames.quiet(&surface, &mut view);
        let grab = at(&view, lanes, 1.0, 0);
        let mut edits = Vec::new();
        edits.extend(frames.pointer(grab, Some(true), &surface, &mut view).edits);
        for step in 1..=3 {
            let to = grab + egui::vec2(25.0 * theme::points(step), 0.0);
            edits.extend(frames.pointer(to, None, &surface, &mut view).edits);
        }
        let escape = egui::Event::Key {
            key: egui::Key::Escape,
            physical_key: Some(egui::Key::Escape),
            pressed: true,
            repeat: false,
            modifiers: egui::Modifiers::NONE,
        };
        let held = grab + egui::vec2(75.0, 0.0);
        edits.extend(
            frames
                .run(
                    vec![egui::Event::PointerMoved(held), escape],
                    &surface,
                    &mut view,
                )
                .0
                .edits,
        );
        edits.extend(frames.pointer(held, Some(false), &surface, &mut view).edits);
        assert!(
            edits.is_empty(),
            "a cancelled drag is no transaction: {edits:?}"
        );
    }

    #[test]
    fn a_selected_key_is_removed_by_delete() {
        let (surface, curve, key, ..) = surface();
        let mut view = TimelineView::default();
        let mut frames = Frames::new();
        let (_, lanes) = frames.quiet(&surface, &mut view);
        let point = at(&view, lanes, 1.0, 0);
        assert!(frames.click(point, &surface, &mut view).edits.is_empty());
        assert!(view.selection.contains(&Selected::Key(curve, key)));
        let delete = egui::Event::Key {
            key: egui::Key::Delete,
            physical_key: Some(egui::Key::Delete),
            pressed: true,
            repeat: false,
            modifiers: egui::Modifiers::NONE,
        };
        let (response, _) = frames.run(
            vec![egui::Event::PointerMoved(point), delete],
            &surface,
            &mut view,
        );
        assert_eq!(
            response.edits,
            [TimelineEdit::RemoveKey { track: curve, key }]
        );
    }

    #[test]
    fn double_clicking_a_keyed_lane_adds_one_key_at_the_sampled_value() {
        let (surface, curve, ..) = surface();
        let mut view = TimelineView::default();
        let mut frames = Frames::new();
        let (_, lanes) = frames.quiet(&surface, &mut view);
        let point = at(&view, lanes, 2.0, 0);
        let first = frames.click(point, &surface, &mut view);
        let second = frames.click(point, &surface, &mut view);
        let edits: Vec<_> = first.edits.into_iter().chain(second.edits).collect();
        let [TimelineEdit::AddKey { track, time, value }] = edits.as_slice() else {
            panic!("a double click is one key: {edits:?}");
        };
        assert_eq!(*track, curve);
        assert!((time - 2.0).abs() < 0.02, "{time}");
        let expected = surface.sample(curve, *time).unwrap();
        assert!(
            (value - expected).abs() < 1e-9,
            "keyed on the curve, not at zero"
        );
    }

    #[test]
    fn dragging_a_clip_end_is_one_trim_on_release() {
        let (surface, _, _, clips, clip) = surface();
        let mut view = TimelineView::default();
        let mut frames = Frames::new();
        let (_, lanes) = frames.quiet(&surface, &mut view);
        let grab = at(&view, lanes, 4.0, 1);
        let mut edits = Vec::new();
        edits.extend(frames.pointer(grab, Some(true), &surface, &mut view).edits);
        for step in 1..=5 {
            let to = grab + egui::vec2(20.0 * theme::points(step), 0.0);
            edits.extend(frames.pointer(to, None, &surface, &mut view).edits);
        }
        let drop = grab + egui::vec2(100.0, 0.0);
        edits.extend(frames.pointer(drop, Some(false), &surface, &mut view).edits);
        let [
            TimelineEdit::TrimSection {
                track,
                section,
                start,
                end,
            },
        ] = edits.as_slice()
        else {
            panic!("one release is one trim: {edits:?}");
        };
        assert_eq!((*track, *section), (clips, clip));
        assert!((start - 2.0).abs() < 1e-9, "the start edge stays: {start}");
        assert!(
            (end - 5.0).abs() < 0.02,
            "the end follows the pointer: {end}"
        );
    }

    #[test]
    fn a_pinch_over_the_lanes_zooms_about_the_pointer() {
        let (surface, ..) = surface();
        let mut view = TimelineView::default();
        let mut frames = Frames::new();
        let (_, lanes) = frames.quiet(&surface, &mut view);
        let point = at(&view, lanes, 3.0, 0);
        let (response, _) = frames.run(
            vec![egui::Event::PointerMoved(point), egui::Event::Zoom(2.0)],
            &surface,
            &mut view,
        );
        assert!(response.edits.is_empty(), "zoom is not a transaction");
        assert!(
            (view.pixels_per_second - 200.0).abs() < 1.0,
            "{}",
            view.pixels_per_second
        );
        assert!((view.time_at(point.x, lanes.x) - 3.0).abs() < 0.01);
    }
}
