use super::apply_language_change::apply_language_change;
use super::apply_loader_setting::apply_loader_setting;
use super::apply_loader_settings::apply_loader_settings;
use super::apply_mod_enabled::apply_mod_enabled;
use super::apply_mod_settings::apply_mod_settings;
use super::change_notice_read_state::change_notice_read_state;
use super::change_storage_folder::change_storage_folder;
use super::copy_activity_log::copy_activity_log;
use super::invoke_updater_request::{UpdaterRequestResult, spawn as spawn_updater_request};
use super::log_configuration_changes::log_configuration_check_transitions;
use super::log_player_name_detection::log_player_name_detection;
use super::open_requested_folder::open_requested_folder;
use super::open_requested_link::open_requested_link;
use super::parse_ui_command::Command;
use super::parse_ui_command::UiCommand;
use super::parse_ui_command::parse_ui_command;
use super::read_host_arguments::Arguments;
use super::read_host_arguments::arguments;
use super::read_installed_mods::read_mods;
use super::read_missing_updater_announcement;
use super::read_release_status::idle_release_status;
use super::read_ui_configuration::default_refresh;
use super::read_ui_configuration::default_toggle_key;
use super::read_ui_configuration::effective_provider;
use super::read_ui_configuration::live_settings_revision;
use super::read_ui_configuration::load_module_config;
use super::read_ui_configuration::load_user_config;
use super::read_ui_configuration::read_central_config;
use super::read_ui_snapshot::default_catalog_state;
use super::read_ui_snapshot::read_ui_snapshot;
use super::read_window_focus::foreground_is_shroudforge_window;
use super::read_window_focus::foreground_process;
use super::read_window_focus::open_event;
use super::refresh_activity_log::refresh_activity_log;
use super::refresh_installed_mod::refresh_installed_mod;
use super::reload_loader_settings::reload_loader_settings;
use super::report_command_result::report_command_result;
use super::request_diagnostics::request_diagnostics;
use super::request_mod_action::request_mod_action;
use super::request_module_visibility::request_module_visibility;
use super::ui_snapshot_types::ModuleConfig;
use super::{
    dispatch_control_actions::dispatch_control_actions,
    log_configuration_errors::log_configuration_errors, watch_toggle_key::watch_toggle_key,
};
use shroudforge_logging::write_activity;
use shroudforge_window_management::WindowDefaults;
use shroudforge_window_management::apply_window_geometry;
use shroudforge_window_management::current_window_geometry;
use shroudforge_window_management::save_window_geometry;
use std::borrow::Cow;
use std::fs;
use std::process::Child;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::atomic::AtomicU32;
use std::sync::atomic::Ordering;
use std::sync::mpsc;
use std::time::Duration;
use std::time::Instant;
use tao::dpi::LogicalSize;
use tao::event::Event;
use tao::event::StartCause;
use tao::event::WindowEvent;
use tao::event_loop::ControlFlow;
use tao::event_loop::EventLoopBuilder;
use tao::platform::windows::WindowBuilderExtWindows;
use tao::window::WindowBuilder;
use windows_sys::Win32::Foundation::CloseHandle;
use windows_sys::Win32::Foundation::WAIT_OBJECT_0;
use windows_sys::Win32::System::Threading::GetCurrentProcessId;
use windows_sys::Win32::System::Threading::WaitForSingleObject;
use wry::WebContext;
use wry::WebViewBuilder;
use wry::WebViewBuilderExtWindows;

pub(crate) const DEFAULT_TOGGLE_KEY: u32 = 120; // F9

#[derive(Clone, Copy)]
pub(crate) enum UiEvent {
    Toggle,
}

pub(crate) fn run() -> Result<(), Box<dyn std::error::Error>> {
    let arguments = arguments()?;
    shroudforge_loader_config::config::initialize_loader_config(&arguments.root)?;
    run_with_arguments(arguments)
}

pub(crate) fn run_with_arguments(arguments: Arguments) -> Result<(), Box<dyn std::error::Error>> {
    if !arguments.desktop
        && read_central_config(&arguments.root)["modules"]["modloaderUi"]["enabled"] == false
    {
        return Ok(());
    }
    let mut module_config = load_module_config(&arguments.root).unwrap_or_else(|_| ModuleConfig {
        toggle_key: default_toggle_key(),
        refresh_milliseconds: default_refresh(),
    });
    let user_config = load_user_config(&arguments.root);
    let provider = effective_provider(&user_config);
    let release_state = Arc::new(Mutex::new(idle_release_status(&arguments.root)));
    let release_announcement = Arc::new(Mutex::new(
        read_missing_updater_announcement::read_cached(&arguments.root),
    ));
    let announcement_config = read_central_config(&arguments.root);
    if !arguments.server {
        read_missing_updater_announcement::start_refresh(
            &arguments.root,
            &announcement_config,
            release_announcement.clone(),
        );
    }
    let catalog_state = Arc::new(Mutex::new(default_catalog_state()));
    let stop_event = arguments
        .stop_name
        .as_deref()
        .map(open_event)
        .unwrap_or(std::ptr::null_mut());
    let event_loop = EventLoopBuilder::<UiEvent>::with_user_event().build();
    let modloader_height = event_loop
        .primary_monitor()
        .map(|monitor| {
            ((monitor.size().height as f64 / monitor.scale_factor()) * 0.92)
                .min(988.0)
                .max(560.0)
        })
        .unwrap_or(988.0);
    let (icon_rgba, icon_width, icon_height) = shroudforge_ui_resources::rgba()?;
    let icon = tao::window::Icon::from_rgba(icon_rgba, icon_width, icon_height)?;
    let title = "ShroudForge | Modloader";
    let (inner_width, inner_height) = (1180.0, modloader_height);
    let window_defaults = WindowDefaults {
        width: inner_width,
        height: inner_height,
    };
    let window = WindowBuilder::new()
        .with_window_icon(Some(icon.clone()))
        .with_taskbar_icon(Some(icon))
        .with_title(title)
        .with_decorations(false)
        .with_always_on_top(true)
        .with_resizable(true)
        .with_visible(arguments.desktop)
        .with_inner_size(LogicalSize::new(inner_width, inner_height))
        .build(&event_loop)?;

    let window_config_module = "modloaderUi";
    let mut saved_position = read_central_config(&arguments.root)["modules"][window_config_module]
        ["window"]["position"]
        .clone();
    let mut effective_window_geometry =
        apply_window_geometry(&window, &saved_position, false, window_defaults);
    let (sender, receiver) = mpsc::channel();
    let hotkey_proxy = event_loop.create_proxy();
    let hotkey_key = Arc::new(AtomicU32::new(module_config.toggle_key));
    let hotkey_key_watcher = hotkey_key.clone();
    let game_pid = arguments.game_pid;
    let desktop = arguments.desktop;
    watch_toggle_key(hotkey_proxy, hotkey_key_watcher, game_pid, desktop);
    let control_sender = sender.clone();
    let (updater_request_sender, updater_request_receiver) =
        mpsc::channel::<UpdaterRequestResult>();
    let handler = move |message: wry::http::Request<String>| {
        let Ok(value) = serde_json::from_str::<UiCommand>(message.body()) else {
            return;
        };
        let Some(command) = parse_ui_command(value, arguments.desktop, arguments.server) else {
            return;
        };
        let _ = sender.send(command);
    };
    // WebView2 locks a user-data directory while a WebContext is alive.
    // The Modloader, Debug Console, and World Editor windows run side by
    // side, so each host must own a distinct profile directory.
    let profile_name = "modloader-ui";
    let profile =
        shroudforge_loader_config::paths::webview_profile(&arguments.root).join(profile_name);
    fs::create_dir_all(&profile)?;
    let mut web_context = WebContext::new(Some(profile));
    let html = include_str!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../build/ui/modloader/index.html"
    ));
    let webview = WebViewBuilder::new_with_web_context(&mut web_context)
        .with_custom_protocol("shroudforge".into(), move |_webview_id, request| {
            let (status, content_type, body) = if request.uri().path() == "/index.html" {
                (
                    wry::http::StatusCode::OK,
                    "text/html; charset=utf-8",
                    Cow::Borrowed(html.as_bytes()),
                )
            } else {
                (
                    wry::http::StatusCode::NOT_FOUND,
                    "text/plain; charset=utf-8",
                    Cow::Borrowed(&[][..]),
                )
            };
            wry::http::Response::builder()
                .status(status)
                .header(wry::http::header::CONTENT_TYPE, content_type)
                .body(body)
                .expect("Modloader UI response is valid")
        })
        .with_https_scheme(true)
        .with_url("shroudforge://localhost/index.html")
        .with_ipc_handler(handler)
        .build(&window)?;

    let mut shown = arguments.desktop;
    let mut previous_configuration_errors = std::collections::HashSet::<String>::new();
    let mut previous_player_name_detection = None;
    let mut previous_configuration_checks =
        std::collections::BTreeMap::<String, (String, String)>::new();
    let mut previous_asset_status = std::collections::BTreeMap::<String, (String, String)>::new();
    let mut previous_mod_statuses = std::collections::BTreeMap::<String, (String, String)>::new();
    let initial_window_state = shroudforge_loader_config::config::window_state(&arguments.root);
    let mut visibility_request_id = initial_window_state["modloaderUi"]["requestId"]
        .as_u64()
        .unwrap_or(0);
    let _ = shroudforge_loader_config::config::publish_window_visibility(
        &arguments.root,
        "modloaderUi",
        shown,
    );
    if read_central_config(&arguments.root)["modules"]["debugConsole"]["enabled"] == false {
        let _ = shroudforge_loader_config::config::publish_window_visibility(
            &arguments.root,
            "debugConsole",
            false,
        );
    }
    let mut next_refresh = Instant::now();
    // The background work of each refresh (status logging, news, player
    // name) always runs. Only its UI snapshot is skipped while the window is
    // hidden, and an unchanged snapshot is not pushed to the page again.
    let mut last_snapshot = String::new();
    let mut webview_visible = window.is_visible();
    let mut next_announcement_retry = Instant::now() + Duration::from_secs(30);
    let mut next_visibility_poll = Instant::now();
    let mut next_window_state_refresh = Instant::now();
    let mut next_control_poll = Instant::now();
    let mut active_provider = provider;
    let mut desktop_debug_console: Option<Child> = None;
    let mut recovered_control_actions = false;
    let mut loaded_settings_revision = live_settings_revision(&arguments.root);
    let mut pending_window_geometry_save: Option<Instant> = None;
    event_loop.run(move |event, _, control_flow| {
        *control_flow = ControlFlow::WaitUntil(Instant::now() + Duration::from_millis(50));
        match event {
            Event::NewEvents(StartCause::ResumeTimeReached { .. })
            | Event::NewEvents(StartCause::Init) => {
                if pending_window_geometry_save
                    .is_some_and(|deadline| Instant::now() >= deadline)
                {
                    pending_window_geometry_save = None;
                    if current_window_geometry(&window)
                        .is_some_and(|geometry| geometry != effective_window_geometry)
                    {
                        if let Ok(Some(geometry)) = save_window_geometry(
                            &arguments.root,
                            window_config_module,
                            &window,
                        ) {
                            saved_position = geometry.clone();
                            effective_window_geometry = geometry;
                        }
                    }
                }
                if desktop_debug_console
                    .as_mut()
                    .is_some_and(|child| child.try_wait().ok().flatten().is_some())
                {
                    desktop_debug_console = None;
                }
                if (!stop_event.is_null()
                    && unsafe { WaitForSingleObject(stop_event, 0) } == WAIT_OBJECT_0)
                    || (!arguments.game_process.is_null()
                        && unsafe { WaitForSingleObject(arguments.game_process, 0) }
                            == WAIT_OBJECT_0)
                {
                    let _ = shroudforge_loader_config::config::publish_window_visibility(
                        &arguments.root,
                        "modloaderUi",
                        false,
                    );
                    if !stop_event.is_null() {
                        unsafe { CloseHandle(stop_event) };
                    }
                    if !arguments.game_process.is_null() {
                        unsafe { CloseHandle(arguments.game_process) };
                    }
                    *control_flow = ControlFlow::Exit;
                    return;
                }
                if Instant::now() >= next_control_poll {
                    let current_settings_revision = live_settings_revision(&arguments.root);
                    if current_settings_revision != loaded_settings_revision {
                        if let Ok(updated_module_config) = load_module_config(&arguments.root) {
                            let toggle_key = updated_module_config.toggle_key;
                            active_provider =
                                effective_provider(&load_user_config(&arguments.root));
                            module_config = updated_module_config;
                            hotkey_key.store(toggle_key, Ordering::Relaxed);
                            if read_central_config(&arguments.root)["modules"]["debugConsole"]["enabled"] == false {
                                let _ = shroudforge_loader_config::config::publish_window_visibility(
                                    &arguments.root,
                                    "debugConsole",
                                    false,
                                );
                            }
                            if !arguments.desktop
                                && read_central_config(&arguments.root)["modules"]["modloaderUi"]["enabled"] == false
                            {
                                shown = false;
                                window.set_visible(false);
                                let _ = shroudforge_loader_config::config::publish_window_visibility(
                                    &arguments.root,
                                    "modloaderUi",
                                    false,
                                );
                                *control_flow = ControlFlow::Exit;
                                return;
                            }
                        }
                        loaded_settings_revision = current_settings_revision;
                    }
                    dispatch_control_actions(&arguments, &control_sender, &mut recovered_control_actions);
                    next_control_poll = Instant::now() + Duration::from_millis(250);
                }
                while let Ok(result) = updater_request_receiver.try_recv() {
                    if result.operation == "remove-mod" {
                        report_command_result(
                            &arguments.root,
                            &webview,
                            &result.request_id,
                            &result.subject,
                            "Remove mod",
                            result.result.map(|_| ()),
                            "mod.removedToast",
                            None,
                            false,
                        );
                        next_refresh = Instant::now();
                        continue;
                    }
                    if result.operation == "search-catalog" {
                        match result.result {
                            Ok(value) => {
                                let items = value["items"]
                                    .as_array()
                                    .map(|items| serde_json::Value::Array(items.clone()))
                                    .unwrap_or_else(|| serde_json::json!([]));
                                if let Ok(mut catalog) = catalog_state.lock() {
                                    catalog["state"] = "ready".into();
                                    catalog["query"] = result.subject.into();
                                    catalog["message"] = if items.as_array().is_some_and(Vec::is_empty) {
                                        "No matching ShroudForge mods found.".into()
                                    } else {
                                        serde_json::Value::Null
                                    };
                                    catalog["items"] = items;
                                }
                            }
                            Err(error) => {
                                let unavailable =
                                    super::report_command_result::is_module_unavailable_warning(&error);
                                if let Ok(mut catalog) = catalog_state.lock() {
                                    catalog["state"] = if unavailable { "idle" } else { "error" }.into();
                                    catalog["message"] = if unavailable {
                                        serde_json::Value::Null
                                    } else {
                                        error.clone().into()
                                    };
                                    catalog["items"] = serde_json::json!([]);
                                }
                                report_command_result(
                                    &arguments.root,
                                    &webview,
                                    &result.request_id,
                                    "ShroudEdit",
                                    "Search catalog",
                                    Err(error),
                                    "catalog.searchFailed",
                                    None,
                                    false,
                                );
                            }
                        }
                        next_refresh = Instant::now();
                        continue;
                    }
                    if result.operation == "check-system-update" {
                        if let Ok(mut release) = release_state.lock() {
                            match &result.result {
                                Ok(value) => {
                                    release["state"] = "idle".into();
                                    release["message"] = value["message"].clone();
                                }
                                Err(error) => {
                                    let unavailable = super::report_command_result::
                                        is_module_unavailable_warning(error);
                                    release["state"] =
                                        if unavailable { "idle" } else { "error" }.into();
                                    release["message"] = if unavailable {
                                        serde_json::Value::Null
                                    } else {
                                        error.clone().into()
                                    };
                                }
                            }
                        }
                    }
                    if result.operation.starts_with("recycle-bin-") {
                        let unavailable = result.result.as_ref().err().is_some_and(|error| {
                            super::report_command_result::is_module_unavailable_warning(error)
                        });
                        if unavailable {
                            if let Some(error) = result.result.as_ref().err() {
                                report_command_result(
                                    &arguments.root,
                                    &webview,
                                    &result.request_id,
                                    "Updates",
                                    "Recycle bin",
                                    Err(error.clone()),
                                    "recycleBin.error",
                                    None,
                                    false,
                                );
                            }
                        }
                        let payload = if unavailable {
                            serde_json::json!({
                                "requestId": result.request_id,
                                "action": result.operation,
                                "success": false,
                                "skipped": true
                            })
                        } else {
                            match result.result {
                                Ok(value) => serde_json::json!({
                                    "requestId": result.request_id,
                                    "action": result.operation,
                                    "success": true,
                                    "value": value
                                }),
                                Err(error) => serde_json::json!({
                                    "requestId": result.request_id,
                                    "action": result.operation,
                                    "success": false,
                                    "error": error
                                }),
                            }
                        };
                        if let Ok(payload) = serde_json::to_string(&payload) {
                            let _ = webview.evaluate_script(&format!(
                                "window.dispatchEvent(new CustomEvent('shroudforge-recycle-bin-result',{{detail:{payload}}}));"
                            ));
                        }
                        continue;
                    }
                    if result.request_id.is_empty() {
                        next_refresh = Instant::now();
                        continue;
                    }
                    let (default_source, action, default_message) =
                        match result.operation.as_str() {
                            "check-system-update" => (
                                "Updates",
                                "Check for updates",
                                "updates.check.upToDate",
                            ),
                            "check-mod-updates" => (
                                "Updates",
                                "Check mod updates",
                                "updates.mods.checked",
                            ),
                            "stage-system-update" => {
                                ("Updates", "Stage update", "updates.stagedQueued")
                            }
                            "enqueue-system-update" => (
                                "Updates",
                                "Add system update to queue",
                                "updates.queue.added",
                            ),
                            "queue-latest-installation" => (
                                "Updates",
                                "Queue latest ShroudForge installation",
                                "updater.installation.queued",
                            ),
                            "queue-uninstall" => (
                                "Updates",
                                "Queue ShroudForge uninstallation",
                                "updater.uninstall.queued",
                            ),
                            "select-update-queue-items" => (
                                "Updates",
                                "Change update selection",
                                "updates.queue.selectionChanged",
                            ),
                            "remove-update-queue-item" => (
                                "Updates",
                                "Remove update from queue",
                                "updates.queue.removed",
                            ),
                            "start-update-queue" => (
                                "Updates",
                                "Start update queue",
                                "updates.queue.started",
                            ),
                            "clear-update-queue" => {
                                ("Updates", "Clear update queue", "updates.queue.cleared")
                            },
                            "cancel-update" => ("Updates", "Cancel update", "updates.cancelled"),
                            "install-mod" => (
                                "ShroudEdit",
                                "Queue mod install",
                                "mod.install.queued",
                            ),
                            "update-mod" => ("ShroudEdit", "Update mod", "mod.update.queued"),
                            "restore-gamefiles" => (
                                "Backup Management",
                                "Restore Gamefiles",
                                "backups.restore.queued",
                            ),
                            "capture-verified-gamefile-originals" => (
                                "Backup Management",
                                "Capture verified original gamefiles",
                                "backups.originals.captured",
                            ),
                            _ => ("Updater", "Run updater request", "updates.queue.started"),
                        };
                    let success_message = result
                        .result
                        .as_ref()
                        .ok()
                        .and_then(|value| value["successMessage"].as_str())
                        .unwrap_or(default_message)
                        .to_owned();
                    let source = if matches!(
                        result.operation.as_str(),
                        "install-mod" | "update-mod"
                    ) && !result.subject.is_empty()
                    {
                        &result.subject
                    } else {
                        default_source
                    };
                    report_command_result(
                        &arguments.root,
                        &webview,
                        &result.request_id,
                        source,
                        action,
                        result.result.map(|_| ()),
                        &success_message,
                        None,
                        false,
                    );
                    next_refresh = Instant::now();
                }
                while let Ok(command) = receiver.try_recv() {
                    match command {
                        Command::MinimizeWindow => window.set_minimized(true),
                        Command::ToggleMaximizeWindow => {
                            window.set_maximized(!window.is_maximized());
                        }
                        Command::ResetWindowGeometry(module, component, request_id) => {
                            let result = shroudforge_window_management::reset_window_geometry(
                                &arguments.root,
                                &module,
                                &component,
                            );
                            let success_message = match component.as_str() {
                                "position" => "settings.modules.windowGeometry.positionResetSuccess",
                                "size" => "settings.modules.windowGeometry.sizeResetSuccess",
                                _ => "settings.modules.windowGeometry.positionResetSuccess",
                            };
                            let action = format!("Reset {component} for {module}");
                            report_command_result(
                                &arguments.root,
                                &webview,
                                &request_id,
                                "Settings",
                                &action,
                                result,
                                success_message,
                                None,
                                false,
                            );
                            next_refresh = Instant::now();
                        }
                        Command::Hide(request_id) => {
                            let result = shroudforge_loader_config::config::publish_window_visibility(
                                &arguments.root,
                                "modloaderUi",
                                false,
                            );
                            report_command_result(
                                &arguments.root,
                                &webview,
                                &request_id,
                                "Modloader UI",
                                "Hide window",
                                result,
                                "settings.visibilityRequested",
                                None,
                                false,
                            );
                            if arguments.desktop {
                                *control_flow = ControlFlow::Exit;
                            } else {
                                shown = false;
                                window.set_visible(false);
                            }
                        }
                        Command::Drag(request_id) => {
                            let result = window.drag_window().map_err(|error| error.to_string());
                            report_command_result(
                                &arguments.root,
                                &webview,
                                &request_id,
                                "Modloader UI",
                                "Move window",
                                result,
                                "settings.visibilityRequested",
                                None,
                                false,
                            );
                        }
                        Command::Refresh(request_id) => refresh_activity_log(
                            request_id,
                            &arguments,
                            &webview,
                            &mut next_refresh,
                        ),
                        Command::CopyLogs(request_id) => copy_activity_log(
                            request_id,
                            &arguments,
                            &webview,
                        ),
                        Command::RefreshMod(id, request_id) => refresh_installed_mod(
                            id,
                            request_id,
                            &arguments,
                            &webview,
                            &mut next_refresh,
                        ),
                        Command::ReloadSettings(request_id) => reload_loader_settings(
                            request_id,
                            &arguments,
                            &webview,
                            &mut next_refresh,
                        ),
                        Command::SaveLanguage(locale, request_id) => apply_language_change(
                            locale,
                            request_id,
                            &arguments,
                            &webview,
                            &mut next_refresh,
                        ),
                        Command::OpenUrl(url, request_id) => open_requested_link(
                            url,
                            request_id,
                            &arguments,
                            &webview,
                        ),
                        Command::SetModEnabled(id, enabled, revision, request_id) => apply_mod_enabled(
                            id,
                            enabled,
                            revision,
                            request_id,
                            &arguments,
                            &webview,
                            &mut next_refresh,
                        ),
                        Command::SaveModSettings(id, values, revision, request_id) => apply_mod_settings(
                            id,
                            values,
                            revision,
                            request_id,
                            &arguments,
                            &webview,
                            &mut next_refresh,
                        ),
                        Command::SaveSettings(settings, request_id) => apply_loader_settings(
                            settings,
                            request_id,
                            &arguments,
                            &webview,
                            &mut active_provider,
                            &mut next_refresh,
                        ),
                        Command::SaveSetting(scope, module, key, setting, request_id) => apply_loader_setting(
                            scope,
                            module,
                            key,
                            setting,
                            request_id,
                            &arguments,
                            &webview,
                            &mut next_refresh,
                        ),
                        Command::BrowsePath(key, request_id) => change_storage_folder(
                            key,
                            request_id,
                            &arguments,
                            &webview,
                            &mut next_refresh,
                        ),
                        Command::OpenPath(key, request_id) => open_requested_folder(
                            key,
                            request_id,
                            &arguments,
                            &webview,
                        ),
                        Command::SetWindowVisibility(module, visible, request_id) => request_module_visibility(
                            module,
                            visible,
                            request_id,
                            &arguments,
                            &webview,
                            &mut desktop_debug_console,
                            &mut next_refresh,
                        ),
                        Command::RunModAction(id, action, request_id) => request_mod_action(
                            id,
                            action,
                            request_id,
                            &arguments,
                            &webview,
                            &mut next_refresh,
                        ),
                        Command::UpdaterRequest(operation, subject, request_id, mut request) => {
                            if operation == "check-system-update" {
                                if let Ok(mut release) = release_state.lock() {
                                    release["state"] = "checking".into();
                                    release["message"] = "Checking for updates…".into();
                                }
                            }
                            if matches!(
                                request["command"].as_str(),
                                Some("search-catalog" | "install-mod" | "update-mod")
                            ) {
                                request["provider"] = active_provider.clone();
                            }
                            if request["command"] == "stage-system-update" {
                                let wait_for_game =
                                    request["waitForGame"].as_bool().unwrap_or(!arguments.desktop);
                                let wait_pid = if wait_for_game {
                                    shroudforge_loader_config::runtime_status::runtime_pid(
                                        &arguments.root,
                                    )
                                    .unwrap_or(arguments.game_pid)
                                } else {
                                    0
                                };
                                request["waitPid"] = serde_json::json!(wait_pid);
                            }
                            spawn_updater_request(
                                arguments.root.clone(),
                                operation,
                                subject,
                                request_id,
                                request,
                                &updater_request_sender,
                            );
                        }
                        Command::SetNewsRead(ids, read, request_id) => change_notice_read_state(
                            ids,
                            read,
                            request_id,
                            &arguments,
                            &webview,
                            &mut next_refresh,
                        ),
                        Command::Diagnostics(action, job_id, request_id) => request_diagnostics(
                            action,
                            job_id,
                            request_id,
                            &arguments,
                            &webview,
                            &mut next_refresh,
                        ),
                        Command::UiReady => write_activity(
                            &arguments.root,
                            "modloader-ui",
                            "WebView rendered",
                            "Ready",
                            None,
                            "debug",
                        ),
                        Command::UiError(message) => write_activity(
                            &arguments.root,
                            "modloader-ui",
                            "WebView JavaScript error",
                            "Failed",
                            Some(&message),
                            "error",
                        ),
                    }
                }
                if Instant::now() >= next_visibility_poll {
                    let window_state =
                        shroudforge_loader_config::config::window_state(&arguments.root);
                    let requested_id = window_state["modloaderUi"]["requestId"]
                        .as_u64()
                        .unwrap_or(0);
                    if requested_id != visibility_request_id {
                        visibility_request_id = requested_id;
                        shown = if arguments.server && !arguments.desktop {
                            false
                        } else {
                            window_state["modloaderUi"]["requestedVisible"]
                                .as_bool()
                                .unwrap_or(shown)
                        };
                        if shown {
                            window.set_minimized(false);
                        }
                        window.set_visible(shown);
                        if shown {
                            window.set_focus();
                        }
                        let _ = shroudforge_loader_config::config::publish_window_visibility(
                            &arguments.root,
                            "modloaderUi",
                            shown,
                        );
                    }
                    next_visibility_poll = Instant::now() + Duration::from_millis(100);
                }
                if Instant::now() >= next_window_state_refresh {
                    let windows = shroudforge_loader_config::config::window_state(&arguments.root);
                    if let Ok(payload) = serde_json::to_string(&windows) {
                        let _ = webview.evaluate_script(&format!(
                            "window.__shroudforgeWindowState({payload});"
                        ));
                    }
                    next_window_state_refresh = Instant::now() + Duration::from_millis(250);
                }
                let foreground = foreground_process();
                let game_focused = arguments.desktop || foreground == arguments.game_pid;
                let ui_focused = foreground == unsafe { GetCurrentProcessId() }
                    || foreground_is_shroudforge_window();
                if !arguments.desktop && shown && !game_focused && !ui_focused {
                    window.set_visible(false);
                } else if shown && (game_focused || ui_focused) && !window.is_visible() {
                    window.set_visible(true);
                }
                let visible = window.is_visible();
                if visible != webview_visible {
                    // Lets WebView2 suspend rendering while the window is hidden.
                    let _ = webview.set_visible(visible);
                    webview_visible = visible;
                    if visible {
                        next_refresh = Instant::now();
                    }
                }

                if Instant::now() >= next_refresh {
                    if Instant::now() >= next_announcement_retry {
                        if !arguments.server {
                            read_missing_updater_announcement::start_refresh(
                                &arguments.root,
                                &read_central_config(&arguments.root),
                                release_announcement.clone(),
                            );
                        }
                        next_announcement_retry = Instant::now() + Duration::from_secs(30);
                    }
                    let next_position = read_central_config(&arguments.root)["modules"]
                        [window_config_module]["window"]["position"]
                        .clone();
                    if next_position != saved_position {
                        pending_window_geometry_save = None;
                        effective_window_geometry = apply_window_geometry(
                            &window,
                            &next_position,
                            false,
                            window_defaults,
                        );
                        saved_position = next_position;
                    }
                    next_refresh = Instant::now()
                        + Duration::from_millis(module_config.refresh_milliseconds.clamp(250, 10_000));
                    let central = read_central_config(&arguments.root);
                    let mods = read_mods(&arguments.root);
                    let notification_mods: Vec<_> = mods.iter().map(|item| shroudforge_news::ModNotification {
                        id: &item.id, name: &item.name, version: &item.version, ecosystem: item.ecosystem, changelog: &item.changelog,
                    }).collect();
                    shroudforge_news::sync_mod_events(&arguments.root, &notification_mods);
                    let game_process_id =
                        (!arguments.desktop && !arguments.server).then_some(arguments.game_pid);
                    let player_name_detection =
                        shroudforge_player_name::detect_active_character_for_process(
                            &arguments.root,
                            game_process_id,
                        );
                    let (snapshot, resolved_player_name) = read_ui_snapshot(
                        &arguments,
                        &active_provider,
                        &release_state,
                        &release_announcement,
                        &catalog_state,
                        central,
                        mods,
                        &player_name_detection,
                    );
                    log_player_name_detection(
                        &arguments.root,
                        &player_name_detection,
                        &resolved_player_name,
                        &mut previous_player_name_detection,
                    );
                    log_configuration_check_transitions(
                        &arguments.root,
                        &snapshot.configuration,
                        &mut previous_configuration_checks,
                        &mut previous_asset_status,
                        &mut previous_mod_statuses,
                    );
                    log_configuration_errors(&arguments.root, &snapshot, &mut previous_configuration_errors);
                    if visible
                        && let Ok(payload) = serde_json::to_string(&snapshot)
                        && payload != last_snapshot
                        && webview
                            .evaluate_script(&format!("window.__shroudforgeUpdate({payload});"))
                            .is_ok()
                    {
                        last_snapshot = payload;
                    }
                }
            }
            Event::UserEvent(UiEvent::Toggle) => {
                if !arguments.server
                    && (arguments.desktop
                        || read_central_config(&arguments.root)["modules"]["modloaderUi"]["enabled"]
                            != false)
                {
                    shown = !shown;
                    if shown {
                        window.set_minimized(false);
                    }
                    window.set_visible(shown);
                    let _ = shroudforge_loader_config::config::publish_window_visibility(
                        &arguments.root,
                        "modloaderUi",
                        shown,
                    );
                    if shown {
                        next_refresh = Instant::now();
                        window.set_focus();
                    }
                }
            }
            Event::WindowEvent {
                event: WindowEvent::Moved(_),
                ..
            }
            | Event::WindowEvent {
                event: WindowEvent::Resized(_),
                ..
            } => {
                if !window.is_maximized() && !window.is_minimized() {
                    pending_window_geometry_save = current_window_geometry(&window)
                        .filter(|geometry| geometry != &effective_window_geometry)
                        .map(|_| Instant::now() + Duration::from_millis(250));
                }
            }
            Event::WindowEvent {
                event: WindowEvent::CloseRequested,
                ..
            } => {
                if arguments.desktop {
                    let _ = shroudforge_loader_config::config::publish_window_visibility(
                        &arguments.root,
                        "modloaderUi",
                        false,
                    );
                    *control_flow = ControlFlow::Exit;
                } else {
                    shown = false;
                    window.set_visible(false);
                    let _ = shroudforge_loader_config::config::publish_window_visibility(
                        &arguments.root,
                        "modloaderUi",
                        shown,
                    );
                }
            }
            Event::LoopDestroyed => {
                if current_window_geometry(&window)
                    .is_some_and(|geometry| geometry != effective_window_geometry)
                {
                    let _ = save_window_geometry(
                        &arguments.root,
                        window_config_module,
                        &window,
                    );
                }
            }
            _ => {}
        }
    });
}
