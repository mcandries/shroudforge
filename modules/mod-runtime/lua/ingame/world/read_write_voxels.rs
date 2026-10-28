use super::*;

pub(super) fn lua_world_grid_get_spec(
    lua: &mlua::Lua,
    args: FunctionArgs,
    r#mod: &Mod,
) -> mlua::Result<(LuaValue, Option<String>)> {
    let state = lua.app_data_ref::<AppState>().unwrap();
    if let Some(reason) = runtime_denial_reason(&state, r#mod, "runtime.world.voxel.grid_spec") {
        return Ok((LuaValue::Nil, Some(reason)));
    }
    if args.len() != 1 {
        return Err(LuaError::generic(
            "runtime.world.voxel.get_grid_spec expects a grid id, currently 'voxel'",
        ));
    }
    let id = args.get::<String>(0)?;
    match runtime_provider::world_grid_get_spec(&id) {
        Ok(spec) => {
            let result = lua.create_table()?;
            result.raw_set("id", spec.id)?;
            for (field, values) in [("origin", spec.origin), ("cellSize", spec.cell_size)] {
                let value = lua.create_table()?;
                for axis in 0..3 {
                    value.raw_set(axis + 1, values[axis])?;
                    value.raw_set(["x", "y", "z"][axis], values[axis])?;
                }
                result.raw_set(field, value)?;
            }
            let maximum = lua.create_table()?;
            for axis in 0..3 {
                maximum.raw_set(axis + 1, spec.maximum[axis])?;
                maximum.raw_set(["x", "y", "z"][axis], spec.maximum[axis])?;
            }
            result.raw_set("maximum", maximum)?;
            Ok((LuaValue::Table(result), None))
        }
        Err(reason) => Ok((LuaValue::Nil, Some(reason))),
    }
}

pub(super) fn lua_world_voxel_read(
    lua: &mlua::Lua,
    args: FunctionArgs,
    r#mod: &Mod,
) -> mlua::Result<(LuaValue, Option<String>)> {
    let started = std::time::Instant::now();
    let result = lua_world_voxel_read_impl(lua, args, r#mod);
    crate::lua::ingame::report_operation_timing::record_call("voxel.read", started);
    crate::lua::ingame::report_operation_timing::report_operation_timing();
    result
}

fn lua_world_voxel_read_impl(
    lua: &mlua::Lua,
    args: FunctionArgs,
    r#mod: &Mod,
) -> mlua::Result<(LuaValue, Option<String>)> {
    let state = lua.app_data_ref::<AppState>().unwrap();
    if let Some(reason) = runtime_denial_reason(&state, r#mod, "runtime.world.voxel.read") {
        return Ok((LuaValue::Nil, Some(reason)));
    }
    if args.len() != 6 {
        return Err(LuaError::generic(
            "runtime.world.voxel.read expects x, y, z, sizeX, sizeY, sizeZ",
        ));
    }
    let origin = [
        args.get::<i32>(0)?,
        args.get::<i32>(1)?,
        args.get::<i32>(2)?,
    ];
    let dimensions = [
        args.get::<u32>(3)?,
        args.get::<u32>(4)?,
        args.get::<u32>(5)?,
    ];
    let count = dimensions
        .iter()
        .try_fold(1usize, |total, value| total.checked_mul(*value as usize));
    let Some(count) = count.filter(|count| *count > 0 && *count <= 65_536) else {
        return Ok((
            LuaValue::Nil,
            Some("voxel read must contain between 1 and 65,536 cells".into()),
        ));
    };
    let native_started = std::time::Instant::now();
    let values = runtime_provider::world_voxel_read(origin, dimensions, count);
    crate::lua::ingame::report_operation_timing::record_call("voxel.native_read", native_started);
    let values = match values {
        Ok(values) => values,
        Err(reason) => return Ok((LuaValue::Nil, Some(reason))),
    };
    let result = lua.create_table_with_capacity(values.len(), 0)?;
    for (index, value) in values.into_iter().enumerate() {
        result.raw_set(index + 1, value)?;
    }
    Ok((LuaValue::Table(result), None))
}

pub(super) fn lua_world_voxel_write(
    lua: &mlua::Lua,
    args: FunctionArgs,
    r#mod: &Mod,
) -> mlua::Result<(bool, Option<String>)> {
    let state = lua.app_data_ref::<AppState>().unwrap();
    if let Some(reason) = runtime_denial_reason(&state, r#mod, "runtime.world.voxel.write") {
        return Ok((false, Some(reason)));
    }
    if args.len() != 7 {
        return Err(LuaError::generic(
            "runtime.world.voxel.write expects x, y, z, sizeX, sizeY, sizeZ, cells",
        ));
    }
    let origin = [
        args.get::<i32>(0)?,
        args.get::<i32>(1)?,
        args.get::<i32>(2)?,
    ];
    let dimensions = [
        args.get::<u32>(3)?,
        args.get::<u32>(4)?,
        args.get::<u32>(5)?,
    ];
    let values = args.get::<mlua::Table>(6)?;
    let count = dimensions
        .iter()
        .try_fold(1usize, |total, value| total.checked_mul(*value as usize));
    let Some(count) = count.filter(|count| *count > 0 && *count <= 65_536) else {
        return Ok((
            false,
            Some("voxel write must contain between 1 and 65,536 cells".into()),
        ));
    };
    if values.raw_len() != count {
        return Ok((
            false,
            Some(format!(
                "voxel cell count mismatch: expected {count}, got {}",
                values.raw_len()
            )),
        ));
    }
    let mut cells = Vec::with_capacity(count);
    for index in 1..=count {
        let value = values.raw_get::<u32>(index)?;
        if value > u16::MAX as u32 {
            return Ok((
                false,
                Some(format!(
                    "voxel cell {index} exceeds the supported 16-bit format"
                )),
            ));
        }
        cells.push(value as u16);
    }
    match runtime_provider::world_voxel_write(origin, dimensions, &cells) {
        Ok(()) => Ok((true, None)),
        Err(reason) => Ok((false, Some(reason))),
    }
}
