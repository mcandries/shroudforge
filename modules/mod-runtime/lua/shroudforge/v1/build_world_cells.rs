use std::collections::HashMap;

use super::build_voxel_view::{chunk_value, flush, plan_contains, with_density, ChunkCells, ViewPlan, CHUNK};

/// A build shown in the world, as Architect Line's preview.world_tiles and
/// preview.world_solids make it.
pub(crate) struct WorldInput {
    /// Chunks by origin divided by CHUNK; a chunk missing reads as not read.
    pub chunks: HashMap<[i32; 3], ChunkCells>,
    /// The cells the build changes (none: only the filled cells around).
    pub plan: Option<ViewPlan>,
    /// The build's box: the plan's, or the start cell alone.
    pub lo: [i32; 3],
    pub hi: [i32; 3],
    /// Tiles of at most `tile` cells a side; none past `total` cells in the plan's box.
    pub tile: i32,
    pub total: usize,
    /// Filled cells `margin` around the box (less on an axis over 128), none past `solids_max` cells.
    pub margin: i32,
    pub solids_max: usize,
    /// The box of the filled cells instead, when given: what may hide the
    /// build from a viewpoint, such as the box holding the build and the eye.
    pub solids_box: Option<([i32; 3], [i32; 3])>,
    /// A letter per voxel value, for a block removed or replaced: what is
    /// there tells how the cell is drawn (the game's blocks of each material
    /// are drawn past their cells by their own amount).
    pub palette: HashMap<i32, u8>,
    /// Terrain cells less dense read as empty (with_density); 1: any.
    pub density_min: i32,
}

/// A box of cells, run-length text x fastest, then y, then z, and how many of
/// them are not `.`.
pub(crate) struct WorldBox {
    pub lo: [i32; 3],
    pub size: [usize; 3],
    pub data: String,
    pub count: usize,
}

pub(crate) struct WorldCells {
    /// The tiles that change something, and the cells changed in all.
    pub tiles: Option<(Vec<WorldBox>, usize)>,
    /// The filled cells around: `#` a filled cell the build does not change.
    pub solids: Option<WorldBox>,
}

/// Run-length text of `size` cells from `lo`, each from `cell`, with the
/// number of cells not `.`.
fn run_length(lo: [i32; 3], hi: [i32; 3], mut cell: impl FnMut(i32, i32, i32) -> u8) -> (String, usize) {
    let mut text = String::new();
    let (mut last, mut run, mut count) = (0u8, 0usize, 0usize);
    for z in lo[2]..=hi[2] {
        for y in lo[1]..=hi[1] {
            for x in lo[0]..=hi[0] {
                let ch = cell(x, y, z);
                if ch != b'.' {
                    count += 1;
                }
                if ch == last {
                    run += 1;
                } else {
                    flush(&mut text, last, run);
                    last = ch;
                    run = 1;
                }
            }
        }
    }
    flush(&mut text, last, run);
    (text, count)
}

fn size_of(lo: [i32; 3], hi: [i32; 3]) -> [usize; 3] {
    [(hi[0] - lo[0] + 1) as usize, (hi[1] - lo[1] + 1) as usize, (hi[2] - lo[2] + 1) as usize]
}

/// The build in the world, many times faster than the mod's own Lua: `+` a
/// cell filled (or replaced, unless the plan keeps the blocks there, a cell
/// not read counting as empty), `-` a block removed, `.` the rest.
pub(crate) fn build_world_cells(input: &WorldInput) -> WorldCells {
    let tiles = input.plan.as_ref().and_then(|plan| {
        let size = size_of(plan.lo, plan.hi);
        if size.iter().product::<usize>() > input.total {
            return None;
        }
        let step = input.tile.max(1) as usize;
        let (mut tiles, mut changed) = (Vec::new(), 0usize);
        for z in (plan.lo[2]..=plan.hi[2]).step_by(step) {
            for y in (plan.lo[1]..=plan.hi[1]).step_by(step) {
                for x in (plan.lo[0]..=plan.hi[0]).step_by(step) {
                    let lo = [x, y, z];
                    let hi = [
                        (x + input.tile - 1).min(plan.hi[0]),
                        (y + input.tile - 1).min(plan.hi[1]),
                        (z + input.tile - 1).min(plan.hi[2]),
                    ];
                    let (data, count) = run_length(lo, hi, |x, y, z| {
                        if !plan_contains(plan, [x, y, z]) {
                            return b'.';
                        }
                        let value = with_density(chunk_value(&input.chunks, x, y, z), input.density_min);
                        let full = value > 0;
                        if plan.add {
                            // Building keeps the blocks already there (replace off),
                            // or replaces them: `*`, drawn on a block of the game
                            // (or the palette's letter for that block).
                            if !full {
                                b'+'
                            } else if plan.only_empty {
                                b'.'
                            } else {
                                input.palette.get(&value).copied().unwrap_or(b'*')
                            }
                        } else if full {
                            input.palette.get(&value).copied().unwrap_or(b'-')
                        } else {
                            b'.'
                        }
                    });
                    if count > 0 {
                        tiles.push(WorldBox { lo, size: size_of(lo, hi), data, count });
                        changed += count;
                    }
                }
            }
        }
        Some((tiles, changed))
    });
    let (mut lo, mut hi) = (input.lo, input.hi);
    if let Some((from, to)) = input.solids_box {
        (lo, hi) = (from, to);
    } else {
        for axis in 0..3 {
            let side = input.hi[axis] - input.lo[axis] + 1;
            let margin = input.margin.min((128 - side).div_euclid(2)).max(0);
            lo[axis] -= margin;
            hi[axis] += margin;
        }
    }
    let size = size_of(lo, hi);
    let solids = if size.iter().any(|&side| side > 256) || size.iter().product::<usize>() > input.solids_max {
        None
    } else {
        // A filled cell the build changes (removed, or replaced) hides nothing.
        let changes = input.plan.as_ref().filter(|plan| !plan.add || !plan.only_empty);
        // The chunk of the cell looked up once for its 16 cells along x: a
        // box holding the eye may have millions of cells, most not read.
        let mut chunk_key = [i32::MIN; 3];
        let mut chunk: Option<&ChunkCells> = None;
        let (data, count) = run_length(lo, hi, |x, y, z| {
            let key = [x.div_euclid(CHUNK), y.div_euclid(CHUNK), z.div_euclid(CHUNK)];
            if key != chunk_key {
                chunk_key = key;
                chunk = input.chunks.get(&key);
            }
            let filled = match chunk {
                None => false,
                Some(ChunkCells::Uniform(value)) => with_density(*value, input.density_min) > 0,
                Some(ChunkCells::Packed(bytes)) => {
                    let index = (x.rem_euclid(CHUNK) + y.rem_euclid(CHUNK) * CHUNK + z.rem_euclid(CHUNK) * CHUNK * CHUNK)
                        as usize
                        * 2;
                    bytes.get(index..index + 2).is_some_and(|pair| {
                        with_density(i32::from(u16::from_le_bytes([pair[0], pair[1]])), input.density_min) > 0
                    })
                }
            };
            if filled && !changes.is_some_and(|plan| plan_contains(plan, [x, y, z])) { b'#' } else { b'.' }
        });
        Some(WorldBox { lo, size, data, count })
    };
    WorldCells { tiles, solids }
}

#[cfg(test)]
mod tests {
    use std::collections::HashSet;

    use super::*;

    fn input(plan: Option<ViewPlan>, lo: [i32; 3], hi: [i32; 3]) -> WorldInput {
        WorldInput {
            chunks: HashMap::new(),
            plan,
            lo,
            hi,
            tile: 256,
            total: 1 << 20,
            margin: 8,
            solids_max: 1 << 18,
            solids_box: None,
            palette: HashMap::new(),
            density_min: 1,
        }
    }

    #[test]
    fn a_line_over_a_block_keeps_it_and_the_rest_is_solids() {
        // x = 2 is a block; the plan builds x = 0..3 at y = z = 0, keeping blocks.
        let mut world = input(
            Some(ViewPlan {
                add: true,
                only_empty: true,
                lo: [0, 0, 0],
                hi: [3, 0, 0],
                cells: Some(HashSet::from([[0, 0, 0], [1, 0, 0], [2, 0, 0], [3, 0, 0]])),
            }),
            [0, 0, 0],
            [3, 0, 0],
        );
        world.margin = 1;
        world.chunks.insert([0, 0, 0], ChunkCells::Uniform(0));
        let mut bytes = vec![0u8; 8192];
        bytes[4] = 155; // x = 2
        world.chunks.insert([0, 0, 0], ChunkCells::Packed(std::sync::Arc::new(bytes)));
        let cells = build_world_cells(&world);
        let (tiles, changed) = cells.tiles.unwrap();
        assert_eq!((tiles.len(), changed), (1, 3));
        assert_eq!(tiles[0].data, "2+.+");
        assert_eq!(tiles[0].size, [4, 1, 1]);
        // Solids: one cell around (x -1..4, y and z -1..1), the block at x = 2,
        // which the build keeps (cells of chunk -1 not read: not filled).
        let solids = cells.solids.unwrap();
        assert_eq!((solids.lo, solids.size, solids.count), ([-1, -1, -1], [6, 3, 3], 1));
        assert_eq!(solids.data, "27.#26.");
    }

    #[test]
    fn replaced_cells_are_told_apart() {
        // Replace on: the filled cell is `*`, the empty one `+`.
        let mut world = input(
            Some(ViewPlan { add: true, only_empty: false, lo: [0, 0, 0], hi: [1, 0, 0], cells: None }),
            [0, 0, 0],
            [1, 0, 0],
        );
        world.chunks.insert([0, 0, 0], ChunkCells::Packed(std::sync::Arc::new({
            let mut bytes = vec![0u8; 16 * 16 * 16 * 2];
            bytes[0] = 7;
            bytes
        })));
        let (tiles, changed) = build_world_cells(&world).tiles.unwrap();
        assert_eq!((tiles[0].data.as_str(), changed), ("*+", 2));
    }

    #[test]
    fn a_palette_tells_the_blocks_removed_or_replaced() {
        // Block 7 is `A`; replacing and removing both write it.
        let chunk = || {
            ChunkCells::Packed(std::sync::Arc::new({
                let mut bytes = vec![0u8; 16 * 16 * 16 * 2];
                bytes[0] = 7;
                bytes[2] = 9;
                bytes
            }))
        };
        let mut world = input(
            Some(ViewPlan { add: true, only_empty: false, lo: [0, 0, 0], hi: [2, 0, 0], cells: None }),
            [0, 0, 0],
            [2, 0, 0],
        );
        world.chunks.insert([0, 0, 0], chunk());
        world.palette.insert(7, b'A');
        assert_eq!(build_world_cells(&world).tiles.unwrap().0[0].data, "A*+");
        world.plan = Some(ViewPlan { add: false, only_empty: false, lo: [0, 0, 0], hi: [2, 0, 0], cells: None });
        assert_eq!(build_world_cells(&world).tiles.unwrap().0[0].data, "A-.");
    }

    #[test]
    fn solids_in_a_box_of_their_own_reach_far_from_the_build() {
        // The build at the origin; the box of the solids reaches x = 40, where
        // a chunk is filled (as between the build and an eye far away).
        let mut world = input(None, [0, 0, 0], [0, 0, 0]);
        world.solids_box = Some(([0, 0, 0], [47, 0, 0]));
        world.chunks.insert([2, 0, 0], ChunkCells::Uniform(155));
        let solids = build_world_cells(&world).solids.unwrap();
        assert_eq!((solids.lo, solids.size, solids.count), ([0, 0, 0], [48, 1, 1], 16));
        assert_eq!(solids.data, "32.16#");
    }

    #[test]
    fn removing_shows_only_the_blocks_there_and_hides_nothing() {
        let mut world = input(
            Some(ViewPlan { add: false, only_empty: false, lo: [0, 0, 0], hi: [1, 0, 0], cells: None }),
            [0, 0, 0],
            [1, 0, 0],
        );
        world.margin = 0;
        world.chunks.insert([0, 0, 0], ChunkCells::Uniform(300));
        let cells = build_world_cells(&world);
        assert_eq!(cells.tiles.unwrap().0[0].data, "2-");
        assert_eq!(cells.solids.unwrap().data, "2.");
    }

    #[test]
    fn too_large_a_build_has_no_cells_and_a_start_alone_only_solids() {
        let mut world = input(
            Some(ViewPlan { add: true, only_empty: false, lo: [0, 0, 0], hi: [99, 99, 99], cells: None }),
            [0, 0, 0],
            [99, 99, 99],
        );
        world.total = 1000;
        assert!(build_world_cells(&world).tiles.is_none());
        let start = build_world_cells(&input(None, [5, 5, 5], [5, 5, 5]));
        assert!(start.tiles.is_none());
        assert_eq!(start.solids.unwrap().size, [17, 17, 17]);
    }

    /// Cross-check with a mod's own Lua builder: WORLD_CELLS_FIXTURE names a
    /// JSON file of an input and the tiles and solids that builder made from
    /// it (Architect Line's tools/world_cells_fixture.lua). Ignored without it.
    #[test]
    #[ignore]
    fn same_world_as_a_fixture() {
        let path = std::env::var("WORLD_CELLS_FIXTURE").expect("WORLD_CELLS_FIXTURE");
        let fixture: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap();
        let cell = |value: &serde_json::Value| -> [i32; 3] {
            let list = value.as_array().unwrap();
            [list[0].as_i64().unwrap() as i32, list[1].as_i64().unwrap() as i32, list[2].as_i64().unwrap() as i32]
        };
        let plan = &fixture["plan"];
        let flat: Vec<i32> = plan["cells"].as_array().unwrap().iter().map(|v| v.as_i64().unwrap() as i32).collect();
        let mut world = input(
            Some(ViewPlan {
                add: plan["add"].as_bool().unwrap(),
                only_empty: plan["only_empty"].as_bool().unwrap(),
                lo: cell(&plan["lo"]),
                hi: cell(&plan["hi"]),
                cells: Some(flat.chunks(3).map(|c| [c[0], c[1], c[2]]).collect()),
            }),
            cell(&fixture["lo"]),
            cell(&fixture["hi"]),
        );
        world.margin = fixture["margin"].as_i64().unwrap() as i32;
        world.tile = fixture["tile"].as_i64().unwrap() as i32;
        for chunk in fixture["chunks"].as_array().unwrap() {
            let cells = match &chunk[3] {
                serde_json::Value::String(hex) => ChunkCells::Packed(std::sync::Arc::new(
                    (0..hex.len()).step_by(2).map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap()).collect(),
                )),
                value => ChunkCells::Uniform(value.as_i64().unwrap() as i32),
            };
            world.chunks.insert(cell(chunk), cells);
        }
        let started = std::time::Instant::now();
        let built = build_world_cells(&world);
        eprintln!("built in {:?}", started.elapsed());
        let expected = &fixture["expected"];
        let (tiles, changed) = built.tiles.unwrap();
        assert_eq!(changed as u64, expected["changed"].as_u64().unwrap());
        let expected_tiles = expected["tiles"].as_array().unwrap();
        assert_eq!(tiles.len(), expected_tiles.len());
        for (tile, wanted) in tiles.iter().zip(expected_tiles) {
            assert_eq!(tile.lo, cell(&wanted["lo"]));
            assert_eq!(tile.data, wanted["data"].as_str().unwrap());
            assert_eq!(tile.count as u64, wanted["changed"].as_u64().unwrap());
        }
        let solids = built.solids.unwrap();
        assert_eq!(solids.lo, cell(&expected["solids"]["lo"]));
        assert_eq!(solids.count as u64, expected["solids"]["filled"].as_u64().unwrap());
        assert_eq!(solids.data, expected["solids"]["data"].as_str().unwrap());
    }
}
