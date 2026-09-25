//! Project material graph files shared by the UI and command surface.

use std::path::{Component, Path, PathBuf};

use cy_editor_core::problem::{Problem, Result};

/// Domain-operation prefix used to journal project graph file changes in the active scene.
pub const GRAPH_DOMAIN_PREFIX: &str = "material_graph:";

/// Store both graph files in one undo payload, including whether either file existed.
pub fn encode_pair(graph: Option<&str>, source: Option<&str>) -> Vec<u8> {
    let mut writer = cy_editor_core::codec::Writer::new();
    for value in [graph, source] {
        match value {
            Some(text) => {
                writer.u8(1);
                writer.text(text);
            }
            None => writer.u8(0),
        }
    }
    writer.finish()
}

/// Decode one graph undo payload.
pub fn decode_pair(bytes: &[u8]) -> Result<(Option<String>, Option<String>)> {
    let mut reader = cy_editor_core::codec::Reader::new(bytes);
    let mut next = || -> Result<Option<String>> {
        match reader.u8()? {
            0 => Ok(None),
            1 => reader.text().map(Some),
            _ => Err(Problem::new(
                "decode a graph history entry",
                "invalid presence tag",
            )),
        }
    };
    Ok((next()?, next()?))
}

/// Resolve the canonical graph and editable canvas paths within a project.
pub fn paths(root: &Path, reference: &str) -> Result<(PathBuf, PathBuf)> {
    let relative = Path::new(reference);
    if relative.extension().is_none_or(|ext| ext != "cygraph")
        || !relative
            .components()
            .all(|part| matches!(part, Component::Normal(_)))
    {
        return Err(Problem::new(
            "access a material graph",
            "the path must be a project-relative .cygraph file",
        ));
    }
    let graph = root.join(relative);
    let source = graph.with_extension("cymatcanvas");
    Ok((graph, source))
}

/// Read an editable material canvas by its graph asset reference.
pub fn read(root: &Path, reference: &str) -> Result<String> {
    let (_, source) = paths(root, reference)?;
    std::fs::read_to_string(&source)
        .map_err(|error| Problem::new(format!("read {}", source.display()), error.to_string()))
}

/// Persist an engine-authored graph and its editable canvas together.
pub fn save(root: &Path, reference: &str, source: &str, graph: &str) -> Result<()> {
    let (graph_path, source_path) = paths(root, reference)?;
    let parent = graph_path
        .parent()
        .ok_or_else(|| Problem::new("save a material graph", "the material has no directory"))?;
    std::fs::create_dir_all(parent)
        .map_err(|error| Problem::new("save a material graph", error.to_string()))?;
    let graph_stage = graph_path.with_extension("cygraph.tmp");
    let source_stage = source_path.with_extension("cymatcanvas.tmp");
    std::fs::write(&graph_stage, graph)
        .map_err(|error| Problem::new("stage a material graph", error.to_string()))?;
    std::fs::write(&source_stage, source)
        .map_err(|error| Problem::new("stage a material canvas", error.to_string()))?;
    let previous = std::fs::read(&source_path).ok();
    std::fs::rename(&source_stage, &source_path)
        .map_err(|error| Problem::new("save a material canvas", error.to_string()))?;
    if let Err(error) = std::fs::rename(&graph_stage, &graph_path) {
        match previous {
            Some(bytes) => {
                let _ = std::fs::write(&source_path, bytes);
            }
            None => {
                let _ = std::fs::remove_file(&source_path);
            }
        }
        return Err(Problem::new("save a material graph", error.to_string()));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn graph_paths_reject_escape_and_non_graph_files() {
        let root = Path::new("/tmp/project");
        assert!(paths(root, "../outside.cygraph").is_err());
        assert!(paths(root, "/absolute.cygraph").is_err());
        assert!(paths(root, "materials/mat.swift").is_err());
        assert!(paths(root, "materials/mat.cygraph").is_ok());
    }

    #[test]
    fn authored_graph_and_editable_canvas_round_trip_together() {
        let root = std::env::temp_dir().join(format!(
            "cy-material-graph-save-{}-{:?}",
            std::process::id(),
            std::thread::current().id()
        ));
        std::fs::create_dir_all(&root).unwrap();
        save(
            &root,
            "materials/cube.cygraph",
            "cymatcanvas 1\n",
            "cygraph 1\n",
        )
        .unwrap();
        assert_eq!(
            read(&root, "materials/cube.cygraph").unwrap(),
            "cymatcanvas 1\n"
        );
        assert_eq!(
            std::fs::read_to_string(root.join("materials/cube.cygraph")).unwrap(),
            "cygraph 1\n"
        );
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn undo_payload_distinguishes_missing_files_from_empty_files() {
        let payload = encode_pair(Some(""), None);
        assert_eq!(decode_pair(&payload).unwrap(), (Some(String::new()), None));
    }
}
