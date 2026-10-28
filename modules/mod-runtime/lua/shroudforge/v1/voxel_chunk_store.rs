use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use mlua::{UserData, UserDataMethods, Value};

use super::build_voxel_view::{ChunkCells, CHUNK};
use super::reuse_voxel_view_base::ViewBases;

/// Chunks a mod keeps on the native side for its voxel views: set once when
/// read (`set`), then taken by every view or world build without copying them
/// from Lua again (27/10: up to 12 MB copied at every 3D view of 256 cells a
/// side, on the mods' loop).
#[derive(Clone, Default)]
pub(crate) struct VoxelChunkStore {
    chunks: Arc<Mutex<HashMap<[i32; 3], ChunkCells>>>,
    /// The last base of the views built from it (reuse_voxel_view_base).
    pub(crate) bases: ViewBases,
}

impl VoxelChunkStore {
    /// The chunks that meet lo..hi (cells), shared, not copied.
    pub(crate) fn region(&self, lo: [i32; 3], hi: [i32; 3]) -> HashMap<[i32; 3], ChunkCells> {
        let mut out = HashMap::new();
        let Ok(chunks) = self.chunks.lock() else { return out };
        for cz in lo[2].div_euclid(CHUNK)..=hi[2].div_euclid(CHUNK) {
            for cy in lo[1].div_euclid(CHUNK)..=hi[1].div_euclid(CHUNK) {
                for cx in lo[0].div_euclid(CHUNK)..=hi[0].div_euclid(CHUNK) {
                    if let Some(cells) = chunks.get(&[cx, cy, cz]) {
                        out.insert([cx, cy, cz], cells.clone());
                    }
                }
            }
        }
        out
    }
}

/// A chunk's cells from Lua: 8192 bytes of little-endian u16, one number for
/// every cell, or nil (not read: removed).
pub(crate) fn chunk_cells(value: Value) -> mlua::Result<Option<ChunkCells>> {
    match value {
        Value::Nil | Value::Boolean(false) => Ok(None),
        Value::String(text) => {
            let bytes = text.as_bytes().to_vec();
            if bytes.len() != (CHUNK * CHUNK * CHUNK * 2) as usize {
                return Err(mlua::Error::runtime("voxel chunks: a chunk holds 8192 bytes"));
            }
            Ok(Some(ChunkCells::Packed(Arc::new(bytes))))
        }
        Value::Integer(value) => Ok(Some(ChunkCells::Uniform(value as i32))),
        Value::Number(value) => Ok(Some(ChunkCells::Uniform(value as i32))),
        _ => Err(mlua::Error::runtime("voxel chunks: cells are a string, a number or nil")),
    }
}

impl UserData for VoxelChunkStore {
    fn add_methods<M: UserDataMethods<Self>>(methods: &mut M) {
        // set(x, y, z, cells): the chunk at origin / 16 = x, y, z.
        methods.add_method("set", |_, store, (x, y, z, cells): (i32, i32, i32, Value)| {
            let cells = chunk_cells(cells)?;
            let mut chunks = store.chunks.lock().map_err(|_| mlua::Error::runtime("voxel chunks: poisoned"))?;
            match cells {
                Some(cells) => chunks.insert([x, y, z], cells),
                None => chunks.remove(&[x, y, z]),
            };
            Ok(())
        });
        methods.add_method("clear", |_, store, ()| {
            if let Ok(mut chunks) = store.chunks.lock() {
                chunks.clear();
            }
            Ok(())
        });
        methods.add_method("count", |_, store, ()| Ok(store.chunks.lock().map(|chunks| chunks.len()).unwrap_or(0)));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_region_takes_the_chunks_it_meets() {
        let store = VoxelChunkStore::default();
        {
            let mut chunks = store.chunks.lock().unwrap();
            chunks.insert([0, 0, 0], ChunkCells::Uniform(5));
            chunks.insert([1, 0, 0], ChunkCells::Uniform(6));
            chunks.insert([-1, 0, 0], ChunkCells::Uniform(7));
        }
        let region = store.region([0, 0, 0], [16, 15, 15]);
        assert_eq!(region.len(), 2);
        assert!(region.contains_key(&[1, 0, 0]) && !region.contains_key(&[-1, 0, 0]));
        assert_eq!(store.region([-1, 0, 0], [-1, 0, 0]).len(), 1);
    }
}
