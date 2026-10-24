use std::time::{SystemTime, UNIX_EPOCH};

use mlua::{Function, Table, Value, Variadic};
use mod_loader::Mod;
use serde_json::json;

use super::game_keys;
use super::set_mod_settings::set_settings;
use crate::lua::LuaValue;

pub fn create(lua: &mlua::Lua, r#mod: &Mod) -> mlua::Result<Table> {
    let table = lua.create_table()?;
    table.raw_set("version", env!("CARGO_PKG_VERSION"))?;
    table.raw_set("mod_id", r#mod.info().id.as_str())?;

    let log = lua.create_table()?;
    for level in ["trace", "debug", "info", "warn", "error"] {
        let id = r#mod.info().id.clone();
        log.raw_set(
            level,
            lua.create_function(move |_, args: Variadic<LuaValue>| {
                let disabled_detail_level = match level {
                    "trace" => {
                        !tracing::enabled!(target: "shroudforge::mod", tracing::Level::TRACE)
                    }
                    "debug" => {
                        !tracing::enabled!(target: "shroudforge::mod", tracing::Level::DEBUG)
                    }
                    _ => false,
                };
                if disabled_detail_level {
                    return Ok(());
                }
                let message = args
                    .into_iter()
                    .map(|value| value.to_string())
                    .collect::<mlua::Result<Vec<_>>>()?
                    .join("\t");
                match level {
                    "trace" => {
                        tracing::trace!(target: "shroudforge::mod", mod_id = %id, "{message}")
                    }
                    "debug" => {
                        tracing::debug!(target: "shroudforge::mod", mod_id = %id, "{message}")
                    }
                    "info" => tracing::info!(target: "shroudforge::mod", mod_id = %id, "{message}"),
                    "warn" => tracing::warn!(target: "shroudforge::mod", mod_id = %id, "{message}"),
                    "error" => {
                        tracing::error!(target: "shroudforge::mod", mod_id = %id, "{message}")
                    }
                    _ => unreachable!(),
                }
                Ok(())
            })?,
        )?;
    }
    table.raw_set("log", log)?;

    let game_dir = lua
        .app_data_ref::<crate::env::AppState>()
        .unwrap()
        .env()
        .game_dir()
        .as_std_path()
        .to_path_buf();
    let mod_id = r#mod.info().id.clone();

    let settings = lua.create_table()?;
    // In-memory values; the runtime refreshes explicitly live settings between updates.
    set_settings(lua, &mod_id, &r#mod.info().setting_values)?;
    let settings_id = mod_id.clone();
    settings.raw_set(
        "get",
        lua.create_function(move |lua, (key, fallback): (String, Option<Value>)| {
            if !valid_identifier(&key) {
                return Err(mlua::Error::runtime("invalid setting key"));
            }
            let current: Table =
                lua.named_registry_value(&format!("shroudforge.settings.{settings_id}"))?;
            let value: Value = current.raw_get(key)?;
            Ok(if value.is_nil() {
                fallback.unwrap_or(Value::Nil)
            } else {
                value
            })
        })?,
    )?;
    table.raw_set("settings", settings)?;

    let input = lua.create_table()?;
    input.raw_set(
        "is_key_down",
        lua.create_function(|_, key: String| {
            // Unknown names read as not held, as before.
            Ok(game_keys::parse(&key).is_ok_and(|combo| game_keys::is_down_now(&combo)))
        })?,
    )?;
    let watch_mod_id = mod_id.clone();
    input.raw_set(
        "watch",
        lua.create_function(move |lua, specs: Vec<String>| {
            let combos = specs
                .iter()
                .map(|spec| game_keys::parse(spec))
                .collect::<Result<Vec<_>, _>>()
                .map_err(mlua::Error::runtime)?;
            let names = lua.create_sequence_from(combos.iter().map(game_keys::Combo::canonical))?;
            game_keys::watch(&watch_mod_id, combos);
            Ok(names)
        })?,
    )?;
    let take_mod_id = mod_id.clone();
    input.raw_set(
        "take_presses",
        lua.create_function(move |lua, ()| {
            lua.create_sequence_from(
                game_keys::take_presses(&take_mod_id)
                    .iter()
                    .map(game_keys::Combo::canonical),
            )
        })?,
    )?;
    input.raw_set(
        "normalize",
        lua.create_function(|_, spec: String| match game_keys::parse(&spec) {
            Ok(combo) => Ok((Some(combo.canonical()), None)),
            Err(reason) => Ok((None, Some(reason))),
        })?,
    )?;
    table.raw_set("input", input)?;

    let ui = lua.create_table()?;
    let ui_mod_id = mod_id.clone();
    ui.raw_set(
        "on_action",
        lua.create_function(move |lua, (action, callback): (String, Function)| {
            if !valid_identifier(&ui_mod_id) || !valid_identifier(&action) {
                return Err(mlua::Error::runtime("invalid ui action"));
            }
            lua.set_named_registry_value(&action_registry_key(&ui_mod_id, &action), callback)
        })?,
    )?;
    table.raw_set("ui", ui)?;

    let notifications = lua.create_table()?;
    notifications.raw_set(
        "publish",
        lua.create_function(move |_, notice: Table| {
            let id = notice.get::<String>("id")?;
            let title = notice.get::<String>("title")?;
            let message = notice.get::<String>("message")?;
            let level = notice.get::<Option<String>>("level")?.unwrap_or_else(|| "info".into());
            let action_url = notice.get::<Option<String>>("action_url")?;
            if !valid_identifier(&mod_id)
                || !valid_identifier(&id)
                || title.len() > 120
                || message.len() > 2_000
            {
                return Err(mlua::Error::runtime("invalid notification payload"));
            }
            if !matches!(level.as_str(), "info" | "success" | "warning" | "error" | "update") {
                return Err(mlua::Error::runtime("invalid notification level"));
            }
            if action_url.as_deref().is_some_and(|url| !url.starts_with("https://")) {
                return Err(mlua::Error::runtime("notification action_url must use HTTPS"));
            }
            let namespaced_id = format!("{mod_id}.{id}");
            let payload = json!({
                "id": namespaced_id,
                "modId": mod_id,
                "title": title,
                "message": message,
                "level": level,
                "actionUrl": action_url,
                "updatedAt": SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs()
            });
            shroudforge_news::write_event(&game_dir, &format!("{mod_id}-{id}"), &payload).map_err(mlua::Error::external)?;
            Ok(())
        })?,
    )?;
    table.raw_set("notifications", notifications)?;

    Ok(table)
}

pub(super) fn action_registry_key(mod_id: &str, action: &str) -> String {
    format!("shroudforge.ui.action.{mod_id}.{action}")
}

pub(super) fn valid_identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 80
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'_'))
}
