use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Mutex};
use std::time::Instant;

use mlua::{Table, UserData, UserDataMethods, Value};

use super::build_voxel_view::{build_voxel_view, ChunkCells, ViewInput, ViewPlan};
use super::build_world_cells::{build_world_cells, WorldBox, WorldCells, WorldInput};
use super::voxel_chunk_store::{chunk_cells, VoxelChunkStore};

/// At most this many cells a side, and in all, for one view.
const MAX_SIDE: i32 = 256;
const MAX_CELLS: usize = 256 * 128 * 256;

/// A view being built on its own thread: `result()` gives nil until it is
/// done, then {box = {x, y, z}, cells = text, seconds = build time}.
struct VoxelViewJob {
    done: Arc<Mutex<Option<([usize; 3], String, f64)>>>,
}

impl UserData for VoxelViewJob {
    fn add_methods<M: UserDataMethods<Self>>(methods: &mut M) {
        methods.add_method("result", |lua, job, ()| {
            let done = job.done.lock().map_err(|_| mlua::Error::runtime("voxel view: the builder failed"))?;
            let Some((size, cells, seconds)) = done.as_ref() else { return Ok(Value::Nil) };
            let result = lua.create_table()?;
            result.raw_set("box", lua.create_sequence_from(size.iter().copied())?)?;
            result.raw_set("cells", lua.create_string(cells)?)?;
            result.raw_set("seconds", *seconds)?;
            Ok(Value::Table(result))
        });
    }
}

/// A build in the world being made on its own thread: `result()` gives nil
/// until it is done, then {cells = {tiles = {{lo, box, data, changed}...},
/// changed} or nil, solids = {lo, box, data, filled} or nil, seconds}.
struct WorldCellsJob {
    done: Arc<Mutex<Option<(WorldCells, f64)>>>,
}

fn box_table(lua: &mlua::Lua, part: &WorldBox, count_field: &str) -> mlua::Result<Table> {
    let table = lua.create_table()?;
    table.raw_set("lo", lua.create_sequence_from(part.lo.iter().copied())?)?;
    table.raw_set("box", lua.create_sequence_from(part.size.iter().copied())?)?;
    table.raw_set("data", lua.create_string(&part.data)?)?;
    table.raw_set(count_field, part.count)?;
    Ok(table)
}

impl UserData for WorldCellsJob {
    fn add_methods<M: UserDataMethods<Self>>(methods: &mut M) {
        methods.add_method("result", |lua, job, ()| {
            let done = job.done.lock().map_err(|_| mlua::Error::runtime("world cells: the builder failed"))?;
            let Some((world, seconds)) = done.as_ref() else { return Ok(Value::Nil) };
            let result = lua.create_table()?;
            if let Some((tiles, changed)) = &world.tiles {
                let cells = lua.create_table()?;
                let list = lua.create_table()?;
                for tile in tiles {
                    list.push(box_table(lua, tile, "changed")?)?;
                }
                cells.raw_set("tiles", list)?;
                cells.raw_set("changed", *changed)?;
                result.raw_set("cells", cells)?;
            }
            if let Some(solids) = &world.solids {
                result.raw_set("solids", box_table(lua, solids, "filled")?)?;
            }
            result.raw_set("seconds", *seconds)?;
            Ok(Value::Table(result))
        });
    }
}

fn cell(value: Value, field: &str) -> mlua::Result<Option<[i32; 3]>> {
    match value {
        Value::Nil => Ok(None),
        Value::Table(table) => {
            let mut out = [0i32; 3];
            for (axis, slot) in out.iter_mut().enumerate() {
                *slot = table.raw_get::<i32>(axis + 1)?;
            }
            Ok(Some(out))
        }
        _ => Err(mlua::Error::runtime(format!("voxel view: {field}: expected {{x, y, z}}"))),
    }
}

fn required_cell(params: &Table, field: &str) -> mlua::Result<[i32; 3]> {
    cell(params.raw_get(field)?, field)?
        .ok_or_else(|| mlua::Error::runtime(format!("voxel view: {field} is required")))
}

/// lo..hi of `params`, at most MAX_SIDE a side and MAX_CELLS in all.
fn read_box(params: &Table) -> mlua::Result<([i32; 3], [i32; 3])> {
    let lo = required_cell(params, "lo")?;
    let hi = required_cell(params, "hi")?;
    let mut total = 1usize;
    for axis in 0..3 {
        let side = hi[axis] - lo[axis] + 1;
        if side < 1 || side > MAX_SIDE {
            return Err(mlua::Error::runtime(format!("voxel view: 1 to {MAX_SIDE} cells a side")));
        }
        total *= side as usize;
    }
    if total > MAX_CELLS {
        return Err(mlua::Error::runtime(format!("voxel view: {MAX_CELLS} cells at most")));
    }
    Ok((lo, hi))
}

/// `chunks`: a store (shroudforge.voxels.chunks(), only its chunks that meet
/// lo..hi taken, shared), or a list of {x, y, z, cells} (the chunk's origin
/// divided by 16; cells: 8192 bytes of little-endian u16, or one number for
/// every cell), copied.
fn read_chunks(params: &Table, lo: [i32; 3], hi: [i32; 3]) -> mlua::Result<HashMap<[i32; 3], ChunkCells>> {
    match params.raw_get::<Value>("chunks")? {
        Value::Nil => Ok(HashMap::new()),
        Value::UserData(data) => Ok(data.borrow::<VoxelChunkStore>()?.region(lo, hi)),
        Value::Table(list) => {
            let mut chunks = HashMap::new();
            for entry in list.sequence_values::<Table>() {
                let entry = entry?;
                let key = [entry.raw_get::<i32>(1)?, entry.raw_get::<i32>(2)?, entry.raw_get::<i32>(3)?];
                match entry.raw_get::<Value>(4)? {
                    value @ (Value::String(_) | Value::Integer(_) | Value::Number(_)) => {
                        if let Some(cells) = chunk_cells(value)? {
                            chunks.insert(key, cells);
                        }
                    }
                    _ => continue,
                }
            }
            Ok(chunks)
        }
        _ => Err(mlua::Error::runtime("voxel view: chunks: a store or a list")),
    }
}

/// `plan`: {kind = "add" or "remove", only_empty, lo, hi, cells = {x1, y1,
/// z1, x2, ...} or nil for the whole box}.
fn read_plan(params: &Table) -> mlua::Result<Option<ViewPlan>> {
    let Some(plan) = params.raw_get::<Option<Table>>("plan")? else { return Ok(None) };
    let cells = match plan.raw_get::<Option<Table>>("cells")? {
        None => None,
        Some(flat) => {
            let values: Vec<i32> = flat.sequence_values::<i32>().collect::<mlua::Result<_>>()?;
            if values.len() % 3 != 0 {
                return Err(mlua::Error::runtime("voxel view: plan cells come by three"));
            }
            Some(values.chunks(3).map(|c| [c[0], c[1], c[2]]).collect::<HashSet<_>>())
        }
    };
    Ok(Some(ViewPlan {
        add: plan.raw_get::<Option<String>>("kind")?.as_deref() == Some("add"),
        only_empty: plan.raw_get::<Option<bool>>("only_empty")?.unwrap_or(false),
        lo: required_cell(&plan, "lo")?,
        hi: required_cell(&plan, "hi")?,
        cells,
    }))
}

/// The input of a view from a mod's table: lo, hi, chunks, plan, start,
/// finish, player, roofs = {values}, surface, depth.
fn read_input(params: &Table) -> mlua::Result<ViewInput> {
    let (lo, hi) = read_box(params)?;
    let chunks = read_chunks(params, lo, hi)?;
    let plan = read_plan(params)?;
    let mut roofs = HashSet::new();
    if let Some(list) = params.raw_get::<Option<Table>>("roofs")? {
        for value in list.sequence_values::<i32>() {
            roofs.insert(value?);
        }
    }
    Ok(ViewInput {
        lo,
        hi,
        chunks,
        plan,
        start: cell(params.raw_get("start")?, "start")?,
        finish: cell(params.raw_get("finish")?, "finish")?,
        player: cell(params.raw_get("player")?, "player")?,
        roofs,
        surface: params.raw_get::<Option<bool>>("surface")?.unwrap_or(false),
        depth: params.raw_get::<Option<i32>>("depth")?.unwrap_or(3).clamp(1, 32),
    })
}

/// The input of a build in the world: lo, hi (the build's box, or the start
/// cell), chunks, plan, tile (256), total (1048576), margin (8), solids_max
/// (262144). Chunks are taken around the box by the margin, capped as the
/// solids are.
fn read_world_input(params: &Table) -> mlua::Result<WorldInput> {
    let lo = required_cell(params, "lo")?;
    let hi = required_cell(params, "hi")?;
    for axis in 0..3 {
        if hi[axis] < lo[axis] || hi[axis] - lo[axis] >= 4096 {
            return Err(mlua::Error::runtime("world cells: lo..hi of 1 to 4096 cells a side"));
        }
    }
    let margin = params.raw_get::<Option<i32>>("margin")?.unwrap_or(8).clamp(0, 64);
    let around = |cell: [i32; 3], sign: i32| [cell[0] + sign * margin, cell[1] + sign * margin, cell[2] + sign * margin];
    let plan = read_plan(params)?;
    // The plan's box and the solids' box, whichever is larger.
    let (mut from, mut to) = (around(lo, -1), around(hi, 1));
    if let Some(plan) = &plan {
        for axis in 0..3 {
            from[axis] = from[axis].min(plan.lo[axis]);
            to[axis] = to[axis].max(plan.hi[axis]);
        }
    }
    Ok(WorldInput {
        chunks: read_chunks(params, from, to)?,
        plan,
        lo,
        hi,
        tile: params.raw_get::<Option<i32>>("tile")?.unwrap_or(256).clamp(1, MAX_SIDE),
        total: params.raw_get::<Option<usize>>("total")?.unwrap_or(1 << 20).min(1 << 24),
        margin,
        solids_max: params.raw_get::<Option<usize>>("solids_max")?.unwrap_or(1 << 18).min(1 << 24),
    })
}

/// `shroudforge.voxels`: `view3d(params)` and `world(params)` copy the input,
/// build on a thread of their own and return a job to ask at the next
/// updates, so a mod never waits for them (see build_voxel_view and
/// build_world_cells); `chunks()` a store of the chunks a mod read, which
/// they take without copying.
pub(crate) fn create(lua: &mlua::Lua) -> mlua::Result<Table> {
    let voxels = lua.create_table()?;
    voxels.raw_set(
        "view3d",
        lua.create_function(|_, params: Table| {
            let input = read_input(&params)?;
            let done = Arc::new(Mutex::new(None));
            let result = Arc::clone(&done);
            std::thread::Builder::new()
                .name("shroudforge-voxel-view".into())
                .spawn(move || {
                    let started = Instant::now();
                    let (size, cells) = build_voxel_view(&input);
                    if let Ok(mut slot) = result.lock() {
                        *slot = Some((size, cells, started.elapsed().as_secs_f64()));
                    }
                })
                .map_err(|error| mlua::Error::runtime(format!("voxel view: no thread: {error}")))?;
            Ok(VoxelViewJob { done })
        })?,
    )?;
    voxels.raw_set(
        "world",
        lua.create_function(|_, params: Table| {
            let input = read_world_input(&params)?;
            let done = Arc::new(Mutex::new(None));
            let result = Arc::clone(&done);
            std::thread::Builder::new()
                .name("shroudforge-world-cells".into())
                .spawn(move || {
                    let started = Instant::now();
                    let world = build_world_cells(&input);
                    if let Ok(mut slot) = result.lock() {
                        *slot = Some((world, started.elapsed().as_secs_f64()));
                    }
                })
                .map_err(|error| mlua::Error::runtime(format!("world cells: no thread: {error}")))?;
            Ok(WorldCellsJob { done })
        })?,
    )?;
    voxels.raw_set("chunks", lua.create_function(|_, ()| Ok(VoxelChunkStore::default()))?)?;
    Ok(voxels)
}
