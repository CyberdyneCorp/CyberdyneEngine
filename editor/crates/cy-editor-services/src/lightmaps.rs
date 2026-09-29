//! Baking a level's lightmaps from inside the editor, with progress and cancellation.
//!
//! `rendering-global-illumination` — "Lightmap baking", and the "Lighting & lightmaps" row of the
//! editor's missing tools (issue #29). The bake is the engine's own (`lightmap_bake::bake_lightmaps`),
//! run the way [`crate::assets`] runs the importer: out of process, as `cy_build lightmap`, because
//! `tools/build` is layer 7 and nothing links layer 7 — and because a bake that takes minutes must
//! never hold the interface thread, which [`crate::OperationService`] already guarantees.
//!
//! --- THE PROTOCOL IS THE TOOL'S, READ LINE BY LINE ----------------------------------------------
//!
//! `cy_build lightmap` prints `progress <stage> <done> <total>` as the bake reports it, a `baked`
//! line with what it made, or `cancelled`; and it stops at a `cancel` line on its stdin, writing
//! nothing. So the operation's progress bar is the bake's own count of traced texels, and a
//! cancelled bake leaves the previous cooked lightmap exactly where it was — "cancellation SHALL
//! leave the project in a valid state" (`editor-ui-ux`) by construction rather than by cleanup.
//!
//! --- WHY IT IS A TRAIT ---------------------------------------------------------------------------
//!
//! [`LightmapBaker`] is what the service drives and [`CliLightmapBaker`] is what the editor gives
//! it, for the reason [`crate::assets::ImportRunner`] is a trait: a test supplies a double and holds
//! the command's own behaviour — refusals, progress, cancellation — with no engine built, and
//! `integration.build_lightmap_cli` holds the real tool to the protocol from the engine's side.

use std::collections::BTreeMap;
use std::io::{BufRead, BufReader, Write as _};
use std::path::{Path, PathBuf};
use std::process::{Command as Process, Stdio};
use std::sync::Arc;
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, Sender};
use std::time::Duration;

use cy_editor_commands::{
    Arguments, Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, Registry,
};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::progress::Cancellation;
use cy_editor_core::value::{Value, ValueKind};

use crate::OperationService;
use crate::assets::COOKED_DIRECTORY;

/// The bake's stages, in the order `cy_build lightmap` reports them, with the share of a bake's
/// time each takes: the trace is nearly all of it (`lightmap_bake/README.md`, "What it costs").
const STAGES: [(&str, f32, f32); 4] = [
    ("prepare", 0.0, 0.05),
    ("trace", 0.05, 0.90),
    ("filter", 0.90, 0.97),
    ("finish", 0.97, 1.0),
];

/// One progress report from the bake.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct BakeStep {
    /// `prepare`, `trace`, `filter` or `finish`.
    pub stage: String,
    /// Units of the stage done: atlas texels for the trace, one for the rest.
    pub done: u64,
    /// Units the stage has.
    pub total: u64,
}

impl BakeStep {
    /// How far the whole bake is, from the stage and its count.
    #[must_use]
    pub fn fraction(&self) -> f32 {
        let Some(&(_, from, to)) = STAGES.iter().find(|(name, _, _)| *name == self.stage) else {
            return 0.0;
        };
        #[allow(clippy::cast_precision_loss)]
        let within = if self.total == 0 {
            0.0
        } else {
            (self.done.min(self.total) as f32) / (self.total as f32)
        };
        from + ((to - from) * within)
    }

    /// What the progress surface says it is doing.
    #[must_use]
    pub fn describe(&self) -> String {
        match self.stage.as_str() {
            "trace" => format!("tracing texel {} of {}", self.done, self.total),
            "prepare" => "packing and rasterising the atlas".to_string(),
            "filter" => "denoising, dilating and stitching seams".to_string(),
            "finish" => "building the mip chain and encoding".to_string(),
            other => other.to_string(),
        }
    }
}

/// What a finished bake made.
#[derive(Clone, PartialEq, Debug)]
pub struct LightmapBakeOutcome {
    /// The cooked lightmap, project-relative.
    pub output: String,
    /// Objects that received a rectangle.
    pub objects: u32,
    /// Atlas pages.
    pub pages: u32,
    /// Texels the path tracer traced.
    pub texels: u64,
    /// Bytes the planes and the shadow mask occupy on the device, every mip level.
    pub device_bytes: u64,
    /// Mip levels below the base.
    pub mips: u32,
    /// Objects whose charts are closer together than the protected mip levels need.
    pub padding_short: u32,
    /// Wall-clock seconds the bake took.
    pub seconds: f64,
}

/// One line of `cy_build lightmap`'s output, parsed.
#[derive(Clone, PartialEq, Debug)]
pub enum BakeLine {
    /// `progress <stage> <done> <total>`
    Progress(BakeStep),
    /// `baked key=value ...`
    Baked(BTreeMap<String, String>),
    /// `cancelled`
    Cancelled,
}

/// Parse one output line; `None` for a line the protocol does not have.
#[must_use]
pub fn parse_line(line: &str) -> Option<BakeLine> {
    let mut words = line.split_whitespace();
    match words.next()? {
        "progress" => {
            let stage = words.next()?.to_string();
            let done = words.next()?.parse().ok()?;
            let total = words.next()?.parse().ok()?;
            Some(BakeLine::Progress(BakeStep { stage, done, total }))
        }
        "baked" => Some(BakeLine::Baked(
            words
                .filter_map(|word| word.split_once('='))
                .map(|(key, value)| (key.to_string(), value.to_string()))
                .collect(),
        )),
        "cancelled" => Some(BakeLine::Cancelled),
        _ => None,
    }
}

/// The outcome a `baked` line describes.
///
/// # Errors
///
/// When a field the protocol names is missing or not a number.
pub fn outcome_of(output: &str, fields: &BTreeMap<String, String>) -> Result<LightmapBakeOutcome> {
    fn field<T: std::str::FromStr>(fields: &BTreeMap<String, String>, name: &str) -> Result<T> {
        fields
            .get(name)
            .and_then(|value| value.parse().ok())
            .ok_or_else(|| {
                Problem::new(
                    "read the lightmap bake's result",
                    format!("its `baked` line has no {name}"),
                )
            })
    }
    Ok(LightmapBakeOutcome {
        output: output.to_string(),
        objects: field(fields, "objects")?,
        pages: field(fields, "pages")?,
        texels: field(fields, "texels")?,
        device_bytes: field(fields, "bytes")?,
        mips: field(fields, "mips")?,
        padding_short: field(fields, "padding-short")?,
        seconds: field(fields, "seconds")?,
    })
}

/// How a bake is actually run.
pub trait LightmapBaker: Send + Sync {
    /// What this baker is, for a message a person reads.
    fn describe(&self) -> String;

    /// Bake `description` (project-relative) into `output` (project-relative) under `root`,
    /// reporting each step. `Ok(None)` when `cancellation` stopped it, having written nothing.
    ///
    /// # Errors
    ///
    /// When the bake cannot run or reports a failure; the problem carries the tool's own reason.
    fn bake(
        &self,
        root: &Path,
        description: &str,
        output: &str,
        step: &mut dyn FnMut(BakeStep),
        cancellation: &Cancellation,
    ) -> Result<Option<LightmapBakeOutcome>>;
}

/// `cy_build lightmap`, run as a child process.
#[derive(Clone, Debug)]
pub struct CliLightmapBaker {
    tool: Option<PathBuf>,
}

impl CliLightmapBaker {
    /// A baker that runs this `cy_build`.
    #[must_use]
    pub fn at(tool: impl Into<PathBuf>) -> Self {
        Self {
            tool: Some(tool.into()),
        }
    }

    /// `CY_BUILD` when it is set, and otherwise the first `build/<profile>/tools/build/cy_build`
    /// found walking up from the project — the search [`crate::assets::CliImportRunner`] makes, for
    /// the reason it gives.
    #[must_use]
    pub fn found_near(root: &Path) -> Self {
        if let Some(named) = std::env::var_os("CY_BUILD") {
            let path = PathBuf::from(named);
            if path.is_file() {
                return Self { tool: Some(path) };
            }
        }
        let relative = Path::new("tools").join("build").join("cy_build");
        let mut directory = Some(root);
        while let Some(current) = directory {
            for profile in ["dev", "profile", "release", "debug"] {
                let candidate = current.join("build").join(profile).join(&relative);
                if candidate.is_file() {
                    return Self {
                        tool: Some(candidate),
                    };
                }
            }
            directory = current.parent();
        }
        Self { tool: None }
    }

    fn tool(&self) -> Result<&Path> {
        self.tool.as_deref().ok_or_else(|| {
            Problem::new("run the lightmap bake", "no cy_build could be found").with_remedy(
                "build the engine's tools (just build-engine), or set CY_BUILD to the cy_build \
                 binary",
            )
        })
    }
}

/// The child's stdout, a line at a time, off the thread that watches for cancellation.
fn read_lines(stdout: std::process::ChildStdout) -> Receiver<String> {
    let (sender, receiver): (Sender<String>, Receiver<String>) = mpsc::channel();
    std::thread::Builder::new()
        .name("cy-editor-lightmap-bake".into())
        .spawn(move || {
            for line in BufReader::new(stdout)
                .lines()
                .map_while(std::io::Result::ok)
            {
                if sender.send(line).is_err() {
                    return;
                }
            }
        })
        .expect("spawning the lightmap bake's reader");
    receiver
}

impl LightmapBaker for CliLightmapBaker {
    fn describe(&self) -> String {
        self.tool.as_ref().map_or_else(
            || "no cy_build found: set CY_BUILD, or build the engine's tools".to_string(),
            |tool| tool.display().to_string(),
        )
    }

    fn bake(
        &self,
        root: &Path,
        description: &str,
        output: &str,
        step: &mut dyn FnMut(BakeStep),
        cancellation: &Cancellation,
    ) -> Result<Option<LightmapBakeOutcome>> {
        let tool = self.tool()?.to_path_buf();
        if let Some(parent) = root.join(output).parent() {
            std::fs::create_dir_all(parent)
                .map_err(|error| Problem::new(format!("bake {description}"), error.to_string()))?;
        }
        let mut child = Process::new(&tool)
            .arg("lightmap")
            .arg("--description")
            .arg(root.join(description))
            .arg("--project")
            .arg(root)
            .arg("--out")
            .arg(root.join(output))
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .map_err(|error| {
                Problem::new(format!("run {}", tool.display()), error.to_string())
                    .with_remedy("check that cy_build is executable")
            })?;
        let lines = read_lines(child.stdout.take().expect("stdout is piped"));
        let mut stdin = child.stdin.take();
        let mut baked = None;
        let mut asked_to_stop = false;
        loop {
            if cancellation.is_cancelled() && !asked_to_stop {
                // The tool's own cancel, so it stops at its next step and writes nothing.
                if let Some(mut input) = stdin.take() {
                    let _ = input.write_all(b"cancel\n");
                }
                asked_to_stop = true;
            }
            match lines.recv_timeout(Duration::from_millis(20)) {
                Ok(line) => match parse_line(&line) {
                    Some(BakeLine::Progress(report)) => step(report),
                    Some(BakeLine::Baked(fields)) => baked = Some(outcome_of(output, &fields)?),
                    Some(BakeLine::Cancelled) | None => {}
                },
                Err(RecvTimeoutError::Timeout) => {}
                Err(RecvTimeoutError::Disconnected) => break,
            }
        }
        drop(stdin);
        let status = child.wait().map_err(|error| {
            Problem::new(format!("wait for {}", tool.display()), error.to_string())
        })?;
        if status.code() == Some(3) {
            return Ok(None);
        }
        if status.success()
            && let Some(outcome) = baked
        {
            return Ok(Some(outcome));
        }
        let mut complaint = String::new();
        if let Some(mut stderr) = child.stderr.take() {
            let _ = std::io::Read::read_to_string(&mut stderr, &mut complaint);
        }
        Err(Problem::new(
            format!("bake {description}"),
            if complaint.trim().is_empty() {
                format!("cy_build lightmap exited with {status}")
            } else {
                complaint.trim().to_string()
            },
        )
        .with_remedy("fix the description or the meshes it names, and bake again"))
    }
}

/// The end of one bake the service started.
#[derive(Clone, Debug)]
pub struct LightmapBakeCompletion {
    /// The operation's stable request identity.
    pub request: u64,
    /// What it made; `Ok(None)` when it was cancelled.
    pub result: Result<Option<LightmapBakeOutcome>>,
}

/// The editor's lightmap bakes: started as operations, one at a time per output.
pub struct LightmapBakeService {
    root: PathBuf,
    baker: Arc<dyn LightmapBaker>,
    completed_tx: Sender<LightmapBakeCompletion>,
    completed_rx: Receiver<LightmapBakeCompletion>,
    /// The request of the bake most recently started, which `lighting.cancel-lightmap-bake` stops
    /// when it names none.
    latest: Option<u64>,
}

impl LightmapBakeService {
    /// The service for a project, with the real `cy_build` found the way the importer is.
    #[must_use]
    pub fn new(root: impl Into<PathBuf>) -> Self {
        let root = root.into();
        let baker = Arc::new(CliLightmapBaker::found_near(&root));
        Self::with_baker(root, baker)
    }

    /// The service over another baker: a test's double.
    #[must_use]
    pub fn with_baker(root: impl Into<PathBuf>, baker: Arc<dyn LightmapBaker>) -> Self {
        let (completed_tx, completed_rx) = mpsc::channel();
        Self {
            root: root.into(),
            baker,
            completed_tx,
            completed_rx,
            latest: None,
        }
    }

    /// Point the service at another project, keeping its baker.
    pub fn rooted_at(&mut self, root: impl Into<PathBuf>) {
        self.root = root.into();
    }

    /// What the baker is.
    #[must_use]
    pub fn describe(&self) -> String {
        self.baker.describe()
    }

    /// The bake most recently started.
    #[must_use]
    pub const fn latest(&self) -> Option<u64> {
        self.latest
    }

    /// Where a description's cooked lightmap goes when the caller names nowhere.
    #[must_use]
    pub fn default_output(description: &str) -> String {
        let stem = Path::new(description)
            .file_stem()
            .and_then(std::ffi::OsStr::to_str)
            .unwrap_or("level");
        format!("{COOKED_DIRECTORY}/lightmaps/{stem}.lightmap")
    }

    /// Start baking `description` into `output` (both project-relative; an empty `output` is
    /// [`Self::default_output`]) as an operation, and return its request identity.
    ///
    /// # Errors
    ///
    /// When the description is not a project-relative `.cylightmap` that exists, or the output
    /// leaves the project.
    pub fn start(
        &mut self,
        operations: &mut OperationService,
        description: &str,
        output: &str,
    ) -> Result<u64> {
        let description = description.trim().to_string();
        let output = if output.trim().is_empty() {
            Self::default_output(&description)
        } else {
            output.trim().to_string()
        };
        self.check(&description, &output)?;
        let root = self.root.clone();
        let baker = Arc::clone(&self.baker);
        let completed = self.completed_tx.clone();
        let label = format!("Bake lightmaps {description}");
        let operation = operations.start(label, move |operation| {
            operation.report(Some(0.0), "starting the bake");
            let cancellation = operation.cancellation();
            let mut report =
                |step: BakeStep| operation.report(Some(step.fraction()), step.describe());
            let result = baker.bake(&root, &description, &output, &mut report, &cancellation);
            let request = operation.id();
            let settled = match &result {
                Ok(_) => Ok(()),
                Err(problem) => Err(problem.clone()),
            };
            let _ = completed.send(LightmapBakeCompletion { request, result });
            settled
        });
        let request = operation.id();
        self.latest = Some(request);
        Ok(request)
    }

    /// Bakes that have ended since the last call, without waiting.
    pub fn take_completed(&mut self) -> Vec<LightmapBakeCompletion> {
        self.completed_rx.try_iter().collect()
    }

    fn check(&self, description: &str, output: &str) -> Result<()> {
        let refuse = |because: String| {
            Err(Problem::new(format!("bake {description}"), because)
                .with_remedy("name a project-relative .cylightmap description and output"))
        };
        for path in [description, output] {
            let candidate = Path::new(path);
            if path.is_empty()
                || candidate.is_absolute()
                || candidate
                    .components()
                    .any(|part| matches!(part, std::path::Component::ParentDir))
            {
                return refuse(format!("{path:?} is not a path inside the project"));
            }
        }
        if Path::new(description)
            .extension()
            .and_then(std::ffi::OsStr::to_str)
            != Some("cylightmap")
        {
            return refuse("a lightmap bake reads a .cylightmap description".to_string());
        }
        if !self.root.join(description).is_file() {
            return refuse(format!("the project has no {description}"));
        }
        Ok(())
    }
}

// --- The commands --------------------------------------------------------------------------------

/// Register `lighting.bake-lightmaps` and `lighting.cancel-lightmap-bake`.
///
/// # Errors
///
/// When the registry refuses a command's metadata.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(bake_lightmaps())?;
    registry.register(cancel_lightmap_bake())?;
    Ok(())
}

fn within_scope(context: &dyn CommandContext, path: &str) -> Result<()> {
    let Some((scope, directories)) = context.permitted_paths() else {
        return Ok(());
    };
    if directories
        .iter()
        .any(|prefix| path.starts_with(prefix.as_str()))
    {
        return Ok(());
    }
    Err(Problem::new(
        format!("bake {path}"),
        format!("the scope {scope:?} does not include it"),
    )
    .with_remedy(format!(
        "the directories it may touch are: {}",
        directories.join(", ")
    )))
}

fn bake_lightmaps() -> Command {
    Command::new(
        Metadata::new(
            "lighting.bake-lightmaps",
            "Bake Lightmaps",
            "Lighting",
            "Bakes a level's lightmaps from its project-relative .cylightmap description with the \
             engine's path tracer, off the interface thread. Returns as soon as the bake is \
             queued, with a request identity the progress surface and \
             lighting.cancel-lightmap-bake use; the operation reports the texels traced. A \
             cancelled bake writes nothing, leaving the previous cooked lightmap in place.",
            EffectClass::ExternalEffect,
        )
        .with(ParameterSpec::required(
            "description",
            ValueKind::Text,
            "The level's project-relative .cylightmap description.",
        ))
        .with(ParameterSpec::optional(
            "output",
            ValueKind::Text,
            "Where the cooked lightmap goes, project-relative; .cy/cooked/lightmaps/<level>.lightmap \
             when omitted.",
            Value::Text(String::new()),
        )),
        |context, arguments: &Arguments| {
            let description = arguments.text("description").unwrap_or_default().trim();
            if description.is_empty() {
                return Err(Problem::new("bake lightmaps", "no description was given"));
            }
            within_scope(context, description)?;
            let output = arguments.text("output").unwrap_or_default().trim();
            let request = context.start_lightmap_bake(description, output)?;
            Ok(
                Outcome::new(format!("Queued the lightmap bake of {description} as #{request}"))
                    .with("request", Value::Int(i64::try_from(request).unwrap_or(i64::MAX)))
                    .with("description", Value::Text(description.to_string())),
            )
        },
    )
}

fn cancel_lightmap_bake() -> Command {
    Command::new(
        Metadata::new(
            "lighting.cancel-lightmap-bake",
            "Cancel Lightmap Bake",
            "Lighting",
            "Stops a running lightmap bake at its next step: the one a request identity names, or \
             the most recently started. Changes nothing in the project — a cancelled bake writes \
             nothing.",
            EffectClass::Read,
        )
        .with(ParameterSpec::optional(
            "request",
            ValueKind::Int,
            "The bake's request identity, as lighting.bake-lightmaps returned it; the latest bake \
             when omitted.",
            Value::Int(0),
        )),
        |context, arguments: &Arguments| {
            let request = match arguments.get("request") {
                Some(Value::Int(value)) => u64::try_from(*value).ok().filter(|value| *value != 0),
                _ => None,
            };
            let stopped = context.cancel_lightmap_bake(request)?;
            Ok(
                Outcome::new(format!("Asked lightmap bake #{stopped} to stop")).with(
                    "request",
                    Value::Int(i64::try_from(stopped).unwrap_or(i64::MAX)),
                ),
            )
        },
    )
}

#[cfg(test)]
mod tests {
    use std::sync::Mutex;
    use std::time::Instant;

    use cy_editor_core::progress::OperationState;

    use super::*;

    #[test]
    fn the_tools_lines_parse_into_steps_and_an_outcome() {
        assert_eq!(
            parse_line("progress trace 2048 65536"),
            Some(BakeLine::Progress(BakeStep {
                stage: "trace".into(),
                done: 2048,
                total: 65536
            }))
        );
        assert_eq!(parse_line("cancelled"), Some(BakeLine::Cancelled));
        assert_eq!(
            parse_line("progress trace"),
            None,
            "a short line is not a step"
        );
        assert_eq!(parse_line("wrote something else"), None);
        let Some(BakeLine::Baked(fields)) = parse_line(
            "baked objects=2 pages=1 texels=2312 dilated=576 rays=56429 bytes=1966080 mips=1 \
             padding-short=0 seconds=0.064",
        ) else {
            panic!("a baked line");
        };
        let outcome = outcome_of("out.lightmap", &fields).unwrap();
        assert_eq!(outcome.objects, 2);
        assert_eq!(outcome.texels, 2312);
        assert_eq!(outcome.device_bytes, 1_966_080);
        assert_eq!(outcome.mips, 1);
        fields_missing_is_refused();
    }

    fn fields_missing_is_refused() {
        let Some(BakeLine::Baked(fields)) = parse_line("baked objects=2") else {
            panic!("a baked line");
        };
        assert!(outcome_of("out.lightmap", &fields).is_err());
    }

    #[test]
    fn progress_is_the_traces_share_of_the_bake() {
        let step = |stage: &str, done, total| BakeStep {
            stage: stage.into(),
            done,
            total,
        };
        assert!(step("prepare", 0, 1).fraction().abs() < 1.0e-6);
        assert!((step("trace", 0, 100).fraction() - 0.05).abs() < 1.0e-6);
        assert!((step("trace", 50, 100).fraction() - 0.475).abs() < 1.0e-6);
        assert!((step("finish", 1, 1).fraction() - 1.0).abs() < 1.0e-6);
        assert!(step("trace", 10, 100).describe().contains("10 of 100"));
    }

    /// A baker that reports a trace in `steps` steps, stopping when cancelled, and records what it
    /// was asked.
    struct Recording {
        steps: u64,
        asked: Mutex<Vec<(String, String)>>,
        gate: Option<Arc<std::sync::Barrier>>,
    }

    impl LightmapBaker for Recording {
        fn describe(&self) -> String {
            "a recording double".into()
        }

        fn bake(
            &self,
            _: &Path,
            description: &str,
            output: &str,
            step: &mut dyn FnMut(BakeStep),
            cancellation: &Cancellation,
        ) -> Result<Option<LightmapBakeOutcome>> {
            self.asked
                .lock()
                .unwrap()
                .push((description.into(), output.into()));
            if let Some(gate) = &self.gate {
                gate.wait();
            }
            for done in 0..=self.steps {
                if cancellation.is_cancelled() {
                    return Ok(None);
                }
                step(BakeStep {
                    stage: "trace".into(),
                    done,
                    total: self.steps,
                });
                std::thread::sleep(Duration::from_millis(2));
            }
            Ok(Some(LightmapBakeOutcome {
                output: output.into(),
                objects: 2,
                pages: 1,
                texels: 100,
                device_bytes: 1024,
                mips: 1,
                padding_short: 0,
                seconds: 0.01,
            }))
        }
    }

    fn project_with_level() -> (tempfile_dir::Dir, PathBuf) {
        let dir = tempfile_dir::Dir::new("lightmap-bake");
        let root = dir.path().to_path_buf();
        std::fs::create_dir_all(root.join("levels")).unwrap();
        std::fs::write(root.join("levels/corner.cylightmap"), "cylightmap 1\n").unwrap();
        (dir, root)
    }

    #[test]
    fn a_bake_runs_as_an_operation_and_reports_the_traces_progress() {
        let (_dir, root) = project_with_level();
        let baker = Arc::new(Recording {
            steps: 50,
            asked: Mutex::new(Vec::new()),
            gate: None,
        });
        let mut service = LightmapBakeService::with_baker(&root, baker.clone());
        let mut operations = OperationService::new();
        let request = service
            .start(&mut operations, "levels/corner.cylightmap", "")
            .unwrap();
        let operation = operations
            .all()
            .iter()
            .find(|operation| operation.id() == request)
            .cloned()
            .unwrap();
        assert_eq!(
            operation.block_until_settled(Duration::from_secs(10)),
            OperationState::Completed
        );
        assert_eq!(
            baker.asked.lock().unwrap().as_slice(),
            &[(
                "levels/corner.cylightmap".to_string(),
                ".cy/cooked/lightmaps/corner.lightmap".to_string()
            )]
        );
        let completed = service.take_completed();
        assert_eq!(completed.len(), 1);
        assert_eq!(completed[0].request, request);
        let outcome = completed[0].result.clone().unwrap().unwrap();
        assert_eq!(outcome.objects, 2);
        assert_eq!(service.latest(), Some(request));
    }

    #[test]
    fn a_cancelled_bake_settles_cancelled_and_reports_no_outcome() {
        let (_dir, root) = project_with_level();
        let gate = Arc::new(std::sync::Barrier::new(2));
        let baker = Arc::new(Recording {
            steps: 100_000,
            asked: Mutex::new(Vec::new()),
            gate: Some(Arc::clone(&gate)),
        });
        let mut service = LightmapBakeService::with_baker(&root, baker);
        let mut operations = OperationService::new();
        let request = service
            .start(&mut operations, "levels/corner.cylightmap", "")
            .unwrap();
        // Held by the gate, so the bake cannot finish before it is cancelled.
        assert!(operations.cancel(request));
        gate.wait();
        let operation = operations
            .all()
            .iter()
            .find(|operation| operation.id() == request)
            .cloned()
            .unwrap();
        let started = Instant::now();
        assert_eq!(
            operation.block_until_settled(Duration::from_secs(10)),
            OperationState::Cancelled
        );
        assert!(started.elapsed() < Duration::from_secs(5));
        let completed = service.take_completed();
        assert!(matches!(completed[0].result, Ok(None)));
    }

    #[test]
    fn a_bake_outside_the_project_or_of_no_description_is_refused() {
        let (_dir, root) = project_with_level();
        let baker = Arc::new(Recording {
            steps: 1,
            asked: Mutex::new(Vec::new()),
            gate: None,
        });
        let mut service = LightmapBakeService::with_baker(&root, baker.clone());
        let mut operations = OperationService::new();
        for (description, output) in [
            ("../elsewhere.cylightmap", ""),
            ("/abs/level.cylightmap", ""),
            ("levels/missing.cylightmap", ""),
            ("levels/corner.txt", ""),
            ("levels/corner.cylightmap", "../outside.lightmap"),
        ] {
            assert!(
                service.start(&mut operations, description, output).is_err(),
                "{description} -> {output} was accepted"
            );
        }
        assert!(baker.asked.lock().unwrap().is_empty(), "nothing was baked");
        assert!(operations.all().is_empty(), "nothing was started");
    }

    #[test]
    fn the_commands_start_a_bake_and_cancel_it_through_the_registry() {
        use cy_editor_commands::Scope;

        let (_dir, root) = project_with_level();
        let gate = Arc::new(std::sync::Barrier::new(2));
        let baker = Arc::new(Recording {
            steps: 100_000,
            asked: Mutex::new(Vec::new()),
            gate: Some(Arc::clone(&gate)),
        });
        let mut editor = crate::Editor::default()
            .with_lightmap_baker(LightmapBakeService::with_baker(&root, baker.clone()));
        let mut registry = Registry::new();
        register(&mut registry).unwrap();
        for id in ["lighting.bake-lightmaps", "lighting.cancel-lightmap-bake"] {
            assert!(registry.metadata(id).is_some(), "{id} is not registered");
        }

        // Cancelling before anything was baked is refused by name.
        let nothing = editor.invoke(
            &registry,
            "lighting.cancel-lightmap-bake",
            &Scope::unrestricted(),
            &Arguments::new(),
        );
        assert!(nothing.is_err());

        let started = editor
            .invoke(
                &registry,
                "lighting.bake-lightmaps",
                &Scope::unrestricted(),
                &Arguments::new().with(
                    "description",
                    Value::Text("levels/corner.cylightmap".into()),
                ),
            )
            .unwrap();
        let Some(Value::Int(request)) = started.values.get("request").cloned() else {
            panic!("the bake returns its request: {started:?}");
        };
        let request = u64::try_from(request).unwrap();
        // Cancel the latest bake — no request named — while the gate holds it.
        editor
            .invoke(
                &registry,
                "lighting.cancel-lightmap-bake",
                &Scope::unrestricted(),
                &Arguments::new(),
            )
            .unwrap();
        gate.wait();
        let operation = editor
            .operations
            .all()
            .iter()
            .find(|operation| operation.id() == request)
            .cloned()
            .unwrap();
        assert_eq!(
            operation.block_until_settled(Duration::from_secs(10)),
            OperationState::Cancelled
        );
        assert!(matches!(
            editor.lightmaps.take_completed()[0].result,
            Ok(None)
        ));
    }

    #[test]
    fn the_real_tool_bakes_and_cancels_when_one_is_built() {
        // Engine-side, `integration.build_lightmap_cli` holds cy_build to the protocol. This case
        // holds THIS reader to it too, over the real binaries, when the tree has built them; with
        // none it says so and proves nothing — the double cases above are the proof that runs
        // everywhere.
        let here = Path::new(env!("CARGO_MANIFEST_DIR"));
        let baker = CliLightmapBaker::found_near(here);
        let importer = here
            .ancestors()
            .map(|dir| dir.join("build/dev/tools/import/cy_import_cli"))
            .find(|path| path.is_file());
        let (Some(_), Some(importer)) = (baker.tool.as_ref(), importer) else {
            eprintln!("no cy_build and cy_import_cli built near this crate; skipping");
            return;
        };
        let (_dir, root) = project_with_level();
        let mesh = real::import_quad(&importer, &root);

        // A malformed description: the tool's own reason reaches the editor.
        std::fs::write(root.join("levels/corner.cylightmap"), "not a description\n").unwrap();
        let refused = baker.bake(
            &root,
            "levels/corner.cylightmap",
            ".cy/cooked/lightmaps/corner.lightmap",
            &mut |_| {},
            &Cancellation::new(),
        );
        let problem = refused.expect_err("a malformed description is refused");
        assert!(problem.because.contains("cylightmap"), "{problem:?}");

        // A real level: progress through the trace, and the outcome the tool reported.
        std::fs::write(root.join("levels/corner.cylightmap"), real::level(&mesh, 8)).unwrap();
        let mut steps = Vec::new();
        let outcome = baker
            .bake(
                &root,
                "levels/corner.cylightmap",
                ".cy/cooked/lightmaps/corner.lightmap",
                &mut |step| steps.push(step),
                &Cancellation::new(),
            )
            .unwrap()
            .expect("an uncancelled bake bakes");
        assert_eq!(outcome.objects, 2);
        assert!(outcome.texels > 0 && outcome.device_bytes > 0);
        let fractions: Vec<f32> = steps.iter().map(BakeStep::fraction).collect();
        assert!(fractions.windows(2).all(|pair| pair[0] <= pair[1]));
        assert!((fractions.last().copied().unwrap_or(0.0) - 1.0).abs() < 1.0e-6);
        let cooked = std::fs::read(root.join(".cy/cooked/lightmaps/corner.lightmap")).unwrap();
        assert_eq!(&cooked[..4], b"CYLM");

        // A slow level, cancelled at its first traced step: nothing written, the old one kept.
        std::fs::write(
            root.join("levels/slow.cylightmap"),
            real::level(&mesh, 4096),
        )
        .unwrap();
        let cancellation = Cancellation::new();
        let stop = cancellation.clone();
        let cancelled = baker
            .bake(
                &root,
                "levels/slow.cylightmap",
                ".cy/cooked/lightmaps/slow.lightmap",
                &mut |step| {
                    if step.stage == "trace" {
                        stop.cancel();
                    }
                },
                &cancellation,
            )
            .unwrap();
        assert!(cancelled.is_none());
        assert!(!root.join(".cy/cooked/lightmaps/slow.lightmap").exists());
        assert_eq!(
            std::fs::read(root.join(".cy/cooked/lightmaps/corner.lightmap")).unwrap(),
            cooked
        );
    }

    /// A quad cooked by the real importer, and a level of two of its instances.
    mod real {
        use std::path::Path;

        pub(super) fn import_quad(importer: &Path, root: &Path) -> String {
            let mut buffer = Vec::new();
            for value in [
                0.0_f32, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0,
            ] {
                buffer.extend_from_slice(&value.to_le_bytes());
            }
            for index in [0_u16, 1, 2, 2, 1, 3] {
                buffer.extend_from_slice(&index.to_le_bytes());
            }
            std::fs::create_dir_all(root.join("assets")).unwrap();
            std::fs::write(root.join("assets/quad.bin"), &buffer).unwrap();
            std::fs::write(
                root.join("assets/quad.gltf"),
                format!(
                    r#"{{"asset":{{"version":"2.0"}},"buffers":[{{"byteLength":{},"uri":"quad.bin"}}],"bufferViews":[{{"buffer":0,"byteOffset":0,"byteLength":48}},{{"buffer":0,"byteOffset":48,"byteLength":12}}],"accessors":[{{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3"}},{{"bufferView":1,"componentType":5123,"count":6,"type":"SCALAR"}}],"meshes":[{{"name":"Panel","primitives":[{{"attributes":{{"POSITION":0}},"indices":1,"mode":4}}]}}],"nodes":[{{"name":"Quad","mesh":0}}],"scenes":[{{"nodes":[0]}}],"scene":0}}"#,
                    buffer.len()
                ),
            )
            .unwrap();
            let output = std::process::Command::new(importer)
                .arg("--project")
                .arg(root)
                .arg("--out")
                .arg(root.join(".cy/cooked"))
                .arg("--cache")
                .arg(root.join(".cy/cache"))
                .args([
                    "--set",
                    "generate-lightmap-uvs=true",
                    "--json",
                    "assets/quad.gltf",
                ])
                .output()
                .unwrap();
            let report = String::from_utf8_lossy(&output.stdout);
            let marker = r#""name": "mesh/Panel", "id": ""#;
            let at = report.find(marker).expect("the importer reports the mesh") + marker.len();
            format!(".cy/cooked/{}.cyasset", &report[at..at + 32])
        }

        pub(super) fn level(mesh: &str, samples: u32) -> String {
            format!(
                "cylightmap 1\nmode directional\nbounces 1\nsamples {samples}\ndensity 8\n\
                 page 256\nsky 0.2 0.25 0.3\nlight point 0 1 1.5 20 20 1 1 1 stationary\n\
                 material \"white\" 0.7 0.7 0.7\n\
                 instance \"{mesh}\" \"mesh\" \"white\" 1 4 0 0 -2  0 4 0 0  0 0 4 0\n\
                 instance \"{mesh}\" \"mesh\" \"white\" 1 4 0 0 -2  0 0 4 0  0 -4 0 4\n"
            )
        }
    }
}

/// A temporary directory for the tests, removed when dropped.
#[cfg(test)]
mod tempfile_dir {
    use std::path::{Path, PathBuf};

    pub(super) struct Dir(PathBuf);

    impl Dir {
        pub(super) fn new(label: &str) -> Self {
            let unique = format!(
                "cy-editor-{label}-{}-{}",
                std::process::id(),
                std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .map_or(0, |elapsed| elapsed.as_nanos())
            );
            let path = std::env::temp_dir().join(unique);
            std::fs::create_dir_all(&path).unwrap();
            Self(path)
        }

        pub(super) fn path(&self) -> &Path {
            &self.0
        }
    }

    impl Drop for Dir {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
}
