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
        constexpr COLORREF border = RGB(37, 43, 48);
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
        chordSegments.clear();
        melodyPassages.clear();
        melodyNotes.clear();
        melodyTrack = 0;
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
            MONITORINFO monitor{sizeof(MONITORINFO)};
            GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor);
            const LONG w = static_cast<LONG>(1920 * renderer->scale);
            const LONG h = static_cast<LONG>(1080 * renderer->scale);
            const int x  = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - w) / 2;
            const int y  = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - h) / 2;
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
        if (!expanded) return;
        content = {132, 88, width - 164, height - 202};
        waveform = {content.x, content.y + 38, content.width, std::clamp(waveHeight, 80.f, std::max(80.f, height - 606))};
        waveDivider = {content.x, waveform.y + waveform.height + 3, content.width, 10};
        for (std::size_t lane = 0; lane < lanes.size(); ++lane) lanes[lane] = {content.x, waveform.y + waveform.height + 18 + 40.f * lane, content.width, lane == 2 ? 44.f : 30.f};
        const float melodyTop = lanes.back().y + lanes.back().height + 10;
        melody = {content.x, melodyTop + 20, content.width, content.y + content.height - melodyTop - 20};
        for (int track = 0; track < 2; ++track) melodyTabs[track] = {content.x + 76 * track, melodyTop - 2, 64, 20};
        overview = {content.x, height - 96, content.width, 48};
    }

    void Window::rebuildAnalysisView() {
        chordSegments.clear();
        chordSegments.reserve(analysis->chords.size());
        melodyPassages.clear();
        melodyPassages.reserve(analysis->keys.size());
        melodyNotes.clear();
        melodyNotes.reserve(analysis->notes.size());
        constexpr std::wstring_view letters = L"CDEFGAB";
        constexpr std::array natural{0, 2, 4, 5, 7, 9, 11};
        constexpr std::array solfege{L"Do", L"Re", L"Mi", L"Fa", L"Sol", L"La", L"Si"};
        constexpr std::array upperNumerals{L"I", L"II", L"III", L"IV", L"V", L"VI", L"VII"};
        constexpr std::array lowerNumerals{L"i", L"ii", L"iii", L"iv", L"v", L"vi", L"vii"};
        for (std::size_t key = 0; key < analysis->keys.size(); ++key) {
            auto interval = analysis->keys[key];
            while (key + 1 < analysis->keys.size() && analysis->keys[key + 1].label == interval.label && std::abs(analysis->keys[key + 1].start - interval.end) < .001) interval.end = analysis->keys[++key].end;
            const auto passageIndex = melodyPassages.size();
            auto& passage = melodyPassages.emplace_back(MelodyPassage{.start = interval.start, .end = interval.end});
            const auto rootLetter = letters.find(static_cast<wchar_t>(interval.label.front()));
            int root = natural[rootLetter];
            for (std::size_t i = 1; i < interval.label.find(':'); ++i) root += interval.label[i] == '#' ? 1 : -1;
            const bool minorKey = interval.label.ends_with(":minor");
            const auto scale = minorKey ? std::array{0, 2, 3, 5, 7, 8, 10, 12} : std::array{0, 2, 4, 5, 7, 9, 11, 12};
            int signature = 0;
            for (std::size_t degree = 0; degree < 7; ++degree) signature += ((root + scale[degree] - natural[(rootLetter + degree) % 7]) % 12 + 18) % 12 - 6;
            // Even rows are scale tones; odd rows hold chromatic tones between them.
            for (int semitone = 0; semitone < 12; ++semitone) {
                const auto degree = static_cast<int>(std::ranges::upper_bound(scale, semitone) - scale.begin()) - 1;
                const bool inScale = semitone == scale[degree];
                const int namedDegree = degree + (!inScale && signature < 0 ? 1 : 0);
                // Major starts on Do, minor on La; Roman degrees remain relative to the tonic.
                const int solfegeDegree = (namedDegree + (minorKey ? 5 : 0)) % 7;
                const int alteration = semitone - scale[namedDegree];
                const std::wstring accidental(std::abs(alteration), alteration < 0 ? L'♭' : L'♯');
                passage.labels[degree * 2 + (inScale ? 0 : 1)] = std::format(L"{}{} ({}{})", accidental, solfege[solfegeDegree], accidental, upperNumerals[namedDegree % 7]);
            }
            for (const auto& chord : analysis->chords) {
                if (chord.start >= interval.end) break;
                if (chord.end <= interval.start) continue;
                std::wstring numeral;
                if (chord.label != "N") {
                    const std::string_view label = chord.label;
                    const auto colon = label.find(':');
                    const auto slash = label.find('/');
                    const auto quality = label.substr(colon + 1, (slash == label.npos ? label.size() : slash) - colon - 1);
                    const auto letter = letters.find(static_cast<wchar_t>(label.front()));
                    int pitch = natural[letter];
                    for (std::size_t i = 1; i < colon; ++i) pitch += label[i] == '#' ? 1 : -1;
                    const int chromatic = ((pitch - root) % 12 + 12) % 12;
                    const auto step = std::ranges::find(scale, chromatic);
                    const auto degree = step != scale.end() ? static_cast<std::size_t>(step - scale.begin()) : (letter + 7 - rootLetter) % 7;
                    // Alterations are relative to the current major or natural-minor scale.
                    const int alteration = ((pitch - root - scale[degree]) % 12 + 18) % 12 - 6;
                    const bool minor = quality.starts_with("min") || quality == "dim" || quality == "dim7" || quality == "hdim7";
                    numeral.assign(std::abs(alteration), alteration < 0 ? L'♭' : L'♯');
                    numeral += (minor ? lowerNumerals : upperNumerals)[degree];
                    if (quality == "dim" || quality == "dim7") numeral += L"°";
                    else if (quality == "hdim7") numeral += L"ø";
                    else if (quality == "aug") numeral += L"+";
                    if (quality == "maj7" || quality == "minmaj7") numeral += L"maj7";
                    else if (quality.ends_with("7")) numeral += L"7";
                    else if (quality == "sus2") numeral += L"sus2";
                    else if (quality == "sus4") numeral += L"sus4";
                    else if (quality == "sus4(b7)") numeral += L"7sus4";
                    else if (quality == "maj6" || quality == "min6") numeral += L"(add6)";
                }
                chordSegments.push_back({std::max(chord.start, interval.start), std::min(chord.end, interval.end), intervalLabel(chord.label, 2), std::move(numeral)});
            }
            for (std::size_t source = 0; source < analysis->notes.size(); ++source) {
                const auto& note = analysis->notes[source];
                if (note.start >= interval.end) break;
                if (note.end <= interval.start) continue;
                const int octave = static_cast<int>(std::floor((note.pitch - root) / 12.));
                const int semitone = note.pitch - root - octave * 12;
                const auto degree = static_cast<int>(std::ranges::upper_bound(scale, semitone) - scale.begin()) - 1;
                const bool inScale = semitone == scale[degree];
                const int namedDegree = degree + (!inScale && signature < 0 ? 1 : 0);
                const auto letter = (rootLetter + namedDegree) % 7;
                const int accidental = (note.pitch - natural[letter] + 18) % 12 - 6;
                std::wstring name(1, letters[letter]);
                name += std::wstring(std::abs(accidental), accidental < 0 ? L'♭' : L'♯');
                name += std::to_wstring((note.pitch - accidental) / 12 - 1);
                const float row = octave * 7 + degree + (inScale ? 0.f : .5f);
                melodyNotes.push_back({source, passageIndex, std::max(note.start, interval.start), std::min(note.end, interval.end), row, inScale, std::move(name), passage.labels[degree * 2 + (inScale ? 0 : 1)]});
                passage.low[note.track] = std::min(passage.low[note.track], row);
                passage.high[note.track] = std::max(passage.high[note.track], row);
            }
        }
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
            if (follow && audio.playing.load() && (drag == Drag::none || drag == Drag::height)) timeline.first = std::clamp(currentFrame - timeline.count / 2, 0., static_cast<double>(clip.frames) - timeline.count);
            const double seconds = currentFrame / clip.sampleRate;
            const double duration = static_cast<double>(clip.frames) / clip.sampleRate;
            const double viewStart = timeline.first / clip.sampleRate;
            const double viewEnd = (timeline.first + timeline.count) / clip.sampleRate;
            const double pixels = content.width / (viewEnd - viewStart);
            const float hairline = 1 / draw.scale;
            const bool dividerHover = hovering && waveDivider.contains(hoverX, hoverY);
            const bool tabHover = hovering && (melodyTabs[0].contains(hoverX, hoverY) || melodyTabs[1].contains(hoverX, hoverY));
            const bool mainHover = hovering && content.contains(hoverX, hoverY) && !dividerHover && !tabHover;
            const bool overviewHover = hovering && overview.contains(hoverX, hoverY);
            const double hoverTime = mainHover ? timeline.frame((hoverX - content.x) / content.width) / clip.sampleRate : (hoverX - overview.x) / overview.width * duration;
            std::wstring hoverLabel;
            std::wstring hoverRange;
            std::wstring context;

            draw.text(clip.path.stem().wstring(), 19, 32, 24, foreground, width - 300);
            draw.text(timeLabel(seconds, true), 23, width - 210, 22, foreground, 118);
            draw.text(L"/ " + timeLabel(duration, false), 12, width - 80, 34, muted, 55);
            draw.text(L"BARS", 11, 32, content.y + 3, muted, 64);
            for (std::size_t lane = 0; lane < lanes.size(); ++lane) draw.text(std::array{L"FORM", L"KEY", L"CHORDS"}[lane], 11, 32, lanes[lane].y + lanes[lane].height / 2 - 8, muted, 64);
            draw.rect({content.x, content.y + 30, content.width, hairline}, ruleColor);
            for (std::uint32_t channel = 0; channel < clip.channels; ++channel) {
                const float center = waveform.y + waveform.height / clip.channels * (channel + .5f);
                draw.text(clip.channels == 1 ? L"M" : channel == 0 ? L"L" : L"R", 11, 51, center - 8, muted, 16);
                draw.rect({waveform.x, center, waveform.width, hairline}, eighthGrid);
            }
            draw.text(L"OVERVIEW", 11, 32, overview.y + 16, muted, 80);
            if (dividerHover || drag == Drag::height) draw.rect({waveDivider.x, waveDivider.y + waveDivider.height / 2, waveDivider.width, hairline}, muted);
            for (int track = 0; track < 2; ++track) {
                const auto& tab = melodyTabs[track];
                const bool active = track == melodyTrack;
                const bool hovered = hovering && tab.contains(hoverX, hoverY);
                draw.text(track == 0 ? L"VOCAL" : L"INST", 11, tab.x + 12, tab.y + 2, analysis && (active || hovered) ? foreground : muted, tab.width - 12);
                if (analysis && active) draw.rect({tab.x + 1, tab.y + 9, 4, 4}, accent, 2);
            }
            const double labelTime = mainHover && melody.contains(hoverX, hoverY) ? hoverTime : seconds;
            const MelodyPassage* labelPassage = nullptr;
            double labelDistance = std::numeric_limits<double>::infinity();
            for (const auto& passage : melodyPassages) {
                if (passage.end <= viewStart || passage.start >= viewEnd || passage.low[melodyTrack] > passage.high[melodyTrack]) continue;
                const double distance = std::max({passage.start - labelTime, labelTime - passage.end, 0.});
                if (distance <= labelDistance) {
                    labelPassage = &passage;
                    labelDistance = distance;
                }
                const float x = melody.x + static_cast<float>(timeline.fraction(std::max(passage.start, viewStart) * clip.sampleRate) * melody.width);
                const float right = melody.x + static_cast<float>(timeline.fraction(std::min(passage.end, viewEnd) * clip.sampleRate) * melody.width);
                const float step = melody.height / (passage.high[melodyTrack] - passage.low[melodyTrack] + 1);
                // Fractional limits retain edge chromatic notes without adding another scale degree.
                for (int row = static_cast<int>(std::ceil(passage.low[melodyTrack])); row <= static_cast<int>(std::floor(passage.high[melodyTrack])); ++row) {
                    const float y = melody.y + (passage.high[melodyTrack] - row) * step;
                    const bool tonic = row % 7 == 0;
                    if (tonic) draw.rect({x, y, right - x, step}, {5 / 255.f, 8 / 255.f, 10 / 255.f, 1});
                    draw.rect({x, y + step - hairline, right - x, hairline}, tonic ? ruleColor : eighthGrid);
                }
            }
            if (labelPassage) {
                const auto passageIndex = static_cast<std::size_t>(labelPassage - melodyPassages.data());
                const float step = melody.height / (labelPassage->high[melodyTrack] - labelPassage->low[melodyTrack] + 1);
                for (int row = static_cast<int>(std::ceil(labelPassage->low[melodyTrack] * 2)); row <= static_cast<int>(std::floor(labelPassage->high[melodyTrack] * 2)); ++row) {
                    const bool inScale = row % 2 == 0;
                    if (!inScale && (step < 22 || std::ranges::none_of(melodyNotes, [&](const auto& note) { return note.passage == passageIndex && analysis->notes[note.source].track == melodyTrack && note.row == row * .5f; }))) continue;
                    const float center = melody.y + (labelPassage->high[melodyTrack] - row * .5f + .5f) * step;
                    const float textSize = inScale ? std::clamp(std::round(step * .36f), 9.f, 14.f) : 8.f;
                    const bool sounding = std::ranges::any_of(melodyNotes, [&](const auto& note) { return note.passage == passageIndex && analysis->notes[note.source].track == melodyTrack && note.row == row * .5f && seconds >= note.start && seconds < note.end; });
                    draw.text(labelPassage->labels[(row % 14 + 14) % 14], textSize, 24, center - textSize * 2 / 3, sounding ? accent : row % 14 == 0 ? foreground : muted, melody.x - 36, false, true);
                }
            }

            if (analysis) {
                int bar = 0;
                const double spacing = 60 / analysis->bpm * pixels;
                const auto stride = std::max(1, static_cast<int>(std::ceil(28 / (spacing * 4))));
                for (const auto& beat : analysis->beats) {
                    if (beat.position == 1) ++bar;
                    if (beat.time < viewStart || beat.time > viewEnd) continue;
                    if (beat.position != 1 && spacing < 14) continue;
                    if (beat.position == 1 && (bar - 1) % stride) continue;
                    const float x = static_cast<float>(std::round((content.x + timeline.fraction(beat.time * clip.sampleRate) * content.width) * draw.scale) / draw.scale);
                    if (beat.position == 1) {
                        const float textWidth = std::min(40.f, content.x + content.width - x - 4);
                        if (textWidth > 0) draw.text(std::to_wstring(bar), 10, x + 4, content.y + 2, muted, textWidth, false, true);
                    }
                    draw.rect({x, content.y + (beat.position == 1 ? 24 : 25), hairline, beat.position == 1 ? 6.f : 5.f}, beat.position == 1 ? muted : ruleColor);
                    draw.rect({x, waveform.y, hairline, waveform.height}, beat.position == 1 ? barGrid : beatGrid);
                    draw.rect({x, melody.y, hairline, melody.height}, beat.position == 1 ? barGrid : beatGrid);
                }
                for (const auto& [beat, next] : analysis->beats | std::views::adjacent<2>) {
                    if (next.time <= viewStart || beat.time >= viewEnd) continue;
                    const double sixteenth = (next.time - beat.time) * beat.denominator / 16;
                    for (int division = 1; division * beat.denominator < 16; ++division) {
                        const bool quarter = division % 4 == 0;
                        const bool eighth = division % 2 == 0;
                        if (sixteenth * pixels < (quarter ? 3.5 : eighth ? 4. : 6.)) continue;
                        const double time = beat.time + sixteenth * division;
                        if (time < viewStart || time >= viewEnd) continue;
                        const float x = static_cast<float>(std::round((melody.x + timeline.fraction(time * clip.sampleRate) * melody.width) * draw.scale) / draw.scale);
                        const auto color = quarter ? beatGrid : eighth ? eighthGrid : sixteenthGrid;
                        draw.rect({x, melody.y, hairline, melody.height}, color);
                    }
                }
                const std::array tracks{&analysis->sections, &analysis->keys, &analysis->chords};
                for (std::size_t lane = 0; lane < tracks.size(); ++lane) {
                    const auto& intervals = *tracks[lane];
                    const auto count = lane == 2 ? chordSegments.size() : intervals.size();
                    for (std::size_t i = 0; i < count; ++i) {
                        const double start = lane == 2 ? chordSegments[i].start : intervals[i].start;
                        double end = lane == 2 ? chordSegments[i].end : intervals[i].end;
                        const auto label = lane == 2 ? chordSegments[i].name : intervalLabel(intervals[i].label, lane);
                        const std::wstring_view numeral = lane == 2 ? std::wstring_view(chordSegments[i].numeral) : std::wstring_view{};
                        const auto fullLabel = numeral.empty() ? label : std::format(L"{} · {}", label, numeral);
                        if (lane == 0) {
                            while (i + 1 < intervals.size() && intervals[i + 1].label == intervals[i].label && std::abs(intervals[i + 1].start - end) < .001) end = intervals[++i].end;
                        }
                        const bool active = seconds >= start && (seconds < end || (currentFrame == clip.frames && end >= duration - .001));
                        if (active) {
                            if (!context.empty()) context += L"   ·   ";
                            context += fullLabel;
                        }
                        const bool hovered = mainHover && lanes[lane].contains(hoverX, hoverY) && hoverTime >= start && hoverTime < end;
                        if (hovered) {
                            hoverLabel = fullLabel;
                            hoverRange = timeLabel(start, true) + L" — " + timeLabel(end, true);
                        }
                        if (end <= viewStart || start >= viewEnd) continue;
                        const float x = content.x + static_cast<float>(timeline.fraction(std::max(start, viewStart) * clip.sampleRate) * content.width);
                        const float right = content.x + static_cast<float>(timeline.fraction(std::min(end, viewEnd) * clip.sampleRate) * content.width);
                        const float segmentWidth = std::max(0.f, right - x - 2);
                        draw.rect({x, lanes[lane].y, segmentWidth, lanes[lane].height}, hovered ? hoverSurface : active ? activeSurface : surfaceColor);
                        if (active) draw.rect({x, lanes[lane].y + lanes[lane].height - hairline, segmentWidth, hairline}, accent);
                        const float padding = lane == 2 ? 6.f : 12.f;
                        if (segmentWidth > padding * 2) {
                            const float textWidth = segmentWidth - padding * 2;
                            draw.text(label, lane == 2 ? 14.f : 12.f, x + padding, lanes[lane].y + (numeral.empty() ? (lanes[lane].height - 18) / 2 : 2), foreground, textWidth, false, true);
                            if (!numeral.empty()) draw.text(numeral, 11, x + padding, lanes[lane].y + 23, active ? accent : muted, textWidth, false, true);
                        }
                    }
                }
                const MelodyNote* hoveredNote = nullptr;
                Rect hoveredRect;
                float hoverDistance = std::numeric_limits<float>::infinity();
                for (const auto& segment : melodyNotes) {
                    if (segment.end <= viewStart || segment.start >= viewEnd) continue;
                    const auto& note = analysis->notes[segment.source];
                    if (note.track != melodyTrack) continue;
                    const auto& area = melody;
                    const auto& passage = melodyPassages[segment.passage];
                    const float step = area.height / (passage.high[note.track] - passage.low[note.track] + 1);
                    const float x = area.x + static_cast<float>(timeline.fraction(std::max(segment.start, viewStart) * clip.sampleRate) * area.width);
                    const float right = area.x + static_cast<float>(timeline.fraction(std::min(segment.end, viewEnd) * clip.sampleRate) * area.width);
                    const float center = area.y + (passage.high[note.track] - segment.row + .5f) * step;
                    const bool sounding = seconds >= segment.start && seconds < segment.end;
                    const auto color = sounding ? accent : noteColor;
                    const float gap = segment.end == note.end ? std::min(hairline, (right - x) * .15f) : 0;
                    const float noteWidth = std::min(area.x + area.width - x, std::max(hairline, right - x - gap));
                    const float noteHeight = segment.inScale ? step * .92f : hairline;
                    const Rect rect{x, center - noteHeight / 2, noteWidth, noteHeight};
                    draw.rect(rect, color, segment.inScale ? std::min({1.5f, noteHeight / 2, noteWidth / 2}) : 0);
                    const float dx = std::max({x - hoverX, hoverX - right, 0.f});
                    const float dy = std::abs(hoverY - center);
                    const float distance = dx * dx + dy * dy;
                    if (mainHover && area.contains(hoverX, hoverY) && dx <= 3 && dy <= std::max(3.f, noteHeight / 2) && distance < hoverDistance) {
                        hoveredNote = &segment;
                        hoveredRect = rect;
                        hoverDistance = distance;
                    }
                }
                if (hoveredNote) {
                    const auto& note = analysis->notes[hoveredNote->source];
                    draw.rect(hoveredRect, accent, hoveredNote->inScale ? std::min({1.5f, hoveredRect.height / 2, hoveredRect.width / 2}) : 0);
                    hoverLabel = std::format(L"{}   ·   {}   ·   {}", hoveredNote->label, hoveredNote->name, note.track == 0 ? L"Vocal" : L"Instrumental");
                    hoverRange = std::format(L"{} — {}   ·   {:.3f} s", timeLabel(note.start, true), timeLabel(note.end, true), note.end - note.start);
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

            draw.text(timeLabel(viewStart, false) + L" — " + timeLabel(viewEnd, false), 10, width - 146, height - 30, muted, 114);
            const float viewLeft = overview.x + static_cast<float>(timeline.first / clip.frames * overview.width);
            const float viewRight = overview.x + static_cast<float>((timeline.first + timeline.count) / clip.frames * overview.width);
            draw.rect({viewLeft, overview.y, viewRight - viewLeft, overview.height}, activeSurface);
            draw.waveLayer = static_cast<std::uint32_t>(draw.vertices.size());
            draw.waveRect = waveform;
            draw.overviewRect = overview;
            draw.waveFirst = timeline.first;
            draw.waveCount = timeline.count;
            draw.wavePosition = currentFrame;

            draw.rect({overview.x, overview.y, viewLeft - overview.x, overview.height}, {background.r, background.g, background.b, .35f});
            draw.rect({viewRight, overview.y, overview.x + overview.width - viewRight, overview.height}, {background.r, background.g, background.b, .35f});
            draw.rect({viewLeft, overview.y, viewRight - viewLeft, hairline}, accent);
            draw.rect({viewLeft, overview.y + overview.height - hairline, viewRight - viewLeft, hairline}, accent);
            draw.rect({viewLeft, overview.y, hairline, overview.height}, accent);
            draw.rect({viewRight - hairline, overview.y, hairline, overview.height}, accent);
            if (overviewHover || drag == Drag::overview || drag == Drag::view || drag == Drag::left || drag == Drag::right) {
                draw.rect({viewLeft - 1, overview.y + 17, 2, 14}, accent, 1);
                draw.rect({viewRight - 1, overview.y + 17, 2, 14}, accent, 1);
            }
            if (analysis) {
                for (std::size_t key = 1; key < analysis->keys.size(); ++key) {
                    const auto& change = analysis->keys[key];
                    if (change.label == analysis->keys[key - 1].label || change.start < viewStart || change.start >= viewEnd) continue;
                    const float x = static_cast<float>(std::round((content.x + timeline.fraction(change.start * clip.sampleRate) * content.width) * draw.scale) / draw.scale);
                    draw.rect({x, waveform.y, hairline, content.y + content.height - waveform.y}, ruleColor);
                }
            }
            if (mainHover && drag == Drag::none) draw.rect({std::round(hoverX * draw.scale) / draw.scale, content.y, hairline, content.height}, {61 / 255.f, 63 / 255.f, 67 / 255.f, 1});
            if (currentFrame >= timeline.first && currentFrame <= timeline.first + timeline.count) {
                const float x = static_cast<float>(std::round((content.x + timeline.fraction(currentFrame) * content.width) * draw.scale) / draw.scale);
                draw.rect({x, content.y, hairline, content.height}, accent);
                draw.rect({x - 2, content.y - 3, 4, 4}, accent, 2);
            }
            const float globalX = overview.x + static_cast<float>(currentFrame / clip.frames * overview.width);
            draw.rect({globalX, overview.y + 2, hairline, overview.height - 4}, accent);
            if ((mainHover || overviewHover) && drag == Drag::none) {
                const float tooltipWidth = hoverLabel.empty() ? 90.f : 320.f;
                const float tooltipHeight = hoverLabel.empty() ? 29.f : 62.f;
                const float x = std::clamp(hoverX + 13, content.x, content.x + content.width - tooltipWidth);
                const float y = mainHover ? (hoverY + 18 + tooltipHeight <= content.y + content.height ? hoverY + 18 : hoverY - tooltipHeight - 12) : overview.y - tooltipHeight - 8;
                draw.rect({x, y, tooltipWidth, tooltipHeight}, ruleColor, 4);
                draw.rect({x + hairline, y + hairline, tooltipWidth - 2 * hairline, tooltipHeight - 2 * hairline}, surfaceColor, 3);
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

    void Window::moveDrag(float x, float y) {
        if (drag == Drag::height) {
            waveHeight = std::clamp(dragHeight + y - dragY, 80.f, std::max(80.f, height - 606));
            layout();
        } else if (drag == Drag::seek) {
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
        } else if ((drag == Drag::overview || drag == Drag::view || drag == Drag::left || drag == Drag::right) && commit && !dragMoved) {
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
                info.ptMinTrackSize = {static_cast<LONG>((expanded ? 720 : 380) * scale), static_cast<LONG>((expanded ? 720 : 220) * scale)};
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
                    if (!analysisStop.stop_requested()) {
                        analysis = std::move(completed);
                        melodyTrack = std::ranges::any_of(analysis->notes, [](const auto& note) { return note.track == 0; }) ? 0 : 1;
                        rebuildAnalysisView();
                    }
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
            if (drag != Drag::none) moveDrag(hoverX, hoverY);
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
                for (int track = 0; track < 2; ++track) {
                    if (!melodyTabs[track].contains(x, y)) continue;
                    if (analysis) {
                        melodyTrack = track;
                        refresh();
                    }
                    return 0;
                }
                if ((wparam & MK_CONTROL) && analysis) {
                    const double time = timeline.frame((x - content.x) / content.width) / audio.clip.sampleRate;
                    const std::array tracks{&analysis->sections, &analysis->keys, &analysis->chords};
                    for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
                        if (!lanes[lane].contains(x, y)) continue;
                        const auto& intervals = *tracks[lane];
                        const auto count = lane == 2 ? chordSegments.size() : intervals.size();
                        for (std::size_t i = 0; i < count; ++i) {
                            const double start = lane == 2 ? chordSegments[i].start : intervals[i].start;
                            double end = lane == 2 ? chordSegments[i].end : intervals[i].end;
                            if (lane == 0) {
                                while (i + 1 < intervals.size() && intervals[i + 1].label == intervals[i].label && std::abs(intervals[i + 1].start - end) < .001) end = intervals[++i].end;
                            }
                            if (time < start || time >= end) continue;
                            const auto frame = static_cast<std::uint64_t>(start * audio.clip.sampleRate);
                            audio.seek(frame);
                            timeline.reveal(static_cast<double>(frame), audio.clip.frames);
                            follow = true;
                            refresh();
                            return 0;
                        }
                        return 0;
                    }
                }
                if ((wparam & MK_SHIFT) && analysis && lanes[1].contains(x, y)) {
                    const double time = timeline.frame((x - content.x) / content.width) / audio.clip.sampleRate;
                    for (auto& key : analysis->keys) {
                        if (time < key.start || time >= key.end) continue;
                        constexpr std::string_view letters = "CDEFGAB";
                        constexpr std::array natural{0, 2, 4, 5, 7, 9, 11};
                        const auto rootLetter = letters.find(key.label.front());
                        int root = natural[rootLetter];
                        for (std::size_t i = 1; i < key.label.find(':'); ++i) root += key.label[i] == '#' ? 1 : -1;
                        const bool minor = key.label.ends_with(":minor");
                        // Relative major/minor preserves the key signature and note pitches.
                        const auto relativeLetter = (rootLetter + (minor ? 2 : 5)) % 7;
                        const int alteration = ((root + (minor ? 3 : 9) - natural[relativeLetter]) % 12 + 18) % 12 - 6;
                        key.label = std::string(1, letters[relativeLetter]) + std::string(std::abs(alteration), alteration < 0 ? 'b' : '#') + (minor ? ":major" : ":minor");
                        rebuildAnalysisView();
                        refresh();
                        break;
                    }
                    return 0;
                }
                if (waveDivider.contains(x, y)) {
                    drag = Drag::height;
                    dragY = y;
                    dragHeight = waveform.height;
                    SetCapture(handle);
                    refresh();
                } else if (content.contains(x, y)) {
                    restorePlayback = audio.playing.exchange(false);
                    drag            = Drag::seek;
                    follow = true;
                    SetCapture(handle);
                    moveDrag(x, y);
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
                if (melodyTabs[0].contains(x, y) || melodyTabs[1].contains(x, y)) return 0;
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
                const auto x = point.x / renderer->scale;
                const auto y = point.y / renderer->scale;
                if (!content.contains(x, y) || melodyTabs[0].contains(x, y) || melodyTabs[1].contains(x, y)) return 0;
                const auto delta = GET_WHEEL_DELTA_WPARAM(wparam) / 120.0;
                if ((GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT) || message == WM_MOUSEHWHEEL) {
                    timeline.pan(timeline.count * delta * (message == WM_MOUSEHWHEEL ? .1 : -.1), audio.clip.frames);
                    follow = false;
                } else timeline.zoom(std::pow(1.25, -delta), (x - content.x) / content.width, audio.clip.frames);
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
                if (drag == Drag::height || waveDivider.contains(x, y)) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                    return TRUE;
                }
                if (drag == Drag::none && (melodyTabs[0].contains(x, y) || melodyTabs[1].contains(x, y))) {
                    SetCursor(LoadCursorW(nullptr, analysis ? IDC_HAND : IDC_ARROW));
                    return TRUE;
                }
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
                } else if (wparam == VK_TAB && analysis && !(lparam & (1LL << 30))) {
                    melodyTrack = 1 - melodyTrack;
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
