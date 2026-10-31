use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use super::build_voxel_view::{read_values, surface_skin, ChunkCells, ViewInput};

/// What a view takes from the world before its plan and marks: the value of
/// every cell and, in surface view, the terrain's skin.
pub(crate) struct ViewBase {
    lo: [i32; 3],
    hi: [i32; 3],
    surface: bool,
    depth: i32,
    density_min: i32,
    /// The chunks it was read from, held: a chunk replaced in the store is
    /// another allocation, never one at the same address.
    chunks: HashMap<[i32; 3], ChunkCells>,
    values: Arc<Vec<i32>>,
    skin: Option<Arc<Vec<bool>>>,
}

/// The last base of a chunks store's views (VoxelChunkStore).
pub(crate) type ViewBases = Arc<Mutex<Option<ViewBase>>>;

/// The values and skin of `input`'s box: those of the last view of the same
/// box, depth and chunks of its store when there is one, else read. While the
/// player aims, only the plan of the view changes: 27/10, a 256 x 64 x 256
/// view was built again in full at every aimed cell, 25 to 40 times in 5 s.
pub(crate) fn view_base(input: &ViewInput, size: [usize; 3]) -> (Arc<Vec<i32>>, Option<Arc<Vec<bool>>>) {
    if let Some(bases) = &input.bases {
        if let Ok(last) = bases.lock() {
            if let Some(base) = last.as_ref().filter(|base| same_base(base, input)) {
                return (base.values.clone(), base.skin.clone());
            }
        }
    }
    let values = Arc::new(read_values(input, size));
    let skin = input.surface.then(|| Arc::new(surface_skin(&values, size, input.depth)));
    if let Some(bases) = &input.bases {
        if let Ok(mut last) = bases.lock() {
            *last = Some(ViewBase {
                lo: input.lo,
                hi: input.hi,
                surface: input.surface,
                depth: input.depth,
                density_min: input.density_min,
                chunks: input.chunks.clone(),
                values: values.clone(),
                skin: skin.clone(),
            });
        }
    }
    (values, skin)
}

fn same_base(base: &ViewBase, input: &ViewInput) -> bool {
    base.lo == input.lo
        && base.hi == input.hi
        && base.surface == input.surface
        && (!input.surface || base.depth == input.depth)
        && base.density_min == input.density_min
        && base.chunks.len() == input.chunks.len()
        && input.chunks.iter().all(|(key, cells)| match (base.chunks.get(key), cells) {
            (Some(ChunkCells::Packed(a)), ChunkCells::Packed(b)) => Arc::ptr_eq(a, b),
            (Some(ChunkCells::Uniform(a)), ChunkCells::Uniform(b)) => a == b,
            _ => false,
        })
}

#[cfg(test)]
mod tests {
    use super::super::build_voxel_view::{build_voxel_view, ViewPlan, CHUNK};
    use super::*;
    use std::collections::HashSet;

    fn input(chunks: HashMap<[i32; 3], ChunkCells>, bases: &ViewBases, plan_x: i32) -> ViewInput {
        ViewInput {
            lo: [0, 0, 0],
            hi: [CHUNK * 2 - 1, CHUNK - 1, CHUNK - 1],
            chunks,
            plan: Some(ViewPlan {
                add: true,
                only_empty: false,
                lo: [plan_x, 8, 0],
                hi: [plan_x, 8, 0],
                cells: None,
            }),
            start: None,
            finish: None,
            player: None,
            roofs: HashSet::new(),
            surface: true,
            depth: 3,
            bases: Some(bases.clone()),
            density_min: 1,
        }
    }

    fn ground() -> ChunkCells {
        let mut bytes = Vec::new();
        for _z in 0..CHUNK {
            for y in 0..CHUNK {
                for _x in 0..CHUNK {
                    bytes.extend_from_slice(&(if y < 6 { 300u16 } else { 0 }).to_le_bytes());
                }
            }
        }
        ChunkCells::Packed(Arc::new(bytes))
    }

    #[test]
    fn a_view_of_the_same_chunks_reuses_their_base_and_draws_its_own_plan() {
        let bases = ViewBases::default();
        let chunks = HashMap::from([([0, 0, 0], ground()), ([1, 0, 0], ChunkCells::Uniform(0))]);
        let first = input(chunks.clone(), &bases, 3);
        let (values, _) = view_base(&first, [32, 16, 16]);
        let (again, _) = view_base(&input(chunks.clone(), &bases, 9), [32, 16, 16]);
        assert!(Arc::ptr_eq(&values, &again), "same chunks: the base is kept");
        // The text is the one of a view built without a base.
        let mut fresh = input(chunks.clone(), &ViewBases::default(), 9);
        fresh.bases = None;
        assert_eq!(build_voxel_view(&input(chunks.clone(), &bases, 9)).1, build_voxel_view(&fresh).1);
        assert_ne!(build_voxel_view(&input(chunks.clone(), &bases, 3)).1, build_voxel_view(&fresh).1);
        // A chunk replaced (read again, changed): read anew.
        let mut changed = chunks.clone();
        changed.insert([0, 0, 0], ground());
        let (other, _) = view_base(&input(changed, &bases, 9), [32, 16, 16]);
        assert!(!Arc::ptr_eq(&values, &other), "another chunk: read again");
    }

    #[test]
    fn values_read_by_chunk_are_those_of_each_cell() {
        let chunks = HashMap::from([([0, 0, 0], ground()), ([1, 0, 0], ChunkCells::Uniform(7))]);
        let mut view = input(chunks, &ViewBases::default(), 3);
        view.lo = [-2, 4, 3];
        view.hi = [20, 9, 5];
        let size = [23, 6, 3];
        let values = read_values(&view, size);
        let mut i = 0;
        for z in view.lo[2]..=view.hi[2] {
            for y in view.lo[1]..=view.hi[1] {
                for x in view.lo[0]..=view.hi[0] {
                    assert_eq!(values[i], super::super::build_voxel_view::chunk_value(&view.chunks, x, y, z), "{x} {y} {z}");
                    i += 1;
                }
            }
        }
    }
}
