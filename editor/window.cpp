module;

#include <dwmapi.h>
#include <oleidl.h>
#include <shellapi.h>
#include <windows.h>
#include <windowsx.h>

module editor.window;

import std;
import music.audio;
import editor.renderer;
import editor.timeline;
import editor.playback;
import music.analysis;
import music.sheetsage2;

namespace editor {
    std::wstring timeLabel(double seconds, bool precise) {
        const auto hundredths = static_cast<std::uint64_t>(std::max(0.0, seconds) * 100);
        if (precise) return std::format(L"{:02}:{:02}.{:02}", hundredths / 6000, hundredths / 100 % 60, hundredths % 100);
        return std::format(L"{:02}:{:02}", hundredths / 6000, hundredths / 100 % 60);
    }

    std::wstring intervalLabel(std::string_view label, std::size_t lane) {
        if (label == "N") return L"—";
        std::wstring text(label.begin(), label.end());
        if (lane == 0) {
            text.front() = std::towupper(text.front());
            return text;
        }
        if (lane == 1) {
            std::ranges::replace(text, L':', L' ');
            std::ranges::replace(text, L'b', L'♭');
            std::ranges::replace(text, L'#', L'♯');
            return text;
        }
        const auto colon = label.find(':');
        const auto slash = label.find('/');
        const auto quality = label.substr(colon + 1, (slash == label.npos ? label.size() : slash) - colon - 1);
        static constexpr std::array<std::pair<std::string_view, std::wstring_view>, 15> qualities{{
            {"maj", L""}, {"min", L"m"}, {"dim", L"dim"}, {"aug", L"aug"}, {"maj7", L"maj7"},
            {"min7", L"m7"}, {"7", L"7"}, {"hdim7", L"m7♭5"}, {"dim7", L"dim7"},
            {"minmaj7", L"m(maj7)"}, {"sus2", L"sus2"}, {"sus4", L"sus4"}, {"sus4(b7)", L"7sus4"},
            {"maj6", L"6"}, {"min6", L"m6"}
        }};
        text.resize(colon);
        text += std::ranges::find(qualities, quality, &decltype(qualities)::value_type::first)->second;
        if (slash != label.npos) {
            constexpr std::string_view letters = "CDEFGAB";
            constexpr std::array<int, 7> natural{0, 2, 4, 5, 7, 9, 11};
            const auto root = letters.find(label.front());
            int pitch = natural[root];
            for (std::size_t i = 1; i < colon; ++i) pitch += label[i] == '#' ? 1 : -1;
            const auto inversion = label.substr(slash + 1);
            const int degree = inversion.back() - '1';
            pitch += natural[degree];
            for (std::size_t i = 0; i + 1 < inversion.size(); ++i) pitch += inversion[i] == '#' ? 1 : -1;
            const auto bass = (root + degree) % 7;
            const int accidental = (pitch - natural[bass] + 18) % 12 - 6;
            text += L'/';
            text += static_cast<wchar_t>(letters[bass]);
            text += std::wstring(std::abs(accidental), accidental < 0 ? L'♭' : L'♯');
        }
        std::ranges::replace(text, L'#', L'♯');
        std::ranges::replace(text, L'b', L'♭');
        return text;
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
        analysisStop.request_stop();
        if (analyzing.valid()) analyzing.wait();
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
        hovering = false;
        follow = true;
        analysisStop.request_stop();
        analysis.reset();
        analysisSamples.reset();
        analysisPending = false;
        analysisError.clear();
        audio.close();
        loaded  = false;
        loading = true;
        error.clear();
        loadingPath = std::move(path);
        loadingClip = std::async(std::launch::async, [path = loadingPath] { return music::AudioClip::read(path); });
        if (!expanded) {
            expanded = true;
            RECT bounds{};
            GetWindowRect(handle, &bounds);
            MONITORINFO monitor{sizeof(MONITORINFO)};
            GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor);
            const LONG w = std::min(static_cast<LONG>(1120 * renderer->scale), monitor.rcWork.right - monitor.rcWork.left - 48);
            const LONG h = std::min(static_cast<LONG>(560 * renderer->scale), monitor.rcWork.bottom - monitor.rcWork.top - 48);
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
        content = {84, 88, width - 116, height - 202};
        for (std::size_t lane = 0; lane < lanes.size(); ++lane) lanes[lane] = {content.x, 126 + 40.f * lane, content.width, 30};
        waveform = {content.x, 260, content.width, height - 374};
        overview = {content.x, height - 96, content.width, 48};
    }

    void Window::refresh() {
        if (loading || audio.playing.load() || analyzing.valid() || analysisPending) SetTimer(handle, 1, 16, nullptr);
        else KillTimer(handle, 1);
        InvalidateRect(handle, nullptr, FALSE);
    }

    void Window::render() {
        auto& draw = *renderer;
        draw.vertices.clear();
        draw.showWave = loaded;
        draw.waveLayer = 0;
        if (!loaded) {
            const auto title = loading ? L"Loading audio..." : error.empty() ? L"Drop an audio file" : L"Unable to open audio";
            draw.text(title, 15, 32, height / 2 - 11, dropActive ? accent : foreground, width - 64, true);
            if (!error.empty()) draw.text(error, 12, 32, height / 2 + 20, {.94f, .65f, .58f, 1}, width - 64, true);
        } else {
            const auto& clip = audio.clip;
            const double currentFrame = drag == Drag::seek ? dragFrame : static_cast<double>(audio.position());
            if (follow && audio.playing.load() && drag == Drag::none) timeline.first = std::clamp(currentFrame - timeline.count / 2, 0., static_cast<double>(clip.frames) - timeline.count);
            const double seconds = currentFrame / clip.sampleRate;
            const double duration = static_cast<double>(clip.frames) / clip.sampleRate;
            const double viewStart = timeline.first / clip.sampleRate;
            const double viewEnd = (timeline.first + timeline.count) / clip.sampleRate;
            const double pixels = content.width / (viewEnd - viewStart);
            const float hairline = 1 / draw.scale;
            const bool mainHover = hovering && content.contains(hoverX, hoverY);
            const bool overviewHover = hovering && overview.contains(hoverX, hoverY);
            const double hoverTime = mainHover ? timeline.frame((hoverX - content.x) / content.width) / clip.sampleRate : (hoverX - overview.x) / overview.width * duration;
            std::wstring hoverLabel;
            std::wstring hoverRange;
            std::wstring context;

            draw.text(clip.path.stem().wstring(), 15, 32, 28, foreground, width - 260);
            draw.text(timeLabel(seconds, true), 13, width - 161, 29, foreground, 75);
            draw.text(L"/ " + timeLabel(duration, false), 12, width - 80, 30, muted, 55);
            draw.text(L"TIME", 9, 32, content.y + 4, muted, 46);
            for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
                draw.text(std::array{L"FORM", L"KEY", L"CHORDS"}[lane], 9, 32, lanes[lane].y + 9, muted, 48);
                draw.rect(lanes[lane], {.075f, .082f, .094f, 1});
            }
            draw.rect({content.x, content.y + 30, content.width, hairline}, ruleColor);
            draw.rect(overview, {.075f, .082f, .094f, 1}, 3);
            for (std::uint32_t channel = 0; channel < clip.channels; ++channel) {
                const float center = waveform.y + waveform.height / clip.channels * (channel + .5f);
                draw.text(clip.channels == 1 ? L"M" : channel == 0 ? L"L" : L"R", 9, 51, center - 6, muted, 16);
                draw.rect({waveform.x, center, waveform.width, hairline}, {.11f, .12f, .14f, 1});
            }
            draw.text(L"ALL", 9, 32, overview.y + 18, muted, 46);

            if (analysis) {
                int bar = 0;
                const double spacing = 60 / analysis->bpm * pixels;
                const auto stride = std::max(1, static_cast<int>(std::ceil(28 / (spacing * 4))));
                for (const auto& beat : analysis->beats) {
                    if (beat.position == 1) ++bar;
                    if (beat.time < viewStart || beat.time > viewEnd) continue;
                    if (beat.position != 1 && spacing < 14) continue;
                    if (beat.position == 1 && bar % stride) continue;
                    const float x = static_cast<float>(std::round((content.x + timeline.fraction(beat.time * clip.sampleRate) * content.width) * draw.scale) / draw.scale);
                    draw.rect({x, content.y + 25, hairline, 5}, ruleColor);
                    draw.rect({x, waveform.y, hairline, waveform.height}, beat.position == 1 ? Color{.14f, .16f, .19f, 1} : Color{.095f, .105f, .12f, 1});
                }
                const std::array tracks{&analysis->sections, &analysis->keys, &analysis->chords};
                for (std::size_t lane = 0; lane < tracks.size(); ++lane) {
                    const auto& intervals = *tracks[lane];
                    std::size_t segment = 0;
                    for (std::size_t i = 0; i < intervals.size(); ++i, ++segment) {
                        const auto& interval = intervals[i];
                        double end = interval.end;
                        if (lane == 0) {
                            while (i + 1 < intervals.size() && intervals[i + 1].label == interval.label && std::abs(intervals[i + 1].start - end) < .001) end = intervals[++i].end;
                        }
                        const bool active = seconds >= interval.start && (seconds < end || (currentFrame == clip.frames && end >= duration - .001));
                        const auto label = intervalLabel(interval.label, lane);
                        if (active) {
                            if (!context.empty()) context += L"   ·   ";
                            context += label;
                        }
                        if (mainHover && lanes[lane].contains(hoverX, hoverY) && hoverTime >= interval.start && hoverTime < end) {
                            hoverLabel = label;
                            hoverRange = timeLabel(interval.start, true) + L" — " + timeLabel(end, true);
                        }
                        if (end <= viewStart || interval.start >= viewEnd) continue;
                        const float x = content.x + static_cast<float>(timeline.fraction(std::max(interval.start, viewStart) * clip.sampleRate) * content.width);
                        const float right = content.x + static_cast<float>(timeline.fraction(std::min(end, viewEnd) * clip.sampleRate) * content.width);
                        const auto color = active ? Color{.14f, .165f, .20f, 1} : segment % 2 ? Color{.102f, .114f, .133f, 1} : Color{.085f, .098f, .114f, 1};
                        draw.rect({x, lanes[lane].y, right - x, lanes[lane].height}, color);
                        if (x > content.x) draw.rect({x, lanes[lane].y + 6, hairline, lanes[lane].height - 12}, ruleColor);
                        const float padding = lane == 2 ? 5.f : 9.f;
                        if (right - x > padding * 2) draw.text(label, 11, x + padding, lanes[lane].y + 7, active ? foreground : muted, right - x - padding * 2, false, true);
                    }
                }
                const auto beat = std::ranges::upper_bound(analysis->beats, seconds, {}, &music::Beat::time);
                const auto& meter = beat == analysis->beats.begin() ? analysis->beats.front() : *std::prev(beat);
                draw.text(std::format(L"{:.1f} BPM   ·   {} / {}", analysis->bpm, meter.numerator, meter.denominator), 10, 32, 53, muted, width - 64);
                draw.text(context, 10, content.x, height - 30, muted, content.width - 130);
            } else if (!analysisError.empty()) draw.text(analysisError, 10, 32, 53, {.94f, .65f, .58f, 1}, width - 64);
            else {
                const auto stage = progress.stage.load();
                const auto status = stage == music::AnalysisStage::loading ? L"Loading model..." : std::format(L"Analyzing {} / {}", progress.window.load(), progress.windows.load());
                draw.text(status, 10, 32, 53, muted, width - 64);
            }

            const double visibleSeconds = viewEnd - viewStart;
            const double desired = visibleSeconds / std::max(2.f, content.width / 110);
            const double magnitude = std::pow(10.0, std::floor(std::log10(desired)));
            double tick = magnitude;
            for (const auto factor : {1., 2., 5., 10.})
                if (magnitude * factor >= desired) {
                    tick = magnitude * factor;
                    break;
                }
            for (double t = std::ceil(viewStart / tick) * tick; t <= viewEnd; t += tick) {
                const float x = content.x + static_cast<float>(timeline.fraction(t * clip.sampleRate) * content.width);
                const auto label = tick < .001 ? std::format(L"{:.4f} s", t) : tick < .1 ? std::format(L"{:.3f} s", t) : tick < 1 ? std::format(L"{:.1f} s", t) : timeLabel(t, false);
                draw.text(label, 10, std::clamp(x - 12, content.x, content.x + content.width - 76), content.y + 2, muted, 76);
                draw.rect({std::round(x * draw.scale) / draw.scale, content.y + 24, hairline, 6}, muted);
            }
            draw.text(timeLabel(viewStart, false) + L" — " + timeLabel(viewEnd, false), 10, width - 146, height - 30, muted, 114);
            draw.waveLayer = static_cast<std::uint32_t>(draw.vertices.size());
            draw.waveRect = waveform;
            draw.overviewRect = overview;
            draw.waveFirst = timeline.first;
            draw.waveCount = timeline.count;

            const float viewLeft = overview.x + static_cast<float>(timeline.first / clip.frames * overview.width);
            const float viewRight = overview.x + static_cast<float>((timeline.first + timeline.count) / clip.frames * overview.width);
            draw.rect({overview.x, overview.y, viewLeft - overview.x, overview.height}, {background.r, background.g, background.b, .72f});
            draw.rect({viewRight, overview.y, overview.x + overview.width - viewRight, overview.height}, {background.r, background.g, background.b, .72f});
            const Color outline{.29f, .34f, .40f, 1};
            draw.rect({viewLeft, overview.y, viewRight - viewLeft, hairline}, outline);
            draw.rect({viewLeft, overview.y + overview.height - hairline, viewRight - viewLeft, hairline}, outline);
            draw.rect({viewLeft, overview.y, hairline, overview.height}, outline);
            draw.rect({viewRight - hairline, overview.y, hairline, overview.height}, outline);
            if (overviewHover || (drag != Drag::none && drag != Drag::seek && drag != Drag::pan)) {
                draw.rect({viewLeft - 1, overview.y + 17, 2, 14}, muted, 1);
                draw.rect({viewRight - 1, overview.y + 17, 2, 14}, muted, 1);
            }
            if (mainHover && drag == Drag::none) draw.rect({std::round(hoverX * draw.scale) / draw.scale, content.y, hairline, content.height}, {.27f, .30f, .34f, 1});
            if (currentFrame >= timeline.first && currentFrame <= timeline.first + timeline.count) {
                const float x = static_cast<float>(std::round((content.x + timeline.fraction(currentFrame) * content.width) * draw.scale) / draw.scale);
                draw.rect({x, content.y, hairline, content.height}, accent);
                draw.rect({x - 2, content.y - 3, 4, 4}, accent, 2);
            }
            const float globalX = overview.x + static_cast<float>(currentFrame / clip.frames * overview.width);
            draw.rect({globalX, overview.y + 2, hairline, overview.height - 4}, accent);
            if ((mainHover || overviewHover) && drag == Drag::none) {
                const float tooltipWidth = hoverLabel.empty() ? 90.f : 240.f;
                const float tooltipHeight = hoverLabel.empty() ? 29.f : 62.f;
                const float x = std::clamp(hoverX + 13, content.x, content.x + content.width - tooltipWidth);
                const float y = mainHover ? std::min(hoverY + 18, waveform.y + waveform.height - tooltipHeight) : overview.y - tooltipHeight - 8;
                draw.rect({x, y, tooltipWidth, tooltipHeight}, {.16f, .18f, .21f, 1}, 4);
                if (!hoverLabel.empty()) {
                    draw.text(hoverLabel, 12, x + 10, y + 6, foreground, tooltipWidth - 20);
                    draw.text(hoverRange, 10, x + 10, y + 25, muted, tooltipWidth - 20);
                }
                draw.text(timeLabel(hoverTime, true), 10, x + 10, y + tooltipHeight - 21, accent, tooltipWidth - 20);
            }
            if (dropActive) {
                draw.rect(content, {background.r, background.g, background.b, .9f});
                draw.text(L"Drop to replace audio", 15, content.x, content.y + content.height / 2 - 11, foreground, content.width, true);
            }
        }
        draw.draw();
    }

    void Window::moveDrag(float x) {
        if (drag == Drag::seek) {
            dragFrame = std::clamp(timeline.frame((x - content.x) / content.width), 0., static_cast<double>(audio.clip.frames));
            audio.seek(static_cast<std::uint64_t>(dragFrame));
        } else if (drag == Drag::pan) {
            timeline.first = std::clamp(dragFirst - static_cast<double>(x - dragX) / content.width * dragCount, 0., static_cast<double>(audio.clip.frames) - dragCount);
        } else if (drag != Drag::none) {
            dragMoved = dragMoved || std::abs(x - dragX) >= 3;
            if (dragMoved) {
                follow = false;
                const double total = static_cast<double>(audio.clip.frames);
                const double delta = (x - dragX) / overview.width * total;
                const double minimum = std::min(32., total);
                if (drag == Drag::left) {
                    timeline.first = std::clamp(dragFirst + delta, 0., dragFirst + dragCount - minimum);
                    timeline.count = dragFirst + dragCount - timeline.first;
                } else if (drag == Drag::right) timeline.count = std::clamp(dragCount + delta, minimum, total - dragFirst);
                else if (drag == Drag::view) timeline.first = std::clamp(dragFirst + delta, 0., total - dragCount);
            }
        }
        refresh();
    }

    void Window::finishDrag(bool commit) {
        if (drag == Drag::seek) {
            audio.playing = restorePlayback;
            SetEvent(audio.command);
        } else if (drag != Drag::none && drag != Drag::pan && commit && !dragMoved) {
            const double frame = std::clamp(static_cast<double>(dragX - overview.x) / overview.width * audio.clip.frames, 0., static_cast<double>(audio.clip.frames));
            audio.seek(static_cast<std::uint64_t>(frame));
            timeline.reveal(frame, audio.clip.frames);
            follow = true;
        }
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
                if (y < 76) return HTCAPTION;
                return HTCLIENT;
            }
        case WM_GETMINMAXINFO:
            {
                auto& info          = *reinterpret_cast<MINMAXINFO*>(lparam);
                const float scale   = renderer ? renderer->scale : GetDpiForWindow(handle) / 96.f;
                info.ptMinTrackSize = {static_cast<LONG>((expanded ? 720 : 380) * scale), static_cast<LONG>((expanded ? 500 : 220) * scale)};
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
            if (audio.failed.load(std::memory_order_acquire)) throw std::runtime_error(audio.error);
            if (analyzing.valid() && analyzing.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                try {
                    auto completed = analyzing.get();
                    if (!analysisStop.stop_requested()) analysis = std::move(completed);
                } catch (const std::exception& exception) {
                    if (!analysisStop.stop_requested()) {
                        const std::string detail = exception.what();
                        analysisError.assign(detail.begin(), detail.end());
                    }
                }
            }
            if (loading && loadingClip.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto completed = std::move(loadingClip);
                loading        = false;
                if (!pendingPath.empty()) {
                    load(std::exchange(pendingPath, {}));
                    return 0;
                }
                try {
                    auto clip = completed.get();
                    analysisSamples = std::make_shared<const std::vector<float>>(std::move(clip.analysis));
                    renderer->upload(clip);
                    audio.open(std::move(clip));
                    timeline.fit(audio.clip.frames);
                    loaded = true;
                    analysisPending = true;
                } catch (const std::exception& exception) {
                    audio.close();
                    const std::string detail = exception.what();
                    error.assign(detail.begin(), detail.end());
                    loaded = false;
                }
            }
            if (loaded && analysisPending && !analyzing.valid()) {
                analysisPending = false;
                analysisStop = std::stop_source{};
                progress.stage = music::AnalysisStage::loading;
                analyzing = std::async(std::launch::async, [this, samples = analysisSamples, stop = analysisStop.get_token()] {
                    if (!model) model.emplace(std::filesystem::path(MUSIC_MODEL_DIRECTORY));
                    return model->analyze(*samples, progress, stop);
                });
            }
            refresh();
            return 0;
        case WM_MOUSEMOVE:
            hoverX = GET_X_LPARAM(lparam) / renderer->scale;
            hoverY = GET_Y_LPARAM(lparam) / renderer->scale;
            if (!hovering) {
                TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE, handle, 0};
                TrackMouseEvent(&tracking);
                hovering = true;
            }
            if (drag != Drag::none) moveDrag(hoverX);
            else refresh();
            return 0;
        case WM_MOUSELEAVE:
            hovering = false;
            refresh();
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            if (loaded && drag == Drag::none) {
                const auto x = GET_X_LPARAM(lparam) / renderer->scale;
                const auto y = GET_Y_LPARAM(lparam) / renderer->scale;
                SetFocus(handle);
                if (content.contains(x, y)) {
                    restorePlayback = audio.playing.exchange(false);
                    drag            = Drag::seek;
                    follow = true;
                    SetCapture(handle);
                    moveDrag(x);
                } else if (overview.contains(x, y)) {
                    dragX = x;
                    dragFirst = timeline.first;
                    dragCount = timeline.count;
                    dragMoved = false;
                    const float left = overview.x + static_cast<float>(timeline.first / audio.clip.frames * overview.width);
                    const float right = overview.x + static_cast<float>((timeline.first + timeline.count) / audio.clip.frames * overview.width);
                    if (std::min(std::abs(x - left), std::abs(x - right)) <= 6) drag = std::abs(x - left) <= std::abs(x - right) ? Drag::left : Drag::right;
                    else drag = x > left && x < right ? Drag::view : Drag::overview;
                    SetCapture(handle);
                }
            }
            return 0;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK:
            if (loaded && drag == Drag::none) {
                const auto x = GET_X_LPARAM(lparam) / renderer->scale;
                const auto y = GET_Y_LPARAM(lparam) / renderer->scale;
                if (content.contains(x, y)) {
                    SetFocus(handle);
                    drag = Drag::pan;
                    dragX = x;
                    dragFirst = timeline.first;
                    dragCount = timeline.count;
                    follow = false;
                    SetCapture(handle);
                    refresh();
                }
            }
            return 0;
        case WM_LBUTTONUP:
            if (drag != Drag::none && drag != Drag::pan) finishDrag(true);
            return 0;
        case WM_MBUTTONUP:
            if (drag == Drag::pan) finishDrag();
            return 0;
        case WM_CAPTURECHANGED:
            if (drag != Drag::none) finishDrag();
            return 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (loaded && drag == Drag::none) {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(handle, &point);
                if (!content.contains(point.x / renderer->scale, point.y / renderer->scale)) return 0;
                const auto delta = GET_WHEEL_DELTA_WPARAM(wparam) / 120.0;
                if ((GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT) || message == WM_MOUSEHWHEEL) {
                    timeline.pan(timeline.count * delta * (message == WM_MOUSEHWHEEL ? .1 : -.1), audio.clip.frames);
                    follow = false;
                } else timeline.zoom(std::pow(1.25, -delta), (point.x / renderer->scale - content.x) / content.width, audio.clip.frames);
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
                if (content.contains(x, y) || overview.contains(x, y)) {
                    const float left = overview.x + static_cast<float>(timeline.first / audio.clip.frames * overview.width);
                    const float right = overview.x + static_cast<float>((timeline.first + timeline.count) / audio.clip.frames * overview.width);
                    const bool edge = overview.contains(x, y) && std::min(std::abs(x - left), std::abs(x - right)) <= 6;
                    SetCursor(LoadCursorW(nullptr, edge ? IDC_SIZEWE : IDC_CROSS));
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
                    if (audio.playing.load()) follow = true;
                    SetEvent(audio.command);
                    refresh();
                } else if (wparam == 'F') {
                    timeline.fit(audio.clip.frames);
                    follow = true;
                    refresh();
                } else if (wparam == VK_HOME) {
                    audio.seek(0);
                    timeline.reveal(0, audio.clip.frames);
                    follow = true;
                    refresh();
                } else if (wparam == VK_LEFT || wparam == VK_RIGHT) {
                    const double frame = std::clamp(static_cast<double>(audio.position()) + (wparam == VK_LEFT ? -1 : 1) * static_cast<double>(audio.clip.sampleRate), 0., static_cast<double>(audio.clip.frames));
                    audio.seek(static_cast<std::uint64_t>(frame));
                    timeline.reveal(frame, audio.clip.frames);
                    follow = true;
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
