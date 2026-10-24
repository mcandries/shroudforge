#include "overlay.h"

#include <cstdarg>
#include <cstdio>

namespace overlay {

// The environment that enables the layer is inherited by the processes the
// game starts (the ShroudForge UI and its WebView2 helpers, which probe
// Vulkan). The layer only draws and logs in the game itself.
bool is_game_process() {
    static const bool game = [] {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        const wchar_t* separator = wcsrchr(path, L'\\');
        return _wcsicmp(separator ? separator + 1 : path, L"enshrouded.exe") == 0;
    }();
    return game;
}

namespace {
std::mutex log_mutex;
}

// Appends to shroudforge_overlay.log beside the layer DLL.
void log_line(const char* format, ...) {
    if (!is_game_process()) return;
    std::lock_guard lock(log_mutex);
    static FILE* file = [] {
        wchar_t path[MAX_PATH]{};
        HMODULE self{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&log_line), &self);
        GetModuleFileNameW(self, path, MAX_PATH);
        wchar_t* dot = wcsrchr(path, L'.');
        if (dot) wcscpy_s(dot, MAX_PATH - (dot - path), L".log");
        FILE* opened = nullptr;
        _wfopen_s(&opened, path, L"a");
        return opened;
    }();
    if (!file) return;
    // UTC wall-clock time, like the ShroudForge log, so both can be read together.
    SYSTEMTIME now{};
    GetSystemTime(&now);
    fprintf(file, "[%lu %04u-%02u-%02u %02u:%02u:%02u,%03u UTC] ", GetCurrentProcessId(), now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    va_list arguments;
    va_start(arguments, format);
    vfprintf(file, format, arguments);
    va_end(arguments);
    fputc('\n', file);
    fflush(file);
}

// Diagnostics: the DLL that holds an address (a caller, or the next layer).
std::string module_name(const void* address) {
    HMODULE module{};
    if (!address || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(address), &module)) return "?";
    char path[MAX_PATH]{};
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* separator = strrchr(path, '\\');
    return separator ? separator + 1 : path;
}

}  // namespace overlay
