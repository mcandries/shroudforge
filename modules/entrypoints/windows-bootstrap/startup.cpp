#include "bootstrap_state.h"

using namespace BootstrapImplementation;


namespace BootstrapImplementation {
HMODULE self_module{};

HANDLE stop_event{};

HANDLE runtime_thread{};

std::filesystem::path module_directory() {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(self_module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length == path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

bool is_dedicated_server_process() {
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) return false;
    const std::wstring_view executable(path, length);
    const auto separator = executable.find_last_of(L"\\/");
    const auto name = separator == std::wstring_view::npos
        ? executable
        : executable.substr(separator + 1);
    return CompareStringOrdinal(name.data(), static_cast<int>(name.size()),
        L"enshrouded_server.exe", -1, TRUE) == CSTR_EQUAL;
}

std::filesystem::path shroudforge_directory(const std::filesystem::path& root) {
    return root / L"shroudforge";
}

void startup_failed(const std::filesystem::path& root, const char* stage,
        const std::string& detail) {
    log('E', std::string("Startup failed at ") + stage + ": " + detail);
    if (!is_dedicated_server_process()) show_startup_error(root, detail);
}

bool is_supported_game_process() {
    wchar_t executable_path[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable_path,
        static_cast<DWORD>(sizeof(executable_path) / sizeof(executable_path[0])));
    if (length == 0 || length >= sizeof(executable_path) / sizeof(executable_path[0])) return false;
    const std::wstring_view path(executable_path, length);
    const auto separator = path.find_last_of(L"\\/");
    const auto name = separator == std::wstring_view::npos ? path : path.substr(separator + 1);
    return CompareStringOrdinal(name.data(), static_cast<int>(name.size()),
        L"enshrouded.exe", -1, TRUE) == CSTR_EQUAL ||
        CompareStringOrdinal(name.data(), static_cast<int>(name.size()),
        L"enshrouded_server.exe", -1, TRUE) == CSTR_EQUAL;
}

void append_environment(const wchar_t* name, const std::wstring& value) {
    std::wstring current(32767, L'\0');
    const auto length = GetEnvironmentVariableW(name, current.data(), static_cast<DWORD>(current.size()));
    current.resize(length < current.size() ? length : 0);
    SetEnvironmentVariableW(name, current.empty() ? value.c_str() : (current + L";" + value).c_str());
}

// The game overlay is a Vulkan layer shipped in shroudforge\overlay. It must
// be enabled before Enshrouded creates its Vulkan instance, so the loader is
// pointed at it from here rather than from the runtime thread.
void enable_game_overlay() {
    const auto overlay = shroudforge_directory(module_directory()) / L"overlay";
    if (GetFileAttributesW((overlay / L"shroudforge_overlay.json").c_str()) == INVALID_FILE_ATTRIBUTES) return;
    wchar_t flag[8]{};
    if (GetEnvironmentVariableW(L"SHROUDFORGE_OVERLAY", flag, 8) && flag[0] == L'0') return;
    append_environment(L"VK_ADD_LAYER_PATH", overlay.wstring());
    append_environment(L"VK_INSTANCE_LAYERS", L"VK_LAYER_SHROUDFORGE_overlay");
}
}

extern "C" __declspec(dllexport) BOOL __cdecl ShroudforgeStop(DWORD timeout_milliseconds) {
    if (stop_event) SetEvent(stop_event);
    if (!runtime_thread) return TRUE;
    return WaitForSingleObject(runtime_thread, timeout_milliseconds) == WAIT_OBJECT_0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        self_module = module;
        DisableThreadLibraryCalls(module);
        // winmm.dll is found beside every executable in the game directory.
        // The bundled ShroudForge UI also runs there, but must never bootstrap
        // another runtime or race Enshrouded's asset startup.
        if (!is_supported_game_process()) return TRUE;
        if (!is_dedicated_server_process()) enable_game_overlay();
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_event) return FALSE;
        if (!StartupGate::Install()) return FALSE;
        runtime_thread = CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
        return runtime_thread != nullptr;
    }
    if (reason == DLL_PROCESS_DETACH && reserved == nullptr && stop_event) {
        SetEvent(stop_event);
    }
    return TRUE;
}
