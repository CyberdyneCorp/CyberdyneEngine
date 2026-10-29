// SPDX-License-Identifier: MIT
//! The UI layout commands the layout editor's panel, scripts and MCP share. Issue #29, "UI layout".
//!
//! Every edit reads the `.cyui` from the project, makes one change to it as a [`UiDocument`], and
//! saves the canonical text as one undoable project transaction — which is also what a running
//! game's `DocumentHost` sees change on disk and reloads, keeping the state of every widget it can
//! match. Previews and status reads change no document and are `Read`: the engine draws what the
//! file says.
//!
//! A widget is named by its index among the document's widget lines (`ui.document.read` lists
//! them) or by `#id`; a parent left empty is the top level.

use cy_editor_commands::{
    Arguments, Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, ProjectHost,
    Registry, UiPreviewChange,
};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};

use crate::authoring::within_scope;
use crate::ui_document::{UiDocument, UiValue, validate_reference, validate_sheet_reference};

const CATEGORY: &str = "UI Layout";

/// Register every UI layout command.
pub fn register(registry: &mut Registry) -> Result<()> {
    for command in [
        document_create(),
        document_read(),
        document_stylesheet(),
        widget_add(),
        widget_remove(),
        widget_move(),
        widget_set(),
        widget_identity(),
        widget_layout(),
        widget_bind(),
        theme_save(),
        preview(),
        status(),
    ] {
        registry.register(command)?;
    }
    Ok(())
}

fn host(context: &mut dyn CommandContext) -> Result<&mut dyn ProjectHost> {
    context
        .project()
        .ok_or_else(|| Problem::new("author an interface", "no project is open"))
}

fn text<'a>(arguments: &'a Arguments, name: &str) -> &'a str {
    arguments.text(name).unwrap_or_default()
}

fn reference() -> ParameterSpec {
    ParameterSpec::required(
        "reference",
        ValueKind::Text,
        "Project-relative .cyui document, e.g. ui/main_menu.cyui.",
    )
}

fn widget() -> ParameterSpec {
    ParameterSpec::required(
        "widget",
        ValueKind::Text,
        "The widget: its index among the document's widget lines, or #id.",
    )
}

fn mutation(id: &'static str, label: &'static str, description: &'static str) -> Metadata {
    Metadata::new(
        id,
        label,
        CATEGORY,
        description,
        EffectClass::ReversibleMutation,
    )
    .with(reference())
}

/// The document at `reference`.
fn read_document(host: &dyn ProjectHost, reference: &str) -> Result<UiDocument> {
    validate_reference(reference)?;
    if !host.source_exists(reference) {
        return Err(
            Problem::new(format!("edit {reference}"), "there is no such UI document")
                .with_remedy("create it with ui.document.create"),
        );
    }
    UiDocument::decode(&host.read_source(reference)?)
}

/// Read, change and save one document as one undoable transaction.
fn edit_document(
    context: &mut dyn CommandContext,
    arguments: &Arguments,
    summary: impl FnOnce(&UiDocument) -> String,
    edit: impl FnOnce(&mut UiDocument) -> Result<Vec<(String, Value)>>,
) -> Result<Outcome> {
    let reference = text(arguments, "reference").to_owned();
    validate_reference(&reference)?;
    within_scope(context, &reference)?;
    let host = host(context)?;
    let mut document = read_document(host, &reference)?;
    let fields = edit(&mut document)?;
    let source = document.encode();
    // A document the editor saves is one the editor can read back: the engine's grammar.
    UiDocument::decode(&source)?;
    host.ui_asset_save(&reference, &source)?;
    let mut outcome = Outcome::new(summary(&document))
        .with("reference", Value::Text(reference))
        .with("source", Value::Text(source));
    for (name, value) in fields {
        outcome = outcome.with(name, value);
    }
    Ok(outcome)
}

/// `None` for an empty parent: the top level.
fn parent_of(document: &UiDocument, parent: &str) -> Result<Option<usize>> {
    if parent.trim().is_empty() {
        Ok(None)
    } else {
        document.resolve(parent).map(Some)
    }
}

fn position(arguments: &Arguments) -> Option<usize> {
    arguments
        .get("position")
        .and_then(Value::as_int)
        .and_then(|value| usize::try_from(value).ok())
}

fn index_value(index: usize) -> Value {
    Value::Int(i64::try_from(index).unwrap_or(i64::MAX))
}

fn document_create() -> Command {
    Command::new(
        mutation(
            "ui.document.create",
            "Create UI Document",
            "Creates a UI document with one full-screen panel as its root, as one undoable \
             transaction. Refused when the file exists.",
        ),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            validate_reference(&reference)?;
            within_scope(context, &reference)?;
            let host = host(context)?;
            if host.source_exists(&reference) {
                return Err(Problem::new(
                    format!("create {reference}"),
                    "that file already exists",
                ));
            }
            let mut document = UiDocument::new();
            let root = document.add(None, None, "panel")?;
            let line = document.widget_mut(root).expect("just added");
            line.id = "root".into();
            line.set_property("anchor", Some(UiValue::numbers(&[0.0, 0.0, 1.0, 1.0])));
            line.set_property("offset", Some(UiValue::numbers(&[0.0, 0.0, 0.0, 0.0])));
            let source = document.encode();
            host.ui_asset_save(&reference, &source)?;
            Ok(Outcome::new(format!("Created {reference}"))
                .with("reference", Value::Text(reference))
                .with("source", Value::Text(source)))
        },
    )
}

fn document_read() -> Command {
    Command::new(
        Metadata::new(
            "ui.document.read",
            "Read UI Document",
            CATEGORY,
            "Returns a UI document's text and every widget: its index, line, parent, kind, id, \
             classes and properties.",
            EffectClass::Read,
        )
        .with(reference()),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let document = read_document(host(context)?, reference)?;
            let mut outcome = Outcome::new(format!("{} widget(s)", document.widget_count()))
                .with("source", Value::Text(document.encode()))
                .with("stylesheets", Value::Text(document.stylesheets().join(" ")));
            for summary in document.widgets() {
                let line = document.widget(summary.index).expect("listed");
                let properties = line
                    .properties
                    .iter()
                    .map(|(key, value)| format!("{key}={}", value.spelled()))
                    .collect::<Vec<_>>()
                    .join(" ");
                outcome = outcome.with(
                    format!("widget.{}", summary.index),
                    Value::Text(format!(
                        "line={} parent={} kind={} id={} classes={} {properties}",
                        summary.line,
                        summary.parent.map_or_else(|| "-".into(), |p| p.to_string()),
                        summary.kind,
                        if summary.id.is_empty() {
                            "-"
                        } else {
                            &summary.id
                        },
                        line.classes.join(","),
                    )),
                );
            }
            Ok(outcome)
        },
    )
}

fn document_stylesheet() -> Command {
    Command::new(
        mutation(
            "ui.document.stylesheet",
            "Set UI Document Style Sheet",
            "Names a project .cyss style sheet in a UI document, or stops naming it, as one \
             undoable transaction.",
        )
        .with(ParameterSpec::required(
            "sheet",
            ValueKind::Text,
            "Project-relative .cyss style sheet.",
        ))
        .with(ParameterSpec::optional(
            "named",
            ValueKind::Bool,
            "False to stop naming it.",
            Value::Bool(true),
        )),
        |context, arguments| {
            let sheet = text(arguments, "sheet").to_owned();
            validate_sheet_reference(&sheet)?;
            let named = !matches!(arguments.get("named"), Some(Value::Bool(false)));
            edit_document(
                context,
                arguments,
                |_| format!("{} {sheet}", if named { "Named" } else { "Unnamed" }),
                |document| {
                    document.set_stylesheet(&sheet, named);
                    Ok(Vec::new())
                },
            )
        },
    )
}

fn widget_add() -> Command {
    Command::new(
        mutation(
            "ui.widget.add",
            "Add Widget",
            "Places a new widget of a kind the engine's toolkit makes under a parent, as its \
             position-th child or last, as one undoable transaction. Answers its index.",
        )
        .with(ParameterSpec::required(
            "kind",
            ValueKind::Text,
            "The widget kind: panel, row, column, grid, stack, scroll-view, label, rich-text, \
             image, button, toggle, checkbox, radio-group, slider, progress-bar, text-field, \
             dropdown, list-view, tabs or dialog.",
        ))
        .with(ParameterSpec::optional(
            "parent",
            ValueKind::Text,
            "The parent widget (index or #id); empty for the top level.",
            Value::Text(String::new()),
        ))
        .with(ParameterSpec::optional(
            "position",
            ValueKind::Int,
            "Its place among the parent's children; past the end, or -1, is last.",
            Value::Int(-1),
        ))
        .with(ParameterSpec::optional(
            "id",
            ValueKind::Text,
            "Its #id; empty for none.",
            Value::Text(String::new()),
        ))
        .with(ParameterSpec::optional(
            "text",
            ValueKind::Text,
            "Its caption, for a kind that shows one; empty for none.",
            Value::Text(String::new()),
        )),
        |context, arguments| {
            let kind = text(arguments, "kind").to_owned();
            if let Some(kinds) = context.project().and_then(ProjectHost::ui_kinds)
                && !kinds.contains(&kind)
            {
                return Err(Problem::new(
                    format!("add a {kind}"),
                    "the engine's toolkit makes no widget of that kind",
                )
                .with_remedy(format!("use one of: {}", kinds.join(", "))));
            }
            let parent = text(arguments, "parent").to_owned();
            let id = text(arguments, "id").to_owned();
            let caption = text(arguments, "text").to_owned();
            let position = position(arguments);
            edit_document(
                context,
                arguments,
                |_| format!("Added a {kind}"),
                |document| {
                    if !id.is_empty() && document.resolve(&format!("#{id}")).is_ok() {
                        return Err(Problem::new(
                            format!("add #{id}"),
                            "another widget of this document has that id",
                        ));
                    }
                    let parent = parent_of(document, &parent)?;
                    let index = document.add(parent, position, &kind)?;
                    let line = document.widget_mut(index).expect("just added");
                    line.id = id;
                    if !caption.is_empty() {
                        line.set_property("text", Some(UiValue::Text(caption)));
                    }
                    Ok(vec![("widget".into(), index_value(index))])
                },
            )
        },
    )
}

fn widget_remove() -> Command {
    Command::new(
        mutation(
            "ui.widget.remove",
            "Remove Widget",
            "Removes a widget and every widget under it, as one undoable transaction.",
        )
        .with(widget()),
        |context, arguments| {
            let target = text(arguments, "widget").to_owned();
            edit_document(
                context,
                arguments,
                |_| format!("Removed {target}"),
                |document| {
                    let index = document.resolve(&target)?;
                    document.remove(index)?;
                    Ok(Vec::new())
                },
            )
        },
    )
}

fn widget_move() -> Command {
    Command::new(
        mutation(
            "ui.widget.move",
            "Move Widget",
            "Moves a widget, with everything under it, to be a parent's position-th child — \
             reordering among its siblings or reparenting — as one undoable transaction. Answers \
             its new index.",
        )
        .with(widget())
        .with(ParameterSpec::optional(
            "parent",
            ValueKind::Text,
            "The new parent (index or #id); empty for the top level.",
            Value::Text(String::new()),
        ))
        .with(ParameterSpec::optional(
            "position",
            ValueKind::Int,
            "Its place among the new parent's children, counted before it moves; -1 is last.",
            Value::Int(-1),
        )),
        |context, arguments| {
            let target = text(arguments, "widget").to_owned();
            let parent = text(arguments, "parent").to_owned();
            let position = position(arguments);
            edit_document(
                context,
                arguments,
                |_| format!("Moved {target}"),
                |document| {
                    let index = document.resolve(&target)?;
                    let parent = parent_of(document, &parent)?;
                    let moved = document.move_widget(index, parent, position)?;
                    Ok(vec![("widget".into(), index_value(moved))])
                },
            )
        },
    )
}

fn widget_set() -> Command {
    Command::new(
        mutation(
            "ui.widget.set",
            "Set Widget Property",
            "Sets one property of a widget — a widget property (text, value, items, ...) or any \
             .cyss property as an inline declaration — in its place on the line, as one undoable \
             transaction. An empty value removes it.",
        )
        .with(widget())
        .with(ParameterSpec::required(
            "key",
            ValueKind::Text,
            "The property: text, value, min, max, step, items, selected, checked, enabled, \
             visible, tooltip, anchor, offset, width, background-color, ...",
        ))
        .with(ParameterSpec::required(
            "value",
            ValueKind::Text,
            "The value as the document spells it: 0.5, \"New game\", true, [\"Low\", \"High\"], \
             #ff8000. Empty removes the property.",
        )),
        |context, arguments| {
            let target = text(arguments, "widget").to_owned();
            let key = text(arguments, "key").to_owned();
            let raw = text(arguments, "value").to_owned();
            if key.is_empty() || !key.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-') {
                return Err(Problem::new(
                    format!("set {key:?}"),
                    "a property is one word of letters, digits and hyphens",
                ));
            }
            let value = if raw.trim().is_empty() {
                None
            } else {
                Some(UiValue::parse(&raw)?)
            };
            edit_document(
                context,
                arguments,
                |_| format!("Set {key} on {target}"),
                |document| {
                    let index = document.resolve(&target)?;
                    document
                        .widget_mut(index)
                        .expect("resolved")
                        .set_property(&key, value);
                    Ok(vec![("widget".into(), index_value(index))])
                },
            )
        },
    )
}

fn widget_identity() -> Command {
    Command::new(
        mutation(
            "ui.widget.identity",
            "Set Widget Id And Classes",
            "Sets a widget's #id and its style classes (at most four, space-separated), as one \
             undoable transaction.",
        )
        .with(widget())
        .with(ParameterSpec::optional(
            "id",
            ValueKind::Text,
            "Its new #id; empty for none.",
            Value::Text(String::new()),
        ))
        .with(ParameterSpec::optional(
            "classes",
            ValueKind::Text,
            "Its classes, space-separated: \"primary wide\".",
            Value::Text(String::new()),
        )),
        |context, arguments| {
            let target = text(arguments, "widget").to_owned();
            let id = text(arguments, "id")
                .trim()
                .trim_start_matches('#')
                .to_owned();
            let classes: Vec<String> = text(arguments, "classes")
                .split_whitespace()
                .map(|class| class.trim_start_matches('.').to_owned())
                .collect();
            let plain = |name: &str| {
                name.bytes()
                    .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
            };
            if classes.len() > crate::ui_document::MAX_CLASSES
                || !plain(&id)
                || classes
                    .iter()
                    .any(|class| class.is_empty() || !plain(class))
            {
                return Err(Problem::new(
                    format!("name {target}"),
                    "an id and each of at most four classes are letters, digits, hyphens and \
                     underscores",
                ));
            }
            edit_document(
                context,
                arguments,
                |_| format!("Named {target}"),
                |document| {
                    let index = document.resolve(&target)?;
                    if !id.is_empty()
                        && document
                            .resolve(&format!("#{id}"))
                            .is_ok_and(|holder| holder != index)
                    {
                        return Err(Problem::new(
                            format!("name a widget #{id}"),
                            "another widget of this document has that id",
                        ));
                    }
                    let line = document.widget_mut(index).expect("resolved");
                    line.id = id;
                    line.classes = classes;
                    Ok(vec![("widget".into(), index_value(index))])
                },
            )
        },
    )
}

/// What a parameter left out of `ui.widget.layout` and `ui.preview` holds: "keep what there is".
const KEEP: &str = "-";
const UNSET4: [f32; 4] = [f32::NAN; 4];
const UNSET2: [f32; 2] = [f32::NAN; 2];

fn layout_parameter(
    name: &'static str,
    kind: ValueKind,
    description: &'static str,
) -> ParameterSpec {
    let default = match kind {
        ValueKind::Vec4 => Value::Vec4(UNSET4),
        ValueKind::Vec2 => Value::Vec2(UNSET2),
        ValueKind::Float => Value::Float(0.0),
        _ => Value::Text(KEEP.into()),
    };
    ParameterSpec::optional(name, kind, description, default)
}

/// A parameter's lanes, or `None` when it was left out (its default is not a number).
fn lanes_of(arguments: &Arguments, name: &str) -> Option<Vec<f32>> {
    let lanes = match arguments.get(name)? {
        Value::Vec4(values) => values.to_vec(),
        Value::Vec2(values) => values.to_vec(),
        _ => return None,
    };
    (!lanes.iter().any(|value| value.is_nan())).then_some(lanes)
}

/// A text parameter, or `None` when it was left out.
fn given<'a>(arguments: &'a Arguments, name: &str) -> Option<&'a str> {
    arguments.text(name).filter(|value| *value != KEEP)
}

fn widget_layout() -> Command {
    Command::new(
        mutation(
            "ui.widget.layout",
            "Set Widget Layout",
            "What the canvas's handles change, as one undoable transaction: anchors (fractions of \
             the parent: min x, min y, max x, max y), offsets (units from those anchors), a \
             preferred size and margins. Only what is given changes.",
        )
        .with(widget())
        .with(layout_parameter(
            "anchor",
            ValueKind::Vec4,
            "Anchors: [min x, min y, max x, max y], each 0 to 1.",
        ))
        .with(layout_parameter(
            "offset",
            ValueKind::Vec4,
            "Offsets from the anchors: [min x, min y, max x, max y].",
        ))
        .with(layout_parameter(
            "size",
            ValueKind::Vec2,
            "Preferred width and height; a negative one removes it.",
        ))
        .with(layout_parameter(
            "margin",
            ValueKind::Vec4,
            "Margins in units: [left, top, right, bottom]; zero removes one.",
        )),
        |context, arguments| {
            let target = text(arguments, "widget").to_owned();
            let anchor = lanes_of(arguments, "anchor");
            let offset = lanes_of(arguments, "offset");
            let size = lanes_of(arguments, "size");
            let margin = lanes_of(arguments, "margin");
            if anchor
                .as_ref()
                .is_some_and(|values| values.iter().any(|v| !(0.0..=1.0).contains(v)))
            {
                return Err(Problem::new(
                    format!("anchor {target}"),
                    "an anchor is a fraction of the parent, from 0 to 1",
                ));
            }
            let finite = [&offset, &size, &margin]
                .into_iter()
                .flatten()
                .all(|values| values.iter().all(|v| v.is_finite()));
            if !finite {
                return Err(Problem::new(
                    format!("lay out {target}"),
                    "every number is finite",
                ));
            }
            edit_document(
                context,
                arguments,
                |_| format!("Laid out {target}"),
                |document| {
                    let index = document.resolve(&target)?;
                    let line = document.widget_mut(index).expect("resolved");
                    if let Some(anchor) = anchor {
                        line.set_property("anchor", Some(UiValue::numbers(&anchor)));
                    }
                    if let Some(offset) = offset {
                        line.set_property("offset", Some(UiValue::numbers(&offset)));
                    }
                    if let Some(size) = size {
                        for (key, value) in ["width", "height"].into_iter().zip(size) {
                            line.set_property(key, (value >= 0.0).then(|| UiValue::number(value)));
                        }
                    }
                    if let Some(margin) = margin {
                        for (key, value) in
                            ["margin-left", "margin-top", "margin-right", "margin-bottom"]
                                .into_iter()
                                .zip(margin)
                        {
                            line.set_property(key, (value != 0.0).then(|| UiValue::number(value)));
                        }
                    }
                    Ok(vec![("widget".into(), index_value(index))])
                },
            )
        },
    )
}

fn widget_bind() -> Command {
    Command::new(
        mutation(
            "ui.widget.bind",
            "Bind Widget",
            "Binds a widget's value to a named game value (`bind=`) one-way or two-way, or unbinds \
             it with an empty name, as one undoable transaction.",
        )
        .with(widget())
        .with(ParameterSpec::required(
            "name",
            ValueKind::Text,
            "The game value's name, as the game's DocumentBindings or Swift binding names it.",
        ))
        .with(ParameterSpec::optional(
            "mode",
            ValueKind::Text,
            "one-way (the game drives the widget) or two-way (the player's changes write back).",
            Value::Text("one-way".into()),
        )),
        |context, arguments| {
            let target = text(arguments, "widget").to_owned();
            let name = text(arguments, "name").trim().to_owned();
            let mode = text(arguments, "mode").to_owned();
            if !matches!(mode.as_str(), "one-way" | "two-way" | "") {
                return Err(Problem::new(
                    format!("bind {target}"),
                    "a binding is one-way or two-way",
                ));
            }
            if !name
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_' | b'.'))
            {
                return Err(Problem::new(
                    format!("bind {target} to {name:?}"),
                    "a binding's name is one word",
                ));
            }
            edit_document(
                context,
                arguments,
                |_| format!("Bound {target}"),
                |document| {
                    let index = document.resolve(&target)?;
                    let line = document.widget_mut(index).expect("resolved");
                    let bound = !name.is_empty();
                    line.set_property("bind", bound.then_some(UiValue::Word(name)));
                    line.set_property(
                        "bind-mode",
                        (bound && mode == "two-way").then_some(UiValue::Word(mode)),
                    );
                    Ok(vec![("widget".into(), index_value(index))])
                },
            )
        },
    )
}

fn theme_save() -> Command {
    Command::new(
        Metadata::new(
            "ui.theme.save",
            "Save UI Theme",
            CATEGORY,
            "Saves a project .cyss theme or style sheet, as one undoable transaction. The canvas \
             is drawn again when it shows that theme or a document naming that sheet.",
            EffectClass::ReversibleMutation,
        )
        .with(ParameterSpec::required(
            "reference",
            ValueKind::Text,
            "Project-relative .cyss file, e.g. ui/themes/dark.cyss.",
        ))
        .with(ParameterSpec::required(
            "source",
            ValueKind::Text,
            "The sheet's .cyss text.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            validate_sheet_reference(&reference)?;
            within_scope(context, &reference)?;
            let source = text(arguments, "source").to_owned();
            host(context)?.ui_asset_save(&reference, &source)?;
            Ok(
                Outcome::new(format!("Saved {reference}"))
                    .with("reference", Value::Text(reference)),
            )
        },
    )
}

fn preview() -> Command {
    Command::new(
        Metadata::new(
            "ui.preview",
            "Preview UI Document",
            CATEGORY,
            "Asks the engine to make a UI document into widgets and draw it on the canvas: at a \
             resolution and DPI, scaled from a reference resolution, in a project theme, with \
             one widget hovered, pressed, focused, disabled or open. Read ui.status, or the \
             editor-ui-layout panel's image, for what it drew. Only what is given changes.",
            EffectClass::Read,
        )
        .with(reference())
        .with(layout_parameter(
            "size",
            ValueKind::Vec2,
            "Target width and height in pixels.",
        ))
        .with(layout_parameter(
            "dpi",
            ValueKind::Float,
            "The platform's DPI scale: 1, 1.5, 2.",
        ))
        .with(layout_parameter(
            "scale_mode",
            ValueKind::Text,
            "fixed, width, height, smaller or larger; left out (-), the last preview's.",
        ))
        .with(layout_parameter(
            "reference_size",
            ValueKind::Vec2,
            "The resolution the document is designed at.",
        ))
        .with(layout_parameter(
            "theme",
            ValueKind::Text,
            "A project .cyss theme; empty for the built-in theme; left out (-), the last one.",
        ))
        .with(layout_parameter(
            "state",
            ValueKind::Text,
            "none, hover, pressed, focused, disabled or open; left out (-), the last one.",
        ))
        .with(layout_parameter(
            "state_widget",
            ValueKind::Text,
            "The widget the state is for (index or #id); empty for none; left out (-), the last.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            let host = host(context)?;
            let document = read_document(host, &reference)?;
            let change = preview_change(arguments, &document)?;
            let request = host.ui_preview(&reference, &change)?;
            Ok(
                Outcome::new(format!("Asked the engine to draw {reference}"))
                    .with("request", Value::Int(i64::try_from(request).unwrap_or(0))),
            )
        },
    )
}

fn preview_change(arguments: &Arguments, document: &UiDocument) -> Result<UiPreviewChange> {
    let mut change = UiPreviewChange::default();
    if let Some(size) = lanes_of(arguments, "size") {
        change.size = Some((whole_pixels(size[0])?, whole_pixels(size[1])?));
    }
    if let Some(dpi) = arguments
        .get("dpi")
        .and_then(Value::as_float)
        .filter(|dpi| *dpi != 0.0)
    {
        if !(dpi > 0.0 && dpi <= 8.0) {
            return Err(Problem::new(
                "preview at a DPI scale",
                "it is above 0 and at most 8",
            ));
        }
        change.dpi = Some(dpi);
    }
    if let Some(mode) = given(arguments, "scale_mode") {
        if crate::ui_layout::ScaleMode::named(mode).is_none() {
            return Err(Problem::new(
                format!("scale by {mode:?}"),
                "a scale mode is fixed, width, height, smaller or larger",
            ));
        }
        change.scale_mode = Some(mode.to_owned());
    }
    if let Some(reference) = lanes_of(arguments, "reference_size") {
        change.reference = Some((reference[0], reference[1]));
    }
    if let Some(theme) = given(arguments, "theme") {
        if !theme.is_empty() {
            validate_sheet_reference(theme)?;
        }
        change.theme = Some(theme.to_owned());
    }
    if let Some(state) = given(arguments, "state") {
        if crate::ui_layout::PreviewState::named(state).is_none() {
            return Err(Problem::new(
                format!("preview the state {state:?}"),
                "a state is none, hover, pressed, focused, disabled or open",
            ));
        }
        change.state = Some(state.to_owned());
    }
    if let Some(widget) = given(arguments, "state_widget") {
        change.state_widget = Some(if widget.trim().is_empty() {
            None
        } else {
            Some(document.resolve(widget)?)
        });
    }
    Ok(change)
}

fn whole_pixels(value: f32) -> Result<u32> {
    if value.is_finite() && (1.0..=8192.0).contains(&value) && value.fract() == 0.0 {
        #[allow(
            clippy::cast_possible_truncation,
            clippy::cast_sign_loss,
            reason = "checked to be a whole number from 1 to 8192"
        )]
        Ok(value as u32)
    } else {
        Err(Problem::new(
            "preview at a resolution",
            "each side is a whole number of pixels from 1 to 8192",
        ))
    }
}

fn status() -> Command {
    Command::new(
        Metadata::new(
            "ui.status",
            "UI Preview Status",
            CATEGORY,
            "The engine's last preview: the document, the target, every widget's rectangle in \
             reference units, the diagnostics the engine named, the classes the sheets select, and \
             what laying out and drawing cost.",
            EffectClass::Read,
        )
        .with(ParameterSpec::optional(
            "reference",
            ValueKind::Text,
            "The document, so rectangles are named by widget index; empty for lines.",
            Value::Text(String::new()),
        )),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            Ok(host(context)?.ui_status(&reference))
        },
    )
}
