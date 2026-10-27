use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Mutex};
use std::time::Instant;

use mlua::{Table, UserData, UserDataMethods, Value};

use super::build_voxel_view::{build_voxel_view, ChunkCells, ViewInput, ViewPlan, CHUNK};

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

/// The input of a view from a mod's table: lo, hi, chunks = a list of {x, y,
/// z, cells} (the chunk's origin divided by 16; cells: 8192 bytes of
/// little-endian u16, or one number for every cell), plan = {kind = "add" or
/// "remove", only_empty, lo, hi, cells = {x1, y1, z1, x2, ...} or nil for the
/// whole box}, start, finish, player, roofs = {values}, surface, depth.
fn read_input(params: &Table) -> mlua::Result<ViewInput> {
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
    let mut chunks = HashMap::new();
    if let Some(list) = params.raw_get::<Option<Table>>("chunks")? {
        for entry in list.sequence_values::<Table>() {
            let entry = entry?;
            let key = [entry.raw_get::<i32>(1)?, entry.raw_get::<i32>(2)?, entry.raw_get::<i32>(3)?];
            let cells = match entry.raw_get::<Value>(4)? {
                Value::String(text) => {
                    let bytes = text.as_bytes().to_vec();
                    if bytes.len() != (CHUNK * CHUNK * CHUNK * 2) as usize {
                        return Err(mlua::Error::runtime("voxel view: a chunk holds 8192 bytes"));
                    }
                    ChunkCells::Packed(bytes)
                }
                Value::Integer(value) => ChunkCells::Uniform(value as i32),
                Value::Number(value) => ChunkCells::Uniform(value as i32),
                _ => continue,
            };
            chunks.insert(key, cells);
        }
    }
    let plan = match params.raw_get::<Option<Table>>("plan")? {
        None => None,
        Some(plan) => {
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
            Some(ViewPlan {
                add: plan.raw_get::<Option<String>>("kind")?.as_deref() == Some("add"),
                only_empty: plan.raw_get::<Option<bool>>("only_empty")?.unwrap_or(false),
                lo: required_cell(&plan, "lo")?,
                hi: required_cell(&plan, "hi")?,
                cells,
            })
        }
    };
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

/// `shroudforge.voxels`: `view3d(params)` copies the input, builds the view on
/// a thread of its own and returns a job to ask at the next updates, so a mod
/// never waits for it (see build_voxel_view).
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
    Ok(voxels)
}
