use crate::lua::LuaValue;
/// Diagnostic reports contain detached finite scalars, arrays and string-keyed
/// objects. Reject cycles, mixed tables and excessive nesting before persistence.
pub(crate) fn lua_to_json(value: LuaValue) -> mlua::Result<serde_json::Value> {
    lua_to_json_with(value, &mut |_| Ok(None))
}

/// `lua_to_json`, each string first given to `strings`, which may convert it
/// itself (`Some`) or leave it as a JSON string (`None`).
pub(crate) fn lua_to_json_with(
    value: LuaValue,
    strings: &mut dyn FnMut(&mlua::String) -> mlua::Result<Option<serde_json::Value>>,
) -> mlua::Result<serde_json::Value> {
    fn convert(
        value: LuaValue,
        depth: usize,
        budget: &mut usize,
        tables: &mut std::collections::HashSet<usize>,
        strings: &mut dyn FnMut(&mlua::String) -> mlua::Result<Option<serde_json::Value>>,
    ) -> mlua::Result<serde_json::Value> {
        use serde_json::Value;
        if depth > 32 || *budget == 0 {
            return Err(mlua::Error::runtime(
                "diagnostic report exceeds its structural limit",
            ));
        }
        *budget -= 1;
        Ok(match value {
            LuaValue::Nil => Value::Null,
            LuaValue::Boolean(value) => Value::Bool(value),
            LuaValue::Integer(value) => Value::Number(value.into()),
            LuaValue::Number(value) => {
                Value::Number(serde_json::Number::from_f64(value).ok_or_else(|| {
                    mlua::Error::runtime("diagnostic report contains a nonfinite number")
                })?)
            }
            LuaValue::String(value) => match strings(&value)? {
                Some(converted) => converted,
                None => Value::String(value.to_str()?.to_string()),
            },
            LuaValue::Table(table) => {
                let identity = table.to_pointer() as usize;
                if !tables.insert(identity) {
                    return Err(mlua::Error::runtime("diagnostic report contains a cycle"));
                }
                let length = table.raw_len();
                let result = if length == 0 {
                    let mut object = serde_json::Map::new();
                    for entry in table.pairs::<LuaValue, LuaValue>() {
                        let (key, value) = entry?;
                        let LuaValue::String(key) = key else {
                            return Err(mlua::Error::runtime(
                                "diagnostic object keys must be strings",
                            ));
                        };
                        object.insert(
                            key.to_str()?.to_string(),
                            convert(value, depth + 1, budget, tables, strings)?,
                        );
                    }
                    Value::Object(object)
                } else {
                    if length > *budget {
                        return Err(mlua::Error::runtime(
                            "diagnostic array exceeds its item limit",
                        ));
                    }
                    let mut count = 0;
                    for entry in table.pairs::<LuaValue, LuaValue>() {
                        let (key, _) = entry?;
                        if !matches!(key, LuaValue::Integer(index) if index >= 1 && index as usize <= length)
                        {
                            return Err(mlua::Error::runtime(
                                "diagnostic reports cannot mix array and object keys",
                            ));
                        }
                        count += 1;
                    }
                    if count != length {
                        return Err(mlua::Error::runtime("diagnostic arrays must be dense"));
                    }
                    let mut values = Vec::with_capacity(length);
                    for index in 1..=length {
                        values.push(convert(table.raw_get(index)?, depth + 1, budget, tables, strings)?);
                    }
                    Value::Array(values)
                };
                tables.remove(&identity);
                result
            }
            _ => {
                return Err(mlua::Error::runtime(
                    "diagnostic report contains an unsupported Lua value",
                ));
            }
        })
    }
    convert(value, 0, &mut 100_000, &mut Default::default(), strings)
}

/// JSON objects are detached Lua tables, never mutable access to provider state.
pub(crate) fn json_to_lua(lua: &mlua::Lua, value: &serde_json::Value) -> mlua::Result<LuaValue> {
    use mlua::IntoLua;
    match value {
        serde_json::Value::Null => Ok(LuaValue::Nil),
        serde_json::Value::Bool(value) => value.into_lua(lua),
        serde_json::Value::String(value) => value.as_str().into_lua(lua),
        serde_json::Value::Number(value) => {
            if let Some(value) = value.as_i64() {
                value.into_lua(lua)
            } else if let Some(value) = value.as_u64() {
                Ok(LuaValue::Integer(value as i64))
            } else {
                value.as_f64().into_lua(lua)
            }
        }
        serde_json::Value::Array(values) => {
            let table = lua.create_table_with_capacity(values.len(), 0)?;
            for (i, value) in values.iter().enumerate() {
                table.raw_set(i + 1, json_to_lua(lua, value)?)?;
            }
            Ok(LuaValue::Table(table))
        }
        serde_json::Value::Object(values) => {
            let table = lua.create_table_with_capacity(0, values.len())?;
            for (key, value) in values {
                table.raw_set(key.as_str(), json_to_lua(lua, value)?)?;
            }
            Ok(LuaValue::Table(table))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use mlua::Lua;
    use serde_json::json;

    #[test]
    fn reports_convert_finite_scalars_dense_arrays_and_string_keyed_objects() -> mlua::Result<()> {
        let lua = Lua::new();
        let report = lua
            .load(
                r#"return {name="Größe", count=-7, ready=true, ratio=0.25,
                    values={1, false, "three"}, empty={}}"#,
            )
            .eval::<LuaValue>()?;
        assert_eq!(
            lua_to_json(report)?,
            json!({"name":"Größe","count":-7,"ready":true,"ratio":0.25,
                "values":[1,false,"three"],"empty":{}})
        );
        for value in [
            json!(null),
            json!(false),
            json!(-17),
            json!(1.5),
            json!("ä ß"),
        ] {
            assert_eq!(lua_to_json(json_to_lua(&lua, &value)?)?, value);
        }
        Ok(())
    }

    #[test]
    fn reports_reject_nonfinite_numbers_unsupported_values_and_invalid_keys() -> mlua::Result<()> {
        let lua = Lua::new();
        for value in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
            assert!(
                lua_to_json(LuaValue::Number(value))
                    .unwrap_err()
                    .to_string()
                    .contains("nonfinite")
            );
        }
        for source in [
            "return function() end",
            "return {[false]=1}",
            "return {[0]=1}",
            "return {[1]=1, named=2}",
            "return {[1]=1, [3]=3}",
        ] {
            assert!(
                lua_to_json(lua.load(source).eval::<LuaValue>()?).is_err(),
                "{source}"
            );
        }
        Ok(())
    }

    #[test]
    fn reports_reject_cycles_but_allow_repeated_detached_table_values() -> mlua::Result<()> {
        let lua = Lua::new();
        for source in [
            "local t={}; t.self=t; return t",
            "local t={}; t.child={parent=t}; return t",
        ] {
            assert!(
                lua_to_json(lua.load(source).eval::<LuaValue>()?)
                    .unwrap_err()
                    .to_string()
                    .contains("cycle")
            );
        }
        let shared = lua
            .load("local child={value=7}; return {first=child, second=child}")
            .eval::<LuaValue>()?;
        assert_eq!(
            lua_to_json(shared)?,
            json!({"first":{"value":7},"second":{"value":7}})
        );
        Ok(())
    }

    #[test]
    fn reports_enforce_depth_and_item_limits_before_persistence() -> mlua::Result<()> {
        let lua = Lua::new();
        let deep = lua
            .load(
                "local root={}; local t=root; for i=1,34 do t.child={}; t=t.child end; return root",
            )
            .eval::<LuaValue>()?;
        assert!(
            lua_to_json(deep)
                .unwrap_err()
                .to_string()
                .contains("structural limit")
        );
        let large = lua.create_table_with_capacity(100_000, 0)?;
        for index in 1..=100_000 {
            large.raw_set(index, index)?;
        }
        assert!(
            lua_to_json(LuaValue::Table(large))
                .unwrap_err()
                .to_string()
                .contains("item limit")
        );
        Ok(())
    }
}
