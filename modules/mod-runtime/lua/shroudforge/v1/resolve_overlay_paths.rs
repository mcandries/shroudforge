use std::collections::HashMap;
use std::sync::Mutex;

use mod_loader::Mod;

/// Zones name their images and fonts relative to the mod folder; the overlay
/// layer reads them from disk, so each becomes an absolute path that must stay
/// inside that folder. A path resolved before is taken from `known`: resolving
/// asks the disk (the path made canonical, then checked for a link), and a
/// zone names its fonts and images again at every set.
pub(crate) fn resolve_overlay_paths(
    zone: &mut serde_json::Value,
    r#mod: &Mod,
    known: &Mutex<HashMap<String, String>>,
) -> Result<(), String> {
    resolve_with(zone, &|path| {
        remembered(known, path, |path| {
            if path.is_empty() || path.starts_with(['/', '\\']) || path.contains(':') {
                return Err(format!("{path}: use a path relative to the mod folder"));
            }
            match r#mod.fs().absolute_path(path) {
                Ok(Some(absolute)) => Ok(absolute.into_string()),
                Ok(None) => Err(format!("{path}: overlay files need a mod folder, not a zip package")),
                Err(error) => Err(format!("{path}: {error}")),
            }
        })
    })
}

/// `resolve(path)`, kept in `known` when it succeeds (a refused path is asked
/// again next time: its file may appear).
fn remembered(
    known: &Mutex<HashMap<String, String>>,
    path: &str,
    resolve: impl Fn(&str) -> Result<String, String>,
) -> Result<String, String> {
    if let Some(found) = known.lock().ok().and_then(|known| known.get(path).cloned()) {
        return Ok(found);
    }
    let resolved = resolve(path)?;
    if let Ok(mut known) = known.lock() {
        known.insert(path.to_owned(), resolved.clone());
    }
    Ok(resolved)
}

/// Rewrites every string `image` and `font` field, at any depth.
fn resolve_with(value: &mut serde_json::Value, resolve: &dyn Fn(&str) -> Result<String, String>) -> Result<(), String> {
    match value {
        serde_json::Value::Object(fields) => {
            for (key, field) in fields.iter_mut() {
                if let (true, serde_json::Value::String(path)) = (matches!(key.as_str(), "image" | "font"), &*field) {
                    *field = resolve(path)?.into();
                } else {
                    resolve_with(field, resolve)?;
                }
            }
        }
        serde_json::Value::Array(items) => {
            for item in items {
                resolve_with(item, resolve)?;
            }
        }
        _ => {}
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use std::cell::Cell;
    use std::collections::HashMap;
    use std::sync::Mutex;

    use super::{remembered, resolve_with};

    #[test]
    fn a_resolved_path_is_kept_a_refused_one_asked_again() {
        let known = Mutex::new(HashMap::new());
        let calls = Cell::new(0);
        let resolve = |path: &str| {
            calls.set(calls.get() + 1);
            if path == "missing.png" { Err(format!("{path}: missing")) } else { Ok(format!("C:/mods/test/{path}")) }
        };
        assert_eq!(remembered(&known, "fonts/a.ttf", resolve).unwrap(), "C:/mods/test/fonts/a.ttf");
        assert_eq!(remembered(&known, "fonts/a.ttf", resolve).unwrap(), "C:/mods/test/fonts/a.ttf");
        assert_eq!(calls.get(), 1);
        assert!(remembered(&known, "missing.png", resolve).is_err());
        assert!(remembered(&known, "missing.png", resolve).is_err());
        assert_eq!(calls.get(), 3);
    }

    #[test]
    fn image_and_font_fields_are_resolved_at_any_depth() {
        let mut zone = serde_json::json!({
            "frame": {"image": "ui/frame.png"},
            "text": {"font": "fonts/title.ttf", "size": 20},
            "items": ["image", {"type": "row", "items": [{"type": "image", "image": "icons/a.png"}]}],
        });
        resolve_with(&mut zone, &|path| Ok(format!("C:/mods/test/{path}"))).unwrap();
        assert_eq!(zone["frame"]["image"], "C:/mods/test/ui/frame.png");
        assert_eq!(zone["text"]["font"], "C:/mods/test/fonts/title.ttf");
        assert_eq!(zone["items"][0], "image");
        assert_eq!(zone["items"][1]["items"][0]["image"], "C:/mods/test/icons/a.png");
    }

    #[test]
    fn a_refused_path_stops_the_zone() {
        let mut zone = serde_json::json!({"items": [{"type": "image", "image": "../other/secret.png"}]});
        let error = resolve_with(&mut zone, &|path| Err(format!("{path}: outside"))).unwrap_err();
        assert_eq!(error, "../other/secret.png: outside");
    }
}
