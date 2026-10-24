module;

#include <dwmapi.h>
#include <oleidl.h>
#include <shellapi.h>
#include <windows.h>
#include <windowsx.h>

module editor.window;

import std;
import musicia.audio;
import editor.renderer;
import editor.timeline;
import editor.playback;
import musicia.analysis;
import musicia.sheetsage2;

namespace editor {
    std::wstring interval_label(std::string_view label, std::size_t lane) {
        if (label == "N") return L"—";
        std::wstring text(label.begin(), label.end());
        if (lane == 0) {
            text.front() = std::towupper(text.front());
            return text;
        }
        if (lane == 1) {
            std::ranges::replace(text, L':', L' ');
            const auto mode = text.find(L' ') + 1;
            text[mode] = std::towupper(text[mode]);
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
        const WNDCLASSEXW window_class{sizeof(WNDCLASSEXW), CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS, procedure, 0, 0, GetModuleHandleW(nullptr), LoadIconW(nullptr, IDI_APPLICATION), LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"MusiciaTimelineWindow", nullptr};
        if (!RegisterClassExW(&window_class)) throw std::runtime_error("Unable to register the Musicia window.");
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO monitor{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
        const float scale = GetDpiForSystem() / 96.f;
        const int w       = static_cast<int>(448 * scale);
        const int h       = static_cast<int>(274 * scale);
        handle            = CreateWindowExW(WS_EX_APPWINDOW, window_class.lpszClassName, L"Musicia", WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN, monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - w) / 2, monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - h) / 2, w, h, nullptr, nullptr, window_class.hInstance, this);
        if (!handle) throw std::runtime_error("Unable to create the Musicia window.");
        constexpr DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
        DwmSetWindowAttribute(handle, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
        constexpr BOOL dark = TRUE;
        DwmSetWindowAttribute(handle, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        constexpr COLORREF border = RGB(43, 43, 43);
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
        analysis_stop.request_stop();
        if (analyzing.valid()) analyzing.wait();
        audio.close();
        renderer.reset();
        if (handle) DestroyWindow(handle);
    }

    void Window::load(std::filesystem::path path) {
        if (loading) {
            pending_path = std::move(path);
            return;
        }
        finish_drag();
        hovering = false;
        follow_resume_at.reset();
        initial_view = true;
        analysis_stop.request_stop();
        analysis.reset();
        bar_focus.clear();
        focus_from.clear();
        focus_target.reset();
        focus_strength = 0;
        focus_strength_from = 0;
        focus_strength_target = 0;
        focus_animating = false;
        focus_started = {};
        focus_fade_started = {};
        chord_segments.clear();
        melody_passages.clear();
        melody_notes.clear();
        melody_track = 0;
        analysis_samples.reset();
        analysis_pending = false;
        analysis_error.clear();
        audio.close();
        meter_db.fill(ui::meter_floor);
        held_peak_db.fill(-std::numeric_limits<float>::infinity());
        peak_hold_until = {};
        meter_updated = std::chrono::steady_clock::now();
        loaded  = false;
        loading = true;
        error.clear();
        loading_path = std::move(path);
        loading_clip = std::async(std::launch::async, [path = loading_path] { return musicia::AudioClip::read(path); });
        refresh();
    }

    void Window::layout() {
        RECT client{};
        GetClientRect(handle, &client);
        const auto scale = renderer ? renderer->scale : GetDpiForWindow(handle) / 96.f;
        width            = client.right / scale;
        height           = client.bottom / scale;
        if (!expanded) return;
        content = {ui::timeline_x, ui::ruler_y, width - ui::timeline_x - ui::right_margin, height - 226};
        waveform = {content.x, content.y + 38, content.width, std::clamp(wave_height, 64.f, std::max(64.f, height - 606))};
        wave_divider = {content.x, waveform.y + waveform.height, content.width, 16};
        for (std::size_t lane = 0; lane < lanes.size(); ++lane) lanes[lane] = {content.x, waveform.y + waveform.height + 16 + 36.f * lane, content.width, lane == 2 ? 54.f : 36.f};
        const float melody_top = lanes.back().y + lanes.back().height;
        melody = {content.x, melody_top + 40, content.width, content.y + content.height - melody_top - 40};
        for (int track = 0; track < 2; ++track) melody_tabs[track] = {content.x + 76 * track, melody_top + 6, 64, 28};
        overview = {content.x, height - 94, content.width, 42};
    }

    void Window::rebuild_analysis_view() {
        chord_segments.clear();
        chord_segments.reserve(analysis->chords.size());
        melody_passages.clear();
        melody_passages.reserve(analysis->keys.size());
        melody_notes.clear();
        melody_notes.reserve(analysis->notes.size());
        constexpr std::wstring_view letters = L"CDEFGAB";
        constexpr std::array natural{0, 2, 4, 5, 7, 9, 11};
        constexpr std::array solfege{L"Do", L"Re", L"Mi", L"Fa", L"Sol", L"La", L"Ti"};
        constexpr std::array upper_numerals{L"I", L"II", L"III", L"IV", L"V", L"VI", L"VII"};
        constexpr std::array lower_numerals{L"i", L"ii", L"iii", L"iv", L"v", L"vi", L"vii"};
        for (std::size_t key = 0; key < analysis->keys.size(); ++key) {
            auto interval = analysis->keys[key];
            while (key + 1 < analysis->keys.size() && analysis->keys[key + 1].label == interval.label && std::abs(analysis->keys[key + 1].start - interval.end) < .001) interval.end = analysis->keys[++key].end;
            const auto passage_index = melody_passages.size();
            auto& passage = melody_passages.emplace_back(MelodyPassage{.start = interval.start, .end = interval.end});
            const auto root_letter = letters.find(static_cast<wchar_t>(interval.label.front()));
            int root = natural[root_letter];
            for (std::size_t i = 1; i < interval.label.find(':'); ++i) root += interval.label[i] == '#' ? 1 : -1;
            const bool minor_key = interval.label.ends_with(":minor");
            const auto scale = minor_key ? std::array{0, 2, 3, 5, 7, 8, 10, 12} : std::array{0, 2, 4, 5, 7, 9, 11, 12};
            int signature = 0;
            for (std::size_t degree = 0; degree < 7; ++degree) signature += ((root + scale[degree] - natural[(root_letter + degree) % 7]) % 12 + 18) % 12 - 6;
            for (int semitone = 0; semitone < 12; ++semitone) {
                const auto degree = static_cast<int>(std::ranges::upper_bound(scale, semitone) - scale.begin()) - 1;
                const bool in_scale = semitone == scale[degree];
                const int named_degree = degree + (!in_scale && signature < 0 ? 1 : 0);
                const int solfege_degree = (named_degree + (minor_key ? 5 : 0)) % 7;
                const int alteration = semitone - scale[named_degree];
                const std::wstring accidental(std::abs(alteration), alteration < 0 ? L'♭' : L'♯');
                const int row = degree * 2 + (in_scale ? 0 : 1);
                passage.pitch_offsets[row] = root + semitone;
                passage.solfege_labels[row] = std::format(L"{}{}", accidental, solfege[solfege_degree]);
                passage.labels[row] = std::format(L"{} ({}{})", passage.solfege_labels[row], accidental, upper_numerals[named_degree % 7]);
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
                    const auto degree = step != scale.end() ? static_cast<std::size_t>(step - scale.begin()) : (letter + 7 - root_letter) % 7;
                    const int alteration = ((pitch - root - scale[degree]) % 12 + 18) % 12 - 6;
                    const bool minor = quality.starts_with("min") || quality == "dim" || quality == "dim7" || quality == "hdim7";
                    numeral.assign(std::abs(alteration), alteration < 0 ? L'♭' : L'♯');
                    numeral += (minor ? lower_numerals : upper_numerals)[degree];
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
                chord_segments.push_back({std::max(chord.start, interval.start), std::min(chord.end, interval.end), interval_label(chord.label, 2), std::move(numeral)});
            }
            for (std::size_t source = 0; source < analysis->notes.size(); ++source) {
                const auto& note = analysis->notes[source];
                if (note.start >= interval.end) break;
                if (note.end <= interval.start) continue;
                const int octave = static_cast<int>(std::floor((note.pitch - root) / 12.));
                const int semitone = note.pitch - root - octave * 12;
                const auto degree = static_cast<int>(std::ranges::upper_bound(scale, semitone) - scale.begin()) - 1;
                const bool in_scale = semitone == scale[degree];
                const int named_degree = degree + (!in_scale && signature < 0 ? 1 : 0);
                const auto letter = (root_letter + named_degree) % 7;
                const int accidental = (note.pitch - natural[letter] + 18) % 12 - 6;
                std::wstring name(1, letters[letter]);
                name += std::wstring(std::abs(accidental), accidental < 0 ? L'♭' : L'♯');
                name += std::to_wstring((note.pitch - accidental) / 12 - 1);
                const float row = octave * 7 + degree + (in_scale ? 0.f : .5f);
                melody_notes.push_back({source, passage_index, std::max(note.start, interval.start), std::min(note.end, interval.end), row, in_scale, std::move(name), passage.labels[degree * 2 + (in_scale ? 0 : 1)]});
                passage.low[note.track] = std::min(passage.low[note.track], row);
                passage.high[note.track] = std::max(passage.high[note.track], row);
            }
        }
    }

    Rect Window::note_rect(const MelodyNote& segment) const {
        const auto& note = analysis->notes[segment.source];
        const auto& passage = melody_passages[segment.passage];
        const float step = melody.height / (passage.high[note.track] - passage.low[note.track] + 1);
        const float hairline = std::round(ui::beat_width * renderer->scale) / renderer->scale;
        const float left = melody.x + static_cast<float>(std::clamp(timeline.fraction(segment.start * audio.clip.sample_rate), 0., 1.) * melody.width);
        const float right = melody.x + static_cast<float>(std::clamp(timeline.fraction(segment.end * audio.clip.sample_rate), 0., 1.) * melody.width);
        const float center = melody.y + (passage.high[note.track] - segment.row + .5f) * step;
        const float gap = segment.end == note.end ? std::min(hairline, (right - left) * .15f) : 0;
        const float note_width = std::min(melody.x + melody.width - left, std::max(hairline, right - left - gap));
        const float note_height = segment.in_scale ? std::max(hairline, step - 2 * std::clamp(step * .12f, 3.f, 8.f)) : 2.f;
        return {left, center - note_height / 2, note_width, note_height};
    }

    const Window::MelodyNote* Window::note_at(float x, float y) const {
        const MelodyNote* result = nullptr;
        float nearest = std::numeric_limits<float>::infinity();
        const double view_start = timeline.first / audio.clip.sample_rate;
        const double view_end = (timeline.first + timeline.count) / audio.clip.sample_rate;
        for (const auto& segment : melody_notes) {
            if (segment.end <= view_start || segment.start >= view_end || analysis->notes[segment.source].track != melody_track) continue;
            const auto rect = note_rect(segment);
            const float dx = std::max({rect.x - x, x - rect.x - rect.width, 0.f});
            const float dy = std::abs(y - rect.y - rect.height / 2);
            const float distance = dx * dx + dy * dy;
            if (dx <= 3 && dy <= std::max(3.f, rect.height / 2) && distance < nearest) {
                result = &segment;
                nearest = distance;
            }
        }
        return result;
    }

    void Window::locate(double frame) {
        initial_view = false;
        audio.seek(static_cast<std::uint64_t>(frame));
        if (audio.playing.load()) {
            follow_from = timeline.first;
            follow_resume_at = std::chrono::steady_clock::now();
        } else {
            timeline.reveal(frame, audio.clip.frames);
            follow_resume_at.reset();
        }
        refresh();
    }

    void Window::refresh() {
        frames.continuous = loading || audio.playing.load() || analyzing.valid() || analysis_pending || focus_animating || (loaded && (std::ranges::max(meter_db) > ui::meter_floor || std::ranges::max(held_peak_db) >= ui::meter_floor));
        frames.dirty = true;
    }

    void Window::render() {
        auto& draw = *renderer;
        draw.vertices.clear();
        draw.show_wave = loaded;
        draw.wave_layer = 0;
        draw.focus_columns.assign(static_cast<std::size_t>(std::lround(width * draw.scale)), std::array{0.f, 0.f});
        draw.focus_levels = {1, 1};
        draw.focus_strength = 0;
        focus_animating = false;
        if (!loaded) {
            const auto title = loading ? L"Loading audio..." : error.empty() ? L"Drop an audio file" : L"Unable to open audio";
            draw.text(title, ui::secondary_size, 32, height / 2 - 11, drop_active ? ui::accent : ui::foreground, width - 64, true);
            if (!error.empty()) draw.text(error, ui::detail_size, 32, height / 2 + 20, {.94f, .65f, .58f, 1}, width - 64, true);
        } else {
            const auto& clip = audio.clip;
            const bool playing = audio.playing.load();
            const double current_frame = static_cast<double>(audio.position());
            if (playing && (drag == Drag::none || drag == Drag::height || drag == Drag::gain)) {
                const double target = std::clamp(current_frame - timeline.count / 2, 0., static_cast<double>(clip.frames) - timeline.count);
                if (follow_resume_at) {
                    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - *follow_resume_at).count();
                    const double phase = std::clamp(elapsed / std::chrono::duration<double>(ui::follow_duration).count(), 0., 1.);
                    const double blend = phase * phase * phase * (10 + phase * (6 * phase - 15));
                    timeline.first = std::lerp(follow_from, target, blend);
                    if (phase == 1) follow_resume_at.reset();
                } else timeline.first = target;
            }
            const double seconds = current_frame / clip.sample_rate;
            const double duration = static_cast<double>(clip.frames) / clip.sample_rate;
            const double view_start = timeline.first / clip.sample_rate;
            const double view_end = (timeline.first + timeline.count) / clip.sample_rate;
            const double pixels = content.width / (view_end - view_start);
            if (analysis) {
                const auto now = std::chrono::steady_clock::now();
                double transition_phase = 1;
                if (focus_target) {
                    transition_phase = std::clamp(std::chrono::duration<double>(now - focus_started).count() / .36, 0., 1.);
                    const float blend = static_cast<float>(transition_phase * transition_phase * transition_phase * (10 + transition_phase * (6 * transition_phase - 15)));
                    for (std::size_t index = 0; index < bar_focus.size(); ++index) bar_focus[index] = std::lerp(focus_from[index], index == *focus_target ? 1.f : 0.f, blend);
                }
                float fade_phase = std::clamp(std::chrono::duration<float>(now - focus_fade_started).count() / .2f, 0.f, 1.f);
                const float fade_blend = fade_phase * fade_phase * fade_phase * (10 + fade_phase * (6 * fade_phase - 15));
                focus_strength = std::lerp(focus_strength_from, focus_strength_target, fade_blend);
                std::size_t focus_index = analysis->beats.size();
                for (auto beat = std::ranges::upper_bound(analysis->beats, seconds, {}, &musicia::Beat::time); beat != analysis->beats.begin();) {
                    --beat;
                    if (beat->position != 1) continue;
                    focus_index = static_cast<std::size_t>(beat - analysis->beats.begin());
                    break;
                }
                const bool in_bar = focus_index != analysis->beats.size() && seconds < duration;
                if (in_bar && focus_target != focus_index) {
                    if (!focus_target || focus_strength == 0) {
                        std::ranges::fill(bar_focus, 0.f);
                        bar_focus[focus_index] = 1;
                    }
                    focus_from = bar_focus;
                    focus_target = focus_index;
                    focus_started = now;
                    transition_phase = 0;
                }
                const float target = playing && in_bar ? 1.f : 0.f;
                if (target != focus_strength_target) {
                    focus_strength_from = focus_strength;
                    focus_strength_target = target;
                    focus_fade_started = now;
                    fade_phase = 0;
                }
                focus_animating = fade_phase < 1 && focus_strength_from != focus_strength_target;
                if (focus_target && focus_strength > 0 && transition_phase < 1 && focus_from[*focus_target] != 1) focus_animating = true;
                frames.continuous |= focus_animating;
                draw.focus_strength = focus_strength;
                if (focus_strength > 0) {
                    for (std::size_t index = 0; index < bar_focus.size(); ++index) {
                        if (bar_focus[index] == 0) continue;
                        const double start = analysis->beats[index].time;
                        const auto next = std::ranges::find(analysis->beats.begin() + index + 1, analysis->beats.end(), 1, &musicia::Beat::position);
                        const double end = next == analysis->beats.end() ? duration : next->time;
                        std::array<double, 4> bounds{start, start, end, end};
                        for (std::size_t previous = index; previous > 0;) {
                            --previous;
                            if (analysis->beats[previous].position != 1) continue;
                            bounds[0] = analysis->beats[previous].time;
                            break;
                        }
                        if (next != analysis->beats.end()) {
                            const auto after = std::ranges::find(std::next(next), analysis->beats.end(), 1, &musicia::Beat::position);
                            bounds[3] = after == analysis->beats.end() ? duration : after->time;
                        }
                        std::array<double, 4> edges;
                        for (std::size_t edge = 0; edge < edges.size(); ++edge) edges[edge] = content.x + (bounds[edge] - view_start) * pixels;
                        const double feather = std::min(16., (edges[2] - edges[1]) * .5);
                        const double left = std::max(static_cast<double>(content.x), edges[0] - feather / 2);
                        const double right = std::min(static_cast<double>(content.x + content.width), edges[3] + feather / 2);
                        if (right <= left) continue;
                        const auto first_column = static_cast<std::size_t>(std::lround(left * draw.scale));
                        const auto last_column = static_cast<std::size_t>(std::lround(right * draw.scale));
                        const float strength = focus_strength * bar_focus[index];
                        for (std::size_t column = first_column; column < last_column; ++column) {
                            const double x = (static_cast<double>(column) + .5) / draw.scale;
                            const double current = std::min(std::clamp((x - edges[1]) / feather + .5, 0., 1.), std::clamp((edges[2] - x) / feather + .5, 0., 1.));
                            const double surrounding = std::min(std::clamp((x - edges[0]) / feather + .5, 0., 1.), std::clamp((edges[3] - x) / feather + .5, 0., 1.));
                            draw.focus_columns[column][0] += strength * static_cast<float>(current);
                            draw.focus_columns[column][1] += strength * static_cast<float>(surrounding - current);
                        }
                    }
                }
            }
            const float hairline = std::round(ui::beat_width * draw.scale) / draw.scale;
            const bool divider_hover = hovering && wave_divider.contains(hover_x, hover_y);
            const bool tab_hover = hovering && (melody_tabs[0].contains(hover_x, hover_y) || melody_tabs[1].contains(hover_x, hover_y));
            const bool main_hover = hovering && content.contains(hover_x, hover_y) && !divider_hover && !tab_hover;
            const bool overview_hover = hovering && overview.contains(hover_x, hover_y);
            const double hover_time = main_hover ? timeline.frame((hover_x - content.x) / content.width) / clip.sample_rate : (hover_x - overview.x) / overview.width * duration;
            std::wstring hover_label;
            std::wstring hover_range;
            std::wstring context, key_label{L"—"}, bpm_label{L"—"}, meter_label{L"—"};
            ui::Color status_color = ui::muted;
            std::wstring view_label = ui::time_label(view_start, false) + L" — " + ui::time_label(view_end, false);

            draw.text(L"MUSICIA", ui::body_size, ui::outer_margin, 20, ui::foreground, 100);
            draw.rect({118, 25, hairline, 12}, ui::rule_color);
            draw.text(clip.path.stem().wstring(), ui::secondary_size, 136, 19, ui::foreground, width - 168);
            draw.rect({ui::outer_margin, ui::header_height, width - 2 * ui::outer_margin, hairline}, ui::rule_color);
            draw.text(ui::time_label(seconds, true), ui::value_size, width - ui::clock_right, ui::toolbar_y, ui::foreground, 115);
            draw.text(L"/ " + ui::time_label(duration, false), ui::label_size, width - 84, ui::toolbar_y + 6, ui::muted, 56);
            draw.text(L"BARS", ui::label_size, 32, content.y + 3, ui::muted, 64);
            for (std::size_t lane = 0; lane < lanes.size(); ++lane) draw.text(std::array{L"FORM", L"KEY", L"CHORDS"}[lane], ui::label_size, 32, lanes[lane].y + lanes[lane].height / 2 - 8, ui::muted, 64);
            draw.rect({content.x, content.y + 30, content.width, hairline}, ui::rule_color);
            for (std::uint32_t channel = 0; channel < clip.channels; ++channel) {
                const float center = waveform.y + waveform.height / clip.channels * (channel + .5f);
                draw.text(clip.channels == 1 ? L"M" : channel == 0 ? L"L" : L"R", ui::label_size, 51, center - 8, ui::muted, 16);
                draw.focus_levels = {.10f, .25f};
                draw.rect({waveform.x, center, waveform.width, hairline}, ui::pitch_grid);
                draw.focus_levels = {1, 1};
            }
            draw.text(L"OVERVIEW", ui::label_size, 32, overview.y + 13, ui::muted, 80);
            if (divider_hover || drag == Drag::height) draw.rect({wave_divider.x, wave_divider.y + wave_divider.height / 2, wave_divider.width, hairline}, ui::muted);
            for (int track = 0; track < 2; ++track) {
                const auto& tab = melody_tabs[track];
                const bool active = track == melody_track;
                const bool hovered = hovering && tab.contains(hover_x, hover_y);
                draw.text(track == 0 ? L"VOCAL" : L"INST", ui::label_size, tab.x + 12, tab.y + 6, analysis && (active || hovered) ? ui::foreground : ui::muted, tab.width - 12);
                const auto color = ui::note_colors[track];
                draw.rect({tab.x + 1, tab.y + 12, 5, 5}, {color.r, color.g, color.b, analysis && active ? 1.f : .35f}, 2.5f);
            }
            const double label_time = main_hover ? hover_time : seconds;
            const MelodyPassage* label_passage = nullptr;
            double label_distance = std::numeric_limits<double>::infinity();
            draw.focus_levels = {.10f, .25f};
            for (const auto& passage : melody_passages) {
                if (passage.end <= view_start || passage.start >= view_end || passage.low[melody_track] > passage.high[melody_track]) continue;
                const double distance = std::max({passage.start - label_time, label_time - passage.end, 0.});
                if (distance <= label_distance) {
                    label_passage = &passage;
                    label_distance = distance;
                }
                const float x = melody.x + static_cast<float>(timeline.fraction(std::max(passage.start, view_start) * clip.sample_rate) * melody.width);
                const float right = melody.x + static_cast<float>(timeline.fraction(std::min(passage.end, view_end) * clip.sample_rate) * melody.width);
                const float step = melody.height / (passage.high[melody_track] - passage.low[melody_track] + 1);
                for (int row = static_cast<int>(std::ceil(passage.low[melody_track])); row <= static_cast<int>(std::floor(passage.high[melody_track])); ++row) {
                    const float y = melody.y + (passage.high[melody_track] - row) * step;
                    const bool tonic = row % 7 == 0;
                    const float top = std::max(melody.y, y), bottom = std::min(melody.y + melody.height, y + step);
                    if (tonic) draw.rect({x, top, right - x, bottom - top}, ui::tonic_row_color);
                    draw.rect({x, bottom - hairline, right - x, hairline}, ui::pitch_grid);
                }
            }
            draw.focus_levels = {1, 1};
            if (label_passage) {
                constexpr std::array names{L"C", L"C#", L"D", L"Eb", L"E", L"F", L"F#", L"G", L"Ab", L"A", L"Bb", L"B"};
                const auto passage_index = static_cast<std::size_t>(label_passage - melody_passages.data());
                const float step = melody.height / (label_passage->high[melody_track] - label_passage->low[melody_track] + 1);
                for (int row = static_cast<int>(std::ceil(label_passage->low[melody_track] * 2)); row <= static_cast<int>(std::floor(label_passage->high[melody_track] * 2)); ++row) {
                    const bool in_scale = row % 2 == 0;
                    if (!in_scale && (step < 22 || std::ranges::none_of(melody_notes, [&](const auto& note) { return note.passage == passage_index && analysis->notes[note.source].track == melody_track && note.row == row * .5f; }))) continue;
                    const float center = melody.y + (label_passage->high[melody_track] - row * .5f + .5f) * step;
                    const int octave = static_cast<int>(std::floor(row / 14.));
                    const int degree = row - octave * 14;
                    const int pitch = octave * 12 + label_passage->pitch_offsets[degree];
                    const auto label = std::format(L"{}{}  ·  {}", names[pitch % 12], pitch / 12 - 1, label_passage->solfege_labels[degree]);
                    const bool sounding = std::ranges::any_of(melody_notes, [&](const auto& note) { return note.passage == passage_index && analysis->notes[note.source].track == melody_track && note.row == row * .5f && seconds >= note.start && seconds < note.end; });
                    draw.text(label, ui::label_size, ui::label_x, center - 7.5f, sounding ? ui::note_active_colors[melody_track] : row % 14 == 0 ? ui::foreground : ui::muted, melody.x - 35);
                }
            }

            if (analysis) {
                draw.focus_levels = {.15f, .30f};
                constexpr std::array chord_font_sizes{96.f, 80.f, 64.f, 48.f, 36.f, 28.f, 24.f};
                const float maximum_chord_size = std::min(96.f, melody.height * .2f);
                for (std::size_t index = 0; index < chord_segments.size(); ++index) {
                    const auto& chord = chord_segments[index];
                    double chord_end = chord.end;
                    while (index + 1 < chord_segments.size() && chord_segments[index + 1].name == chord.name && chord_segments[index + 1].numeral == chord.numeral && std::abs(chord_segments[index + 1].start - chord_end) < .001) chord_end = chord_segments[++index].end;
                    if (chord.start >= view_end) break;
                    if (chord_end <= view_start || chord.numeral.empty()) continue;
                    auto next_bar = std::ranges::upper_bound(analysis->beats, chord.start, {}, &musicia::Beat::time);
                    for (double start = chord.start; start < chord_end && start < view_end;) {
                        next_bar = std::ranges::find_if(next_bar, analysis->beats.end(), [](const auto& beat) { return beat.position == 1; });
                        const double end = next_bar == analysis->beats.end() ? chord_end : std::min(chord_end, next_bar->time);
                        if (end > view_start) {
                            const float left = melody.x + static_cast<float>((std::max(start, view_start) - view_start) * pixels);
                            const float right = melody.x + static_cast<float>((std::min(end, view_end) - view_start) * pixels);
                            const float center = melody.x + static_cast<float>((std::midpoint(start, end) - view_start) * pixels);
                            const float text_width = 2 * std::min(center - left, right - center) - 24;
                            if (text_width >= chord_font_sizes.back()) {
                                float emphasis = 0;
                                for (auto beat = next_bar; beat != analysis->beats.begin();) {
                                    --beat;
                                    if (beat->position != 1) continue;
                                    emphasis = focus_strength * bar_focus[static_cast<std::size_t>(beat - analysis->beats.begin())];
                                    break;
                                }
                                const float chord_light = std::lerp(.28f, .50f, emphasis);
                                const float numeral_light = std::lerp(.36f, .58f, emphasis);
                                const ui::Color chord_background_text{chord_light, chord_light, chord_light, 1};
                                const ui::Color numeral_background_text{numeral_light, numeral_light, numeral_light, 1};
                                for (const float font_size : chord_font_sizes) {
                                    if (font_size > maximum_chord_size) continue;
                                    const float numeral_size = std::max(14.f, font_size * .5f);
                                    const float line_gap = std::max(4.f, font_size * .08f);
                                    const float text_y = melody.y + (melody.height - (font_size + numeral_size) * 1.4f - line_gap) / 2;
                                    const auto first_vertex = draw.vertices.size();
                                    if (draw.text(chord.name, font_size, center - text_width / 2, text_y, chord_background_text, text_width, true, true) > text_width) continue;
                                    if (draw.text(chord.numeral, numeral_size, center - text_width / 2, text_y + font_size * 1.4f + line_gap, numeral_background_text, text_width, true, true) <= text_width) break;
                                    draw.vertices.resize(first_vertex);
                                }
                            }
                        }
                        start = end;
                        if (next_bar != analysis->beats.end()) ++next_bar;
                    }
                }
                draw.focus_levels = {1, 1};
                int bar = 0, first_bar = 0, last_bar = 0;
                float last_label_x = -std::numeric_limits<float>::infinity();
                float last_bar_x = -std::numeric_limits<float>::infinity();
                constexpr std::array grid_colors{ui::bar_grid, ui::beat_grid, ui::eighth_grid, ui::sixteenth_grid};
                for (std::size_t index = 0; index < analysis->beats.size(); ++index) {
                    const auto& beat = analysis->beats[index];
                    if (beat.position == 1) ++bar;
                    if (beat.time <= view_start) first_bar = bar;
                    if (beat.time < view_end) last_bar = bar;
                    const bool last_beat = index + 1 == analysis->beats.size();
                    const double next_time = last_beat ? beat.time : analysis->beats[index + 1].time;
                    if (next_time < view_start || beat.time >= view_end) continue;
                    const double sixteenth = (next_time - beat.time) * beat.denominator / 16;
                    const bool compound = beat.denominator == 8 && beat.numerator >= 6 && beat.numerator % 3 == 0;
                    for (int division = 0; division == 0 || (!last_beat && division * beat.denominator < 16); ++division) {
                        const double time = beat.time + sixteenth * division;
                        if (time < view_start || time >= view_end) continue;
                        const bool downbeat = division == 0 && beat.position == 1;
                        const bool pulse = division == 0 && (!compound || (beat.position - 1) % 3 == 0);
                        const int level = downbeat ? 0 : pulse || (division > 0 && division % 4 == 0) ? 1 : division % 2 == 0 ? 2 : 3;
                        const float x = static_cast<float>(std::round((content.x + timeline.fraction(time * clip.sample_rate) * content.width) * draw.scale) / draw.scale);
                        if (downbeat && x - last_label_x >= 48) {
                            const float text_width = std::min(44.f, content.x + content.width - x - 6);
                            const float emphasis = focus_strength * bar_focus[index];
                            const float brightness = 1 - .45f * (focus_strength - emphasis);
                            const ui::Color label_color{std::lerp(ui::muted.r, ui::accent.r, emphasis) * brightness, std::lerp(ui::muted.g, ui::accent.g, emphasis) * brightness, std::lerp(ui::muted.b, ui::accent.b, emphasis) * brightness, 1};
                            if (text_width > 0) draw.text(std::to_wstring(bar), ui::label_size, x + 6, content.y + 2, label_color, text_width, false, true);
                            last_label_x = x;
                        }
                        if (level == 0) {
                            if (x - last_bar_x < 12) continue;
                            last_bar_x = x;
                        } else if (!last_beat && sixteenth * pixels * (level == 1 ? 4 : level == 2 ? 2 : 1) < 12) continue;
                        const float mark_width = std::round((level == 0 ? ui::bar_width : level == 1 ? ui::beat_width : ui::dot_width) * draw.scale) / draw.scale;
                        const float mark_left = std::round((x - mark_width / 2) * draw.scale) / draw.scale;
                        const float left = std::max(content.x, mark_left);
                        const float right = std::min(content.x + content.width, mark_left + mark_width);
                        const auto color = grid_colors[level];
                        if (level < 2) draw.rect({left, content.y + (downbeat ? 23.f : 25.f), right - left, downbeat ? 7.f : 5.f}, downbeat ? ui::muted : ui::rule_color);
                        draw.focus_levels = {.10f, .25f};
                        if (level < 2) {
                            draw.rect({left, waveform.y, right - left, waveform.height}, {color.r, color.g, color.b, .55f});
                            draw.rect({left, melody.y, right - left, melody.height}, color);
                        } else for (const auto& passage : melody_passages) {
                            if (time < passage.start || time >= passage.end || passage.low[melody_track] > passage.high[melody_track]) continue;
                            const float step = melody.height / (passage.high[melody_track] - passage.low[melody_track] + 1);
                            for (int row = static_cast<int>(std::ceil(passage.low[melody_track])); row <= static_cast<int>(std::floor(passage.high[melody_track])); ++row) {
                                const float center = melody.y + (passage.high[melody_track] - row + .5f) * step;
                                const float top = std::max(melody.y, std::round((center - mark_width / 2) * draw.scale) / draw.scale);
                                const float bottom = std::min(melody.y + melody.height, top + mark_width);
                                draw.rect({left, top, right - left, bottom - top}, color);
                            }
                        }
                        draw.focus_levels = {1, 1};
                    }
                }
                view_label = last_bar ? std::format(L"Bars {}–{}", std::max(1, first_bar), last_bar) : L"Before bar 1";
                const std::array tracks{&analysis->sections, &analysis->keys, &analysis->chords};
                constexpr std::array annotation_font_sizes{21.f, 19.f, 17.f, 15.f, 13.f, 11.f};
                constexpr ui::Color annotation_boundary{53 / 255.f, 53 / 255.f, 53 / 255.f, 1};
                for (std::size_t lane = 0; lane < tracks.size(); ++lane) {
                    const auto& area = lanes[lane];
                    const auto& intervals = *tracks[lane];
                    const auto count = lane == 2 ? chord_segments.size() : intervals.size();
                    for (std::size_t i = 0; i < count; ++i) {
                        const double start = lane == 2 ? chord_segments[i].start : intervals[i].start;
                        double end = lane == 2 ? chord_segments[i].end : intervals[i].end;
                        const auto label = lane == 2 ? chord_segments[i].name : interval_label(intervals[i].label, lane);
                        const std::wstring_view numeral = lane == 2 ? std::wstring_view(chord_segments[i].numeral) : std::wstring_view{};
                        const auto full_label = numeral.empty() ? label : std::format(L"{} · {}", label, numeral);
                        if (lane == 0) {
                            while (i + 1 < intervals.size() && intervals[i + 1].label == intervals[i].label && std::abs(intervals[i + 1].start - end) < .001) end = intervals[++i].end;
                        }
                        const bool active = seconds >= start && (seconds < end || (current_frame == clip.frames && end >= duration - .001));
                        if (active && lane == 1) key_label = label;
                        else if (active) {
                            if (!context.empty()) context += L"   ·   ";
                            context += full_label;
                        }
                        const bool hovered = main_hover && area.contains(hover_x, hover_y) && hover_time >= start && hover_time < end;
                        if (hovered) {
                            hover_label = full_label;
                            hover_range = ui::time_label(start, true) + L" — " + ui::time_label(end, true);
                        }
                        if (end <= view_start || start >= view_end) continue;
                        const float x = content.x + static_cast<float>(timeline.fraction(std::max(start, view_start) * clip.sample_rate) * content.width);
                        const float right = content.x + static_cast<float>(timeline.fraction(std::min(end, view_end) * clip.sample_rate) * content.width);
                        const float segment_width = right - x;
                        const float brightness = active || hovered ? 1.f : std::lerp(1.f, .5f, focus_strength);
                        if (start >= view_start) {
                            const float boundary_x = std::max(area.x, std::round(x * draw.scale) / draw.scale);
                            const float boundary_width = std::min(right - boundary_x, std::round((active ? 2.f : 1.f) * draw.scale) / draw.scale);
                            const auto boundary_color = hovered ? ui::foreground : active ? ui::accent : annotation_boundary;
                            if (boundary_width > 0) draw.rect({boundary_x, area.y + 8, boundary_width, area.height - 16}, {boundary_color.r * brightness, boundary_color.g * brightness, boundary_color.b * brightness, 1});
                        }
                        const float padding = segment_width < 64 ? 4.f : 12.f;
                        const float text_width = segment_width - padding - (segment_width < 64 ? 4.f : 8.f);
                        if (text_width < ui::label_size) continue;
                        auto display_label = label;
                        if (lane == 0) for (auto& character : display_label) character = static_cast<wchar_t>(std::towupper(character));
                        const float maximum_font_size = lane == 0 ? ui::label_size : lane == 1 ? 19.f : 21.f;
                        const auto base_color = active ? ui::accent : hovered ? ui::foreground : ui::muted;
                        const ui::Color text_color{base_color.r * brightness, base_color.g * brightness, base_color.b * brightness, 1};
                        for (const float font_size : annotation_font_sizes) {
                            if (font_size > maximum_font_size) continue;
                            const float numeral_height = numeral.empty() ? 0.f : ui::label_size * 1.4f + 2;
                            const float text_y = area.y + (area.height - font_size * 1.4f - numeral_height) / 2;
                            const bool fit = font_size != annotation_font_sizes.back();
                            const float advance = draw.text(display_label, font_size, x + padding, text_y, text_color, text_width, false, fit);
                            if (fit && advance > text_width) continue;
                            if (!numeral.empty()) draw.text(numeral, ui::label_size, x + padding, text_y + font_size * 1.4f + 2, text_color, text_width);
                            break;
                        }
                    }
                }
                const auto* hovered_note = main_hover && melody.contains(hover_x, hover_y) ? note_at(hover_x, hover_y) : nullptr;
                const auto draw_note_label = [&](const MelodyNote& segment, Rect rect) {
                    if (!segment.in_scale) return;
                    const auto& passage = melody_passages[segment.passage];
                    const float step = melody.height / (passage.high[melody_track] - passage.low[melody_track] + 1);
                    const float font_size = std::clamp(step * .36f, 11.f, 20.f);
                    const float text_height = font_size * 1.4f;
                    const float text_width = rect.width - 18;
                    if (text_width <= 0 || rect.height < text_height + 4) return;
                    const float text_y = rect.y + (rect.height - text_height) / 2;
                    const std::wstring_view label = segment.label;
                    const auto split = label.find(L' ');
                    const float advance = draw.text(label.substr(0, split), font_size, rect.x + 9, text_y, ui::note_text_color, text_width, false, true);
                    if (advance < text_width) draw.text(label.substr(split), font_size, rect.x + 9 + advance, text_y, {ui::note_text_color.r, ui::note_text_color.g, ui::note_text_color.b, .72f}, text_width - advance, false, true);
                };
                draw.focus_levels = {.20f, .40f};
                for (const auto& segment : melody_notes) {
                    if (segment.end <= view_start || segment.start >= view_end) continue;
                    const auto& note = analysis->notes[segment.source];
                    if (note.track != melody_track) continue;
                    const auto rect = note_rect(segment);
                    const bool sounding = seconds >= segment.start && seconds < segment.end;
                    const auto color = sounding ? ui::note_active_colors[note.track] : ui::note_colors[note.track];
                    const float radius = segment.in_scale ? std::min({ui::note_radius, rect.height / 2, rect.width / 2}) : 0;
                    const float border_width = std::round((sounding ? ui::note_active_border_width : ui::note_border_width) * draw.scale) / draw.scale;
                    draw.rect(rect, segment.in_scale ? sounding ? ui::note_active_border : ui::note_border_colors[note.track] : color, radius);
                    if (segment.in_scale && rect.width > 2 * border_width && rect.height > 2 * border_width) draw.rect({rect.x + border_width, rect.y + border_width, rect.width - 2 * border_width, rect.height - 2 * border_width}, color, std::max(0.f, radius - border_width));
                    draw_note_label(segment, rect);
                }
                draw.focus_levels = {1, 1};
                if (hovered_note) {
                    const auto hovered_rect = note_rect(*hovered_note);
                    const auto& note = analysis->notes[hovered_note->source];
                    const bool sounding = seconds >= hovered_note->start && seconds < hovered_note->end;
                    const auto color = sounding ? ui::note_active_colors[note.track] : ui::note_hover_colors[note.track];
                    const float radius = hovered_note->in_scale ? std::min({ui::note_radius, hovered_rect.height / 2, hovered_rect.width / 2}) : 0;
                    const float border_width = std::round((sounding ? ui::note_active_border_width : ui::note_border_width) * draw.scale) / draw.scale;
                    draw.rect(hovered_rect, hovered_note->in_scale ? ui::note_active_border : color, radius);
                    if (hovered_note->in_scale && hovered_rect.width > 2 * border_width && hovered_rect.height > 2 * border_width) draw.rect({hovered_rect.x + border_width, hovered_rect.y + border_width, hovered_rect.width - 2 * border_width, hovered_rect.height - 2 * border_width}, color, std::max(0.f, radius - border_width));
                    draw_note_label(*hovered_note, hovered_rect);
                    hover_label = std::format(L"{}   ·   {}   ·   {}", hovered_note->label, hovered_note->name, note.track == 0 ? L"Vocal" : L"Instrumental");
                    hover_range = std::format(L"{} — {}   ·   {:.3f} s", ui::time_label(note.start, true), ui::time_label(note.end, true), note.end - note.start);
                }
                if (focus_strength > 0) {
                    const float underline_height = std::round(2 * draw.scale) / draw.scale;
                    for (std::size_t index = 0; index < bar_focus.size(); ++index) {
                        if (bar_focus[index] == 0) continue;
                        const auto next = std::ranges::find(analysis->beats.begin() + index + 1, analysis->beats.end(), 1, &musicia::Beat::position);
                        const double end = next == analysis->beats.end() ? duration : next->time;
                        const float left = std::clamp(content.x + static_cast<float>((analysis->beats[index].time - view_start) * pixels), content.x, content.x + content.width);
                        const float right = std::clamp(content.x + static_cast<float>((end - view_start) * pixels), content.x, content.x + content.width);
                        if (right > left) draw.rect({left, content.y + 30 - underline_height, right - left, underline_height}, {ui::accent.r, ui::accent.g, ui::accent.b, focus_strength * bar_focus[index]});
                    }
                }
                const auto beat = std::ranges::upper_bound(analysis->beats, seconds, {}, &musicia::Beat::time);
                if (beat != analysis->beats.begin()) {
                    const auto& meter = *std::prev(beat);
                    meter_label = std::format(L"{}/{}", meter.numerator, meter.denominator);
                }
                bpm_label = std::format(L"{:.1f}", analysis->bpm);
            } else if (!analysis_error.empty()) {
                context = analysis_error;
                status_color = ui::overload_color;
            } else {
                const auto stage = progress.stage.load();
                context = stage == musicia::AnalysisStage::loading ? L"Loading model..." : std::format(L"Analyzing {} / {}", progress.window.load(), progress.windows.load());
            }
            draw.text(key_label, ui::value_size, ui::key_x, ui::toolbar_y, key_label == L"—" ? ui::muted : ui::foreground, 160);
            draw.text(bpm_label, ui::value_size, ui::tempo_x, ui::toolbar_y, bpm_label == L"—" ? ui::muted : ui::foreground, 60);
            draw.text(L"BPM", ui::unit_size, ui::tempo_unit_x, ui::toolbar_y + 7, ui::muted, 35);
            draw.text(meter_label, ui::secondary_size, ui::meter_x, ui::toolbar_y + 2, ui::muted, 60);
            draw.rect({ui::outer_margin, height - ui::footer_height, width - 2 * ui::outer_margin, hairline}, ui::rule_color);
            draw.text(context, ui::label_size, ui::label_x, height - 28, status_color, width - 680);
            draw.text(view_label, ui::label_size, width - 180, overview.y - 20, ui::muted, 152);
            {
                const auto now = std::chrono::steady_clock::now();
                const float elapsed = std::chrono::duration<float>(now - meter_updated).count();
                for (std::size_t channel = 0; channel < meter_db.size(); ++channel) {
                    const float amplitude = audio.output_peaks[channel].exchange(0.f, std::memory_order_relaxed);
                    const float db = amplitude > 0 ? 20.f * std::log10(amplitude) : -std::numeric_limits<float>::infinity();
                    meter_db[channel] = std::max({ui::meter_floor, db, meter_db[channel] - ui::meter_release * elapsed});
                    if (amplitude > 0 && db >= held_peak_db[channel]) {
                        held_peak_db[channel] = db;
                        peak_hold_until[channel] = now + ui::peak_hold;
                    } else if (now > peak_hold_until[channel]) {
                        const float release = std::chrono::duration<float>(now - std::max(meter_updated, peak_hold_until[channel])).count();
                        held_peak_db[channel] = std::max(db, held_peak_db[channel] - ui::peak_release * release);
                        if (held_peak_db[channel] < ui::meter_floor) held_peak_db[channel] = -std::numeric_limits<float>::infinity();
                    }
                }
                meter_updated = now;
                draw.text(std::format(L"{} Hz  ·  {} samples", audio.sample_rate, audio.period), ui::unit_size, width - ui::output_info_right, height - 27, ui::muted, 160);
                const Rect gain_area{width - ui::master_right, height - 40, 90, 36};
                const bool gain_hover = hovering && drag == Drag::none && gain_area.contains(hover_x, hover_y);
                if (drag == Drag::gain || gain_hover) draw.rect(gain_area, ui::active_surface, ui::control_radius);
                draw.text(L"MASTER", ui::micro_size, width - ui::master_right + 8, height - 36, ui::muted, 76);
                const float gain_db = audio.master_db.load(std::memory_order_relaxed);
                draw.text(gain_db == minimum_master_db ? L"−∞ dB" : std::format(L"{:.1f} dB", gain_db == 0 ? 0.f : gain_db), ui::detail_size, width - ui::master_right + 8, height - 23, drag == Drag::gain || gain_hover ? ui::accent : ui::foreground, 76);
                const float meter_x = width - ui::output_meter_right, meter_width = ui::output_meter_width;
                for (std::size_t channel = 0; channel < meter_db.size(); ++channel) {
                    const float y = height - 32 + static_cast<float>(channel) * 8;
                    const float filled = meter_width * std::clamp((meter_db[channel] - ui::meter_floor) / -ui::meter_floor, 0.f, 1.f);
                    draw.text(channel == 0 ? L"L" : L"R", ui::caption_size, meter_x - 15, y - 4, ui::muted, 10);
                    draw.rect({meter_x, y, meter_width, 4}, ui::rule_color, 1);
                    if (filled > 0) draw.rect({meter_x, y, std::min(filled, meter_width * .9f), 4}, ui::accent, 1);
                    if (filled > meter_width * .9f) draw.rect({meter_x + meter_width * .9f, y, filled - meter_width * .9f, 4}, meter_db[channel] > 0 ? ui::overload_color : ui::warning_color, 1);
                    if (held_peak_db[channel] >= ui::meter_floor) {
                        const float held_x = meter_x + (meter_width - 1) * std::clamp((held_peak_db[channel] - ui::meter_floor) / -ui::meter_floor, 0.f, 1.f);
                        draw.rect({held_x, y - 1, 1, 6}, held_peak_db[channel] > 0 ? ui::overload_color : ui::foreground);
                    }
                }
                for (const int db : {-60, -36, -12, 0}) {
                    const float x = meter_x + meter_width * (db + 60.f) / 60.f;
                    draw.text(std::to_wstring(db), ui::caption_size, x - 12, height - 15, ui::muted, 24, true);
                }
                const float peak_db = std::ranges::max(held_peak_db);
                draw.text(L"PEAK dBFS", ui::caption_size, width - ui::peak_right, height - 36, ui::muted, 70);
                draw.text(peak_db == -std::numeric_limits<float>::infinity() ? L"−∞" : std::format(L"{:.1f}", peak_db), ui::detail_size, width - ui::peak_right, height - 23, peak_db > 0 ? ui::overload_color : ui::foreground, 70);
                const bool overloaded = audio.overloaded.load(std::memory_order_relaxed);
                const Rect overload_area{width - ui::overload_right, height - 38, 46, 32};
                if (overloaded) draw.rect(overload_area, ui::overload_surface, ui::control_radius);
                else if (hovering && drag == Drag::none && overload_area.contains(hover_x, hover_y)) draw.rect(overload_area, ui::hover_surface, ui::control_radius);
                draw.text(L"OVER", ui::unit_size, width - ui::overload_right, height - 27, overloaded ? ui::overload_color : ui::muted, 46, true);
            }
            const float view_left = overview.x + static_cast<float>(timeline.first / clip.frames * overview.width);
            const float view_right = overview.x + static_cast<float>((timeline.first + timeline.count) / clip.frames * overview.width);
            draw.rect({view_left, overview.y, view_right - view_left, overview.height}, {ui::accent.r, ui::accent.g, ui::accent.b, .09f});
            draw.wave_layer = static_cast<std::uint32_t>(draw.vertices.size());
            draw.wave_rect = waveform;
            draw.overview_rect = overview;
            draw.wave_first = timeline.first;
            draw.wave_count = timeline.count;

            draw.rect({overview.x, overview.y, view_left - overview.x, overview.height}, {ui::background.r, ui::background.g, ui::background.b, .35f});
            draw.rect({view_right, overview.y, overview.x + overview.width - view_right, overview.height}, {ui::background.r, ui::background.g, ui::background.b, .35f});
            draw.rect({view_left, overview.y, view_right - view_left, hairline}, ui::accent);
            draw.rect({view_left, overview.y + overview.height - hairline, view_right - view_left, hairline}, ui::accent);
            draw.rect({view_left, overview.y, hairline, overview.height}, ui::accent);
            draw.rect({view_right - hairline, overview.y, hairline, overview.height}, ui::accent);
            if (overview_hover || drag == Drag::pan_overview || drag == Drag::view || drag == Drag::left || drag == Drag::right) {
                draw.rect({view_left - 1, overview.y + 14, 2, 14}, ui::accent, 1);
                draw.rect({view_right - 1, overview.y + 14, 2, 14}, ui::accent, 1);
            }
            if (analysis) {
                draw.focus_levels = {.10f, .25f};
                for (std::size_t key = 1; key < analysis->keys.size(); ++key) {
                    const auto& change = analysis->keys[key];
                    if (change.label == analysis->keys[key - 1].label || change.start < view_start || change.start >= view_end) continue;
                    const float x = static_cast<float>(std::round((content.x + timeline.fraction(change.start * clip.sample_rate) * content.width) * draw.scale) / draw.scale);
                    draw.rect({x, lanes[1].y, hairline, content.y + content.height - lanes[1].y}, {ui::accent.r, ui::accent.g, ui::accent.b, .35f});
                }
                draw.focus_levels = {1, 1};
            }
            if (main_hover && drag == Drag::none) draw.rect({std::round(hover_x * draw.scale) / draw.scale, content.y, hairline, content.height}, {ui::foreground.r, ui::foreground.g, ui::foreground.b, .3f});
            if (current_frame >= timeline.first && current_frame <= timeline.first + timeline.count) {
                const float playhead_width = std::round(ui::playhead_width * draw.scale) / draw.scale;
                const float x = static_cast<float>(std::round((content.x + timeline.fraction(current_frame) * content.width - playhead_width / 2) * draw.scale) / draw.scale);
                const float left = std::max(content.x, x), right = std::min(content.x + content.width, x + playhead_width);
                draw.rect({left, content.y, right - left, content.height}, ui::accent);
            }
            const float global_x = overview.x + static_cast<float>(current_frame / clip.frames * overview.width);
            draw.rect({global_x, overview.y + 2, hairline, overview.height - 4}, ui::accent);
            if ((main_hover || overview_hover) && drag == Drag::none) {
                const float tooltip_width = hover_label.empty() ? 90.f : 320.f;
                const float tooltip_height = hover_label.empty() ? 29.f : 62.f;
                const float x = std::clamp(hover_x + 13, content.x, content.x + content.width - tooltip_width);
                const float y = main_hover ? (hover_y + 18 + tooltip_height <= content.y + content.height ? hover_y + 18 : hover_y - tooltip_height - 12) : overview.y - tooltip_height - 8;
                draw.rect({x, y, tooltip_width, tooltip_height}, ui::rule_color, ui::popup_radius);
                draw.rect({x + hairline, y + hairline, tooltip_width - 2 * hairline, tooltip_height - 2 * hairline}, ui::surface_color, ui::popup_radius - hairline);
                if (!hover_label.empty()) {
                    draw.text(hover_label, ui::detail_size, x + 10, y + 6, ui::foreground, tooltip_width - 20);
                    draw.text(hover_range, ui::unit_size, x + 10, y + 25, ui::muted, tooltip_width - 20);
                }
                draw.text(ui::time_label(hover_time, true), ui::unit_size, x + 10, y + tooltip_height - 21, ui::accent, tooltip_width - 20);
            }
            if (hovering && drag == Drag::none && Rect{width - ui::output_info_right, height - 40, 160, 36}.contains(hover_x, hover_y)) {
                const Rect area{width - ui::output_info_right, height - 81, 310, 31};
                draw.rect(area, ui::rule_color, ui::popup_radius);
                draw.rect({area.x + 1, area.y + 1, area.width - 2, area.height - 2}, ui::surface_color, ui::popup_radius - 1);
                draw.text(std::format(L"Source · {} Hz · {} {}", clip.sample_rate, clip.channels, clip.channels == 1 ? L"channel" : L"channels"), ui::label_size, area.x + 10, area.y + 8, ui::foreground, area.width - 20);
            }
            if (drop_active) {
                draw.rect(content, {ui::background.r, ui::background.g, ui::background.b, .9f});
                draw.text(L"Drop to replace audio", ui::secondary_size, content.x, content.y + content.height / 2 - 11, ui::foreground, content.width, true);
            }
        }
        draw.draw();
    }

    void Window::cursor() {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(handle, &point);
        const float x = point.x / renderer->scale;
        const float y = point.y / renderer->scale;
        LPCWSTR shape = IDC_ARROW;
        if (drag == Drag::gain || drag == Drag::height) shape = IDC_SIZENS;
        else if (drag == Drag::left || drag == Drag::right) shape = IDC_SIZEWE;
        else if (drag == Drag::pan || drag == Drag::pan_overview) shape = drag_moved ? IDC_HAND : IDC_CROSS;
        else if (drag == Drag::view) shape = IDC_HAND;
        else if (Rect{width - ui::master_right, height - 40, 90, 36}.contains(x, y) || wave_divider.contains(x, y)) shape = IDC_SIZENS;
        else if (Rect{width - ui::overload_right, height - 38, 46, 32}.contains(x, y)) shape = IDC_HAND;
        else if (melody_tabs[0].contains(x, y) || melody_tabs[1].contains(x, y)) shape = analysis ? IDC_HAND : IDC_ARROW;
        else if (overview.contains(x, y)) {
            const float left = overview.x + static_cast<float>(timeline.first / audio.clip.frames * overview.width);
            const float right = overview.x + static_cast<float>((timeline.first + timeline.count) / audio.clip.frames * overview.width);
            shape = std::min(std::abs(x - left), std::abs(x - right)) <= 6 ? IDC_SIZEWE : x > left && x < right ? IDC_HAND : IDC_CROSS;
        } else if (content.contains(x, y)) shape = IDC_CROSS;
        SetCursor(LoadCursorW(nullptr, shape));
    }

    void Window::move_drag(float x, float y) {
        if (drag == Drag::gain) {
            audio.master_db.store(std::clamp(std::round((original_gain + (drag_y - y) * .1f) * 10.f) / 10.f, minimum_master_db, maximum_master_db), std::memory_order_relaxed);
        } else if (drag == Drag::height) {
            wave_height = std::clamp(drag_height + y - drag_y, 64.f, std::max(64.f, height - 606));
            layout();
        } else if (drag != Drag::none) {
            if (!drag_moved && (std::abs(x - drag_x) * renderer->scale >= GetSystemMetricsForDpi(SM_CXDRAG, GetDpiForWindow(handle)) || std::abs(y - drag_y) * renderer->scale >= GetSystemMetricsForDpi(SM_CYDRAG, GetDpiForWindow(handle)))) drag_moved = true;
            if (drag_moved) {
                const double total = static_cast<double>(audio.clip.frames);
                const double delta = static_cast<double>(x - drag_x) / overview.width * total;
                const double minimum = std::min(32., total);
                if (drag == Drag::pan) timeline.first = std::clamp(drag_first - static_cast<double>(x - drag_x) / content.width * drag_count, 0., total - drag_count);
                else if (drag == Drag::left) {
                    timeline.first = std::clamp(drag_first + delta, 0., drag_first + drag_count - minimum);
                    timeline.count = drag_first + drag_count - timeline.first;
                } else if (drag == Drag::right) timeline.count = std::clamp(drag_count + delta, minimum, total - drag_first);
                else if (drag == Drag::view || drag == Drag::pan_overview) timeline.first = std::clamp(drag_first + delta, 0., total - drag_count);
            }
        }
        cursor();
        refresh();
    }

    void Window::finish_drag(bool commit) {
        if (drag == Drag::gain && !commit) audio.master_db.store(original_gain, std::memory_order_relaxed);
        if ((drag == Drag::pan || drag == Drag::pan_overview) && commit && !drag_moved) locate(drag_frame);
        if (drag != Drag::none && drag != Drag::height && drag != Drag::gain) {
            if (audio.playing.load()) {
                follow_from = timeline.first;
                follow_resume_at = std::chrono::steady_clock::now() + (drag_moved ? ui::follow_delay : std::chrono::milliseconds{0});
            } else follow_resume_at.reset();
        }
        drag = Drag::none;
        if (GetCapture() == handle) ReleaseCapture();
        if (handle && renderer && loaded) cursor();
        if (handle) refresh();
    }
    LRESULT Window::message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CONTEXTMENU:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
        case WM_RBUTTONUP:
        case WM_NCRBUTTONDOWN:
        case WM_NCRBUTTONUP:
        case WM_NCRBUTTONDBLCLK: return 0;
        case WM_NCCALCSIZE:
            if (wparam) return 0;
            break;
        case WM_NCHITTEST:
            {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(handle, &point);
                const float scale = renderer ? renderer->scale : GetDpiForWindow(handle) / 96.f;
                const float x     = point.x / scale;
                const float y     = point.y / scale;
                RECT client{};
                GetClientRect(handle, &client);
                if (!IsZoomed(handle)) {
                    const bool left = x < 7, right = x > client.right / scale - 7, top = y < 7, bottom = y > client.bottom / scale - 7;
                    if (top) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
                    if (bottom) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
                    if (left) return HTLEFT;
                    if (right) return HTRIGHT;
                }
                if (y < ui::header_height) return HTCAPTION;
                return HTCLIENT;
            }
        case WM_GETMINMAXINFO:
            {
                auto& info          = *reinterpret_cast<MINMAXINFO*>(lparam);
                const float scale   = renderer ? renderer->scale : GetDpiForWindow(handle) / 96.f;
                info.ptMinTrackSize = {static_cast<LONG>((expanded ? 960 : 380) * scale), static_cast<LONG>((expanded ? 600 : 220) * scale)};
                MONITORINFO monitor{sizeof(MONITORINFO)};
                GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor);
                info.ptMaxPosition = {monitor.rcWork.left - monitor.rcMonitor.left, monitor.rcWork.top - monitor.rcMonitor.top};
                info.ptMaxSize = {monitor.rcWork.right - monitor.rcWork.left, monitor.rcWork.bottom - monitor.rcWork.top};
                return 0;
            }
        case WM_SIZE:
            if (renderer) {
                finish_drag();
                hovering = false;
                if (wparam != SIZE_MINIMIZED) {
                    layout();
                    renderer->resize();
                    refresh();
                }
            }
            return 0;
        case WM_DPICHANGED:
            {
                if (renderer) {
                    finish_drag();
                    hovering = false;
                    renderer->scale = HIWORD(wparam) / 96.f;
                    renderer->reset_fonts();
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
                frames.dirty = false;
                return 0;
            }
        case WM_DISPLAYCHANGE:
            frames.monitor = nullptr;
            frames.dirty = true;
            return 0;
        case WM_APP + 6:
            if (audio.failed.load(std::memory_order_acquire)) throw std::runtime_error(audio.error);
            if (analyzing.valid() && analyzing.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                try {
                    auto completed = analyzing.get();
                    if (!analysis_stop.stop_requested()) {
                        analysis = std::move(completed);
                        bar_focus.assign(analysis->beats.size(), 0.f);
                        focus_from.assign(analysis->beats.size(), 0.f);
                        melody_track = std::ranges::any_of(analysis->notes, [](const auto& note) { return note.track == 0; }) ? 0 : 1;
                        rebuild_analysis_view();
                        if (initial_view) {
                            const auto first = std::ranges::find(analysis->beats, 1, &musicia::Beat::position);
                            if (first != analysis->beats.end()) {
                                const float bar_width = (3440.f / renderer->scale - ui::timeline_x - ui::right_margin) / 6;
                                const int visible_bars = std::max(1, static_cast<int>(std::lround(content.width / bar_width)));
                                double end = static_cast<double>(audio.clip.frames) / audio.clip.sample_rate;
                                int bars = 0;
                                for (auto beat = first; beat != analysis->beats.end(); ++beat) {
                                    if (beat->position != 1 || bars++ != visible_bars) continue;
                                    end = beat->time;
                                    break;
                                }
                                const double start = std::max(0., first->time - .25);
                                timeline.count = std::min(static_cast<double>(audio.clip.frames), (end - start) * audio.clip.sample_rate);
                                timeline.first = std::clamp(start * audio.clip.sample_rate, 0., static_cast<double>(audio.clip.frames) - timeline.count);
                            }
                            initial_view = false;
                        }
                    }
                } catch (const std::exception& exception) {
                    if (!analysis_stop.stop_requested()) {
                        const std::string detail = exception.what();
                        analysis_error.assign(detail.begin(), detail.end());
                    }
                }
            }
            if (loading && loading_clip.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto completed = std::move(loading_clip);
                loading        = false;
                if (!pending_path.empty()) {
                    load(std::exchange(pending_path, {}));
                    return 0;
                }
                try {
                    auto clip = completed.get();
                    analysis_samples = std::make_shared<const std::vector<float>>(std::move(clip.analysis));
                    renderer->upload(clip);
                    audio.open(std::move(clip));
                    timeline.fit(audio.clip.frames);
                    loaded = true;
                    analysis_pending = true;
                    if (!expanded) {
                        expanded = true;
                        MONITORINFO monitor{sizeof(MONITORINFO)};
                        GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor);
                        const LONG w = std::min(static_cast<LONG>(1600 * renderer->scale), monitor.rcWork.right - monitor.rcWork.left - 80);
                        const LONG h = std::min(static_cast<LONG>(980 * renderer->scale), monitor.rcWork.bottom - monitor.rcWork.top - 80);
                        const LONG x = monitor.rcMonitor.left + (monitor.rcWork.right - monitor.rcWork.left - w) / 2;
                        const LONG y = monitor.rcMonitor.top + (monitor.rcWork.bottom - monitor.rcWork.top - h) / 2;
                        WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
                        GetWindowPlacement(handle, &placement);
                        placement.flags = 0;
                        placement.showCmd = SW_SHOWMAXIMIZED;
                        placement.rcNormalPosition = {x, y, x + w, y + h};
                        SetWindowPlacement(handle, &placement);
                    } else ShowWindow(handle, SW_MAXIMIZE);
                    layout();
                } catch (const std::exception& exception) {
                    audio.close();
                    const std::string detail = exception.what();
                    error.assign(detail.begin(), detail.end());
                    loaded = false;
                }
            }
            if (loaded && analysis_pending && !analyzing.valid()) {
                analysis_pending = false;
                analysis_stop = std::stop_source{};
                progress.stage = musicia::AnalysisStage::loading;
                analyzing = std::async(std::launch::async, [this, samples = analysis_samples, stop = analysis_stop.get_token()] {
                    if (!model) model.emplace(std::filesystem::path(MUSICIA_MODEL_DIRECTORY));
                    return model->analyze(*samples, progress, stop);
                });
            }
            render();
            refresh();
            frames.dirty = false;
            return 0;
        case WM_MOUSEMOVE:
            hover_x = GET_X_LPARAM(lparam) / renderer->scale;
            hover_y = GET_Y_LPARAM(lparam) / renderer->scale;
            if (!hovering) {
                TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE, handle, 0};
                TrackMouseEvent(&tracking);
                hovering = true;
            }
            if (drag != Drag::none) move_drag(hover_x, hover_y);
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
                if (Rect{width - ui::master_right, height - 40, 90, 36}.contains(x, y)) {
                    if (message == WM_LBUTTONDBLCLK) audio.master_db.store(0.f, std::memory_order_relaxed);
                    else {
                        drag = Drag::gain;
                        drag_y = y;
                        original_gain = audio.master_db.load(std::memory_order_relaxed);
                        SetCapture(handle);
                    }
                    cursor();
                    refresh();
                    return 0;
                }
                if (Rect{width - ui::overload_right, height - 38, 46, 32}.contains(x, y)) {
                    audio.overloaded.store(false, std::memory_order_relaxed);
                    refresh();
                    return 0;
                }
                for (int track = 0; track < 2; ++track) {
                    if (!melody_tabs[track].contains(x, y)) continue;
                    if (analysis) {
                        melody_track = track;
                        refresh();
                    }
                    return 0;
                }
                if ((wparam & MK_CONTROL) && analysis) {
                    const double time = timeline.frame((x - content.x) / content.width) / audio.clip.sample_rate;
                    const std::array tracks{&analysis->sections, &analysis->keys, &analysis->chords};
                    for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
                        if (!lanes[lane].contains(x, y)) continue;
                        const auto& intervals = *tracks[lane];
                        const auto count = lane == 2 ? chord_segments.size() : intervals.size();
                        for (std::size_t i = 0; i < count; ++i) {
                            const double start = lane == 2 ? chord_segments[i].start : intervals[i].start;
                            double end = lane == 2 ? chord_segments[i].end : intervals[i].end;
                            if (lane == 0) {
                                while (i + 1 < intervals.size() && intervals[i + 1].label == intervals[i].label && std::abs(intervals[i + 1].start - end) < .001) end = intervals[++i].end;
                            }
                            if (time < start || time >= end) continue;
                            locate(start * audio.clip.sample_rate);
                            return 0;
                        }
                        return 0;
                    }
                    if (melody.contains(x, y)) {
                        if (const auto* segment = note_at(x, y)) {
                            locate(analysis->notes[segment->source].start * audio.clip.sample_rate);
                            return 0;
                        }
                    }
                }
                if ((wparam & MK_SHIFT) && analysis && lanes[1].contains(x, y)) {
                    const double time = timeline.frame((x - content.x) / content.width) / audio.clip.sample_rate;
                    for (auto& key : analysis->keys) {
                        if (time < key.start || time >= key.end) continue;
                        constexpr std::string_view letters = "CDEFGAB";
                        constexpr std::array natural{0, 2, 4, 5, 7, 9, 11};
                        const auto root_letter = letters.find(key.label.front());
                        int root = natural[root_letter];
                        for (std::size_t i = 1; i < key.label.find(':'); ++i) root += key.label[i] == '#' ? 1 : -1;
                        const bool minor = key.label.ends_with(":minor");
                        const auto relative_letter = (root_letter + (minor ? 2 : 5)) % 7;
                        const int alteration = ((root + (minor ? 3 : 9) - natural[relative_letter]) % 12 + 18) % 12 - 6;
                        key.label = std::string(1, letters[relative_letter]) + std::string(std::abs(alteration), alteration < 0 ? 'b' : '#') + (minor ? ":major" : ":minor");
                        rebuild_analysis_view();
                        refresh();
                        break;
                    }
                    return 0;
                }
                if (wave_divider.contains(x, y)) {
                    drag = Drag::height;
                    drag_y = y;
                    drag_height = waveform.height;
                    SetCapture(handle);
                    cursor();
                    refresh();
                } else if (overview.contains(x, y)) {
                    const float left = overview.x + static_cast<float>(timeline.first / audio.clip.frames * overview.width);
                    const float right = overview.x + static_cast<float>((timeline.first + timeline.count) / audio.clip.frames * overview.width);
                    if (std::min(std::abs(x - left), std::abs(x - right)) <= 6) drag = std::abs(x - left) <= std::abs(x - right) ? Drag::left : Drag::right;
                    else if (x > left && x < right) drag = Drag::view;
                    else return 0;
                    initial_view = false;
                    follow_resume_at.reset();
                    drag_x = x;
                    drag_y = y;
                    drag_first = timeline.first;
                    drag_count = timeline.count;
                    drag_moved = false;
                    SetCapture(handle);
                    cursor();
                    refresh();
                }
            }
            return 0;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK:
            if (loaded && drag == Drag::none) {
                const auto x = GET_X_LPARAM(lparam) / renderer->scale;
                const auto y = GET_Y_LPARAM(lparam) / renderer->scale;
                if (melody_tabs[0].contains(x, y) || melody_tabs[1].contains(x, y)) return 0;
                if (x >= 28 && x < content.x && y >= content.y && y < content.y + content.height) {
                    SetFocus(handle);
                    timeline.first = 0;
                    locate(0);
                } else if (content.contains(x, y) || overview.contains(x, y)) {
                    SetFocus(handle);
                    initial_view = false;
                    follow_resume_at.reset();
                    const bool in_overview = overview.contains(x, y);
                    drag = in_overview ? Drag::pan_overview : Drag::pan;
                    drag_x = x;
                    drag_y = y;
                    drag_first = timeline.first;
                    drag_count = timeline.count;
                    drag_frame = std::clamp(in_overview ? static_cast<double>(x - overview.x) / overview.width * audio.clip.frames : timeline.frame((x - content.x) / content.width), 0., static_cast<double>(audio.clip.frames));
                    drag_moved = false;
                    SetCapture(handle);
                    cursor();
                    refresh();
                }
            }
            return 0;
        case WM_LBUTTONUP:
            if (drag != Drag::none && drag != Drag::pan && drag != Drag::pan_overview) {
                move_drag(GET_X_LPARAM(lparam) / renderer->scale, GET_Y_LPARAM(lparam) / renderer->scale);
                finish_drag(true);
            }
            return 0;
        case WM_MBUTTONUP:
            if (drag == Drag::pan || drag == Drag::pan_overview) {
                move_drag(GET_X_LPARAM(lparam) / renderer->scale, GET_Y_LPARAM(lparam) / renderer->scale);
                finish_drag(true);
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (drag != Drag::none && reinterpret_cast<HWND>(lparam) != handle) finish_drag();
            return 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (loaded && drag == Drag::none) {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(handle, &point);
                const auto x = point.x / renderer->scale;
                const auto y = point.y / renderer->scale;
                if (message == WM_MOUSEWHEEL && Rect{width - ui::master_right, height - 40, 90, 36}.contains(x, y)) {
                    const float db = audio.master_db.load(std::memory_order_relaxed) + GET_WHEEL_DELTA_WPARAM(wparam) / 120.f * .5f;
                    audio.master_db.store(std::clamp(std::round(db * 10.f) / 10.f, minimum_master_db, maximum_master_db), std::memory_order_relaxed);
                    refresh();
                    return 0;
                }
                if (!content.contains(x, y) || melody_tabs[0].contains(x, y) || melody_tabs[1].contains(x, y)) return 0;
                initial_view = false;
                const auto delta = GET_WHEEL_DELTA_WPARAM(wparam) / 120.0;
                if ((GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL) && message == WM_MOUSEWHEEL) timeline.zoom(std::pow(1.15, -delta), (x - content.x) / content.width, audio.clip.frames);
                else timeline.pan(timeline.count / content.width * delta * (message == WM_MOUSEHWHEEL ? 100 : -100), audio.clip.frames);
                follow_from = timeline.first;
                follow_resume_at = std::chrono::steady_clock::now() + ui::follow_delay;
                refresh();
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT && loaded) {
                cursor();
                return TRUE;
            }
            break;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE && drag != Drag::none) {
                finish_drag();
                return 0;
            }
            if (wparam == VK_F11 && !(lparam & (1LL << 30))) {
                finish_drag();
                ShowWindow(handle, IsZoomed(handle) ? SW_RESTORE : SW_MAXIMIZE);
                return 0;
            }
            if (loaded && drag == Drag::none) {
                if (wparam == VK_SPACE && !(lparam & (1LL << 30))) {
                    initial_view = false;
                    if (audio.position() >= audio.clip.frames) audio.seek(0);
                    audio.playing = !audio.playing.load();
                    if (audio.playing.load()) {
                        follow_from = timeline.first;
                        follow_resume_at = std::chrono::steady_clock::now();
                    } else follow_resume_at.reset();
                    SetEvent(audio.command);
                    refresh();
                } else if (wparam == VK_TAB && analysis && !(lparam & (1LL << 30))) {
                    melody_track = 1 - melody_track;
                    refresh();
                } else if (wparam == '0' && GetKeyState(VK_CONTROL) < 0) {
                    initial_view = false;
                    timeline.fit(audio.clip.frames);
                    follow_resume_at.reset();
                    refresh();
                } else if (wparam == VK_HOME) {
                    locate(0);
                } else if (wparam == VK_LEFT || wparam == VK_RIGHT) {
                    const double frame = std::clamp(static_cast<double>(audio.position()) + (wparam == VK_LEFT ? -1 : 1) * static_cast<double>(audio.clip.sample_rate), 0., static_cast<double>(audio.clip.frames));
                    locate(frame);
                }
            }
            return 0;
        case WM_CANCELMODE:
            finish_drag();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE) {
                finish_drag();
                hovering = false;
            }
            break;
        case WM_CLOSE:
            finish_drag();
            audio.playing = false;
            if (audio.command) SetEvent(audio.command);
            analysis_stop.request_stop();
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            RevokeDragDrop(handle);
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
            app->fatal_error = exception.what();
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
        accepts_drop = SUCCEEDED(object->QueryGetData(&format));
        drop_active  = accepts_drop;
        *effect     = accepts_drop ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        refresh();
        return S_OK;
    }

    HRESULT Window::DragOver(DWORD, POINTL, DWORD* effect) {
        *effect = accepts_drop ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT Window::DragLeave() {
        drop_active = false;
        refresh();
        return S_OK;
    }

    HRESULT Window::Drop(IDataObject* object, DWORD, POINTL, DWORD* effect) {
        drop_active = false;
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
            fatal_error = exception.what();
            PostQuitMessage(1);
            return E_FAIL;
        }
        *effect = DROPEFFECT_COPY;
        return S_OK;
    }
}
