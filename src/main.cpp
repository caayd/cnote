#include <windows.h>

#include "App.h"

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    ctrainer::App app(instance);
    return app.Run(showCommand);
}
