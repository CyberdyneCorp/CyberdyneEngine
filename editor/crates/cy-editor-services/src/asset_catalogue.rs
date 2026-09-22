// SPDX-License-Identifier: MIT
//! Deterministic, observable project asset discovery for the Content Browser.

use std::path::{Component, Path, PathBuf};

use cy_editor_core::observe::Revision;
use cy_editor_core::problem::{Problem, Result};

/// One project-relative asset known to the browser.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct AssetEntry {
    /// Slash-separated path relative to the declared project root.
    pub path: String,
    /// Stable lower-case type label derived from the extension.
    pub kind: String,
    /// Fingerprint of the source plus its identity/import metadata sidecars.
    pub fingerprint: String,
    /// Stable imported identity, when this is a logical sub-asset.
    pub identity: Option<String>,
    /// Project-relative source which owns this entry.
    pub source: String,
    /// Stable name within `source`, absent for an ordinary source file.
    pub sub_asset: Option<String>,
}

/// Result of an identity-preserving asset move.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum AssetMove {
    /// Source and every present metadata sidecar moved together.
    Moved {
        /// Fingerprint of the moved bundle at its destination.
        fingerprint: String,
    },
    /// Source or metadata changed since the browser presented it.
    Conflict {
        /// Current fingerprint of the bundle on disk.
        actual: String,
    },
}

/// A project asset index refreshed explicitly, never by an idle UI frame.
#[derive(Debug)]
pub struct AssetCatalogueService {
    root: PathBuf,
    entries: Vec<AssetEntry>,
    revision: Revision,
}

impl AssetCatalogueService {
    /// An empty index rooted at a project.
    #[must_use]
    pub fn new(root: impl Into<PathBuf>) -> Self {
        Self {
            root: root.into(),
            entries: Vec::new(),
            revision: Revision::INITIAL,
        }
    }

    /// Re-index project files in deterministic path order.
    pub fn refresh(&mut self) -> Result<bool> {
        let mut entries = Vec::new();
        if self.root.is_dir() {
            visit(&self.root, &self.root, &mut entries)?;
        }
        entries.sort_by(|left, right| left.path.cmp(&right.path));
        if entries == self.entries {
            return Ok(false);
        }
        self.entries = entries;
        self.revision = Revision::from_u64(self.revision.as_u64().saturating_add(1));
        Ok(true)
    }

    /// Indexed assets.
    #[must_use]
    pub fn entries(&self) -> &[AssetEntry] {
        &self.entries
    }

    /// Changes only when a refresh observes a different listing.
    #[must_use]
    pub const fn revision(&self) -> Revision {
        self.revision
    }

    /// Fingerprint source bytes together with its `.meta` and `.import` identity records.
    pub fn fingerprint(&self, path: &str) -> Result<String> {
        let source = self.resolve(path)?;
        if !source.is_file() {
            return Ok("missing".to_string());
        }
        fingerprint_bundle(&source)
    }

    /// Move a source and its present metadata only when the observed bundle is still current.
    pub fn move_if_unchanged(&mut self, from: &str, to: &str, expected: &str) -> Result<AssetMove> {
        let source = self.resolve(from)?;
        let destination = self.resolve(to)?;
        if source == destination {
            return Err(
                Problem::new("move an asset", "source and destination are the same")
                    .with_remedy("choose a different name or folder"),
            );
        }
        let actual = self.fingerprint(from)?;
        if actual != expected {
            return Ok(AssetMove::Conflict { actual });
        }
        if !source.is_file() {
            return Err(Problem::not_found(format!("asset {from}")));
        }
        let sources = bundle_paths(&source);
        let destinations = bundle_paths(&destination);
        if destinations.iter().any(|path| path.exists()) {
            return Err(Problem::new(
                format!("move {from} to {to}"),
                "the destination or one of its metadata sidecars already exists",
            )
            .with_remedy("choose an unused destination; assets are never overwritten by move"));
        }
        if let Some(parent) = destination.parent() {
            std::fs::create_dir_all(parent).map_err(|error| {
                Problem::new(format!("create {}", parent.display()), error.to_string())
            })?;
        }
        let mut moved = Vec::new();
        for (old, new) in sources.into_iter().zip(destinations) {
            if !old.exists() {
                continue;
            }
            if let Err(error) = std::fs::rename(&old, &new) {
                for (rollback_from, rollback_to) in moved.into_iter().rev() {
                    let _ = std::fs::rename(rollback_from, rollback_to);
                }
                return Err(Problem::new(
                    format!("move {} to {}", old.display(), new.display()),
                    error.to_string(),
                )
                .with_remedy("check that both asset folders are writable"));
            }
            moved.push((new, old));
        }
        let fingerprint = self.fingerprint(to)?;
        self.refresh()?;
        Ok(AssetMove::Moved { fingerprint })
    }

    fn resolve(&self, path: &str) -> Result<PathBuf> {
        let relative = Path::new(path);
        if path.trim().is_empty()
            || relative.is_absolute()
            || relative.components().any(|part| {
                matches!(
                    part,
                    Component::ParentDir | Component::RootDir | Component::Prefix(_)
                )
            })
        {
            return Err(Problem::new(
                format!("resolve asset path {path:?}"),
                "an asset path must stay project-relative",
            )
            .with_remedy("choose a path inside the project"));
        }
        let full = self.root.join(relative);
        let mut ancestor = full.as_path();
        while !ancestor.exists() {
            ancestor = ancestor.parent().ok_or_else(|| {
                Problem::new(
                    format!("resolve {path:?}"),
                    "the path has no project ancestor",
                )
            })?;
        }
        let root = self
            .root
            .canonicalize()
            .unwrap_or_else(|_| self.root.clone());
        let canonical = ancestor.canonicalize().map_err(|error| {
            Problem::new(format!("resolve {}", ancestor.display()), error.to_string())
        })?;
        if !canonical.starts_with(root) {
            return Err(Problem::new(
                format!("resolve asset path {path:?}"),
                "a symlink leaves the project",
            ));
        }
        Ok(full)
    }
}

fn bundle_paths(source: &Path) -> [PathBuf; 3] {
    let mut meta = source.as_os_str().to_os_string();
    meta.push(".meta");
    let mut import = source.as_os_str().to_os_string();
    import.push(".import");
    [
        source.to_path_buf(),
        PathBuf::from(meta),
        PathBuf::from(import),
    ]
}

fn hash_label(digest: &mut u64, label: Option<&str>) {
    hash_bytes(digest, label.unwrap_or_default().as_bytes());
}

fn hash_bytes(digest: &mut u64, bytes: &[u8]) {
    for byte in bytes {
        *digest ^= u64::from(*byte);
        *digest = digest.wrapping_mul(0x0000_0100_0000_01b3);
    }
}

fn visit(root: &Path, directory: &Path, entries: &mut Vec<AssetEntry>) -> Result<()> {
    let mut children = std::fs::read_dir(directory)
        .map_err(|error| Problem::new(format!("read {}", directory.display()), error.to_string()))?
        .collect::<std::io::Result<Vec<_>>>()
        .map_err(|error| {
            Problem::new(format!("read {}", directory.display()), error.to_string())
        })?;
    children.sort_by_key(std::fs::DirEntry::file_name);
    for child in children {
        let file_type = child
            .file_type()
            .map_err(|error| Problem::new("inspect a project asset", error.to_string()))?;
        let path = child.path();
        if file_type.is_symlink() {
            continue;
        }
        if file_type.is_dir() {
            if !excluded_directory(&child.file_name().to_string_lossy()) {
                visit(root, &path, entries)?;
            }
        } else if file_type.is_file()
            && !path.to_string_lossy().ends_with(".meta")
            && !path.to_string_lossy().ends_with(".import")
            && let Ok(relative) = path.strip_prefix(root)
        {
            let relative_path = relative.to_string_lossy().replace('\\', "/");
            let identity = primary_identity(&path)?;
            entries.push(AssetEntry {
                kind: kind_of(&relative_path).to_string(),
                fingerprint: fingerprint_bundle(&path)?,
                identity,
                source: relative_path.clone(),
                sub_asset: None,
                path: relative_path.clone(),
            });
            append_imported_sub_assets(&path, &relative_path, entries)?;
        }
    }
    Ok(())
}

fn primary_identity(source: &Path) -> Result<Option<String>> {
    let mut path = source.as_os_str().to_os_string();
    path.push(".meta");
    let path = PathBuf::from(path);
    let text = match std::fs::read_to_string(&path) {
        Ok(text) => text,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(error) => {
            return Err(Problem::new(
                format!("read {}", path.display()),
                error.to_string(),
            ));
        }
    };
    Ok(text.lines().find_map(|line| {
        line.trim()
            .strip_prefix("id = ")
            .and_then(unquote)
            .map(str::to_string)
    }))
}

fn append_imported_sub_assets(
    source: &Path,
    relative: &str,
    entries: &mut Vec<AssetEntry>,
) -> Result<()> {
    let mut record_path = source.as_os_str().to_os_string();
    record_path.push(".import");
    let record_path = PathBuf::from(record_path);
    let record = match std::fs::read_to_string(&record_path) {
        Ok(record) => record,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(()),
        Err(error) => {
            return Err(Problem::new(
                format!("read {}", record_path.display()),
                error.to_string(),
            ));
        }
    };
    let fingerprint = fingerprint_bundle(source)?;
    for line in record.lines() {
        let Some(binding) = line.trim().strip_prefix("sub_asset.") else {
            continue;
        };
        let Some((name, identity)) = parse_import_binding(binding) else {
            return Err(Problem::new(
                format!("read {}", record_path.display()),
                format!("malformed sub-asset binding: {line}"),
            ));
        };
        entries.push(AssetEntry {
            path: format!("{relative}#{name}"),
            kind: sub_asset_kind(&name).to_string(),
            fingerprint: fingerprint.clone(),
            identity: Some(identity),
            source: relative.to_string(),
            sub_asset: Some(name),
        });
    }
    Ok(())
}

fn parse_import_binding(text: &str) -> Option<(String, String)> {
    let (name, identity) = text.split_once(" = ")?;
    Some((unquote(name)?.to_string(), unquote(identity)?.to_string()))
}

fn unquote(text: &str) -> Option<&str> {
    text.strip_prefix('"')?.strip_suffix('"')
}

fn sub_asset_kind(name: &str) -> &'static str {
    match name.split_once('/').map_or(name, |(prefix, _)| prefix) {
        "mesh" | "collision" => "mesh",
        "material" => "material",
        "texture" | "image" => "texture",
        "prefab" => "prefab",
        "animation" => "animation",
        _ => "file",
    }
}

fn fingerprint_bundle(source: &Path) -> Result<String> {
    let mut digest = 0xcbf2_9ce4_8422_2325_u64;
    let mut bytes = 0_u64;
    for candidate in bundle_paths(source) {
        hash_label(
            &mut digest,
            candidate.extension().and_then(|value| value.to_str()),
        );
        match std::fs::read(&candidate) {
            Ok(content) => {
                hash_bytes(&mut digest, &content);
                bytes = bytes.saturating_add(u64::try_from(content.len()).unwrap_or(u64::MAX));
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                hash_bytes(&mut digest, b"<missing>");
            }
            Err(error) => {
                return Err(Problem::new(
                    format!("read {}", candidate.display()),
                    error.to_string(),
                ));
            }
        }
    }
    Ok(format!("fnv1a64:{digest:016x}:{bytes}"))
}

fn excluded_directory(name: &str) -> bool {
    matches!(name, ".git" | ".cy" | "build" | "target")
}

fn kind_of(path: &str) -> &'static str {
    match Path::new(path)
        .extension()
        .and_then(std::ffi::OsStr::to_str)
        .unwrap_or_default()
        .to_ascii_lowercase()
        .as_str()
    {
        "cyworld" => "world",
        "cyprefab" => "prefab",
        "cymesh" | "obj" | "fbx" | "gltf" | "glb" => "mesh",
        "cymat" | "cygraph" => "material",
        "png" | "jpg" | "jpeg" | "tga" | "hdr" => "texture",
        "wav" | "ogg" | "mp3" => "audio",
        "swift" => "swift",
        _ => "file",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temporary() -> PathBuf {
        let test = std::thread::current().name().unwrap_or("test").to_string();
        let test = test
            .chars()
            .map(|character| {
                if character.is_ascii_alphanumeric() || matches!(character, '-' | '_') {
                    character
                } else {
                    '_'
                }
            })
            .collect::<String>();
        let path = std::env::temp_dir().join(format!(
            "cy-asset-catalogue-{}-{}",
            std::process::id(),
            test
        ));
        let _ = std::fs::remove_dir_all(&path);
        std::fs::create_dir_all(path.join("art/props")).unwrap();
        path
    }

    #[test]
    fn discovery_is_deterministic_typed_and_idle_is_free() {
        let root = temporary();
        std::fs::write(root.join("art/z.png"), []).unwrap();
        std::fs::write(root.join("art/props/a.obj"), []).unwrap();
        std::fs::write(root.join("Player.swift"), []).unwrap();
        std::fs::create_dir_all(root.join("target")).unwrap();
        std::fs::write(root.join("target/ignored.obj"), []).unwrap();
        let mut catalogue = AssetCatalogueService::new(&root);
        assert!(catalogue.refresh().unwrap());
        assert_eq!(
            catalogue
                .entries()
                .iter()
                .map(|entry| (entry.path.as_str(), entry.kind.as_str()))
                .collect::<Vec<_>>(),
            [
                ("Player.swift", "swift"),
                ("art/props/a.obj", "mesh"),
                ("art/z.png", "texture"),
            ]
        );
        let revision = catalogue.revision();
        assert!(!catalogue.refresh().unwrap());
        assert_eq!(catalogue.revision(), revision);
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn imported_sub_assets_are_stable_typed_logical_entries() {
        let root = temporary();
        let source = root.join("art/robot.fbx");
        std::fs::write(&source, b"fbx").unwrap();
        std::fs::write(
            root.join("art/robot.fbx.import"),
            "version = 1\nimporter = \"fbx\"\nsub_asset.\"mesh/Body\" = \"mesh-id\"\nsub_asset.\"material/Paint\" = \"material-id\"\nsub_asset.\"prefab\" = \"prefab-id\"\n",
        )
        .unwrap();
        std::fs::write(
            root.join("art/robot.fbx.meta"),
            "meta_version = 1\nid = \"prefab-id\"\nkind = \"prefab\"\n",
        )
        .unwrap();
        let mut catalogue = AssetCatalogueService::new(&root);
        catalogue.refresh().unwrap();

        let logical: Vec<_> = catalogue
            .entries()
            .iter()
            .filter(|entry| entry.sub_asset.is_some())
            .map(|entry| {
                (
                    entry.path.as_str(),
                    entry.kind.as_str(),
                    entry.identity.as_deref(),
                    entry.source.as_str(),
                )
            })
            .collect();
        assert_eq!(
            logical,
            [
                (
                    "art/robot.fbx#material/Paint",
                    "material",
                    Some("material-id"),
                    "art/robot.fbx"
                ),
                (
                    "art/robot.fbx#mesh/Body",
                    "mesh",
                    Some("mesh-id"),
                    "art/robot.fbx"
                ),
                (
                    "art/robot.fbx#prefab",
                    "prefab",
                    Some("prefab-id"),
                    "art/robot.fbx"
                ),
            ]
        );
        assert_eq!(
            catalogue
                .entries()
                .iter()
                .find(|entry| entry.path == "art/robot.fbx")
                .and_then(|entry| entry.identity.as_deref()),
            Some("prefab-id")
        );
    }

    #[test]
    fn move_keeps_metadata_refuses_collisions_traversal_and_external_changes() {
        let root = temporary();
        let source = root.join("art/props/chair.obj");
        std::fs::write(&source, "mesh").unwrap();
        std::fs::write(root.join("art/props/chair.obj.meta"), "stable-id").unwrap();
        std::fs::write(root.join("art/props/chair.obj.import"), "scale=1").unwrap();
        let mut catalogue = AssetCatalogueService::new(&root);
        catalogue.refresh().unwrap();
        let expected = catalogue.fingerprint("art/props/chair.obj").unwrap();

        std::fs::write(&source, "external mesh").unwrap();
        assert!(matches!(
            catalogue
                .move_if_unchanged("art/props/chair.obj", "art/chair.obj", &expected)
                .unwrap(),
            AssetMove::Conflict { .. }
        ));
        assert!(source.exists(), "a stale move changes nothing");

        let expected = catalogue.fingerprint("art/props/chair.obj").unwrap();
        assert!(matches!(
            catalogue
                .move_if_unchanged("art/props/chair.obj", "art/chair.obj", &expected)
                .unwrap(),
            AssetMove::Moved { .. }
        ));
        assert_eq!(
            std::fs::read_to_string(root.join("art/chair.obj.meta")).unwrap(),
            "stable-id"
        );
        assert_eq!(
            std::fs::read_to_string(root.join("art/chair.obj.import")).unwrap(),
            "scale=1"
        );

        std::fs::write(root.join("art/other.obj"), "occupied").unwrap();
        let expected = catalogue.fingerprint("art/chair.obj").unwrap();
        let collision = catalogue
            .move_if_unchanged("art/chair.obj", "art/other.obj", &expected)
            .expect_err("moves never overwrite");
        assert!(collision.because.contains("already exists"), "{collision}");
        assert!(
            catalogue
                .move_if_unchanged("art/chair.obj", "../outside.obj", &expected)
                .is_err()
        );
        std::fs::remove_dir_all(root).unwrap();
    }
}
