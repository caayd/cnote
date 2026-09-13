#include <windows.h>
#include <shellapi.h>

#include <optional>
#include <string>

#include "App.h"

namespace {

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif

void EnablePerMonitorDpiAwareness() {
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
    static const auto setProcessDpiAwarenessContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (setProcessDpiAwarenessContext == nullptr) {
        return;
    }

    if (setProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == FALSE) {
        static_cast<void>(setProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE));
    }
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    EnablePerMonitorDpiAwareness();

    std::optional<std::filesystem::path> startupPath;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr) {
        if (argc > 1 && argv[1] != nullptr && argv[1][0] != L'\0') {
            startupPath = argv[1];
        }
        LocalFree(argv);
    }

    cnote::App app(instance);
    return app.Run(showCommand, std::move(startupPath));
}
