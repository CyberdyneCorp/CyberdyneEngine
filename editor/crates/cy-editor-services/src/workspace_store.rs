//! Versioned, per-user persistence for open documents and presentation state.
//!
//! The file is deliberately not part of the project. It records which assets this user had open,
//! their view state, and the opaque panel layout; restoring it therefore cannot dirty a document or
//! appear in source control. Unknown records are ignored so a newer editor can add state without
//! making an older build unusable.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};

use cy_editor_core::ids::{DocumentId, NodeId};
use cy_editor_core::problem::{Problem, Result};

use crate::Editor;
use crate::workspace::ViewState;

const HEADER: &str = "cy-workspace 1";

/// What restoring a persisted workspace found.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct RestoreReport {
    /// Documents successfully reopened.
    pub opened: Vec<String>,
    /// Assets no longer present in the project, skipped without creating empty documents.
    pub missing: Vec<String>,
    /// Records from a newer format that this build safely ignored.
    pub unknown_records: usize,
}

/// One user's workspace file for one project.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct WorkspaceStore {
    path: PathBuf,
}

impl WorkspaceStore {
    /// Store state at an explicit path, primarily for embedding and tests.
    #[must_use]
    pub fn new(path: impl Into<PathBuf>) -> Self {
        Self { path: path.into() }
    }

    /// Choose the current user's state directory, namespaced by project path.
    pub fn for_user(project: &Path) -> Result<Self> {
        if let Some(path) = std::env::var_os("CYBERDYNE_EDITOR_WORKSPACE") {
            return Ok(Self::new(path));
        }
        let base = std::env::var_os("XDG_STATE_HOME")
            .map(PathBuf::from)
            .or_else(|| {
                std::env::var_os("HOME")
                    .map(PathBuf::from)
                    .map(|home| home.join(".local/state"))
            })
            .ok_or_else(|| {
                Problem::new(
                    "locate this user's editor workspace",
                    "neither XDG_STATE_HOME nor HOME is available",
                )
                .with_remedy("set CYBERDYNE_EDITOR_WORKSPACE to a writable file")
            })?;
        let project_key = encode_bytes(project.to_string_lossy().as_bytes());
        Ok(Self::new(
            base.join("cyberdyne-engine/workspaces")
                .join(format!("{project_key}.cyworkspace")),
        ))
    }

    /// The backing file.
    #[must_use]
    pub fn path(&self) -> &Path {
        &self.path
    }

    /// Atomically replace the persisted presentation state.
    pub fn write(&self, editor: &Editor) -> Result<()> {
        let encoded = encode(editor);
        if let Some(parent) = self.path.parent() {
            std::fs::create_dir_all(parent).map_err(|error| {
                Problem::new(format!("create {}", parent.display()), error.to_string())
                    .with_remedy("choose a writable user workspace directory")
            })?;
        }
        let temporary = self.path.with_extension("cyworkspace.tmp");
        std::fs::write(&temporary, encoded).map_err(|error| {
            Problem::new(
                format!("write the workspace {}", temporary.display()),
                error.to_string(),
            )
            .with_remedy("check that the user workspace directory is writable")
        })?;
        std::fs::rename(&temporary, &self.path).map_err(|error| {
            Problem::new(
                format!("replace the workspace {}", self.path.display()),
                error.to_string(),
            )
            .with_remedy("check that the user workspace file is writable")
        })
    }

    /// Restore what can still be opened; missing assets are reported and skipped.
    pub fn restore(&self, editor: &mut Editor) -> Result<RestoreReport> {
        let text = match std::fs::read_to_string(&self.path) {
            Ok(text) => text,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                return Ok(RestoreReport::default());
            }
            Err(error) => {
                return Err(Problem::new(
                    format!("read the workspace {}", self.path.display()),
                    error.to_string(),
                )
                .with_remedy("remove the workspace file to start with the default layout"));
            }
        };
        let decoded = decode(&text)?;
        let mut report = RestoreReport {
            unknown_records: decoded.unknown_records,
            ..RestoreReport::default()
        };

        for saved in &decoded.documents {
            if editor
                .documents
                .project_root()
                .is_some_and(|root| !root.join(&saved.asset).is_file())
            {
                report.missing.push(saved.asset.clone());
                continue;
            }
            let id = editor.open_document(&saved.asset)?;
            editor
                .workspace
                .update_view_state(id, |view| *view = saved.view.clone());
            report.opened.push(saved.asset.clone());
        }
        editor.workspace.set_layout(decoded.layout);
        if let Some(active) = decoded.active {
            let id = DocumentId::of_asset(&active);
            if editor.documents.get(id).is_some() {
                editor.workspace.activate(id);
            }
        }
        Ok(report)
    }
}

#[derive(Debug)]
struct SavedDocument {
    asset: String,
    view: ViewState,
}

#[derive(Default, Debug)]
struct Decoded {
    documents: Vec<SavedDocument>,
    active: Option<String>,
    layout: String,
    unknown_records: usize,
}

fn encode(editor: &Editor) -> String {
    let mut output = format!(
        "{HEADER}\nlayout {}\n",
        encode_bytes(editor.workspace.layout().as_bytes())
    );
    if let Some(active) = editor
        .workspace
        .active()
        .and_then(|id| editor.documents.get(id))
        .and_then(|document| document.assets().first())
    {
        writeln!(output, "active {}", encode_bytes(active.as_bytes()))
            .expect("writing to a String cannot fail");
    }
    for id in editor.workspace.open_documents() {
        let Some(document) = editor.documents.get(*id) else {
            continue;
        };
        let Some(asset) = document.assets().first() else {
            continue;
        };
        let view = editor
            .workspace
            .view_state(*id)
            .cloned()
            .unwrap_or_default();
        writeln!(
            output,
            "document {} {}",
            encode_bytes(asset.as_bytes()),
            encode_view(&view)
        )
        .expect("writing to a String cannot fail");
    }
    output
}

fn encode_view(view: &ViewState) -> String {
    let camera = view.camera.map_or_else(
        || "-".to_string(),
        |(position, look)| {
            position
                .into_iter()
                .chain(look)
                .map(|value| value.to_string())
                .collect::<Vec<_>>()
                .join(",")
        },
    );
    let expanded = view
        .expanded
        .iter()
        .map(|id| format!("{:032x}", id.as_u128()))
        .collect::<Vec<_>>()
        .join(",");
    let active_tab = view
        .active_tab
        .as_ref()
        .map_or_else(|| "-".to_string(), |tab| encode_bytes(tab.as_bytes()));
    format!(
        "{camera}|{expanded}|{active_tab}|{}|{}",
        encode_bytes(view.filter.as_bytes()),
        view.scroll
    )
}

fn decode(text: &str) -> Result<Decoded> {
    let mut lines = text.lines();
    let header = lines.next().unwrap_or_default();
    if !header.starts_with("cy-workspace ") {
        return Err(Problem::new(
            "restore the editor workspace",
            "its header is not recognised",
        )
        .with_remedy("remove the workspace file to start with the default layout"));
    }
    let mut decoded = Decoded::default();
    for line in lines {
        let (kind, payload) = line.split_once(' ').unwrap_or((line, ""));
        match kind {
            "layout" => decoded.layout = decode_text(payload)?,
            "active" => decoded.active = Some(decode_text(payload)?),
            "document" => {
                let (asset, view) = payload
                    .split_once(' ')
                    .ok_or_else(|| malformed("document"))?;
                decoded.documents.push(SavedDocument {
                    asset: decode_text(asset)?,
                    view: decode_view(view)?,
                });
            }
            "" => {}
            _ => decoded.unknown_records += 1,
        }
    }
    Ok(decoded)
}

fn decode_view(encoded: &str) -> Result<ViewState> {
    let fields: Vec<&str> = encoded.split('|').collect();
    if fields.len() != 5 {
        return Err(malformed("document view"));
    }
    let camera = if fields[0] == "-" {
        None
    } else {
        let values = fields[0]
            .split(',')
            .map(str::parse::<f32>)
            .collect::<std::result::Result<Vec<_>, _>>()
            .map_err(|_| malformed("camera"))?;
        if values.len() != 6 || values.iter().any(|value| !value.is_finite()) {
            return Err(malformed("camera"));
        }
        Some((
            [values[0], values[1], values[2]],
            [values[3], values[4], values[5]],
        ))
    };
    let expanded = if fields[1].is_empty() {
        Vec::new()
    } else {
        fields[1]
            .split(',')
            .map(|raw| {
                u128::from_str_radix(raw, 16)
                    .map(NodeId::from_u128)
                    .map_err(|_| malformed("expanded node"))
            })
            .collect::<Result<Vec<_>>>()?
    };
    let active_tab = if fields[2] == "-" {
        None
    } else {
        Some(decode_text(fields[2])?)
    };
    let scroll = fields[4]
        .parse()
        .map_err(|_| malformed("scroll position"))?;
    Ok(ViewState {
        camera,
        expanded,
        active_tab,
        filter: decode_text(fields[3])?,
        scroll,
    })
}

fn encode_bytes(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut encoded = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        encoded.push(char::from(HEX[usize::from(byte >> 4)]));
        encoded.push(char::from(HEX[usize::from(byte & 0x0f)]));
    }
    encoded
}

fn decode_text(encoded: &str) -> Result<String> {
    if !encoded.len().is_multiple_of(2) {
        return Err(malformed("hex text"));
    }
    let bytes = encoded
        .as_bytes()
        .chunks_exact(2)
        .map(|pair| {
            let high = hex_digit(pair[0])?;
            let low = hex_digit(pair[1])?;
            Ok((high << 4) | low)
        })
        .collect::<Result<Vec<_>>>()?;
    String::from_utf8(bytes).map_err(|_| malformed("UTF-8 text"))
}

fn hex_digit(byte: u8) -> Result<u8> {
    match byte {
        b'0'..=b'9' => Ok(byte - b'0'),
        b'a'..=b'f' => Ok(byte - b'a' + 10),
        b'A'..=b'F' => Ok(byte - b'A' + 10),
        _ => Err(malformed("hex digit")),
    }
}

fn malformed(field: &str) -> Problem {
    Problem::new(
        "restore the editor workspace",
        format!("its {field} record is malformed"),
    )
    .with_remedy("remove the workspace file to start with the default layout")
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ProjectService;
    use cy_editor_core::Actor;

    fn scratch(name: &str) -> PathBuf {
        std::env::temp_dir().join(format!("cy-workspace-{name}-{}", std::process::id()))
    }

    fn project(name: &str) -> (PathBuf, Editor) {
        let root = scratch(name);
        std::fs::create_dir_all(root.join("worlds")).unwrap();
        std::fs::write(root.join("project.json"), "{}").unwrap();
        (
            root.clone(),
            Editor::new(Actor::human("designer")).with_project(ProjectService::new(root)),
        )
    }

    #[test]
    fn restart_restores_open_order_active_document_layout_and_view_state() {
        let (root, mut first) = project("round-trip");
        std::fs::write(root.join("worlds/city.cyworld"), "cyworld 1\n").unwrap();
        std::fs::write(root.join("worlds/forest.cyworld"), "cyworld 1\n").unwrap();
        let city = first.open_document("worlds/city.cyworld").unwrap();
        let forest = first.open_document("worlds/forest.cyworld").unwrap();
        first.workspace.activate(city);
        first.workspace.set_layout("layout from the user");
        first.workspace.update_view_state(city, |view| {
            view.camera = Some(([1.0, 2.0, 3.0], [0.0, 0.0, 1.0]));
            view.expanded.push(NodeId::in_document(city, 7));
            view.active_tab = Some("Scene".into());
            view.filter = "lamp | sun".into();
            view.scroll = 42;
        });
        let store = WorkspaceStore::new(root.join("user/workspace.cyworkspace"));
        store.write(&first).unwrap();

        let mut restarted =
            Editor::new(Actor::human("designer")).with_project(ProjectService::new(&root));
        let report = store.restore(&mut restarted).unwrap();
        assert_eq!(
            report.opened,
            vec!["worlds/city.cyworld", "worlds/forest.cyworld"]
        );
        assert_eq!(restarted.workspace.open_documents(), &[city, forest]);
        assert_eq!(restarted.workspace.active(), Some(city));
        assert_eq!(restarted.workspace.layout(), "layout from the user");
        assert_eq!(
            restarted.workspace.view_state(city),
            first.workspace.view_state(city)
        );
        assert!(!restarted.documents.any_dirty());
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn missing_assets_and_unknown_newer_records_are_safe() {
        let (root, mut editor) = project("missing-newer");
        let store = WorkspaceStore::new(root.join("workspace.cyworkspace"));
        std::fs::write(
            store.path(),
            format!(
                "cy-workspace 2\nfuture something\ndocument {} -||-|00|0\n",
                encode_bytes(b"worlds/missing.cyworld")
            ),
        )
        .unwrap();

        let report = store.restore(&mut editor).unwrap();
        assert_eq!(report.missing, vec!["worlds/missing.cyworld"]);
        assert_eq!(report.unknown_records, 1);
        assert!(editor.workspace.open_documents().is_empty());
        assert!(!editor.documents.any_dirty());
        std::fs::remove_dir_all(root).unwrap();
    }
}
