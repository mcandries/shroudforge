use mod_loader::Mod;

/// Zones name their images and fonts relative to the mod folder; the overlay
/// layer reads them from disk, so each becomes an absolute path that must stay
/// inside that folder.
pub(crate) fn resolve_overlay_paths(zone: &mut serde_json::Value, r#mod: &Mod) -> Result<(), String> {
    resolve_with(zone, &|path| {
        if path.is_empty() || path.starts_with(['/', '\\']) || path.contains(':') {
            return Err(format!("{path}: use a path relative to the mod folder"));
        }
        match r#mod.fs().absolute_path(path) {
            Ok(Some(absolute)) => Ok(absolute.into_string()),
            Ok(None) => Err(format!("{path}: overlay files need a mod folder, not a zip package")),
            Err(error) => Err(format!("{path}: {error}")),
        }
    })
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
    use super::resolve_with;

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
