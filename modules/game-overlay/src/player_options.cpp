// Player options of the overlay, edited in F9 (Settings > Modules > Game
// overlay) and saved in modules.gameOverlay of modloader-config.json. The
// layer reads them itself, so changes apply while the game runs; the
// bootstrap only reads `enabled` at startup to load the layer or not.

#include "overlay.h"
#include "scene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <utility>

namespace overlay {

namespace {

constexpr ULONGLONG kCheckMilliseconds = 1000;

// Same names as game_keys.rs, where mods read their keys.
const std::pair<const char*, int> kNamedKeys[] = {
    {"backspace", 0x08}, {"tab", 0x09}, {"enter", 0x0D}, {"pause", 0x13}, {"capslock", 0x14},
    {"escape", 0x1B}, {"esc", 0x1B}, {"space", 0x20}, {"pageup", 0x21}, {"pagedown", 0x22}, {"end", 0x23},
    {"home", 0x24}, {"left", 0x25}, {"up", 0x26}, {"right", 0x27}, {"down", 0x28}, {"insert", 0x2D},
    {"delete", 0x2E}, {"numpadmultiply", 0x6A}, {"numpadadd", 0x6B}, {"numpadsubtract", 0x6D},
    {"numpaddecimal", 0x6E}, {"numpaddivide", 0x6F}, {"semicolon", 0xBA}, {"equals", 0xBB},
    {"comma", 0xBC}, {"minus", 0xBD}, {"period", 0xBE}, {"slash", 0xBF}, {"backquote", 0xC0},
    {"leftbracket", 0xDB}, {"backslash", 0xDC}, {"rightbracket", 0xDD}, {"quote", 0xDE},
    {"mousemiddle", 0x04}, {"mouse4", 0x05}, {"mouse5", 0x06},
};

int key_code(const std::string& name) {
    if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0])))
        return std::toupper(static_cast<unsigned char>(name[0]));
    if (name.size() >= 2 && name[0] == 'f' && std::all_of(name.begin() + 1, name.end(), ::isdigit)) {
        const int number = std::stoi(name.substr(1));
        if (number >= 1 && number <= 24) return VK_F1 + number - 1;
    }
    if (name.size() == 7 && name.rfind("numpad", 0) == 0 && std::isdigit(static_cast<unsigned char>(name[6])))
        return VK_NUMPAD0 + (name[6] - '0');
    for (const auto& [known, code] : kNamedKeys)
        if (name == known) return code;
    return 0;
}

bool down(int key) {
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

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
        const auto hide_key = section.value("hideKey", nlohmann::json(""));
        const std::string spec = hide_key.is_string() ? hide_key.get<std::string>() : "";
        options.hide_key = parse_key_combo(spec);
        set_options(options);
        log_line("player options: %s, size %d%%, opacity %d%%, show/hide key '%s'%s", options.enabled ? "on" : "off",
            static_cast<int>(options.scale * 100.0f + 0.5f), static_cast<int>(options.opacity * 100.0f + 0.5f),
            spec.c_str(), !spec.empty() && !options.hide_key.key ? " (not recognized)" : "");
    } catch (...) {
        // Being saved, or invalid: keep the previous options.
    }
}

bool game_has_focus() {
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}

// Diagnostics, to find a signal for the game's menus: logs when the mouse
// cursor is shown or hidden, or stops being confined.
void log_cursor_changes() {
    static int last = -1;
    CURSORINFO cursor{sizeof(cursor)};
    RECT clip{};
    if (!GetCursorInfo(&cursor) || !GetClipCursor(&clip)) return;
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const bool confined = clip.right - clip.left < width || clip.bottom - clip.top < height;
    const int state = ((cursor.flags & CURSOR_SHOWING) ? 1 : 0) | (confined ? 2 : 0) | (game_has_focus() ? 4 : 0);
    if (state == last) return;
    last = state;
    log_line("mouse cursor %s, %s, game %s", (state & 1) ? "shown" : "hidden", (state & 2) ? "confined" : "free",
        (state & 4) ? "focused" : "not focused");
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
    log_cursor_changes();
    const KeyCombo combo = current_options().hide_key;
    if (!combo.key) return;
    // Modifiers must match exactly, as for mod keys: F12 and Shift+F12 differ.
    const bool pressed = down(combo.key) && down(VK_CONTROL) == combo.ctrl && down(VK_MENU) == combo.alt &&
        down(VK_SHIFT) == combo.shift;
    if (pressed && !key_was_down && game_has_focus()) {
        set_hidden(!is_hidden());
        log_line("overlay %s by the player", is_hidden() ? "hidden" : "shown");
    }
    key_was_down = pressed;
}

KeyCombo parse_key_combo(const std::string& spec) {
    KeyCombo combo;
    std::string part;
    std::string lower;
    for (char c : spec) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t start = 0;
    while (start <= lower.size()) {
        const size_t end = lower.find('+', start);
        part = lower.substr(start, end == std::string::npos ? std::string::npos : end - start);
        part.erase(std::remove_if(part.begin(), part.end(), ::isspace), part.end());
        const bool last = end == std::string::npos;
        if (!last && (part == "ctrl" || part == "control")) combo.ctrl = true;
        else if (!last && part == "alt") combo.alt = true;
        else if (!last && part == "shift") combo.shift = true;
        else if (last) combo.key = key_code(part);
        else return {};
        if (last) break;
        start = end + 1;
    }
    return combo.key ? combo : KeyCombo{};
}

}  // namespace overlay
