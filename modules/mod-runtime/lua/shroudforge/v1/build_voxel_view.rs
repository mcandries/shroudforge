use std::collections::{HashMap, HashSet};
use std::sync::Arc;

use super::reuse_voxel_view_base::{view_base, ViewBases};

/// Side of the chunks a mod keeps its voxel reads in: 16 x 16 x 16 cells,
/// x fastest, then y, then z.
pub(crate) const CHUNK: i32 = 16;

/// What a mod read for one chunk: two bytes per cell (little-endian u16, the
/// value `runtime.world.voxel.read` gives), one value for every cell, or
/// nothing yet. Shared, so that a view takes the chunks a mod keeps on the
/// native side (voxel_chunk_store) without copying them.
#[derive(Clone)]
pub(crate) enum ChunkCells {
    Packed(Arc<Vec<u8>>),
    Uniform(i32),
}

/// The value of a cell in `chunks`, -1 when its chunk was not read.
pub(crate) fn chunk_value(chunks: &HashMap<[i32; 3], ChunkCells>, x: i32, y: i32, z: i32) -> i32 {
    let key = [x.div_euclid(CHUNK), y.div_euclid(CHUNK), z.div_euclid(CHUNK)];
    match chunks.get(&key) {
        None => -1,
        Some(ChunkCells::Uniform(value)) => *value,
        Some(ChunkCells::Packed(bytes)) => {
            let index =
                (x.rem_euclid(CHUNK) + y.rem_euclid(CHUNK) * CHUNK + z.rem_euclid(CHUNK) * CHUNK * CHUNK) as usize * 2;
            match bytes.get(index..index + 2) {
                Some(pair) => i32::from(u16::from_le_bytes([pair[0], pair[1]])),
                None => -1,
            }
        }
    }
}

/// The cells a build changes, as the view draws them.
#[derive(Clone)]
pub(crate) struct ViewPlan {
    /// Building (true) or removing.
    pub add: bool,
    /// Building keeps the blocks already there.
    pub only_empty: bool,
    pub lo: [i32; 3],
    pub hi: [i32; 3],
    /// The planned cells; none: every cell of lo..hi.
    pub cells: Option<HashSet<[i32; 3]>>,
}

/// A view of the cells lo..hi, as Architect Line's preview.build3d makes it.
pub(crate) struct ViewInput {
    pub lo: [i32; 3],
    pub hi: [i32; 3],
    /// Chunks by origin divided by CHUNK; a chunk missing reads as unknown (-1).
    pub chunks: HashMap<[i32; 3], ChunkCells>,
    pub plan: Option<ViewPlan>,
    pub start: Option<[i32; 3]>,
    pub finish: Option<[i32; 3]>,
    pub player: Option<[i32; 3]>,
    /// Voxel values of roof materials.
    pub roofs: HashSet<i32>,
    /// Surface view: terrain only as a skin `depth` cells deep, and near the plan.
    pub surface: bool,
    pub depth: i32,
    /// The last base built from the same chunks store, reused when only the
    /// plan or the marks changed (reuse_voxel_view_base).
    pub bases: Option<ViewBases>,
}

/// The characters of the view's categories (preview.CELL).
const AIR: u8 = b'.';
const BLOCK: u8 = b'#';
const TERRAIN: u8 = b'%';
const ROOF: u8 = b'^';
const ADD: u8 = b'+';
const REMOVE: u8 = b'-';
const START: u8 = b'S';
const FINISH: u8 = b'E';
const PLAYER: u8 = b'P';
const UNKNOWN: u8 = b'?';

/// The box of the view and its cells, run-length text x fastest, then y, then
/// z (a count before a character repeats it): the same text as the mod's own
/// Lua builder, many times faster.
pub(crate) fn build_voxel_view(input: &ViewInput) -> ([usize; 3], String) {
    let size = [
        (input.hi[0] - input.lo[0] + 1).max(0) as usize,
        (input.hi[1] - input.lo[1] + 1).max(0) as usize,
        (input.hi[2] - input.lo[2] + 1).max(0) as usize,
    ];
    let (sx, sy, sz) = (size[0], size[1], size[2]);
    let total = sx * sy * sz;
    let (values, skin) = view_base(input, size);
    let index_of = |cell: [i32; 3]| -> Option<usize> {
        for axis in 0..3 {
            if cell[axis] < input.lo[axis] || cell[axis] > input.hi[axis] {
                return None;
            }
        }
        let [x, y, z] = [
            (cell[0] - input.lo[0]) as usize,
            (cell[1] - input.lo[1]) as usize,
            (cell[2] - input.lo[2]) as usize,
        ];
        Some(x + y * sx + z * sx * sy)
    };
    // The player's mark: on their cell, or the first empty cell above it.
    let player = input.player.map(|player| {
        for up in 0..=8 {
            let cell = [player[0], player[1] + up, player[2]];
            match index_of(cell) {
                None => return cell,
                Some(index) if values[index] <= 0 => return cell,
                _ => {}
            }
        }
        player
    });
    let start_index = input.start.and_then(|cell| index_of(cell));
    let finish_index = input.finish.and_then(|cell| index_of(cell));
    let player_index = player.and_then(|cell| index_of(cell));
    let near = input.plan.as_ref().map(|plan| {
        let d = input.depth;
        (
            [plan.lo[0] - d, plan.lo[1] - d, plan.lo[2] - d],
            [plan.hi[0] + d, plan.hi[1] + d, plan.hi[2] + d],
        )
    });
    let mut text = String::with_capacity(total / 8 + 16);
    let mut last = 0u8;
    let mut run = 0usize;
    let mut push = |ch: u8, text: &mut String| {
        if ch == last {
            run += 1;
            return;
        }
        flush(text, last, run);
        last = ch;
        run = 1;
    };
    let mut i = 0usize;
    for z in input.lo[2]..=input.hi[2] {
        for y in input.lo[1]..=input.hi[1] {
            for x in input.lo[0]..=input.hi[0] {
                let value = values[i];
                let mut ch = if Some(i) == start_index {
                    START
                } else if Some(i) == finish_index {
                    FINISH
                } else if let Some(plan) = input.plan.as_ref().filter(|plan| plan_contains(plan, [x, y, z])) {
                    if plan.add && value > 0 && plan.only_empty {
                        // Building keeps the blocks already there (replace off).
                        solid(value, &input.roofs)
                    } else if plan.add {
                        ADD
                    } else if value == 0 {
                        AIR
                    } else {
                        REMOVE
                    }
                } else if value < 0 {
                    UNKNOWN
                } else if value == 0 {
                    AIR
                } else if value >= 256 {
                    // Terrain: under the surface view, only its skin and near the plan.
                    let shown = skin.as_ref().map_or(true, |skin| skin[i])
                        || near.is_some_and(|(lo, hi)| {
                            x >= lo[0] && x <= hi[0] && y >= lo[1] && y <= hi[1] && z >= lo[2] && z <= hi[2]
                        });
                    if shown { TERRAIN } else { AIR }
                } else {
                    solid(value, &input.roofs)
                };
                if ch == AIR && Some(i) == player_index {
                    ch = PLAYER;
                }
                push(ch, &mut text);
                i += 1;
            }
        }
    }
    flush(&mut text, last, run);
    (size, text)
}

pub(crate) fn flush(text: &mut String, ch: u8, run: usize) {
    if run == 0 {
        return;
    }
    if run > 1 {
        text.push_str(&run.to_string());
    }
    text.push(ch as char);
}

fn solid(value: i32, roofs: &HashSet<i32>) -> u8 {
    if value >= 256 {
        TERRAIN
    } else if roofs.contains(&value) {
        ROOF
    } else {
        BLOCK
    }
}

pub(crate) fn plan_contains(plan: &ViewPlan, cell: [i32; 3]) -> bool {
    for axis in 0..3 {
        if cell[axis] < plan.lo[axis] || cell[axis] > plan.hi[axis] {
            return false;
        }
    }
    plan.cells.as_ref().map_or(true, |cells| cells.contains(&cell))
}

/// Every value of the zone, x fastest, then y, then z; -1 for a cell not read.
/// Chunk by chunk, a row of up to 16 cells at a time (27/10: a lookup per
/// cell took most of the 100 to 170 ms of a 256 x 64 x 256 view).
pub(crate) fn read_values(input: &ViewInput, size: [usize; 3]) -> Vec<i32> {
    let (sx, sy) = (size[0], size[1]);
    let mut values = vec![-1; size[0] * size[1] * size[2]];
    for cz in input.lo[2].div_euclid(CHUNK)..=input.hi[2].div_euclid(CHUNK) {
        for cy in input.lo[1].div_euclid(CHUNK)..=input.hi[1].div_euclid(CHUNK) {
            for cx in input.lo[0].div_euclid(CHUNK)..=input.hi[0].div_euclid(CHUNK) {
                let Some(cells) = input.chunks.get(&[cx, cy, cz]) else { continue };
                let x0 = (cx * CHUNK).max(input.lo[0]);
                let x1 = (cx * CHUNK + CHUNK - 1).min(input.hi[0]);
                for z in (cz * CHUNK).max(input.lo[2])..=(cz * CHUNK + CHUNK - 1).min(input.hi[2]) {
                    for y in (cy * CHUNK).max(input.lo[1])..=(cy * CHUNK + CHUNK - 1).min(input.hi[1]) {
                        let row = (y - input.lo[1]) as usize * sx + (z - input.lo[2]) as usize * sx * sy;
                        for x in x0..=x1 {
                            values[row + (x - input.lo[0]) as usize] = match cells {
                                ChunkCells::Uniform(value) => *value,
                                ChunkCells::Packed(bytes) => {
                                    let index = (x.rem_euclid(CHUNK)
                                        + y.rem_euclid(CHUNK) * CHUNK
                                        + z.rem_euclid(CHUNK) * CHUNK * CHUNK) as usize
                                        * 2;
                                    match bytes.get(index..index + 2) {
                                        Some(pair) => i32::from(u16::from_le_bytes([pair[0], pair[1]])),
                                        None => -1,
                                    }
                                }
                            };
                        }
                    }
                }
            }
        }
    }
    values
}

/// Surface view: the cells with an empty (or unknown) cell at most `depth`
/// cells away along one of the six axes; above the zone's top counts as
/// empty, its sides and bottom as solid. Six linear scans.
pub(crate) fn surface_skin(values: &[i32], size: [usize; 3], depth: i32) -> Vec<bool> {
    let (sx, sy, sz) = (size[0], size[1], size[2]);
    let mut skin = vec![false; values.len()];
    let (stride_y, stride_z) = (sx as isize, (sx * sy) as isize);
    let mut scan = |first: isize, step: isize, count: usize, open: bool| {
        let mut since = if open { 0 } else { depth + 1 };
        let mut index = first;
        for _ in 0..count {
            let i = index as usize;
            if values[i] <= 0 {
                since = 0;
            } else {
                since += 1;
                if since <= depth {
                    skin[i] = true;
                }
            }
            index += step;
        }
    };
    for z in 0..sz as isize {
        for y in 0..sy as isize {
            let first = y * stride_y + z * stride_z;
            scan(first, 1, sx, false);
            scan(first + sx as isize - 1, -1, sx, false);
        }
    }
    for z in 0..sz as isize {
        for x in 0..sx as isize {
            let first = x + z * stride_z;
            scan(first, stride_y, sy, false);
            // From the top down: above the zone is open air.
            scan(first + (sy as isize - 1) * stride_y, -stride_y, sy, true);
        }
    }
    for y in 0..sy as isize {
        for x in 0..sx as isize {
            let first = x + y * stride_y;
            scan(first, stride_z, sz, false);
            scan(first + (sz as isize - 1) * stride_z, -stride_z, sz, false);
        }
    }
    skin
}

#[cfg(test)]
mod tests {
    use super::*;

    fn input(lo: [i32; 3], hi: [i32; 3]) -> ViewInput {
        ViewInput {
            lo,
            hi,
            chunks: HashMap::new(),
            plan: None,
            start: None,
            finish: None,
            player: None,
            roofs: HashSet::new(),
            surface: false,
            depth: 3,
            bases: None,
        }
    }

    fn packed(value_at: impl Fn(i32, i32, i32) -> u16) -> ChunkCells {
        let mut bytes = Vec::with_capacity(4096 * 2);
        for z in 0..CHUNK {
            for y in 0..CHUNK {
                for x in 0..CHUNK {
                    bytes.extend_from_slice(&value_at(x, y, z).to_le_bytes());
                }
            }
        }
        ChunkCells::Packed(Arc::new(bytes))
    }

    #[test]
    fn unread_cells_are_unknown_and_runs_are_counted() {
        let (size, text) = build_voxel_view(&input([0, 0, 0], [3, 0, 0]));
        assert_eq!(size, [4, 1, 1]);
        assert_eq!(text, "4?");
    }

    #[test]
    fn categories_markers_and_plan() {
        let mut view = input([0, 0, 0], [5, 0, 0]);
        // x = 0 air, 1 block, 2 roof, 3 terrain, 4 block, 5 air.
        view.chunks.insert(
            [0, 0, 0],
            packed(|x, _, _| match x {
                1 | 4 => 155,
                2 => 130,
                3 => 300,
                _ => 0,
            }),
        );
        view.roofs.insert(130);
        view.start = Some([5, 0, 0]);
        let (_, text) = build_voxel_view(&view);
        assert_eq!(text, ".#^%#S");
        // Building over x = 3..4, keeping the blocks there; removing elsewhere.
        view.plan = Some(ViewPlan { add: true, only_empty: true, lo: [0, 0, 0], hi: [5, 0, 0], cells: None });
        view.start = None;
        let (_, text) = build_voxel_view(&view);
        assert_eq!(text, "+#^%#+");
        view.plan = Some(ViewPlan {
            add: false,
            only_empty: false,
            lo: [0, 0, 0],
            hi: [5, 0, 0],
            cells: Some(HashSet::from([[0, 0, 0], [1, 0, 0]])),
        });
        let (_, text) = build_voxel_view(&view);
        assert_eq!(text, ".-^%#.");
    }

    /// Cross-check with a mod's own Lua builder: VOXEL_VIEW_FIXTURE names a
    /// JSON file of an input and the text that builder made from it
    /// (Architect Line writes one with a random world). Ignored without it.
    #[test]
    #[ignore]
    fn same_view_as_a_fixture() {
        let path = std::env::var("VOXEL_VIEW_FIXTURE").expect("VOXEL_VIEW_FIXTURE");
        let fixture: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap();
        let cell = |value: &serde_json::Value| -> [i32; 3] {
            let list = value.as_array().unwrap();
            [list[0].as_i64().unwrap() as i32, list[1].as_i64().unwrap() as i32, list[2].as_i64().unwrap() as i32]
        };
        let mut view = input(cell(&fixture["lo"]), cell(&fixture["hi"]));
        for chunk in fixture["chunks"].as_array().unwrap() {
            let key = cell(chunk);
            let cells = match &chunk[3] {
                serde_json::Value::String(hex) => ChunkCells::Packed(Arc::new(
                    (0..hex.len()).step_by(2).map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap()).collect(),
                )),
                value => ChunkCells::Uniform(value.as_i64().unwrap() as i32),
            };
            view.chunks.insert(key, cells);
        }
        let plan = &fixture["plan"];
        let flat: Vec<i32> = plan["cells"].as_array().unwrap().iter().map(|v| v.as_i64().unwrap() as i32).collect();
        view.plan = Some(ViewPlan {
            add: plan["add"].as_bool().unwrap(),
            only_empty: plan["only_empty"].as_bool().unwrap(),
            lo: cell(&plan["lo"]),
            hi: cell(&plan["hi"]),
            cells: Some(flat.chunks(3).map(|c| [c[0], c[1], c[2]]).collect()),
        });
        view.start = Some(cell(&fixture["start"]));
        view.finish = Some(cell(&fixture["finish"]));
        view.player = Some(cell(&fixture["player"]));
        view.roofs = fixture["roofs"].as_array().unwrap().iter().map(|v| v.as_i64().unwrap() as i32).collect();
        view.surface = fixture["surface"].as_bool().unwrap();
        view.depth = fixture["depth"].as_i64().unwrap() as i32;
        let started = std::time::Instant::now();
        let (size, text) = build_voxel_view(&view);
        eprintln!("{:?} in {:?}", size, started.elapsed());
        assert_eq!(text, fixture["expected"].as_str().unwrap());
    }

    #[test]
    fn surface_keeps_a_skin_of_terrain_and_the_player_above_ground() {
        // A column of terrain 10 high in a zone 1 x 12 x 1: only its top 3 show.
        let mut view = input([0, 0, 0], [0, 11, 0]);
        view.chunks.insert([0, 0, 0], packed(|_, y, _| if y < 10 { 300 } else { 0 }));
        view.surface = true;
        view.player = Some([0, 5, 0]);
        let (_, text) = build_voxel_view(&view);
        // The player is inside the ground: marked on the first air cell above.
        assert_eq!(text, "7.3%P.");
    }
}
