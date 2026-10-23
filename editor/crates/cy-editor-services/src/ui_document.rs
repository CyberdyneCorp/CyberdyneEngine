// SPDX-License-Identifier: MIT
//! UI documents: the `.cyui` text the UI layout editor writes, as a tree it can edit. Issue #29,
//! "UI layout", over issue #91's toolkit.
//!
//! **The text is the engine's.** One element a line, its children indented two spaces, `#id`, then
//! `.class`es, then `key=value` properties, after a `cyui 1` header and the `stylesheet` and `param`
//! directives — the grammar `src/ui/toolkit/src/document.cpp` parses and the order its writer puts
//! tokens in. [`UiDocument::encode`] writes exactly what the engine's `write_document` writes for
//! the same document, so an edit made here changes the lines the engine's own editing would and a
//! diff shows one token per property changed (`ui-system`: "WHEN one property of one element
//! changes THEN the text diff SHALL show that single change"). Numbers keep the spelling they were
//! read with, and comments stay where they were.
//!
//! **A widget is addressed by its index among the element lines**, in document order — the order
//! the engine's `ui.preview.render` reports their rectangles in — or by `#id`. Comment and blank
//! lines are kept but are not widgets.
//!
//! The editor decides nothing about what a document means: the engine makes it into widgets, lays
//! it out and draws it (`ui.preview.render`).

use cy_editor_core::problem::{Problem, Result};

/// The undo record kind for a saved UI document: this prefix and the project-relative reference.
pub const DOMAIN_PREFIX: &str = "ui_document:";
/// What a UI document file is called.
pub const EXTENSION: &str = "cyui";
/// What a style sheet or a theme is called.
pub const SHEET_EXTENSION: &str = "cyss";
const HEADER: &str = "cyui 1";
/// The toolkit's own limit, `document.cpp`'s "at most four classes an element".
pub const MAX_CLASSES: usize = 4;

/// A value on a line: a number (with its spelling), a quoted string, a word, a `$parameter` or a
/// list.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum UiValue {
    /// A number, as it was spelled.
    Number(String),
    /// A quoted string, unescaped.
    Text(String),
    /// A bare word: `true`, `end`, `two-way`.
    Word(String),
    /// `$name`: a parameter of the document.
    Parameter(String),
    /// `[a, b, c]`.
    List(Vec<UiValue>),
}

impl UiValue {
    /// A number spelled the way the engine's `set_number` spells it (`%g`).
    #[must_use]
    pub fn number(value: f32) -> Self {
        Self::Number(format_number(value))
    }

    /// A list of numbers.
    #[must_use]
    pub fn numbers(values: &[f32]) -> Self {
        Self::List(values.iter().copied().map(Self::number).collect())
    }

    /// The value as a number, when it is one.
    #[must_use]
    pub fn as_number(&self) -> Option<f32> {
        match self {
            Self::Number(spelling) => spelling.parse().ok(),
            _ => None,
        }
    }

    /// The numbers of a list of numbers.
    #[must_use]
    pub fn as_numbers(&self) -> Option<Vec<f32>> {
        match self {
            Self::List(items) => items.iter().map(Self::as_number).collect(),
            _ => None,
        }
    }

    /// The text a string, word, number or parameter holds; a list's items joined.
    #[must_use]
    pub fn as_text(&self) -> String {
        match self {
            Self::Number(text) | Self::Text(text) | Self::Word(text) => text.clone(),
            Self::Parameter(name) => format!("${name}"),
            Self::List(items) => items
                .iter()
                .map(Self::as_text)
                .collect::<Vec<_>>()
                .join(", "),
        }
    }

    /// Parse one value as it would appear after `key=`: what a person or an agent types.
    ///
    /// # Errors
    ///
    /// When the text is not one whole value.
    pub fn parse(text: &str) -> Result<Self> {
        let mut cursor = Cursor::new(text.trim());
        let value = cursor.value().map_err(|because| {
            Problem::new(format!("read {text:?} as a UI document value"), because)
        })?;
        cursor.skip_spaces();
        if !cursor.done() {
            return Err(Problem::new(
                format!("read {text:?} as a UI document value"),
                "there is more after the value",
            )
            .with_remedy("quote text that has spaces: \"New game\""));
        }
        Ok(value)
    }

    /// The value as the document spells it after `key=`.
    #[must_use]
    pub fn spelled(&self) -> String {
        let mut out = String::new();
        self.write(&mut out);
        out
    }

    fn write(&self, out: &mut String) {
        match self {
            Self::Number(text) | Self::Word(text) => out.push_str(text),
            Self::Parameter(name) => {
                out.push('$');
                out.push_str(name);
            }
            Self::Text(text) => {
                out.push('"');
                for character in text.chars() {
                    match character {
                        '"' | '\\' => {
                            out.push('\\');
                            out.push(character);
                        }
                        '\n' => out.push_str("\\n"),
                        _ => out.push(character),
                    }
                }
                out.push('"');
            }
            Self::List(items) => {
                out.push('[');
                for (index, item) in items.iter().enumerate() {
                    if index > 0 {
                        out.push_str(", ");
                    }
                    item.write(out);
                }
                out.push(']');
            }
        }
    }
}

/// `%g`, as C prints it for the numbers an editor writes: up to six significant digits, no
/// trailing zeros, no exponent for the magnitudes an interface uses.
#[must_use]
pub fn format_number(value: f32) -> String {
    if value == 0.0 || !value.is_finite() {
        return "0".into();
    }
    let magnitude = value.abs();
    #[allow(
        clippy::cast_possible_truncation,
        reason = "the exponent of an f32 is within i32"
    )]
    let exponent = f64::from(magnitude).log10().floor() as i32;
    let decimals = usize::try_from((5 - exponent).max(0)).unwrap_or(0);
    let mut text = format!("{value:.decimals$}");
    if text.contains('.') {
        while text.ends_with('0') {
            text.pop();
        }
        if text.ends_with('.') {
            text.pop();
        }
    }
    if text == "-0" { "0".into() } else { text }
}

/// One directive before the first element.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Directive {
    /// `stylesheet "path"`.
    Stylesheet(UiValue),
    /// `param name value`.
    Param(String, UiValue),
}

/// What one line after the directives is.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum LineKind {
    /// A widget: its kind (`slider`).
    Element(String),
    /// `use "path"`: another document instantiated here.
    Use(UiValue),
    /// `// text`, at its depth.
    Comment(String),
    /// An empty line.
    Blank,
}

/// One line: what it is, how deep, its id, classes and properties.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct UiLine {
    /// What the line is.
    pub kind: LineKind,
    /// Its indentation level.
    pub depth: usize,
    /// `#id`, empty for none.
    pub id: String,
    /// `.class`es, at most four.
    pub classes: Vec<String>,
    /// `key=value`, in the order they are written.
    pub properties: Vec<(String, UiValue)>,
}

impl UiLine {
    /// A widget line of `kind` at `depth`.
    #[must_use]
    pub fn element(kind: &str, depth: usize) -> Self {
        Self {
            kind: LineKind::Element(kind.to_owned()),
            depth,
            id: String::new(),
            classes: Vec::new(),
            properties: Vec::new(),
        }
    }

    /// Whether this line makes a widget (an element or a `use`), rather than being a comment.
    #[must_use]
    pub const fn is_widget(&self) -> bool {
        matches!(self.kind, LineKind::Element(_) | LineKind::Use(_))
    }

    /// The widget kind, `use` for a component, or empty for a comment.
    #[must_use]
    pub fn kind_name(&self) -> &str {
        match &self.kind {
            LineKind::Element(kind) => kind,
            LineKind::Use(_) => "use",
            LineKind::Comment(_) | LineKind::Blank => "",
        }
    }

    /// A property's value, when the line has it.
    #[must_use]
    pub fn property(&self, key: &str) -> Option<&UiValue> {
        self.properties
            .iter()
            .find(|(name, _)| name == key)
            .map(|(_, value)| value)
    }

    /// Set a property IN ITS PLACE ON THE LINE, or append it — the engine's `set_value` rule, so a
    /// changed property is one changed token. `None` removes it.
    pub fn set_property(&mut self, key: &str, value: Option<UiValue>) {
        let at = self.properties.iter().position(|(name, _)| name == key);
        match (at, value) {
            (Some(index), Some(value)) => self.properties[index].1 = value,
            (None, Some(value)) => self.properties.push((key.to_owned(), value)),
            (Some(index), None) => {
                self.properties.remove(index);
            }
            (None, None) => {}
        }
    }

    fn write(&self, out: &mut String) {
        for _ in 0..self.depth {
            out.push_str("  ");
        }
        match &self.kind {
            LineKind::Blank => {
                out.push('\n');
                return;
            }
            LineKind::Comment(text) => {
                out.push_str("//");
                out.push_str(text);
                out.push('\n');
                return;
            }
            LineKind::Element(kind) => out.push_str(kind),
            LineKind::Use(path) => {
                out.push_str("use ");
                path.write(out);
            }
        }
        if !self.id.is_empty() {
            out.push_str(" #");
            out.push_str(&self.id);
        }
        for class in &self.classes {
            out.push_str(" .");
            out.push_str(class);
        }
        for (key, value) in &self.properties {
            out.push(' ');
            out.push_str(key);
            out.push('=');
            value.write(out);
        }
        out.push('\n');
    }
}

/// A `.cyui` document: its directives and its lines, in order.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct UiDocument {
    /// `stylesheet` and `param`, in the order written.
    pub directives: Vec<Directive>,
    /// Every line after them, parents before children.
    pub lines: Vec<UiLine>,
}

/// One widget line of a document, as a reader lists it.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct WidgetSummary {
    /// Its index among the widget lines.
    pub index: usize,
    /// Its line in the text, from one: what the engine reports it by.
    pub line: u32,
    /// The widget index of its parent, or `None` at the top.
    pub parent: Option<usize>,
    /// Its depth.
    pub depth: usize,
    /// Its kind, or `use`.
    pub kind: String,
    /// Its id, empty for none.
    pub id: String,
}

impl UiDocument {
    /// The empty document: `cyui 1` and nothing else.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Read a document. Strict where the engine would report a diagnostic, because a document the
    /// editor saves is one the engine must load.
    ///
    /// # Errors
    ///
    /// Naming the line, for a missing header, bad indentation, a malformed token or value, more
    /// than four classes, or a directive after the first line.
    pub fn decode(text: &str) -> Result<Self> {
        let mut lines = text.split('\n');
        if lines.next() != Some(HEADER) {
            return Err(problem(1, "a UI document starts `cyui 1`"));
        }
        let mut document = Self::new();
        let all: Vec<&str> = lines.collect();
        let count = all.len();
        for (offset, content) in all.into_iter().enumerate() {
            // The text's final newline leaves one empty piece, which is not a line.
            if offset + 1 == count && content.is_empty() {
                break;
            }
            let number = offset + 2;
            document
                .read_line(content)
                .map_err(|because| problem(number, &because))?;
        }
        Ok(document)
    }

    fn read_line(&mut self, content: &str) -> std::result::Result<(), String> {
        let spaces = content.bytes().take_while(|byte| *byte == b' ').count();
        let rest = &content[spaces..];
        if rest.is_empty() {
            self.lines.push(UiLine {
                kind: if spaces == 0 {
                    LineKind::Blank
                } else {
                    LineKind::Comment(String::new())
                },
                depth: spaces / 2,
                id: String::new(),
                classes: Vec::new(),
                properties: Vec::new(),
            });
            return Ok(());
        }
        if let Some(comment) = rest.strip_prefix("//") {
            let mut line = UiLine::element("", spaces / 2);
            line.kind = LineKind::Comment(comment.to_owned());
            self.lines.push(line);
            return Ok(());
        }
        if spaces % 2 != 0 {
            return Err("indentation is two spaces a level".into());
        }
        let depth = spaces / 2;
        let word_end = rest
            .find(|character: char| character == ' ' || character == '\t' || character == '\r')
            .unwrap_or(rest.len());
        let word = &rest[..word_end];
        let mut cursor = Cursor::new(&rest[word_end..]);
        if word == "stylesheet" || word == "param" {
            if depth != 0 || !self.lines.is_empty() {
                return Err("a directive comes before the first element, unindented".into());
            }
            let directive = if word == "stylesheet" {
                Directive::Stylesheet(cursor.value()?)
            } else {
                cursor.skip_spaces();
                let name = cursor.identifier();
                Directive::Param(name, cursor.value()?)
            };
            self.directives.push(directive);
            return Ok(());
        }
        let parent_depth = self.lines.iter().rev().find(|line| line.is_widget());
        if depth > parent_depth.map_or(0, |line| line.depth + 1) {
            return Err("indented more than one level past its parent".into());
        }
        let mut line = UiLine::element(word, depth);
        if word == "use" {
            let path = cursor.value()?;
            if !matches!(path, UiValue::Text(_)) {
                return Err("`use` takes a quoted path".into());
            }
            line.kind = LineKind::Use(path);
        }
        cursor.tokens(&mut line)?;
        self.lines.push(line);
        Ok(())
    }

    /// The canonical text: what the engine's `write_document` writes for this document.
    #[must_use]
    pub fn encode(&self) -> String {
        let mut out = String::from(HEADER);
        out.push('\n');
        for directive in &self.directives {
            match directive {
                Directive::Stylesheet(path) => {
                    out.push_str("stylesheet ");
                    path.write(&mut out);
                }
                Directive::Param(name, value) => {
                    out.push_str("param ");
                    out.push_str(name);
                    out.push(' ');
                    value.write(&mut out);
                }
            }
            out.push('\n');
        }
        for line in &self.lines {
            line.write(&mut out);
        }
        out
    }

    /// The index into `lines` of every widget line, in order.
    fn widget_lines(&self) -> Vec<usize> {
        self.lines
            .iter()
            .enumerate()
            .filter(|(_, line)| line.is_widget())
            .map(|(index, _)| index)
            .collect()
    }

    /// How many widgets the document's own lines make.
    #[must_use]
    pub fn widget_count(&self) -> usize {
        self.lines.iter().filter(|line| line.is_widget()).count()
    }

    /// The widget line at `index`.
    #[must_use]
    pub fn widget(&self, index: usize) -> Option<&UiLine> {
        self.widget_lines()
            .get(index)
            .map(|&position| &self.lines[position])
    }

    /// The widget line at `index`, to change.
    pub fn widget_mut(&mut self, index: usize) -> Option<&mut UiLine> {
        let position = *self.widget_lines().get(index)?;
        self.lines.get_mut(position)
    }

    /// The text's line number of widget `index`, from one: the header is line one, then one line
    /// per directive.
    #[must_use]
    pub fn line_of(&self, index: usize) -> Option<u32> {
        let position = *self.widget_lines().get(index)?;
        u32::try_from(position + 2 + self.directives.len()).ok()
    }

    /// The widget a text line number holds.
    #[must_use]
    pub fn widget_at_line(&self, line: u32) -> Option<usize> {
        (0..self.widget_count()).find(|index| self.line_of(*index) == Some(line))
    }

    /// Resolve `#id` or a widget index.
    ///
    /// # Errors
    ///
    /// When neither names a widget of this document.
    pub fn resolve(&self, widget: &str) -> Result<usize> {
        let widget = widget.trim();
        let found = if let Some(id) = widget.strip_prefix('#') {
            (0..self.widget_count()).find(|index| self.widget(*index).is_some_and(|w| w.id == id))
        } else {
            widget
                .parse::<usize>()
                .ok()
                .filter(|index| *index < self.widget_count())
        };
        found.ok_or_else(|| {
            Problem::new(
                format!("find the widget {widget:?}"),
                format!(
                    "the document has {} widget(s), and none is {widget}",
                    self.widget_count()
                ),
            )
            .with_remedy("name a widget by its index (`ui.document.read` lists them) or `#id`")
        })
    }

    /// Every widget line: index, line, parent, depth, kind and id.
    #[must_use]
    pub fn widgets(&self) -> Vec<WidgetSummary> {
        let mut out: Vec<WidgetSummary> = Vec::new();
        let mut stack: Vec<usize> = Vec::new();
        for (index, line) in self.widget_lines().into_iter().enumerate() {
            let line_data = &self.lines[line];
            stack.truncate(line_data.depth);
            out.push(WidgetSummary {
                index,
                line: self.line_of(index).unwrap_or(0),
                parent: stack.last().copied(),
                depth: line_data.depth,
                kind: line_data.kind_name().to_owned(),
                id: line_data.id.clone(),
            });
            stack.push(index);
        }
        out
    }

    /// The `lines` range widget `index` and everything under it occupy, comments inside included.
    fn subtree(&self, index: usize) -> Option<std::ops::Range<usize>> {
        let start = *self.widget_lines().get(index)?;
        let depth = self.lines[start].depth;
        let end = (start + 1..self.lines.len())
            .find(|&at| {
                let line = &self.lines[at];
                line.depth <= depth && !matches!(line.kind, LineKind::Blank)
            })
            .unwrap_or(self.lines.len());
        Some(start..end)
    }

    /// Whether `ancestor` is `widget` or holds it.
    #[must_use]
    pub fn contains(&self, ancestor: usize, widget: usize) -> bool {
        let (Some(range), Some(&at)) = (self.subtree(ancestor), self.widget_lines().get(widget))
        else {
            return false;
        };
        range.contains(&at)
    }

    /// Where a new child of `parent` (`None`: the top level) goes as its `position`-th child, or
    /// last: the index into `lines`, and the child's depth.
    fn insertion(&self, parent: Option<usize>, position: Option<usize>) -> Result<(usize, usize)> {
        let (first, end, depth) = match parent {
            Some(parent) => {
                let range = self.subtree(parent).ok_or_else(|| {
                    Problem::new("place a widget", format!("there is no widget {parent}"))
                })?;
                (
                    range.start + 1,
                    range.end,
                    self.lines[range.start].depth + 1,
                )
            }
            None => (0, self.lines.len(), 0),
        };
        let children: Vec<usize> = (first..end)
            .filter(|&at| self.lines[at].is_widget() && self.lines[at].depth == depth)
            .collect();
        let at = match position {
            Some(position) if position < children.len() => children[position],
            _ => end,
        };
        Ok((at, depth))
    }

    /// Add a widget of `kind` under `parent` as its `position`-th child (last when `None` or past
    /// the end). Answers the new widget's index.
    ///
    /// # Errors
    ///
    /// When `parent` is not a widget, or `kind` is empty or not one word.
    pub fn add(
        &mut self,
        parent: Option<usize>,
        position: Option<usize>,
        kind: &str,
    ) -> Result<usize> {
        if kind.is_empty() || !kind.bytes().all(is_ident) {
            return Err(Problem::new(
                format!("add a {kind:?}"),
                "a widget kind is one word: letters, digits and hyphens",
            ));
        }
        let (at, depth) = self.insertion(parent, position)?;
        self.lines.insert(at, UiLine::element(kind, depth));
        Ok(self.lines[..at]
            .iter()
            .filter(|line| line.is_widget())
            .count())
    }

    /// Remove a widget and everything under it.
    ///
    /// # Errors
    ///
    /// When there is no such widget.
    pub fn remove(&mut self, widget: usize) -> Result<()> {
        let range = self.subtree(widget).ok_or_else(|| {
            Problem::new("remove a widget", format!("there is no widget {widget}"))
        })?;
        self.lines.drain(range);
        Ok(())
    }

    /// Move a widget, with everything under it, to be `parent`'s `position`-th child. Answers its
    /// new index.
    ///
    /// # Errors
    ///
    /// When either is not a widget, or the widget would be moved into itself.
    pub fn move_widget(
        &mut self,
        widget: usize,
        parent: Option<usize>,
        position: Option<usize>,
    ) -> Result<usize> {
        if parent.is_some_and(|parent| self.contains(widget, parent)) {
            return Err(Problem::new(
                "move a widget",
                "a widget cannot be moved into itself or one of its children",
            ));
        }
        let range = self
            .subtree(widget)
            .ok_or_else(|| Problem::new("move a widget", format!("there is no widget {widget}")))?;
        // Where it goes is decided before it is taken out, in the document as it is, then shifted.
        let (at, depth) = self.insertion(parent, position)?;
        let taken: Vec<UiLine> = self.lines.drain(range.clone()).collect();
        let at = if at > range.start {
            at - range.len()
        } else {
            at
        };
        let shift = depth.cast_signed() - taken[0].depth.cast_signed();
        for (offset, mut line) in taken.into_iter().enumerate() {
            line.depth = line.depth.saturating_add_signed(shift);
            self.lines.insert(at + offset, line);
        }
        Ok(self.lines[..at]
            .iter()
            .filter(|line| line.is_widget())
            .count())
    }

    /// The sheets the document names, in order.
    #[must_use]
    pub fn stylesheets(&self) -> Vec<String> {
        self.directives
            .iter()
            .filter_map(|directive| match directive {
                Directive::Stylesheet(path) => Some(path.as_text()),
                Directive::Param(..) => None,
            })
            .collect()
    }

    /// Every path the document reaches by itself: its sheets, then its `use`d documents.
    #[must_use]
    pub fn dependencies(&self) -> Vec<String> {
        let mut paths = self.stylesheets();
        for line in &self.lines {
            if let LineKind::Use(path) = &line.kind {
                paths.push(path.as_text());
            }
        }
        paths
    }

    /// Name a sheet, or stop naming it.
    pub fn set_stylesheet(&mut self, path: &str, named: bool) {
        let existing = self.directives.iter().position(
            |directive| matches!(directive, Directive::Stylesheet(p) if p.as_text() == path),
        );
        match (existing, named) {
            (None, true) => {
                let after = self
                    .directives
                    .iter()
                    .rposition(|directive| matches!(directive, Directive::Stylesheet(_)))
                    .map_or(0, |at| at + 1);
                self.directives
                    .insert(after, Directive::Stylesheet(UiValue::Text(path.to_owned())));
            }
            (Some(at), false) => {
                self.directives.remove(at);
            }
            _ => {}
        }
    }
}

fn problem(line: usize, because: &str) -> Problem {
    Problem::new(
        format!("read line {line} of a UI document"),
        because.to_owned(),
    )
}

const fn is_ident(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_'
}

/// The engine's tokenizer, over one line's text after its first word.
struct Cursor<'a> {
    text: &'a [u8],
    at: usize,
}

impl<'a> Cursor<'a> {
    const fn new(text: &'a str) -> Self {
        Self {
            text: text.as_bytes(),
            at: 0,
        }
    }

    const fn done(&self) -> bool {
        self.at >= self.text.len()
    }

    fn peek(&self) -> Option<u8> {
        self.text.get(self.at).copied()
    }

    fn skip_spaces(&mut self) {
        while matches!(self.peek(), Some(b' ' | b'\t' | b'\r')) {
            self.at += 1;
        }
    }

    fn identifier(&mut self) -> String {
        let begin = self.at;
        while self.peek().is_some_and(is_ident) {
            self.at += 1;
        }
        String::from_utf8_lossy(&self.text[begin..self.at]).into_owned()
    }

    /// `#id`, `.class` and `key=value`, to the end of the line.
    fn tokens(&mut self, line: &mut UiLine) -> std::result::Result<(), String> {
        loop {
            self.skip_spaces();
            let Some(lead) = self.peek() else {
                return Ok(());
            };
            if lead == b'#' || lead == b'.' {
                self.at += 1;
                let name = self.identifier();
                if name.is_empty() {
                    return Err("an id or a class needs a name".into());
                }
                if lead == b'#' {
                    line.id = name;
                } else if line.classes.len() < MAX_CLASSES {
                    line.classes.push(name);
                } else {
                    return Err(format!("at most four classes an element: {name}"));
                }
                continue;
            }
            let key = self.identifier();
            if key.is_empty() || self.peek() != Some(b'=') {
                return Err("a token is `#id`, `.class` or `key=value`".into());
            }
            self.at += 1;
            let value = self.value()?;
            line.properties.push((key, value));
        }
    }

    fn value(&mut self) -> std::result::Result<UiValue, String> {
        self.skip_spaces();
        match self.peek() {
            None => Err("a value is missing".into()),
            Some(b'"') => self.string(),
            Some(b'[') => self.list(),
            Some(lead) => {
                let begin = self.at;
                while self
                    .peek()
                    .is_some_and(|byte| !matches!(byte, b' ' | b'\t' | b'\r' | b',' | b']'))
                {
                    self.at += 1;
                }
                let token = String::from_utf8_lossy(&self.text[begin..self.at]).into_owned();
                Ok(if lead == b'$' {
                    UiValue::Parameter(token[1..].to_owned())
                } else if is_number(&token) {
                    UiValue::Number(token)
                } else {
                    UiValue::Word(token)
                })
            }
        }
    }

    fn string(&mut self) -> std::result::Result<UiValue, String> {
        self.at += 1;
        let mut bytes = Vec::new();
        while let Some(byte) = self.peek() {
            self.at += 1;
            match byte {
                b'"' => {
                    return String::from_utf8(bytes)
                        .map(UiValue::Text)
                        .map_err(|_| "a string is not UTF-8".into());
                }
                b'\\' => {
                    let escaped = self.peek().unwrap_or(b'\\');
                    self.at += 1;
                    bytes.push(if escaped == b'n' { b'\n' } else { escaped });
                }
                _ => bytes.push(byte),
            }
        }
        Err("a string is not closed".into())
    }

    fn list(&mut self) -> std::result::Result<UiValue, String> {
        self.at += 1;
        let mut items = Vec::new();
        loop {
            self.skip_spaces();
            match self.peek() {
                None => return Err("a list is not closed".into()),
                Some(b']') => {
                    self.at += 1;
                    return Ok(UiValue::List(items));
                }
                Some(_) => {
                    items.push(self.value()?);
                    self.skip_spaces();
                    if self.peek() == Some(b',') {
                        self.at += 1;
                    }
                }
            }
        }
    }
}

/// The engine's `parse_number`: an optional sign, digits, an optional fraction, nothing else.
fn is_number(token: &str) -> bool {
    let body = token
        .strip_prefix('-')
        .or_else(|| token.strip_prefix('+'))
        .unwrap_or(token);
    let (whole, fraction) = body.split_once('.').unwrap_or((body, ""));
    let digits = |part: &str| part.bytes().all(|byte| byte.is_ascii_digit());
    (!whole.is_empty() || !fraction.is_empty()) && digits(whole) && digits(fraction)
}

/// Whether a reference is a project-relative UI document.
///
/// # Errors
///
/// When it is absolute, leaves the project, or is not a `.cyui` with a plain name.
pub fn validate_reference(reference: &str) -> Result<()> {
    validate_path(reference, EXTENSION, "UI document")
}

/// Whether a reference is a project-relative style sheet or theme.
///
/// # Errors
///
/// When it is absolute, leaves the project, or is not a `.cyss` with a plain name.
pub fn validate_sheet_reference(reference: &str) -> Result<()> {
    validate_path(reference, SHEET_EXTENSION, "style sheet")
}

fn validate_path(reference: &str, extension: &str, what: &str) -> Result<()> {
    let path = std::path::Path::new(reference);
    let stem = path
        .file_stem()
        .and_then(|stem| stem.to_str())
        .unwrap_or("");
    let fine = !reference.is_empty()
        && !reference.starts_with('/')
        && !reference.contains(':')
        && !reference.contains('\\')
        && path
            .components()
            .all(|component| matches!(component, std::path::Component::Normal(_)))
        && path.extension().and_then(|found| found.to_str()) == Some(extension)
        && !stem.is_empty()
        && stem.bytes().all(is_ident);
    if fine {
        Ok(())
    } else {
        Err(Problem::new(
            format!("use {reference:?} as a {what}"),
            format!(
                "a {what} is a project-relative .{extension} whose name is letters, digits, \
                 underscores or hyphens"
            ),
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SETTINGS: &str = r#"cyui 1
stylesheet "menu.cyss"
param heading "Settings"
column #settings .panel gap=8 width=320
  // the volume, bound to the game
  label #title .title text=$heading
  slider #volume min=0 max=1 step=0.05 value=0.8 bind=volume
  toggle #subtitles text="Subtitles \"on\"" checked=true
  dropdown #quality items=["Low", "Medium", "High"] selected=1
  use "stat.cyui" #health label="Health" value=0.7

  row justify-content=end
    button #cancel text="Cancel"
    button #apply .primary text="Apply"
"#;

    #[test]
    fn the_engines_own_example_reads_and_writes_back_byte_for_byte() {
        let document = UiDocument::decode(SETTINGS).unwrap();
        assert_eq!(document.encode(), SETTINGS);
        assert_eq!(document.widget_count(), 9);
        assert_eq!(document.stylesheets(), vec!["menu.cyss"]);
        assert_eq!(document.dependencies(), vec!["menu.cyss", "stat.cyui"]);
        let summary = document.widgets();
        assert_eq!(summary[0].line, 4);
        assert_eq!(summary[1].line, 6, "the comment on line 5 is not a widget");
        assert_eq!(summary[1].parent, Some(0));
        assert_eq!(summary[7].kind, "button");
        assert_eq!(summary[7].parent, Some(6));
        assert_eq!(document.widget_at_line(6), Some(1));
        assert_eq!(document.resolve("#apply").unwrap(), 8);
        assert_eq!(document.resolve("2").unwrap(), 2);
        assert!(document.resolve("#missing").is_err());
        assert!(document.resolve("9").is_err());
    }

    #[test]
    fn one_property_changed_is_one_token_of_one_line_changed() {
        let mut document = UiDocument::decode(SETTINGS).unwrap();
        let volume = document.resolve("#volume").unwrap();
        document
            .widget_mut(volume)
            .unwrap()
            .set_property("max", Some(UiValue::number(10.0)));
        let edited = document.encode();
        let changed: Vec<_> = SETTINGS
            .lines()
            .zip(edited.lines())
            .filter(|(before, after)| before != after)
            .collect();
        assert_eq!(
            changed,
            vec![(
                "  slider #volume min=0 max=1 step=0.05 value=0.8 bind=volume",
                "  slider #volume min=0 max=10 step=0.05 value=0.8 bind=volume"
            )]
        );
        document
            .widget_mut(volume)
            .unwrap()
            .set_property("min", None);
        assert!(document.encode().contains("slider #volume max=10 step"));
    }

    #[test]
    fn widgets_are_added_moved_into_another_parent_and_removed_with_their_children() {
        let mut document = UiDocument::new();
        let root = document.add(None, None, "panel").unwrap();
        let column = document.add(Some(root), None, "column").unwrap();
        let play = document.add(Some(column), None, "button").unwrap();
        let quit = document.add(Some(column), None, "button").unwrap();
        document.widget_mut(play).unwrap().id = "play".into();
        document.widget_mut(quit).unwrap().id = "quit".into();
        // Put a new one first among the column's children.
        let title = document.add(Some(column), Some(0), "label").unwrap();
        assert_eq!(title, 2);
        assert_eq!(
            document.encode(),
            "cyui 1\npanel\n  column\n    label\n    button #play\n    button #quit\n"
        );
        // Reparent quit to the panel, after the column: it moves up a level.
        let moved = document
            .move_widget(document.resolve("#quit").unwrap(), Some(root), None)
            .unwrap();
        assert_eq!(moved, 4);
        assert_eq!(
            document.encode(),
            "cyui 1\npanel\n  column\n    label\n    button #play\n  button #quit\n"
        );
        // And back in front of the label, a level down.
        let moved = document
            .move_widget(document.resolve("#quit").unwrap(), Some(column), Some(0))
            .unwrap();
        assert_eq!(moved, 2);
        assert_eq!(
            document.encode(),
            "cyui 1\npanel\n  column\n    button #quit\n    label\n    button #play\n"
        );
        // A widget cannot go inside itself.
        assert!(document.move_widget(column, Some(column), None).is_err());
        assert!(document.move_widget(root, Some(column), None).is_err());
        // Removing the column removes its three children with it.
        document.remove(column).unwrap();
        assert_eq!(document.encode(), "cyui 1\npanel\n");
    }

    #[test]
    fn moving_a_subtree_down_keeps_its_children_and_their_depths() {
        let mut document = UiDocument::decode(
            "cyui 1\ncolumn #a\n  row #b\n    button #c\n  // kept\n  label #d\ncolumn #e\n",
        )
        .unwrap();
        let b = document.resolve("#b").unwrap();
        let e = document.resolve("#e").unwrap();
        document.move_widget(b, Some(e), None).unwrap();
        assert_eq!(
            document.encode(),
            "cyui 1\ncolumn #a\n  // kept\n  label #d\ncolumn #e\n  row #b\n    button #c\n"
        );
    }

    #[test]
    fn values_are_typed_and_spelled_as_the_engine_spells_them() {
        assert_eq!(
            UiValue::parse("0.5").unwrap(),
            UiValue::Number("0.5".into())
        );
        assert_eq!(
            UiValue::parse("\"New game\"").unwrap(),
            UiValue::Text("New game".into())
        );
        assert_eq!(
            UiValue::parse("two-way").unwrap(),
            UiValue::Word("two-way".into())
        );
        assert_eq!(
            UiValue::parse("$label").unwrap(),
            UiValue::Parameter("label".into())
        );
        assert_eq!(
            UiValue::parse("[0.5, 1, \"a b\"]").unwrap(),
            UiValue::List(vec![
                UiValue::Number("0.5".into()),
                UiValue::Number("1".into()),
                UiValue::Text("a b".into())
            ])
        );
        assert!(UiValue::parse("New game").is_err());
        assert!(UiValue::parse("\"open").is_err());
        assert_eq!(format_number(0.25), "0.25");
        assert_eq!(format_number(-160.0), "-160");
        assert_eq!(format_number(1.0 / 3.0), "0.333333");
        assert_eq!(format_number(1920.0), "1920");
        assert_eq!(format_number(0.0), "0");
        assert_eq!(
            UiValue::numbers(&[0.5, 0.0]).as_numbers(),
            Some(vec![0.5, 0.0])
        );
        assert!(is_number("-1.5") && is_number(".5") && is_number("3."));
        assert!(!is_number("-") && !is_number("1e3") && !is_number("."));
    }

    #[test]
    fn what_the_engine_would_report_is_refused_with_its_line() {
        for (text, because) in [
            ("panel\n", "starts `cyui 1`"),
            ("cyui 1\npanel\n   label\n", "two spaces"),
            ("cyui 1\npanel\n    label\n", "more than one level"),
            (
                "cyui 1\npanel\nstylesheet \"a.cyss\"\n",
                "before the first element",
            ),
            ("cyui 1\nlabel text=\"open\n", "not closed"),
            ("cyui 1\nlabel .a .b .c .d .e\n", "four classes"),
            ("cyui 1\nlabel text\n", "key=value"),
            ("cyui 1\nuse stat.cyui\n", "quoted path"),
        ] {
            let refusal = UiDocument::decode(text).unwrap_err().to_string();
            assert!(refusal.contains(because), "{text:?}: {refusal}");
        }
    }

    #[test]
    fn a_sheet_is_named_once_after_the_others_and_unnamed() {
        let mut document = UiDocument::decode("cyui 1\nparam a 1\npanel\n").unwrap();
        document.set_stylesheet("ui/menu.cyss", true);
        document.set_stylesheet("ui/menu.cyss", true);
        document.set_stylesheet("ui/extra.cyss", true);
        assert_eq!(
            document.encode(),
            "cyui 1\nstylesheet \"ui/menu.cyss\"\nstylesheet \"ui/extra.cyss\"\nparam a 1\npanel\n"
        );
        assert_eq!(document.line_of(0), Some(5));
        document.set_stylesheet("ui/menu.cyss", false);
        assert_eq!(document.stylesheets(), vec!["ui/extra.cyss"]);
    }

    #[test]
    fn references_are_project_relative_documents_and_sheets() {
        assert!(validate_reference("game/ui/main_menu.cyui").is_ok());
        assert!(validate_reference("/abs/menu.cyui").is_err());
        assert!(validate_reference("../menu.cyui").is_err());
        assert!(validate_reference("game/ui/menu.cyss").is_err());
        assert!(validate_sheet_reference("game/ui/themes/dark.cyss").is_ok());
        assert!(validate_sheet_reference("game/ui/dark theme.cyss").is_err());
    }
}
