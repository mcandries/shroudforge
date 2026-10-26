use super::*;
pub(super) fn lua_ecs_read_fields(
    lua: &mlua::Lua,
    args: FunctionArgs,
    r#mod: &Mod,
) -> mlua::Result<(LuaValue, Option<String>)> {
    let started = std::time::Instant::now();
    let result = match ecs_fields(lua, args, r#mod, false) {
        Ok(value) => (value, None),
        Err(reason) => (LuaValue::Nil, Some(reason)),
    };
    crate::lua::ingame::report_operation_timing::record_call("ecs.read_fields", started);
    crate::lua::ingame::report_operation_timing::report_operation_timing();
    Ok(result)
}

pub(super) fn lua_ecs_write_fields(
    lua: &mlua::Lua,
    args: FunctionArgs,
    r#mod: &Mod,
) -> mlua::Result<(bool, Option<String>, LuaValue)> {
    Ok(match ecs_fields(lua, args, r#mod, true) {
        Ok(confirmed) => (true, None, confirmed),
        Err(reason) => (false, Some(reason), LuaValue::Nil),
    })
}

pub(super) fn ecs_fields(
    lua: &mlua::Lua,
    args: FunctionArgs,
    r#mod: &Mod,
    write: bool,
) -> Result<LuaValue, String> {
    let state = lua.app_data_ref::<AppState>().unwrap();
    let operation = if write {
        "runtime.ecs.write"
    } else {
        "runtime.ecs.read"
    };
    if let Some(reason) = runtime_denial_reason(&state, r#mod, operation) {
        return Err(reason);
    }
    if !available(&state, r#mod, operation) {
        return Err(format!("{operation} is currently unavailable"));
    }
    if args.len() != 3 && !(write && args.len() == 4) {
        return Err(if write {
            "runtime.ecs.write_fields expects entity, component, changes, and optional expected fields"
        } else {
            "runtime.ecs.read_fields expects entity, component, and an array of field paths"
        }
        .into());
    }
    let entity = args.get::<u32>(0).map_err(|e| e.to_string())?;
    let name = runtime_type_name(lua, &args, 1).map_err(|e| e.to_string())?;
    let component = runtime_provider::resolve(&name)
        .ok_or_else(|| format!("live ECS component unavailable: {name}"))?;
    let registry = state.type_registry();
    let metadata = registry
        .get_by_name(kfc::reflection::LookupKey::Qualified(&name))
        .ok_or_else(|| format!("component type metadata unavailable: {name}"))?;
    if metadata.size != component.size {
        return Err(format!(
            "component layout mismatch for {name}: parser={}, runtime={}",
            metadata.size, component.size
        ));
    }
    let selections = args.get::<mlua::Table>(2).map_err(|e| e.to_string())?;
    let mut fields = std::collections::BTreeMap::new();
    for pair in selections.clone().pairs::<LuaValue, LuaValue>() {
        let (key, value) = pair.map_err(|e| e.to_string())?;
        if fields.len() >= 1024 {
            return Err("at most 1024 field paths may be selected".into());
        }
        let path = if write {
            let LuaValue::String(path) = key else {
                return Err("write_fields keys must be reflected field paths".into());
            };
            path.to_str().map_err(|e| e.to_string())?.to_owned()
        } else {
            let LuaValue::Integer(position) = key else {
                return Err("read_fields expects a dense array of field paths".into());
            };
            if position <= 0 || position as usize > selections.raw_len() {
                return Err("read_fields expects a dense array of field paths".into());
            }
            let LuaValue::String(path) = &value else {
                return Err("read_fields expects string field paths".into());
            };
            path.to_str().map_err(|e| e.to_string())?.to_owned()
        };
        let field = state
            .inline_field(metadata.index, &path)
            .map_err(|e| format!("{path}: {e}"))?;
        if fields.insert(path, (field, value)).is_some() {
            return Err("duplicate field path".into());
        }
    }
    if fields.is_empty() || (!write && fields.len() != selections.raw_len()) {
        return Err("select a nonempty set of field paths".into());
    }
    let before = runtime_provider::read(entity, &name, component.size)
        .ok_or_else(|| format!("component read failed: {name}"))?;
    if write {
        if !runtime_provider::can_write_component(&name) {
            return Err("component write context or original command phase is unavailable".into());
        }
        if args.len() == 4 {
            let expected = args.get::<mlua::Table>(3).map_err(|e| e.to_string())?;
            crate::env::runtime_values::expect_inline_fields(
                lua,
                registry,
                metadata.index,
                &before,
                &expected,
            )?;
        }
        let mut after = before.clone();
        let mut mask = vec![0; before.len()];
        for (path, (field, value)) in &fields {
            field
                .write_into(lua, value, &mut after, &mut mask)
                .map_err(|e| format!("{path}: {e}"))?;
        }
        crate::env::runtime_attributes::recalculate_component_update(
            &state, &name, &before, &mut after, &mut mask,
        )?;
        crate::env::runtime_ownership::check_write(
            &state,
            r#mod,
            &crate::env::runtime_ownership::component_target(entity, &name),
        )?;
        let confirmed = lua
            .create_table_with_capacity(0, fields.len())
            .map_err(|error| error.to_string())?;
        for (path, (field, _)) in &fields {
            confirmed
                .raw_set(path.as_str(), field.read(lua, &after)?)
                .map_err(|error| error.to_string())?;
        }
        if after != before {
            runtime_provider::compare_exchange(entity, &name, &before, &mask, &after)?;
        }
        Ok(LuaValue::Table(confirmed))
    } else {
        let result = lua
            .create_table_with_capacity(0, fields.len())
            .map_err(|e| e.to_string())?;
        for (path, (field, _)) in fields {
            result
                .raw_set(
                    path.as_str(),
                    field
                        .read(lua, &before)
                        .map_err(|e| format!("{path}: {e}"))?,
                )
                .map_err(|e| e.to_string())?;
        }
        Ok(LuaValue::Table(result))
    }
}
