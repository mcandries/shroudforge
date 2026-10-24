use std::path::Path;

/// Language the player chose for ShroudForge (`general.locale`, such as "en",
/// "de" or "fr"), so mods can speak it; "en" when the setting is unreadable.
pub(crate) fn read_ui_locale(game_dir: &Path) -> String {
    shroudforge_loader_config::config::read_loader(game_dir)
        .ok()
        .and_then(|config| config["general"]["locale"].as_str().map(str::to_owned))
        .filter(|locale| !locale.is_empty())
        .unwrap_or_else(|| "en".to_owned())
}
