#include <windows.h>
#include <shellapi.h>

#include <optional>
#include <string>

#include "App.h"

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    std::optional<std::wstring> startupPath;
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
