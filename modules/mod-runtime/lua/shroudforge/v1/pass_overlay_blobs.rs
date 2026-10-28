use crate::lua::convert_json::lua_to_json_with;
use crate::lua::LuaValue;

/// Strings of a zone this long or longer go to the overlay as blobs.
pub(crate) const BLOB_MIN_BYTES: usize = 16 * 1024;

/// The long strings of a mod's overlay zones (a 3D view, the tiles of a
/// build), each given to the layer once as a blob and named in the zone's
/// JSON by `{"blob": id}`: a view of a few megabytes is no longer copied and
/// escaped into JSON, then parsed by the layer, at every change of its zone
/// (27/10: 2 ms a set for a 256 x 64 x 256 view, on the mods loop). A Lua
/// string is the same blob while the same string is given (Lua strings do not
/// change; a blob holds its string, so that no other one takes its place).
#[derive(Default)]
pub(crate) struct OverlayBlobs {
    blobs: Vec<Blob>,
    next: u64,
}

struct Blob {
    text: mlua::String,
    id: u64,
    zones: Vec<String>,
}

impl OverlayBlobs {
    /// The JSON of `zone`, set as `zone_id`, its long strings given by `give`
    /// (a new blob; false: the layer did not take it, the string stays in the
    /// JSON). The blobs no zone names any longer go to `drop`.
    pub(crate) fn zone_json(
        &mut self,
        zone: LuaValue,
        zone_id: &str,
        give: &mut dyn FnMut(u64, &[u8]) -> bool,
        drop: &mut dyn FnMut(u64),
    ) -> mlua::Result<serde_json::Value> {
        let mut named = Vec::new();
        let json = lua_to_json_with(zone, &mut |text| {
            if text.as_bytes().len() < BLOB_MIN_BYTES {
                return Ok(None);
            }
            let pointer = text.to_pointer();
            let id = match self.blobs.iter().find(|blob| blob.text.to_pointer() == pointer) {
                Some(blob) => blob.id,
                None => {
                    let id = self.next;
                    if !give(id, &text.as_bytes()) {
                        return Ok(None);
                    }
                    self.next += 1;
                    self.blobs.push(Blob { text: text.clone(), id, zones: Vec::new() });
                    id
                }
            };
            named.push(id);
            Ok(Some(serde_json::json!({ "blob": id })))
        })?;
        for blob in &mut self.blobs {
            let names = named.contains(&blob.id);
            let named_before = blob.zones.iter().any(|zone| zone == zone_id);
            if names && !named_before {
                blob.zones.push(zone_id.to_owned());
            } else if !names && named_before {
                blob.zones.retain(|zone| zone != zone_id);
            }
        }
        self.drop_unnamed(drop);
        Ok(json)
    }

    /// The zone `zone_id` removed: its blobs no other zone names are dropped.
    pub(crate) fn forget_zone(&mut self, zone_id: &str, drop: &mut dyn FnMut(u64)) {
        for blob in &mut self.blobs {
            blob.zones.retain(|zone| zone != zone_id);
        }
        self.drop_unnamed(drop);
    }

    /// Every blob dropped (the mod's zones cleared, or the layer lost them).
    pub(crate) fn forget_all(&mut self, drop: &mut dyn FnMut(u64)) {
        for blob in self.blobs.drain(..) {
            drop(blob.id);
        }
    }

    fn drop_unnamed(&mut self, drop: &mut dyn FnMut(u64)) {
        self.blobs.retain(|blob| {
            if blob.zones.is_empty() {
                drop(blob.id);
            }
            !blob.zones.is_empty()
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use mlua::Lua;
    use serde_json::json;

    fn zone(lua: &Lua, cells: &mlua::String, label: &str) -> mlua::Result<LuaValue> {
        let table = lua.create_table()?;
        table.set("cells", cells.clone())?;
        table.set("label", label)?;
        Ok(LuaValue::Table(table))
    }

    #[test]
    fn a_long_string_is_given_once_and_named_while_a_zone_names_it() -> mlua::Result<()> {
        let lua = Lua::new();
        let view = lua.create_string("#".repeat(BLOB_MIN_BYTES))?;
        let other = lua.create_string(".".repeat(BLOB_MIN_BYTES + 1))?;
        let mut blobs = OverlayBlobs::default();
        let mut given = Vec::new();
        let mut dropped = Vec::new();
        let first = blobs.zone_json(
            zone(&lua, &view, "a")?,
            "preview",
            &mut |id, data| {
                given.push((id, data.len()));
                true
            },
            &mut |id| dropped.push(id),
        )?;
        assert_eq!(first, json!({"cells": {"blob": 0}, "label": "a"}));
        // The same string again (its label changed): named, not given again.
        let again = blobs.zone_json(
            zone(&lua, &view, "b")?,
            "preview",
            &mut |id, data| {
                given.push((id, data.len()));
                true
            },
            &mut |id| dropped.push(id),
        )?;
        assert_eq!(again, json!({"cells": {"blob": 0}, "label": "b"}));
        assert_eq!(given, vec![(0, BLOB_MIN_BYTES)]);
        // Another zone naming it keeps it while the first moves on.
        blobs.zone_json(zone(&lua, &view, "c")?, "copy", &mut |_, _| true, &mut |id| dropped.push(id))?;
        let moved = blobs.zone_json(
            zone(&lua, &other, "d")?,
            "preview",
            &mut |id, data| {
                given.push((id, data.len()));
                true
            },
            &mut |id| dropped.push(id),
        )?;
        assert_eq!(moved, json!({"cells": {"blob": 1}, "label": "d"}));
        assert!(dropped.is_empty());
        blobs.forget_zone("copy", &mut |id| dropped.push(id));
        assert_eq!(dropped, vec![0]);
        blobs.forget_all(&mut |id| dropped.push(id));
        assert_eq!(dropped, vec![0, 1]);
        Ok(())
    }

    #[test]
    fn short_strings_and_refused_blobs_stay_in_the_json() -> mlua::Result<()> {
        let lua = Lua::new();
        let short = lua.create_string("12.")?;
        let long = lua.create_string("#".repeat(BLOB_MIN_BYTES))?;
        let mut blobs = OverlayBlobs::default();
        let json = blobs.zone_json(zone(&lua, &short, "a")?, "z", &mut |_, _| panic!("short"), &mut |_| {})?;
        assert_eq!(json["cells"], "12.");
        let json = blobs.zone_json(zone(&lua, &long, "a")?, "z", &mut |_, _| false, &mut |_| {})?;
        assert_eq!(json["cells"].as_str().map(str::len), Some(BLOB_MIN_BYTES));
        Ok(())
    }
}
