module;

#include <dwmapi.h>
#include <oleidl.h>
#include <shellapi.h>
#include <windows.h>
#include <windowsx.h>

module music.window;

import std;
import music.audio;
import music.renderer;

namespace music {
    std::wstring timeLabel(double seconds, bool precise) {
        const auto hundredths = static_cast<std::uint64_t>(std::max(0.0, seconds) * 100);
        if (precise) return std::format(L"{:02}:{:02}.{:02}", hundredths / 6000, hundredths / 100 % 60, hundredths % 100);
        return std::format(L"{:02}:{:02}", hundredths / 6000, hundredths / 100 % 60);
    }

    Window::Window() {
        const WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW), CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS, procedure, 0, 0, GetModuleHandleW(nullptr), LoadIconW(nullptr, IDI_APPLICATION), LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"MusicTimelineWindow", nullptr};
        if (!RegisterClassExW(&windowClass)) throw std::runtime_error("Unable to register the music window.");
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO monitor{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
        const float scale = GetDpiForSystem() / 96.f;
        const int w       = static_cast<int>(448 * scale);
        const int h       = static_cast<int>(274 * scale);
        handle            = CreateWindowExW(WS_EX_APPWINDOW, windowClass.lpszClassName, L"Music", WS_POPUP | WS_THICKFRAME, monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - w) / 2, monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - h) / 2, w, h, nullptr, nullptr, windowClass.hInstance, this);
        if (!handle) throw std::runtime_error("Unable to create the music window.");
        constexpr DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
        DwmSetWindowAttribute(handle, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
        constexpr BOOL dark = TRUE;
        DwmSetWindowAttribute(handle, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        constexpr COLORREF border = RGB(40, 43, 48);
        DwmSetWindowAttribute(handle, DWMWA_BORDER_COLOR, &border, sizeof(border));
        renderer.emplace(handle);
        renderer->scale       = GetDpiForWindow(handle) / 96.f;
        const auto registered = RegisterDragDrop(handle, this);
        if (FAILED(registered)) throw std::runtime_error("Unable to register file drop handling.");
        layout();
        ShowWindow(handle, SW_SHOW);
        UpdateWindow(handle);
    }

    Window::~Window() {
        audio.close();
        renderer.reset();
        if (handle) DestroyWindow(handle);
    }

    void Window::load(std::filesystem::path path) {
        if (loading) {
            pendingPath = std::move(path);
            return;
        }
        finishDrag();
        audio.close();
        loaded  = false;
        loading = true;
        error.clear();
        loadingPath = std::move(path);
        loadingClip = std::async(std::launch::async, [path = loadingPath] { return AudioClip::read(path); });
        if (!expanded) {
            expanded = true;
            RECT bounds{};
            GetWindowRect(handle, &bounds);
            MONITORINFO monitor{sizeof(MONITORINFO)};
            GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor);
            const LONG w = std::min(static_cast<LONG>(1060 * renderer->scale), monitor.rcWork.right - monitor.rcWork.left - 48);
            const LONG h = std::min(static_cast<LONG>(334 * renderer->scale), monitor.rcWork.bottom - monitor.rcWork.top - 48);
            const int x  = std::clamp((bounds.left + bounds.right - w) / 2, monitor.rcWork.left + 24, monitor.rcWork.right - w - 24);
            const int y  = std::clamp((bounds.top + bounds.bottom - h) / 2, monitor.rcWork.top + 24, monitor.rcWork.bottom - h - 24);
            SetWindowPos(handle, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        refresh();
    }

    void Window::layout() {
        RECT client{};
        GetClientRect(handle, &client);
        const auto scale = renderer ? renderer->scale : GetDpiForWindow(handle) / 96.f;
        width            = client.right / scale;
        height           = client.bottom / scale;
        waveform         = {32, 108, width - 64, height - 140};
        overview         = {32, height - 28, width - 64, 12};
    }

    void Window::refresh() {
        if (loading || audio.playing.load()) SetTimer(handle, 1, 16, nullptr);
        else KillTimer(handle, 1);
        InvalidateRect(handle, nullptr, FALSE);
    }

    void Window::render() {
        auto& draw = *renderer;
        draw.vertices.clear();
        draw.showWave  = loaded;
        draw.waveLayer = 0;
        if (!loaded) {
            const auto title = loading ? L"Loading audio..." : error.empty() ? L"Drop an audio file" : L"Unable to open audio";
            draw.text(title, 15, 32, height / 2 - 11, dropActive ? accent : foreground, width - 64, true);
            if (!error.empty()) draw.text(error, 12, 32, height / 2 + 20, {0.94f, 0.65f, 0.58f, 1}, width - 64, true);
        } else {
            const auto& clip        = audio.clip;
            const auto currentFrame = drag == Drag::seek ? dragFrame : static_cast<double>(audio.position());
            for (std::uint32_t channel = 0; channel < clip.channels; ++channel) draw.rect({waveform.x, waveform.y + waveform.height / clip.channels * (channel + .5f), waveform.width, 1 / draw.scale}, ruleColor);
            draw.waveLayer = static_cast<std::uint32_t>(draw.vertices.size());
            draw.waveRect  = waveform;
            draw.waveFirst = timeline.first;
            draw.waveCount = timeline.count;
            draw.text(clip.path.stem().wstring(), 14, 32, 30, foreground, width - 240);
            draw.text(timeLabel(currentFrame / clip.sampleRate, true), 12, width - 126, 31, foreground, 54);
            draw.text(L"/ " + timeLabel(static_cast<double>(clip.frames) / clip.sampleRate, false), 12, width - 69, 31, muted, 44);

            const auto visibleSeconds = timeline.count / clip.sampleRate;
            const auto desired        = visibleSeconds / std::max(2.f, waveform.width / 130);
            const auto magnitude      = std::pow(10.0, std::floor(std::log10(desired)));
            double tick               = magnitude;
            for (const auto factor : {1.0, 2.0, 5.0, 10.0})
                if (magnitude * factor >= desired) {
                    tick = magnitude * factor;
                    break;
                }
            const auto firstSeconds = timeline.first / clip.sampleRate;
            for (auto t = std::ceil(firstSeconds / tick) * tick; t <= firstSeconds + visibleSeconds; t += tick) {
                const auto x     = waveform.x + static_cast<float>((t - firstSeconds) / visibleSeconds * waveform.width);
                const auto label = tick < .001 ? std::format(L"{:.4f} s", t) : tick < .1 ? std::format(L"{:.3f} s", t) : tick < 1 ? std::format(L"{:.1f} s", t) : timeLabel(t, false);
                draw.text(label, 11, std::clamp(x - 12, waveform.x, waveform.x + waveform.width - 76), 76, muted, 76);
                draw.rect({std::round(x * draw.scale) / draw.scale, 96, 1 / draw.scale, 4}, ruleColor);
            }
            if (currentFrame >= timeline.first && currentFrame <= timeline.first + timeline.count) {
                const float x       = std::clamp(waveform.x + static_cast<float>((currentFrame - timeline.first) / timeline.count * waveform.width), waveform.x, waveform.x + waveform.width - 1 / draw.scale);
                const float snapped = std::round(x * draw.scale) / draw.scale;
                draw.rect({snapped, waveform.y - 4, 1 / draw.scale, waveform.height + 4}, accent);
                draw.rect({snapped - 1.5f, waveform.y - 6, 4, 4}, accent, 2);
            }
            if (timeline.count < clip.frames) {
                draw.rect({overview.x, overview.y + 5, overview.width, 1 / draw.scale}, ruleColor);
                const auto length = std::max(4.f, static_cast<float>(timeline.count / clip.frames * overview.width));
                const auto start  = std::min(overview.width - length, static_cast<float>(timeline.first / clip.frames * overview.width));
                draw.rect({overview.x + start, overview.y + 4, length, 2 / draw.scale}, muted);
            }
            if (dropActive) {
                draw.rect(waveform, {background.r, background.g, background.b, .9f});
                draw.text(L"Drop to replace audio", 15, waveform.x, waveform.y + waveform.height / 2 - 11, foreground, waveform.width, true);
            }
        }
        draw.draw();
    }

    void Window::moveDrag(float x) {
        if (drag == Drag::seek) {
            dragFrame = std::clamp(timeline.first + (x - waveform.x) / waveform.width * timeline.count, 0.0, static_cast<double>(audio.clip.frames));
            audio.seek(static_cast<std::uint64_t>(dragFrame));
        } else if (drag == Drag::view) timeline.first = std::clamp((x - overview.x) / overview.width * audio.clip.frames - dragViewOffset, 0.0, static_cast<double>(audio.clip.frames) - timeline.count);
        refresh();
    }

    void Window::finishDrag() {
        if (drag == Drag::seek) audio.playing = restorePlayback;
        drag = Drag::none;
        if (GetCapture() == handle) ReleaseCapture();
        if (handle) refresh();
    }

    LRESULT Window::message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_NCCALCSIZE: return 0;
        case WM_NCHITTEST:
            {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(handle, &point);
                const float scale = renderer ? renderer->scale : GetDpiForWindow(handle) / 96.f;
                const float x     = point.x / scale;
                const float y     = point.y / scale;
                const bool left = x < 6, right = x >= width - 6, top = y < 6, bottom = y >= height - 6;
                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
                if (top) return HTTOP;
                if (bottom) return HTBOTTOM;
                if (y < 64) return HTCAPTION;
                return HTCLIENT;
            }
        case WM_GETMINMAXINFO:
            {
                auto& info          = *reinterpret_cast<MINMAXINFO*>(lparam);
                const float scale   = renderer ? renderer->scale : GetDpiForWindow(handle) / 96.f;
                info.ptMinTrackSize = {static_cast<LONG>((expanded ? 620 : 380) * scale), static_cast<LONG>((expanded ? 260 : 220) * scale)};
                return 0;
            }
        case WM_SIZE:
            if (renderer && wparam != SIZE_MINIMIZED) {
                layout();
                renderer->resize();
                refresh();
            }
            return 0;
        case WM_DPICHANGED:
            {
                if (renderer) {
                    renderer->scale = HIWORD(wparam) / 96.f;
                    renderer->resetFonts();
                }
                const auto& rect = *reinterpret_cast<const RECT*>(lparam);
                SetWindowPos(handle, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOACTIVATE | SWP_NOZORDER);
                return 0;
            }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
            {
                PAINTSTRUCT paint{};
                BeginPaint(handle, &paint);
                EndPaint(handle, &paint);
                if (renderer && !IsIconic(handle)) render();
                return 0;
            }
        case WM_TIMER:
            if (loading && loadingClip.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto completed = std::move(loadingClip);
                loading        = false;
                if (!pendingPath.empty()) {
                    load(std::exchange(pendingPath, {}));
                    return 0;
                }
                try {
                    auto clip = completed.get();
                    renderer->upload(clip);
                    audio.open(std::move(clip));
                    timeline.fit(audio.clip.frames);
                    loaded = true;
                } catch (const std::exception& exception) {
                    audio.close();
                    const std::string detail = exception.what();
                    error.assign(detail.begin(), detail.end());
                    loaded = false;
                }
            }
            refresh();
            return 0;
        case WM_MOUSEMOVE:
            if (drag != Drag::none) moveDrag(GET_X_LPARAM(lparam) / renderer->scale);
            return 0;
        case WM_LBUTTONDOWN:
            if (loaded) {
                const auto x = GET_X_LPARAM(lparam) / renderer->scale;
                const auto y = GET_Y_LPARAM(lparam) / renderer->scale;
                SetFocus(handle);
                if (waveform.contains(x, y)) {
                    restorePlayback = audio.playing.exchange(false);
                    drag            = Drag::seek;
                    SetCapture(handle);
                    moveDrag(x);
                } else if (timeline.count < audio.clip.frames && overview.contains(x, y)) {
                    drag             = Drag::view;
                    const auto frame = (x - overview.x) / overview.width * audio.clip.frames;
                    dragViewOffset   = frame >= timeline.first && frame <= timeline.first + timeline.count ? frame - timeline.first : timeline.count / 2;
                    SetCapture(handle);
                    moveDrag(x);
                }
            }
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (drag != Drag::none) finishDrag();
            return 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (loaded) {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(handle, &point);
                const auto delta = GET_WHEEL_DELTA_WPARAM(wparam) / 120.0;
                if (GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL) timeline.zoom(std::pow(1.25, -delta), std::clamp((point.x / renderer->scale - waveform.x) / waveform.width, 0.f, 1.f), audio.clip.frames);
                else timeline.pan(timeline.count * delta * (message == WM_MOUSEHWHEEL ? .1 : -.1), audio.clip.frames);
                refresh();
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT && loaded) {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(handle, &point);
                const auto x = point.x / renderer->scale;
                const auto y = point.y / renderer->scale;
                if (waveform.contains(x, y)) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
            break;
        case WM_KEYDOWN:
            if (wparam == 'W' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                PostQuitMessage(0);
                return 0;
            }
            if (loaded && drag == Drag::none) {
                if (wparam == VK_SPACE && !(lparam & (1LL << 30))) {
                    if (audio.position() >= audio.clip.frames) audio.seek(0);
                    audio.playing = !audio.playing.load();
                    refresh();
                } else if (wparam == 'F') {
                    timeline.fit(audio.clip.frames);
                    refresh();
                } else if (wparam == VK_HOME) {
                    audio.seek(0);
                    refresh();
                } else if (wparam == VK_LEFT || wparam == VK_RIGHT) {
                    audio.seek(static_cast<std::uint64_t>(std::clamp(static_cast<double>(audio.position()) + (wparam == VK_LEFT ? -1 : 1) * static_cast<double>(audio.clip.sampleRate), 0.0, static_cast<double>(audio.clip.frames))));
                    refresh();
                }
            }
            return 0;
        case WM_SYSKEYDOWN:
            if (wparam == VK_F4) return 0;
            break;
        case WM_CLOSE: PostQuitMessage(0); return 0;
        case WM_DESTROY:
            RevokeDragDrop(handle);
            KillTimer(handle, 1);
            audio.playing = false;
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY:
            {
                const auto result = DefWindowProcW(handle, message, wparam, lparam);
                SetWindowLongPtrW(handle, GWLP_USERDATA, 0);
                handle = nullptr;
                return result;
            }
        }
        return DefWindowProcW(handle, message, wparam, lparam);
    }

    LRESULT CALLBACK Window::procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* app = reinterpret_cast<Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app         = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            app->handle = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (!app) return DefWindowProcW(window, message, wparam, lparam);
        try {
            return app->message(message, wparam, lparam);
        } catch (const std::exception& exception) {
            app->fatalError = exception.what();
            PostQuitMessage(1);
            return 0;
        }
    }

    HRESULT Window::QueryInterface(REFIID iid, void** object) {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDropTarget)) {
            *object = static_cast<IDropTarget*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG Window::AddRef() {
        return 1;
    }

    ULONG Window::Release() {
        return 1;
    }

    HRESULT Window::DragEnter(IDataObject* object, DWORD, POINTL, DWORD* effect) {
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        acceptsDrop = SUCCEEDED(object->QueryGetData(&format));
        dropActive  = acceptsDrop;
        *effect     = acceptsDrop ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        refresh();
        return S_OK;
    }

    HRESULT Window::DragOver(DWORD, POINTL, DWORD* effect) {
        *effect = acceptsDrop ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT Window::DragLeave() {
        dropActive = false;
        refresh();
        return S_OK;
    }

    HRESULT Window::Drop(IDataObject* object, DWORD, POINTL, DWORD* effect) {
        dropActive = false;
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        const auto result = object->GetData(&format, &medium);
        if (FAILED(result)) return result;
        const auto files = static_cast<HDROP>(medium.hGlobal);
        std::wstring path(DragQueryFileW(files, 0, nullptr, 0) + 1, L'\0');
        DragQueryFileW(files, 0, path.data(), static_cast<UINT>(path.size()));
        path.pop_back();
        ReleaseStgMedium(&medium);
        try {
            load(std::filesystem::path(path));
        } catch (const std::exception& exception) {
            fatalError = exception.what();
            PostQuitMessage(1);
            return E_FAIL;
        }
        *effect = DROPEFFECT_COPY;
        return S_OK;
    }
}
