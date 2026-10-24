// Player options of the overlay, edited in F9 (Settings > Modules > Game
// overlay) and saved in modules.gameOverlay of modloader-config.json. The
// layer reads them itself, so changes apply while the game runs; the
// bootstrap only reads `enabled` at startup to load the layer or not.

#include "overlay.h"
#include "scene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace overlay {

namespace {

constexpr ULONGLONG kCheckMilliseconds = 1000;

ULONGLONG last_check = 0;
FILETIME last_write{};
bool key_was_down = false;

// <game>\shroudforge\config\modloader-config.json, next to the layer folder.
std::wstring config_path() {
    wchar_t path[MAX_PATH]{};
    HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&config_path), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    // From the DLL path, drop the file name, then the overlay folder.
    std::wstring folder(path);
    folder.erase(folder.find_last_of(L'\\'));
    folder.erase(folder.find_last_of(L'\\') + 1);
    return folder + L"config\\modloader-config.json";
}

void read_options(const std::wstring& path) {
    try {
        std::ifstream file(path, std::ios::binary);
        if (!file) return;
        const auto config = nlohmann::json::parse(file);
        const auto section = config.value("modules", nlohmann::json::object()).value("gameOverlay", nlohmann::json::object());
        Options options;
        options.enabled = section.value("enabled", true);
        options.scale = std::clamp(section.value("scale", 100), 25, 400) / 100.0f;
        options.opacity = std::clamp(section.value("opacity", 100), 0, 100) / 100.0f;
        options.hide_key = std::clamp(section.value("hideKey", 0), 0, 255);
        set_options(options);
        log_line("player options: %s, size %d%%, opacity %d%%, show/hide key %d", options.enabled ? "on" : "off",
            static_cast<int>(options.scale * 100.0f + 0.5f), static_cast<int>(options.opacity * 100.0f + 0.5f),
            options.hide_key);
    } catch (...) {
        // Being saved, or invalid: keep the previous options.
    }
}

bool game_has_focus() {
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}

}  // namespace

void update_player_options() {
    const ULONGLONG now = GetTickCount64();
    if (now - last_check >= kCheckMilliseconds) {
        last_check = now;
        static const std::wstring path = config_path();
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) &&
                CompareFileTime(&attributes.ftLastWriteTime, &last_write) != 0) {
            last_write = attributes.ftLastWriteTime;
            read_options(path);
        }
    }
    const int key = current_options().hide_key;
    if (!key) return;
    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    if (down && !key_was_down && game_has_focus()) {
        set_hidden(!is_hidden());
        log_line("overlay %s by the player", is_hidden() ? "hidden" : "shown");
    }
    key_was_down = down;
}

}  // namespace overlay
