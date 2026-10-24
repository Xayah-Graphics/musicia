#include <ole2.h>
#include <shellapi.h>
#include <windows.h>

import std;
import editor.window;

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto initialized = OleInitialize(nullptr);
    if (FAILED(initialized)) return 1;
    int result = 0;
    try {
        editor::Window window;
        int argc{};
        auto* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::filesystem::path path;
        if (argc > 1) path = argv[1];
        LocalFree(argv);
        if (!path.empty()) window.load(std::move(path));
        MSG message{};
        bool running = true;
        while (running) {
            for (int count = 0; count < 128 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
                if (message.message == WM_QUIT) {
                    running = false;
                    break;
                }
                if (message.message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0 && (message.wParam == 'W' || message.wParam == 'Q')) {
                    SendMessageW(window.handle, WM_CLOSE, 0, 0);
                    continue;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (running && window.frames.wait(window.handle)) SendMessageW(window.handle, WM_APP + 6, 0, 0);
        }
        if (!window.fatal_error.empty()) throw std::runtime_error(window.fatal_error);
        result = static_cast<int>(message.wParam);
    } catch (const std::exception& exception) {
        const std::string message = exception.what();
        const std::wstring wide(message.begin(), message.end());
        MessageBoxW(nullptr, wide.c_str(), L"Musicia — Error", MB_OK | MB_ICONERROR);
        result = 1;
    }
    OleUninitialize();
    return result;
}
