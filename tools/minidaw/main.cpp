#include <ole2.h>
#include <windows.h>
import std;
import minidaw.window;

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto existing = FindWindowW(L"MiniDAW.Editor", nullptr);
    if (existing) {
        PostMessageW(existing, WM_APP + 4, 0, 0);
        return 0;
    }
    if (FAILED(OleInitialize(nullptr))) return 1;
    int result{};
    try {
        minidaw::Window window;
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0 && (message.wParam == 'W' || message.wParam == 'Q')) {
                SendMessageW(window.handle, WM_CLOSE, 0, 0);
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!window.fatal_error.empty()) throw std::runtime_error(window.fatal_error);
        result = static_cast<int>(message.wParam);
    } catch (const std::exception& exception) {
        const std::string text = exception.what();
        const std::wstring wide(text.begin(), text.end());
        MessageBoxW(nullptr, wide.c_str(), L"MiniDAW — Error", MB_OK | MB_ICONERROR);
        result = 1;
    }
    OleUninitialize();
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(result));
    std::unreachable();
}
