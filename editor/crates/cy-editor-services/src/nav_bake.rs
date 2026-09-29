// SPDX-License-Identifier: MIT
//! The payloads of the engine's `navigation.*` service. Issue #28, task 4.2.
//!
//! The engine owns every navigation computation: it voxelises, tiles, hashes the source geometry,
//! searches paths and picks points on the mesh. This module only encodes requests and decodes what
//! comes back, byte for byte as `src/editor_backend/README.md` ("Navigation operations") states the
//! wire. Nothing here hashes geometry or tests a ray, so an editor and an engine can never disagree
//! about a fingerprint.

use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::problem::{Problem, Result};

/// The most tiles, path points or flow-field cells a result may claim, so a corrupt count cannot
/// ask for an unbounded allocation. The engine caps a flow field at 65536 cells.
const MAX_RECORDS: u32 = 65_536;

fn invalid(reason: &str) -> Problem {
    Problem::new("read the navigation service result", reason)
        .with_remedy("rebuild the editor and runtime from compatible revisions")
}

fn count(input: &mut Reader<'_>) -> Result<usize> {
    let value = input.u32()?;
    if value > MAX_RECORDS {
        return Err(invalid("too many navigation records"));
    }
    usize::try_from(value).map_err(|_| invalid("too many navigation records"))
}

fn done(input: &Reader<'_>) -> Result<()> {
    if input.remaining() != 0 {
        return Err(invalid("trailing navigation result data"));
    }
    Ok(())
}

fn flag(input: &mut Reader<'_>) -> Result<bool> {
    Ok(input.u8()? != 0)
}

fn signed(input: &mut Reader<'_>) -> Result<i32> {
    Ok(i32::from_le_bytes(input.u32()?.to_le_bytes()))
}

fn vec3(input: &mut Reader<'_>) -> Result<[f32; 3]> {
    Ok([input.f32()?, input.f32()?, input.f32()?])
}

fn put_vec3(output: &mut Writer, value: [f32; 3]) {
    for lane in value {
        output.f32(lane);
    }
}

/// The back end a bake names. `Automatic` is not offered: a saved bake must say what produced it.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum NavBackend {
    /// The engine's own voxeliser. `cy::navigation::NavBuildBackend::Engine`.
    Engine,
    /// Recast. `cy::navigation::NavBuildBackend::Recast`.
    Recast,
}

impl NavBackend {
    /// The wire and document value: 1 Engine, 2 Recast.
    #[must_use]
    pub const fn code(self) -> i64 {
        match self {
            NavBackend::Engine => 1,
            NavBackend::Recast => 2,
        }
    }

    /// The word a command takes.
    #[must_use]
    pub const fn keyword(self) -> &'static str {
        match self {
            NavBackend::Engine => "engine",
            NavBackend::Recast => "recast",
        }
    }

    /// The back end a document value names, or `None` for `Automatic` or an unknown value.
    #[must_use]
    pub const fn from_code(code: i64) -> Option<Self> {
        match code {
            1 => Some(NavBackend::Engine),
            2 => Some(NavBackend::Recast),
            _ => None,
        }
    }

    /// The back end a word or a code names.
    #[must_use]
    pub fn parse(text: &str) -> Option<Self> {
        match text.trim().to_ascii_lowercase().as_str() {
            "engine" | "1" => Some(NavBackend::Engine),
            "recast" | "2" => Some(NavBackend::Recast),
            _ => None,
        }
    }
}

/// The settings block every bake and status request carries.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct NavSettingsBlock {
    /// Agent radius, metres.
    pub agent_radius: f32,
    /// Agent height, metres.
    pub agent_height: f32,
    /// Steepest walkable slope, degrees.
    pub max_slope: f32,
    /// Highest step an agent climbs, metres: `agent_max_climb`.
    pub step_height: f32,
    /// Voxel cell size, metres.
    pub cell_size: f32,
    /// Voxel cell height, metres.
    pub cell_height: f32,
    /// Tile size, metres: the `NavMesh` constructor argument.
    pub tile_size: f32,
    /// Source layer mask.
    pub layers: u64,
    /// Source tag mask.
    pub tags: u64,
    /// Which voxeliser bakes.
    pub backend: NavBackend,
}

/// The engine's largest tile, in voxel columns (`kMaxCellsPerTile` in `src/navigation`).
const MAX_CELLS_PER_TILE: f32 = 4_194_304.0;

fn positive(value: f32) -> bool {
    value.is_finite() && value > 0.0
}

impl NavSettingsBlock {
    /// The engine's defaults (`cy::navigation::NavBakeSettings`).
    pub const DEFAULT: Self = Self {
        agent_radius: 0.5,
        agent_height: 2.0,
        max_slope: 45.0,
        step_height: 0.4,
        cell_size: 0.3,
        cell_height: 0.2,
        tile_size: 16.0,
        layers: u64::MAX,
        tags: u64::MAX,
        backend: NavBackend::Engine,
    };

    /// Refuse what `validate_bake_settings` refuses, before a request is sent, so a person sees
    /// the reason at the field rather than as an engine failure a frame later. The engine checks
    /// again; it is the authority.
    pub fn validate(&self) -> Result<()> {
        let refuse = |because: &str| {
            Err(Problem::new("use these navigation settings", because)
                .with_remedy("edit the value with navigation.settings.set"))
        };
        if !positive(self.cell_size)
            || !positive(self.cell_height)
            || !positive(self.tile_size)
            || !positive(self.agent_height)
        {
            return refuse("cell size, cell height, tile size and agent height must be positive");
        }
        if !self.agent_radius.is_finite()
            || self.agent_radius < 0.0
            || !self.step_height.is_finite()
            || self.step_height < 0.0
            || !self.max_slope.is_finite()
        {
            return refuse("agent radius and step height must not be negative");
        }
        let cells = (self.tile_size / self.cell_size).ceil();
        if !(1.0..=MAX_CELLS_PER_TILE).contains(&(cells * cells)) {
            return refuse(
                "the tile size must hold between one cell and four million voxel columns",
            );
        }
        Ok(())
    }

    /// Seven `f32`, `u64` layers, `u64` tags and a `u8` back end.
    pub fn encode(&self, output: &mut Writer) {
        for value in [
            self.agent_radius,
            self.agent_height,
            self.max_slope,
            self.step_height,
            self.cell_size,
            self.cell_height,
            self.tile_size,
        ] {
            output.f32(value);
        }
        output.u64(self.layers);
        output.u64(self.tags);
        output.u8(match self.backend {
            NavBackend::Engine => 1,
            NavBackend::Recast => 2,
        });
    }
}

/// `navigation.bake`: `u32` world, settings.
#[must_use]
pub fn bake_request(world: u32, settings: &NavSettingsBlock) -> Vec<u8> {
    let mut output = Writer::new();
    output.u32(world);
    settings.encode(&mut output);
    output.finish()
}

/// `navigation.status`: `u32` world, settings, `u64` saved identity, `u64` saved fingerprint.
#[must_use]
pub fn status_request(
    world: u32,
    settings: &NavSettingsBlock,
    identity: u64,
    fingerprint: u64,
) -> Vec<u8> {
    let mut output = Writer::new();
    output.u32(world);
    settings.encode(&mut output);
    output.u64(identity);
    output.u64(fingerprint);
    output.finish()
}

/// `navigation.path.query`: `u32` world, start, end, extents.
#[must_use]
pub fn path_request(world: u32, start: [f32; 3], end: [f32; 3], extents: [f32; 3]) -> Vec<u8> {
    let mut output = Writer::new();
    output.u32(world);
    put_vec3(&mut output, start);
    put_vec3(&mut output, end);
    put_vec3(&mut output, extents);
    output.finish()
}

/// `navigation.flowfield.query`: `u32` world, target, region minimum and maximum, `f32` cell.
#[must_use]
pub fn flow_request(
    world: u32,
    target: [f32; 3],
    region: ([f32; 3], [f32; 3]),
    cell: f32,
) -> Vec<u8> {
    let mut output = Writer::new();
    output.u32(world);
    put_vec3(&mut output, target);
    put_vec3(&mut output, region.0);
    put_vec3(&mut output, region.1);
    output.f32(cell);
    output.finish()
}

/// `navigation.point.pick`: `u32` world, `u32` viewport, `u64` frame, `f32` x, `f32` y.
#[must_use]
pub fn pick_request(world: u32, viewport: u32, frame: u64, pixel: [f32; 2]) -> Vec<u8> {
    let mut output = Writer::new();
    output.u32(world);
    output.u32(viewport);
    output.u64(frame);
    output.f32(pixel[0]);
    output.f32(pixel[1]);
    output.finish()
}

/// One baked tile: its coordinate, polygon count, emptiness and content digest.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct NavBakeTile {
    /// Tile column.
    pub x: i32,
    /// Tile row.
    pub z: i32,
    /// Tile layer.
    pub layer: i32,
    /// Polygons the tile holds.
    pub polys: u32,
    /// Whether the tile had no walkable cells.
    pub empty: bool,
    /// The engine's `tile_digest`.
    pub digest: u64,
}

impl NavBakeTile {
    fn decode(input: &mut Reader<'_>) -> Result<Self> {
        Ok(Self {
            x: signed(input)?,
            z: signed(input)?,
            layer: signed(input)?,
            polys: input.u32()?,
            empty: flag(input)?,
            digest: input.u64()?,
        })
    }
}

/// One PROGRESS event of a bake: a tile finished.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct NavBakeProgress {
    /// Tiles finished so far.
    pub done: u32,
    /// Tiles the bake covers.
    pub total: u32,
    /// The tile that just finished.
    pub tile: NavBakeTile,
}

impl NavBakeProgress {
    /// Decode `u32` done, `u32` total, then the tile.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut input = Reader::new(payload);
        let progress = Self {
            done: input.u32()?,
            total: input.u32()?,
            tile: NavBakeTile::decode(&mut input)?,
        };
        done(&input)?;
        if progress.done > progress.total {
            return Err(invalid("a bake reported more tiles done than it covers"));
        }
        Ok(progress)
    }
}

/// The build counters of a bake or of the last one a status names.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct NavBuildCounters {
    /// Source triangles.
    pub triangles_in: u32,
    /// Triangles the layer and tag masks filtered out.
    pub triangles_filtered: u32,
    /// Triangles too steep to walk.
    pub triangles_steep: u32,
    /// Voxel spans.
    pub spans: u32,
    /// Polygons produced.
    pub polys: u32,
    /// Vertices produced.
    pub vertices: u32,
    /// Wall-clock nanoseconds. Diagnostics only; no identity includes it.
    pub duration_ns: u64,
    /// The back end that ran, as its engine code.
    pub backend: u8,
    /// Tiles built.
    pub tiles_built: u32,
    /// Tiles with no walkable cells.
    pub tiles_empty: u32,
}

impl NavBuildCounters {
    fn decode(input: &mut Reader<'_>) -> Result<Self> {
        Ok(Self {
            triangles_in: input.u32()?,
            triangles_filtered: input.u32()?,
            triangles_steep: input.u32()?,
            spans: input.u32()?,
            polys: input.u32()?,
            vertices: input.u32()?,
            duration_ns: input.u64()?,
            backend: input.u8()?,
            tiles_built: input.u32()?,
            tiles_empty: input.u32()?,
        })
    }
}

/// A completed bake: what the document records and what the panel shows.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct NavBakeReport {
    /// The navigation world baked.
    pub world: u32,
    /// The engine's source fingerprint of the bake's inputs.
    pub fingerprint: u64,
    /// The bake identity: the fingerprint and the ordered tile digests.
    pub identity: u64,
    /// The project-relative `.cynavmesh` sidecar the host wrote.
    pub sidecar: String,
    /// The build counters.
    pub counters: NavBuildCounters,
    /// Links that could not snap onto the mesh.
    pub link_failures: u32,
    /// Every tile, in bake order.
    pub tiles: Vec<NavBakeTile>,
}

impl NavBakeReport {
    /// Decode a `navigation.bake` COMPLETED payload.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut input = Reader::new(payload);
        let world = input.u32()?;
        let fingerprint = input.u64()?;
        let identity = input.u64()?;
        let sidecar = input.text()?;
        let counters = NavBuildCounters::decode(&mut input)?;
        let link_failures = input.u32()?;
        let tiles = (0..count(&mut input)?)
            .map(|_| NavBakeTile::decode(&mut input))
            .collect::<Result<Vec<_>>>()?;
        done(&input)?;
        Ok(Self {
            world,
            fingerprint,
            identity,
            sidecar,
            counters,
            link_failures,
            tiles,
        })
    }

    /// Tiles that hold at least one polygon.
    #[must_use]
    pub fn tile_count(&self) -> usize {
        self.tiles.iter().filter(|tile| !tile.empty).count()
    }
}

/// A FAILED event of any navigation operation: a stable code and what went wrong.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct NavBakeFailure {
    /// The stable failure code, such as `navigation.settings.invalid`.
    pub code: String,
    /// The engine's explanation.
    pub message: String,
}

impl NavBakeFailure {
    /// A failure the editor itself diagnosed, such as a disconnect.
    #[must_use]
    pub fn local(code: &str, message: &str) -> Self {
        Self {
            code: code.into(),
            message: message.into(),
        }
    }

    /// Decode `u32` version 1, text code, text detail.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut input = Reader::new(payload);
        if input.u32()? != 1 {
            return Err(invalid("unsupported navigation failure schema"));
        }
        let failure = Self {
            code: input.text()?,
            message: input.text()?,
        };
        done(&input)?;
        Ok(failure)
    }

    /// `code: message`, for a notification and a status outcome.
    #[must_use]
    pub fn summary(&self) -> String {
        format!("{}: {}", self.code, self.message)
    }
}

/// A `navigation.status` answer.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct NavStatusReport {
    /// The world asked about.
    pub world: u32,
    /// Whether the engine holds a mesh for it.
    pub baked: bool,
    /// Whether the engine's fingerprint of the current sources differs from the saved one.
    pub stale: bool,
    /// The engine's fingerprint of the current sources.
    pub current_fingerprint: u64,
    /// The fingerprint the document recorded.
    pub saved_fingerprint: u64,
    /// The identity of the mesh the engine holds.
    pub identity: u64,
    /// Tiles resident in that mesh.
    pub resident_tiles: u32,
    /// The last bake's counters.
    pub counters: NavBuildCounters,
    /// Links that could not snap.
    pub link_failures: u32,
    /// Whether the document records a bake whose sidecar the engine could not find (a fresh
    /// clone without the committed `.cynavmesh`, or a deleted file): the engine then holds no
    /// mesh, and a rebake recovers the state.
    pub sidecar_missing: bool,
}

impl NavStatusReport {
    /// Decode a `navigation.status` COMPLETED payload.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut input = Reader::new(payload);
        let report = Self {
            world: input.u32()?,
            baked: flag(&mut input)?,
            stale: flag(&mut input)?,
            current_fingerprint: input.u64()?,
            saved_fingerprint: input.u64()?,
            identity: input.u64()?,
            resident_tiles: input.u32()?,
            counters: NavBuildCounters::decode(&mut input)?,
            link_failures: input.u32()?,
            sidecar_missing: flag(&mut input)?,
        };
        done(&input)?;
        Ok(report)
    }
}

/// One corner of a straightened test path.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct NavPathPoint {
    /// World-space position.
    pub position: [f32; 3],
    /// Whether the path takes an off-mesh link from here.
    pub enters_link: bool,
}

/// A `navigation.path.query` answer.
#[derive(Clone, PartialEq, Debug)]
pub struct NavPathResult {
    /// Whether a path reached the end.
    pub found: bool,
    /// Whether it stopped at the nearest reachable polygon instead.
    pub partial: bool,
    /// Whether the search ran out of budget.
    pub budget_exceeded: bool,
    /// Its cost.
    pub cost: f32,
    /// A* nodes expanded.
    pub nodes_expanded: u32,
    /// The straightened corners.
    pub points: Vec<NavPathPoint>,
}

impl NavPathResult {
    /// Decode a `navigation.path.query` COMPLETED payload.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut input = Reader::new(payload);
        let found = flag(&mut input)?;
        let partial = flag(&mut input)?;
        let budget_exceeded = flag(&mut input)?;
        let cost = input.f32()?;
        let nodes_expanded = input.u32()?;
        let points = (0..count(&mut input)?)
            .map(|_| {
                Ok(NavPathPoint {
                    position: vec3(&mut input)?,
                    enters_link: flag(&mut input)?,
                })
            })
            .collect::<Result<Vec<_>>>()?;
        done(&input)?;
        Ok(Self {
            found,
            partial,
            budget_exceeded,
            cost,
            nodes_expanded,
            points,
        })
    }
}

/// One flow-field cell: the direction toward the target, and whether it is reachable.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct NavFlowCell {
    /// Direction, x.
    pub dx: f32,
    /// Direction, z.
    pub dz: f32,
    /// Whether the target is reachable from here.
    pub reachable: bool,
}

/// A `navigation.flowfield.query` answer, row by row.
#[derive(Clone, PartialEq, Debug)]
pub struct NavFlowField {
    /// Cells across.
    pub width: u32,
    /// Cells deep.
    pub depth: u32,
    /// Cell size, metres.
    pub cell: f32,
    /// Cells that cannot reach the target.
    pub unreachable: u32,
    /// The cells.
    pub cells: Vec<NavFlowCell>,
}

impl NavFlowField {
    /// Decode a `navigation.flowfield.query` COMPLETED payload.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut input = Reader::new(payload);
        let width = input.u32()?;
        let depth = input.u32()?;
        let cell = input.f32()?;
        let unreachable = input.u32()?;
        let cells = width
            .checked_mul(depth)
            .filter(|cells| *cells <= MAX_RECORDS)
            .ok_or_else(|| invalid("a flow field larger than the engine produces"))?;
        let cells = (0..cells)
            .map(|_| {
                Ok(NavFlowCell {
                    dx: input.f32()?,
                    dz: input.f32()?,
                    reachable: flag(&mut input)?,
                })
            })
            .collect::<Result<Vec<_>>>()?;
        done(&input)?;
        Ok(Self {
            width,
            depth,
            cell,
            unreachable,
            cells,
        })
    }
}

/// A `navigation.point.pick` answer.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct NavPick {
    /// Whether the pixel's ray hit the navmesh.
    pub hit: bool,
    /// The hit point.
    pub point: [f32; 3],
    /// The polygon hit, as the engine's `PolyRef`.
    pub polygon: u64,
    /// Distance along the ray.
    pub distance: f32,
}

impl NavPick {
    /// Decode a `navigation.point.pick` COMPLETED payload.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut input = Reader::new(payload);
        let pick = Self {
            hit: flag(&mut input)?,
            point: vec3(&mut input)?,
            polygon: input.u64()?,
            distance: input.f32()?,
        };
        done(&input)?;
        Ok(pick)
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    pub(crate) fn put_tile(output: &mut Writer, tile: &NavBakeTile) {
        output.u32(u32::from_le_bytes(tile.x.to_le_bytes()));
        output.u32(u32::from_le_bytes(tile.z.to_le_bytes()));
        output.u32(u32::from_le_bytes(tile.layer.to_le_bytes()));
        output.u32(tile.polys);
        output.u8(u8::from(tile.empty));
        output.u64(tile.digest);
    }

    pub(crate) fn put_counters(output: &mut Writer, counters: &NavBuildCounters) {
        for value in [
            counters.triangles_in,
            counters.triangles_filtered,
            counters.triangles_steep,
            counters.spans,
            counters.polys,
            counters.vertices,
        ] {
            output.u32(value);
        }
        output.u64(counters.duration_ns);
        output.u8(counters.backend);
        output.u32(counters.tiles_built);
        output.u32(counters.tiles_empty);
    }

    /// The COMPLETED payload the engine's `encode_completed` writes.
    pub(crate) fn completed_payload(report: &NavBakeReport) -> Vec<u8> {
        let mut output = Writer::new();
        output.u32(report.world);
        output.u64(report.fingerprint);
        output.u64(report.identity);
        output.text(&report.sidecar);
        put_counters(&mut output, &report.counters);
        output.u32(report.link_failures);
        output.u32(u32::try_from(report.tiles.len()).unwrap());
        for tile in &report.tiles {
            put_tile(&mut output, tile);
        }
        output.finish()
    }

    pub(crate) fn progress_payload(done: u32, total: u32, tile: &NavBakeTile) -> Vec<u8> {
        let mut output = Writer::new();
        output.u32(done);
        output.u32(total);
        put_tile(&mut output, tile);
        output.finish()
    }

    pub(crate) fn failure_payload(code: &str, detail: &str) -> Vec<u8> {
        let mut output = Writer::new();
        output.u32(1);
        output.text(code);
        output.text(detail);
        output.finish()
    }

    pub(crate) fn sample_report(world: u32, identity: u64) -> NavBakeReport {
        NavBakeReport {
            world,
            fingerprint: 0xF00D,
            identity,
            sidecar: format!("navigation/{identity:016x}.cynavmesh"),
            counters: NavBuildCounters {
                triangles_in: 12,
                polys: 8,
                vertices: 20,
                backend: 1,
                tiles_built: 2,
                tiles_empty: 1,
                ..NavBuildCounters::default()
            },
            link_failures: 0,
            tiles: vec![
                NavBakeTile {
                    x: -1,
                    z: 0,
                    layer: 0,
                    polys: 4,
                    empty: false,
                    digest: 7,
                },
                NavBakeTile {
                    x: 0,
                    z: 0,
                    layer: 0,
                    polys: 4,
                    empty: false,
                    digest: 9,
                },
                NavBakeTile {
                    x: 1,
                    z: 0,
                    layer: 0,
                    polys: 0,
                    empty: true,
                    digest: 0,
                },
            ],
        }
    }

    /// The COMPLETED payload the engine's `encode_status` writes.
    pub(crate) fn status_payload(sidecar_missing: bool) -> Vec<u8> {
        let mut output = Writer::new();
        output.u32(3);
        output.u8(0);
        output.u8(0);
        output.u64(0x1234);
        output.u64(0x5678);
        output.u64(0);
        output.u32(0);
        put_counters(&mut output, &NavBuildCounters::default());
        output.u32(0);
        output.u8(u8::from(sidecar_missing));
        output.finish()
    }

    #[test]
    fn a_status_answer_reports_a_missing_sidecar_apart_from_a_stale_bake() {
        let missing = NavStatusReport::decode(&status_payload(true)).unwrap();
        assert!(missing.sidecar_missing);
        assert!(!missing.baked);
        assert!(!missing.stale);
        assert_eq!(missing.current_fingerprint, 0x1234);
        let present = NavStatusReport::decode(&status_payload(false)).unwrap();
        assert!(!present.sidecar_missing);
        let mut short = status_payload(false);
        short.pop();
        assert!(NavStatusReport::decode(&short).is_err());
    }

    #[test]
    fn a_completed_bake_decodes_as_the_engine_writes_it() {
        let report = sample_report(1, 0xABCD_EF01_2345_6789);
        let decoded = NavBakeReport::decode(&completed_payload(&report)).unwrap();
        assert_eq!(decoded, report);
        assert_eq!(decoded.tile_count(), 2);
        assert_eq!(decoded.tiles[0].x, -1, "tile coordinates are signed");
    }

    #[test]
    fn progress_and_failure_decode_and_trailing_bytes_are_refused() {
        let tile = sample_report(1, 1).tiles[1];
        let progress = NavBakeProgress::decode(&progress_payload(2, 3, &tile)).unwrap();
        assert_eq!((progress.done, progress.total, progress.tile), (2, 3, tile));
        assert!(NavBakeProgress::decode(&progress_payload(4, 3, &tile)).is_err());
        let failure =
            NavBakeFailure::decode(&failure_payload("navigation.surface.missing", "none")).unwrap();
        assert_eq!(failure.summary(), "navigation.surface.missing: none");
        let mut padded = failure_payload("a", "b");
        padded.push(0);
        assert!(NavBakeFailure::decode(&padded).is_err());
    }

    #[test]
    fn settings_validation_refuses_what_the_engine_refuses() {
        assert!(NavSettingsBlock::DEFAULT.validate().is_ok());
        for broken in [
            NavSettingsBlock {
                cell_size: 0.0,
                ..NavSettingsBlock::DEFAULT
            },
            NavSettingsBlock {
                agent_radius: -0.1,
                ..NavSettingsBlock::DEFAULT
            },
            NavSettingsBlock {
                tile_size: 10_000.0,
                cell_size: 0.1,
                ..NavSettingsBlock::DEFAULT
            },
            NavSettingsBlock {
                max_slope: f32::NAN,
                ..NavSettingsBlock::DEFAULT
            },
        ] {
            assert!(broken.validate().is_err(), "{broken:?}");
        }
    }

    #[test]
    fn a_bake_request_is_a_world_and_the_settings_block() {
        let bytes = bake_request(3, &NavSettingsBlock::DEFAULT);
        assert_eq!(bytes.len(), 4 + 7 * 4 + 8 + 8 + 1);
        let mut input = Reader::new(&bytes);
        assert_eq!(input.u32().unwrap(), 3);
        assert!((input.f32().unwrap() - 0.5).abs() < f32::EPSILON);
        let status = status_request(3, &NavSettingsBlock::DEFAULT, 5, 6);
        assert_eq!(status.len(), bytes.len() + 16);
    }

    #[test]
    fn query_answers_decode() {
        let mut path = Writer::new();
        path.u8(1);
        path.u8(0);
        path.u8(0);
        path.f32(4.5);
        path.u32(9);
        path.u32(2);
        for (point, link) in [([0.0, 0.0, 0.0], 0), ([4.0, 0.0, 1.0], 1)] {
            put_vec3(&mut path, point);
            path.u8(link);
        }
        let path = NavPathResult::decode(&path.finish()).unwrap();
        assert!(path.found && !path.partial);
        assert!(path.points[1].enters_link);

        let mut pick = Writer::new();
        pick.u8(1);
        put_vec3(&mut pick, [1.0, 0.0, 2.0]);
        pick.u64(77);
        pick.f32(3.0);
        let pick = NavPick::decode(&pick.finish()).unwrap();
        assert_eq!((pick.hit, pick.polygon), (true, 77));

        let mut flow = Writer::new();
        flow.u32(2);
        flow.u32(1);
        flow.f32(0.5);
        flow.u32(1);
        for reachable in [1, 0] {
            flow.f32(1.0);
            flow.f32(0.0);
            flow.u8(reachable);
        }
        let flow = NavFlowField::decode(&flow.finish()).unwrap();
        assert_eq!(flow.cells.len(), 2);
        assert!(!flow.cells[1].reachable);
    }
}
