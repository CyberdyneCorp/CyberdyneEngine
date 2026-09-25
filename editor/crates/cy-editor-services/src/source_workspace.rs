// SPDX-License-Identifier: MIT
//! Project-relative Swift source discovery and conflict-safe file access.
//!
//! The source workspace owns facts about files on disk: which Swift sources exist, the fingerprint
//! of the bytes last observed, and whether an attempted save still targets those bytes. Text being
//! edited belongs to a view model, not here, so typing cannot mutate project state.

use std::fmt;
use std::path::{Component, Path, PathBuf};

use cy_editor_core::observe::{Revision, Versioned};
use cy_editor_core::problem::{Problem, Result};

/// The identity of one exact on-disk file state.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum SourceFingerprint {
    /// The path did not exist.
    Missing,
    /// The path held bytes with this length and FNV-1a digest.
    Present {
        /// FNV-1a digest of the complete byte sequence.
        digest: u64,
        /// Byte length, included to make the representation easier to inspect and validate.
        bytes: u64,
    },
}

impl SourceFingerprint {
    /// Fingerprint a byte slice deterministically, without process-randomised hashing.
    #[must_use]
    pub fn of(bytes: &[u8]) -> Self {
        let mut digest = 0xcbf2_9ce4_8422_2325_u64;
        for byte in bytes {
            digest ^= u64::from(*byte);
            digest = digest.wrapping_mul(0x0000_0100_0000_01b3);
        }
        Self::Present {
            digest,
            bytes: u64::try_from(bytes.len()).unwrap_or(u64::MAX),
        }
    }

    /// Parse the stable command-line/wire representation.
    pub fn parse(text: &str) -> Result<Self> {
        if text == "missing" {
            return Ok(Self::Missing);
        }
        let Some(rest) = text.strip_prefix("fnv1a64:") else {
            return Err(invalid_fingerprint(text));
        };
        let Some((digest, bytes)) = rest.split_once(':') else {
            return Err(invalid_fingerprint(text));
        };
        let digest = u64::from_str_radix(digest, 16).map_err(|_| invalid_fingerprint(text))?;
        let bytes = bytes
            .parse::<u64>()
            .map_err(|_| invalid_fingerprint(text))?;
        Ok(Self::Present { digest, bytes })
    }
}

impl fmt::Display for SourceFingerprint {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Missing => formatter.write_str("missing"),
            Self::Present { digest, bytes } => write!(formatter, "fnv1a64:{digest:016x}:{bytes}"),
        }
    }
}

fn invalid_fingerprint(text: &str) -> Problem {
    Problem::new(
        format!("read the source fingerprint {text:?}"),
        "it is neither `missing` nor `fnv1a64:<hex>:<byte-count>`",
    )
    .with_remedy("read the source again and use the fingerprint it reports")
}

/// One discovered Swift source.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct SourceFile {
    /// Project-relative path, with `/` separators.
    pub path: String,
    /// Fingerprint of the bytes observed during discovery.
    pub fingerprint: SourceFingerprint,
}

/// A source opened for editing.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct SourceSnapshot {
    /// Project-relative path.
    pub path: String,
    /// Exact UTF-8 text read from disk.
    pub text: String,
    /// Fingerprint of `text`.
    pub fingerprint: SourceFingerprint,
}

/// Result of a compare-and-save operation.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum SourceSave {
    /// The expected bytes still existed and were replaced.
    Written {
        /// Fingerprint of the new contents.
        fingerprint: SourceFingerprint,
    },
    /// The disk moved after the caller read its base.
    Conflict {
        /// What is on disk now.
        actual: SourceFingerprint,
        /// Current disk text, or `None` when the path was removed.
        disk: Option<String>,
    },
}

/// Observable source-file state for one project.
#[derive(Clone, Debug)]
pub struct SourceWorkspaceService {
    root: PathBuf,
    canonical_root: PathBuf,
    files: Versioned<Vec<SourceFile>>,
}

impl SourceWorkspaceService {
    /// A workspace rooted at a project directory. Discovery is explicit and therefore never blocks
    /// construction of the Editor.
    #[must_use]
    pub fn new(root: impl Into<PathBuf>) -> Self {
        let root = root.into();
        let canonical_root = root.canonicalize().unwrap_or_else(|_| root.clone());
        Self {
            root,
            canonical_root,
            files: Versioned::new(Vec::new()),
        }
    }

    /// Project root.
    #[must_use]
    pub fn root(&self) -> &Path {
        &self.root
    }

    /// Revision of the discovered path/fingerprint set.
    #[must_use]
    pub const fn revision(&self) -> Revision {
        self.files.revision()
    }

    /// Swift files in deterministic project-relative order.
    #[must_use]
    pub fn files(&self) -> &[SourceFile] {
        self.files.get()
    }

    /// Find the editable source that registers a Swift behaviour by its authored class name.
    pub fn behaviour_source(&self, name: &str) -> Result<String> {
        let name = name.trim();
        if name.is_empty() {
            return Err(
                Problem::new("open a Swift behaviour", "the class name is empty")
                    .with_remedy("enter a registered behaviour name in ScriptBehaviour.class"),
            );
        }
        for file in self.files() {
            let snapshot = self.open(&file.path)?;
            if declares_behaviour(&snapshot.text, name) {
                return Ok(file.path.clone());
            }
        }
        Err(Problem::new(
            format!("open Swift behaviour {name}"),
            "no project Swift source registers this behaviour",
        )
        .with_remedy("check the @Behaviour name and refresh the Swift Workspace"))
    }

    /// Rediscover Swift files and report whether the observable set changed.
    pub fn refresh(&mut self) -> Result<bool> {
        let mut files = Vec::new();
        self.walk(&self.root, &mut files)?;
        files.sort_by(|left, right| left.path.cmp(&right.path));
        if self.files.get() == &files {
            return Ok(false);
        }
        self.files.set(files);
        Ok(true)
    }

    /// Open a UTF-8 source and capture the exact disk base used by an editor buffer.
    pub fn open(&self, path: &str) -> Result<SourceSnapshot> {
        require_swift(path)?;
        let full = self.resolve(path)?;
        let bytes = std::fs::read(&full).map_err(|error| {
            Problem::new(format!("read {path}"), error.to_string())
                .with_remedy("refresh the Swift Workspace and choose an existing source")
        })?;
        let text = String::from_utf8(bytes.clone()).map_err(|_| {
            Problem::new(format!("edit {path}"), "the file is not UTF-8 text")
                .with_remedy("convert the source to UTF-8 or edit it with a binary-safe tool")
        })?;
        Ok(SourceSnapshot {
            path: path.to_string(),
            text,
            fingerprint: SourceFingerprint::of(&bytes),
        })
    }

    /// Current fingerprint, including the identity of a missing file.
    pub fn fingerprint(&self, path: &str) -> Result<SourceFingerprint> {
        let full = self.resolve(path)?;
        match std::fs::read(full) {
            Ok(bytes) => Ok(SourceFingerprint::of(&bytes)),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                Ok(SourceFingerprint::Missing)
            }
            Err(error) => Err(
                Problem::new(format!("fingerprint {path}"), error.to_string())
                    .with_remedy("check that the source is readable"),
            ),
        }
    }

    /// Replace a file only if it still has the fingerprint the caller edited from.
    pub fn save_if_unchanged(
        &mut self,
        path: &str,
        contents: &str,
        expected: SourceFingerprint,
    ) -> Result<SourceSave> {
        let full = self.resolve(path)?;
        let actual = self.fingerprint(path)?;
        if actual != expected {
            let disk = match std::fs::read_to_string(&full) {
                Ok(text) => Some(text),
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => None,
                Err(error) => {
                    return Err(Problem::new(
                        format!("read conflicting source {path}"),
                        error.to_string(),
                    )
                    .with_remedy("resolve the external file access problem, then save again"));
                }
            };
            return Ok(SourceSave::Conflict { actual, disk });
        }
        if let Some(parent) = full.parent() {
            std::fs::create_dir_all(parent).map_err(|error| {
                Problem::new(format!("create {}", parent.display()), error.to_string())
                    .with_remedy("check that the project is writable")
            })?;
        }
        std::fs::write(&full, contents).map_err(|error| {
            Problem::new(format!("write {path}"), error.to_string())
                .with_remedy("check that the project is writable")
        })?;
        let fingerprint = SourceFingerprint::of(contents.as_bytes());
        self.refresh()?;
        Ok(SourceSave::Written { fingerprint })
    }

    fn walk(&self, directory: &Path, found: &mut Vec<SourceFile>) -> Result<()> {
        let entries = match std::fs::read_dir(directory) {
            Ok(entries) => entries,
            Err(error)
                if directory == self.root && error.kind() == std::io::ErrorKind::NotFound =>
            {
                return Ok(());
            }
            Err(error) => {
                return Err(Problem::new(
                    format!("discover Swift sources under {}", directory.display()),
                    error.to_string(),
                )
                .with_remedy("check that the project source directories are readable"));
            }
        };
        for entry in entries {
            let entry = entry.map_err(|error| {
                Problem::new("read a project directory entry", error.to_string())
            })?;
            let file_type = entry.file_type().map_err(|error| {
                Problem::new(
                    format!("inspect {}", entry.path().display()),
                    error.to_string(),
                )
            })?;
            if file_type.is_symlink() {
                continue;
            }
            let path = entry.path();
            if file_type.is_dir() {
                if excluded_source_directory(&entry.file_name().to_string_lossy()) {
                    continue;
                }
                self.walk(&path, found)?;
            } else if path.extension().and_then(std::ffi::OsStr::to_str) == Some("swift") {
                if path == self.root.join("Package.swift") {
                    continue;
                }
                let relative = path.strip_prefix(&self.root).map_err(|_| {
                    Problem::new(
                        format!("discover {}", path.display()),
                        "the source is outside the project",
                    )
                })?;
                let path = relative
                    .to_string_lossy()
                    .replace(std::path::MAIN_SEPARATOR, "/");
                found.push(SourceFile {
                    fingerprint: self.fingerprint(&path)?,
                    path,
                });
            }
        }
        Ok(())
    }

    fn resolve(&self, path: &str) -> Result<PathBuf> {
        let relative = Path::new(path);
        let invalid = path.trim().is_empty()
            || relative.is_absolute()
            || relative.components().any(|component| {
                matches!(
                    component,
                    Component::ParentDir | Component::RootDir | Component::Prefix(_)
                )
            });
        if invalid {
            return Err(Problem::new(
                format!("resolve source path {path:?}"),
                "a source path must stay project-relative",
            )
            .with_remedy("choose a path inside the project, such as game/Player.swift"));
        }
        let full = self.root.join(relative);
        let mut existing = full.as_path();
        while !existing.exists() {
            existing = existing.parent().ok_or_else(|| {
                Problem::new(
                    format!("resolve source path {path:?}"),
                    "it has no project ancestor",
                )
            })?;
        }
        let canonical = existing.canonicalize().map_err(|error| {
            Problem::new(format!("resolve {}", existing.display()), error.to_string())
        })?;
        if !canonical.starts_with(&self.canonical_root) {
            return Err(Problem::new(
                format!("resolve source path {path:?}"),
                "a symlink leaves the project",
            )
            .with_remedy("store editable sources directly inside the project"));
        }
        Ok(full)
    }
}

fn excluded_source_directory(name: &str) -> bool {
    matches!(name, ".build" | ".git" | ".cy" | "build" | "target")
}

fn require_swift(path: &str) -> Result<()> {
    if Path::new(path)
        .extension()
        .and_then(std::ffi::OsStr::to_str)
        == Some("swift")
    {
        Ok(())
    } else {
        Err(Problem::new(
            format!("open {path} in the Swift Workspace"),
            "the path is not a .swift source",
        )
        .with_remedy("choose a .swift file, or use the content browser for another asset type"))
    }
}

fn declares_behaviour(source: &str, name: &str) -> bool {
    source.split("@Behaviour(").skip(1).any(|after| {
        after.split_once(')').is_some_and(|(arguments, _)| {
            arguments.split(',').any(|argument| {
                argument.split_once(':').is_some_and(|(key, value)| {
                    key.trim() == "name" && value.trim().trim_matches('"') == name
                })
            })
        })
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Sandbox(PathBuf);

    impl Sandbox {
        fn new(name: &str) -> Self {
            let unique = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map_or(0, |elapsed| elapsed.as_nanos());
            let path = std::env::temp_dir().join(format!(
                "cy-source-workspace-{name}-{}-{unique}",
                std::process::id()
            ));
            std::fs::create_dir_all(&path).unwrap();
            Self(path)
        }
    }

    impl Drop for Sandbox {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn discovery_is_swift_only_deterministic_and_revision_driven() {
        let sandbox = Sandbox::new("discover");
        std::fs::create_dir_all(sandbox.0.join("game/Zed")).unwrap();
        std::fs::write(sandbox.0.join("game/Zed/B.swift"), "struct B {}").unwrap();
        std::fs::write(sandbox.0.join("game/A.swift"), "struct A {}").unwrap();
        std::fs::write(sandbox.0.join("game/notes.txt"), "not source").unwrap();

        let mut service = SourceWorkspaceService::new(&sandbox.0);
        assert!(service.refresh().unwrap());
        assert_eq!(
            service
                .files()
                .iter()
                .map(|file| file.path.as_str())
                .collect::<Vec<_>>(),
            vec!["game/A.swift", "game/Zed/B.swift"]
        );
        let revision = service.revision();
        assert!(!service.refresh().unwrap());
        assert_eq!(service.revision(), revision, "idle discovery moves nothing");

        std::fs::write(sandbox.0.join("game/A.swift"), "struct A { var hp = 1 }").unwrap();
        assert!(service.refresh().unwrap());
        assert!(
            service.revision() > revision,
            "an external edit is observable"
        );
    }

    #[test]
    fn traversal_and_non_swift_open_are_refused() {
        let sandbox = Sandbox::new("refuse");
        let service = SourceWorkspaceService::new(&sandbox.0);
        assert!(service.fingerprint("../outside.swift").is_err());
        assert!(service.open("game/readme.txt").is_err());
    }

    #[test]
    fn save_compares_the_disk_fingerprint_before_writing() {
        let sandbox = Sandbox::new("conflict");
        std::fs::create_dir_all(sandbox.0.join("game")).unwrap();
        let path = sandbox.0.join("game/Player.swift");
        std::fs::write(&path, "struct Player {}").unwrap();
        let mut service = SourceWorkspaceService::new(&sandbox.0);
        let base = service.open("game/Player.swift").unwrap();

        std::fs::write(&path, "struct Player { var external = true }").unwrap();
        let saved = service
            .save_if_unchanged(
                "game/Player.swift",
                "struct Player { var hp = 3 }",
                base.fingerprint,
            )
            .unwrap();
        assert!(matches!(saved, SourceSave::Conflict { .. }));
        assert_eq!(
            std::fs::read_to_string(path).unwrap(),
            "struct Player { var external = true }"
        );
    }
}
