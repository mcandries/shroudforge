use super::*;

pub(super) fn lua_ecs_read(
    lua: &mlua::Lua,
    args: FunctionArgs,
    r#mod: &Mod,
) -> mlua::Result<(LuaValue, Option<String>)> {
    let started = std::time::Instant::now();
    let result = lua_ecs_read_impl(lua, args, r#mod, false);
    crate::lua::ingame::report_operation_timing::record_call("ecs.read", started);
    crate::lua::ingame::report_operation_timing::report_operation_timing();
    result
}

pub(super) fn lua_ecs_read_bytes(
    lua: &mlua::Lua,
    args: FunctionArgs,
    r#mod: &Mod,
) -> mlua::Result<(LuaValue, Option<String>)> {
    lua_ecs_read_impl(lua, args, r#mod, true)
}

pub(super) fn lua_ecs_read_impl(
    lua: &mlua::Lua,
    args: FunctionArgs,
    r#mod: &Mod,
    raw: bool,
) -> mlua::Result<(LuaValue, Option<String>)> {
    let state = lua.app_data_ref::<AppState>().unwrap();
    if let Some(reason) = runtime_denial_reason(&state, r#mod, "runtime.ecs.read") {
        return Ok((LuaValue::Nil, Some(reason)));
    }
    if !matches!(
        state.api().runtime("runtime.ecs.read"),
        Availability::Available
    ) {
        return unavailable_runtime_result(lua, &state, "runtime.ecs.read");
    }
    let entity_id = args.get::<u32>(0)?;
    let component_name = runtime_type_name(lua, &args, 1)?;
    let Some(component) = runtime_provider::resolve(&component_name) else {
        return Ok((
            LuaValue::Nil,
            Some(format!("live ECS component unavailable: {component_name}")),
        ));
    };
    let Some(metadata) = state
        .type_registry()
        .get_by_name(kfc::reflection::LookupKey::Qualified(&component_name))
    else {
        return Ok((
            LuaValue::Nil,
            Some(format!("type metadata unavailable: {component_name}")),
        ));
    };
    if metadata.size != component.size {
        return Ok((
            LuaValue::Nil,
            Some(format!(
                "component layout mismatch for {component_name}: parser={}, runtime={}",
                metadata.size, component.size
            )),
        ));
    }
    let Some(bytes) = runtime_provider::read(entity_id, &component_name, component.size) else {
        return Ok((
            LuaValue::Nil,
            Some(format!("component read failed: {component_name}")),
        ));
    };
    if raw {
        return Ok((LuaValue::String(lua.create_string(&bytes)?), None));
    }
    let bytes: Rc<[u8]> = bytes.into();
    let mapped = MappedValue::from_bytes(state.type_registry(), metadata, &bytes)
        .map_err(LuaError::external)?;
    Ok((convert_value_to_lua(&mapped, lua)?, None))
}
