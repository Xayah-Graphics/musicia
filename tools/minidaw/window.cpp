module;
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>
#include <windowsx.h>
#include <wrl/client.h>

module minidaw.window;
import std;
import minidaw.renderer;
import minidaw.document;
import minidaw.engine;

namespace minidaw {
    Window::Window() {
        const WNDCLASSW type{.style = CS_DBLCLKS, .lpfnWndProc = procedure, .hInstance = GetModuleHandleW(nullptr), .hCursor = LoadCursorW(nullptr, IDC_ARROW), .lpszClassName = L"MiniDAW.Editor"};
        RegisterClassW(&type);
        const auto dpi    = GetDpiForSystem();
        const float scale = dpi / 96.f;
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int w     = std::min(static_cast<int>(1600 * scale), static_cast<int>(work.right - work.left - 80));
        const int h     = std::min(static_cast<int>(980 * scale), static_cast<int>(work.bottom - work.top - 80));
        handle          = CreateWindowExW(0, type.lpszClassName, L"MiniDAW", WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w, h, nullptr, nullptr, type.hInstance, this);
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(handle, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        renderer        = std::make_unique<Renderer>(handle);
        renderer->scale = scale;
        RECT client{};
        GetClientRect(handle, &client);
        width  = client.right / scale;
        height = client.bottom / scale;
        PWSTR path{};
        SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path);
        const auto directory = std::filesystem::path(path) / "MiniDAW";
        CoTaskMemFree(path);
        std::filesystem::create_directories(directory);
        engine = std::make_unique<Engine>(handle, directory);
        engine->publish(document.sequence);
        ShowWindow(handle, SW_MAXIMIZE);
        document.pixels_per_beat = grid.width / (4.0 * document.sequence.beats * 4 / document.sequence.beat_unit);
        SetTimer(handle, 1, 16, nullptr);
        paint();
        PostMessageW(handle, WM_APP + 1, 0, 0);
    }
    Window::~Window() {
        finish_tempo_input(false);
        finish_gesture(false);
        engine.reset();
        renderer.reset();
        if (handle) DestroyWindow(handle);
        if ((pencil_cursor && GetCursor() == pencil_cursor) || (copy_cursor && GetCursor() == copy_cursor)) SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        if (pencil_cursor) DestroyCursor(pencil_cursor);
        if (copy_cursor) DestroyCursor(copy_cursor);
    }
    void Window::paint() {
        if (!renderer || IsIconic(handle) || !IsWindowVisible(handle)) return;
        auto& draw = *renderer;
        const auto& sequence = document.sequence;
        const double bar_length = sequence.beats * 4.0 / sequence.beat_unit;
        draw.vertices.clear();
        grid = {132, 132, width - 164, height - 218};
        pedal_lane = {grid.x, grid.y + grid.height, grid.width, 32};
        constrain_pitch_view();
        draw.text(L"MINIDAW", 13, 28, 20, foreground, 100);
        draw.rect({118, 25, 1, 12}, rule_color);
        draw.text(L"PIANO SKETCH", 11, 136, 22, muted, 160);
        draw.text(L"KONTAKT 8", 11, width - 155, 22, engine->ready ? foreground : muted, 100);
        draw.rect({width - 35, 28, 4, 4}, engine->ready ? accent : muted, 2);
        draw.rect({28, 55, width - 56, 1}, rule_color);
        constexpr std::array names{L"C", L"C#", L"D", L"Eb", L"E", L"F", L"F#", L"G", L"Ab", L"A", L"Bb", L"B"};
        draw.text(std::format(L"{} {}", names[sequence.tonic], sequence.minor ? L"Minor" : L"Major"), 17, 30, 71, foreground, 160);
        draw.text(std::to_wstring(sequence.bpm), 17, 210, 71, foreground, 60);
        draw.text(L"BPM", 10, 266, 78, muted, 35);
        draw.text(std::format(L"{}/{}", sequence.beats, sequence.beat_unit), 15, 327, 73, muted, 60);
        constexpr std::array steps{1.0, .5, .25, .125, 2.0 / 3, 1.0 / 3, 1.0 / 6};
        constexpr std::array step_names{L"1/4", L"1/8", L"1/16", L"1/32", L"1/4 T", L"1/8 T", L"1/16 T"};
        draw.text(step_names[std::ranges::find(steps, snap) - steps.begin()], 13, width - 100, 76, muted, 70);
        const float px = static_cast<float>(document.pixels_per_beat);
        const float rh = static_cast<float>(document.row_height);
        const auto x_at = [&](double beat) { return grid.x + static_cast<float>(beat - document.first_beat) * px; };
        const float end_x = grid.x + grid.width, end_y = grid.y + grid.height;
        const auto [lowest, highest] = degree_range();
        const int first_degree = std::min(highest, static_cast<int>(std::ceil(document.top_degree)));
        for (int degree = first_degree; degree >= lowest; --degree) {
            const float top = grid.y + static_cast<float>(document.top_degree - degree) * rh;
            if (top >= end_y) break;
            const float y = std::max(grid.y, top), bottom = std::min(end_y, top + rh);
            if (bottom <= y) continue;
            const bool tonic = degree % 7 == 0;
            if (tonic) draw.rect({grid.x, y, grid.width, bottom - y}, tonic_row_color);
            if (hover == Gesture::audition && grid.x > pointer_x && pointer_y >= y && pointer_y < bottom) draw.rect({28, y, grid.x - 32, bottom - y}, active_surface, 3);
            draw.rect({grid.x, bottom - 1, grid.width, 1}, pitch_grid);
            if (top >= grid.y && top + rh <= end_y) draw.text(document.label(degree), 11, 30, top + (rh - 15) / 2, tonic ? foreground : muted, 97);
        }
        const int snap_divisions = snap == 2.0 / 3 ? 3 : static_cast<int>(std::round(1 / snap));
        const int divisions = std::lcm(snap_divisions, sequence.beat_unit / 4);
        const int bar_steps = sequence.beats * 4 * divisions / sequence.beat_unit;
        const int beat_steps = 4 * divisions / sequence.beat_unit * (sequence.beat_unit == 8 && sequence.beats >= 6 ? 3 : 1);
        int visible_divisions = snap_divisions;
        while (visible_divisions > 1 && px / visible_divisions < 12) visible_divisions = visible_divisions % 2 == 0 ? visible_divisions / 2 : 1;
        const int minor_stride = divisions / visible_divisions;
        const int bar_stride = std::max(1, static_cast<int>(std::ceil(12 / (px * bar_length))));
        const int stride = px >= 12 ? std::gcd(minor_stride, beat_steps) : bar_steps * bar_stride;
        const int label_stride = bar_stride * std::max(1, static_cast<int>(std::ceil(48 / (px * bar_length * bar_stride))));
        const int first_line = static_cast<int>(std::floor(document.first_beat * divisions / stride)) * stride;
        const int last_line = static_cast<int>(std::ceil((document.first_beat + grid.width / px) * divisions / stride)) * stride;
        constexpr std::array mark_sizes{2.f, 1.f, 2.f, 2.f, 1.f};
        constexpr std::array mark_colors{bar_grid, beat_grid, eighth_grid, sixteenth_grid, thirty_second_grid};
        for (int step = first_line; step <= last_line; step += stride) {
            const double beat = static_cast<double>(step) / divisions;
            const float x = x_at(beat);
            const bool bar = step % bar_steps == 0;
            const bool pulse = step % beat_steps == 0;
            if (!bar && !pulse && step % minor_stride != 0) continue;
            const int phase = step % divisions;
            int level{};
            if (bar) level = 0;
            else if (pulse) level = 1;
            else if (divisions % 3 == 0) level = phase % (divisions / 3) == 0 ? 2 : 3;
            else if (phase * 2 % divisions == 0) level = 2;
            else if (phase * 4 % divisions == 0) level = 3;
            else level = 4;
            const float mark_size = std::round(mark_sizes[level] * draw.scale) / draw.scale;
            const float mark_left = std::round((x - mark_size * .5f) * draw.scale) / draw.scale;
            const float left = std::max(grid.x, mark_left), right = std::min(end_x, mark_left + mark_size);
            if (right > left) {
                if (level < 2) draw.rect({left, grid.y, right - left, grid.height}, mark_colors[level]);
                else for (int degree = first_degree; degree >= lowest; --degree) {
                    const float center = grid.y + (static_cast<float>(document.top_degree - degree) + .5f) * rh;
                    const float top = std::round((center - mark_size * .5f) * draw.scale) / draw.scale;
                    if (top >= end_y) break;
                    const float y = std::max(grid.y, top), bottom = std::min(end_y, top + mark_size);
                    if (bottom > y) draw.rect({left, y, right - left, bottom - y}, mark_colors[level]);
                }
            }
            if (bar && step / bar_steps % label_stride == 0 && x + 6 >= grid.x && x + 6 < end_x) draw.text(std::to_wstring(step / bar_steps + 1), 11, x + 6, 113, muted, std::min(45.f, end_x - x - 6));
        }
        constexpr std::array solfege{L"Do", L"Re", L"Mi", L"Fa", L"Sol", L"La", L"Ti"};
        constexpr std::array numerals{L"I", L"II", L"III", L"IV", L"V", L"VI", L"VII"};
        const float note_font_size = std::clamp(rh * .36f, 11.f, 20.f);
        const float note_text_height = note_font_size * 1.4f;
        for (const auto& note : sequence.notes) {
            const int degree = document.degree(note.pitch);
            const float top = grid.y + static_cast<float>(document.top_degree - degree) * rh + 3;
            const float y = std::max(grid.y, top), bottom = std::min(end_y, top + rh - 6);
            const float start = x_at(note.beat);
            const float finish = std::max(start + 4, x_at(note.beat + note.length) - 2);
            const float left = std::max(grid.x, start);
            const float right = std::min(end_x, finish);
            if (right <= left || bottom <= y) continue;
            const bool selected = selection.contains(note.id);
            const bool bass = note.voice == Voice::bass;
            const bool pointing = (gesture == Gesture::none || gesture == Gesture::velocity) && hovered == note.id;
            const Color fill = selected ? (bass ? bass_note_selected_color : treble_note_selected_color) : pointing ? (bass ? bass_note_hover_color : treble_note_hover_color) : (bass ? bass_note_color : treble_note_color);
            const Color border = selected ? note_selected_border_color : bass ? bass_note_border_color : treble_note_border_color;
            const Color text = note_text_color;
            const float border_width = std::round((selected ? 2.5f : 1.5f) * draw.scale) / draw.scale;
            const float radius = std::min({3.f, (right - left) * .5f, (bottom - y) * .5f});
            draw.rect({left, y, right - left, bottom - y}, border, radius);
            if (right - left > 2 * border_width && bottom - y > 2 * border_width) draw.rect({left + border_width, y + border_width, right - left - 2 * border_width, bottom - y - 2 * border_width}, fill, std::max(0.f, radius - border_width));
            const auto edge_gesture = pointing && GetKeyState(VK_CONTROL) >= 0 ? hover : dragged == note.id ? gesture : Gesture::none;
            const float edge_width = std::min(resize_edge_width, (finish - start) * resize_edge_fraction);
            const float cap_left = std::max(left + border_width, finish - edge_width);
            const float cap_right = right - border_width;
            if (finish <= end_x && cap_right > cap_left && bottom - y > 2 * border_width) {
                const bool active_edge = edge_gesture == Gesture::resize_end;
                const bool dragging_edge = active_edge && dragged == note.id && gesture == Gesture::resize_end;
                const float cap_radius = std::min(std::max(0.f, radius - border_width), (cap_right - cap_left) * .5f);
                draw.rect({cap_left, y + border_width, cap_right - cap_left, bottom - y - 2 * border_width}, {0, 0, 0, dragging_edge ? .2f : pointing || active_edge ? .14f : .08f}, cap_radius);
                if (cap_right - cap_left >= 3 && bottom - y > 10) {
                    const float edge = std::max(cap_left, cap_right - 5);
                    const float handle_height = std::min(18.f, bottom - y - 10);
                    draw.rect({edge, y + (bottom - y - handle_height) * .5f, 3, handle_height}, {text.r, text.g, text.b, active_edge ? 1.f : selected ? .8f : .5f}, 1);
                }
            }
            const float text_y = top + (rh - 11 - note_text_height) / 2;
            const float text_width = right - left - 18;
            if (text_width > 0 && text_y >= y + 2 && text_y + note_text_height <= bottom - 7) {
                const int solfege_degree = (degree % 7 + (sequence.minor ? 5 : 0)) % 7;
                const float advance = draw.text(solfege[solfege_degree], note_font_size, left + 9, text_y, text, text_width, false, true);
                if (advance < text_width) draw.text(std::format(L" ({})", numerals[degree % 7]), note_font_size, left + 9 + advance, text_y, {text.r, text.g, text.b, .72f}, text_width - advance, false, true);
            }
            if ((selected || pointing) && bottom - y > 10 && right - left > 12) draw.rect({left + 6, bottom - 5, (right - left - 12) * note.velocity / 127.f, 2}, {text.r, text.g, text.b, .42f}, 1);
        }
        if (gesture == Gesture::marquee && moved) {
            const float ax = std::clamp(x_at(anchor_beat), grid.x, end_x);
            const float ay = std::clamp(grid.y + static_cast<float>(document.top_degree - anchor_row) * rh, grid.y, end_y);
            const float bx = std::clamp(pointer_x, grid.x, end_x), by = std::clamp(pointer_y, grid.y, end_y);
            const Rect area{std::min(ax, bx), std::min(ay, by), std::abs(bx - ax), std::abs(by - ay)};
            draw.rect(area, {.4f, .72f, .9f, .12f}, 2);
            draw.rect({area.x, area.y, area.width, 1}, accent);
            draw.rect({area.x, area.y + area.height, area.width, 1}, accent);
            draw.rect({area.x, area.y, 1, area.height}, accent);
            draw.rect({area.x + area.width, area.y, 1, area.height}, accent);
        }
        draw.rect({28, pedal_lane.y, width - 56, 1}, rule_color);
        const bool sustain = engine->automatic_sustain;
        const Color pedal_color = sustain ? accent : muted;
        draw.text(L"SUSTAIN", 10, 30, pedal_lane.y + 10, pedal_color, 97);
        const float pedal_high = pedal_lane.y + 9, pedal_low = pedal_lane.y + 24;
        const double last_beat = document.first_beat + grid.width / px;
        if (bar_length * px < 2) draw.rect({grid.x, pedal_high, grid.width, 1}, pedal_color);
        else for (double bar = sustain_range(document.first_beat, bar_length).first; bar < last_beat; bar += bar_length) {
            const auto [down, up] = sustain_range(bar, bar_length);
            const float start = x_at(down), release = x_at(up), end = x_at(down + bar_length);
            const float high_left = std::clamp(start, grid.x, end_x), high_right = std::clamp(release, grid.x, end_x);
            const float low_left = std::clamp(release, grid.x, end_x), low_right = std::clamp(end, grid.x, end_x);
            if (high_right > high_left) draw.rect({high_left, pedal_high, high_right - high_left, 1}, pedal_color);
            if (low_right > low_left) draw.rect({low_left, pedal_low, low_right - low_left, 1}, pedal_color);
            if (start >= grid.x && start < end_x) draw.rect({start, pedal_high, 1, pedal_low - pedal_high}, pedal_color);
            if (release >= grid.x && release < end_x) draw.rect({release, pedal_high, 1, pedal_low - pedal_high}, pedal_color);
        }
        const float playhead_width = std::round(4.f * draw.scale) / draw.scale;
        const float playhead = std::round((x_at(engine->position) - playhead_width * .5f) * draw.scale) / draw.scale;
        const float playhead_left = std::max(grid.x, playhead), playhead_right = std::min(end_x, playhead + playhead_width);
        if (playhead_right > playhead_left) draw.rect({playhead_left, 111, playhead_right - playhead_left, pedal_lane.y + pedal_lane.height - 111}, accent);
        draw.rect({28, height - 42, width - 56, 1}, rule_color);
        std::wstring status = std::format(L"Write: {}  ·  {} {}  /  {} selected", input_voice == Voice::treble ? L"Treble" : L"Bass", sequence.notes.size(), sequence.notes.size() == 1 ? L"note" : L"notes", selection.size());
        const auto detail = std::ranges::find(sequence.notes, dragged ? dragged : hovered, &Note::id);
        if (detail != sequence.notes.end()) status = std::format(L"{}  ·  {}  ·  Velocity {}", detail->voice == Voice::treble ? L"Treble" : L"Bass", document.label(document.degree(detail->pitch)), detail->velocity);
        if (std::chrono::steady_clock::now() < toast_until) status = toast;
        else if (!engine->ready) status = L"Loading Kontakt 8…";
        draw.text(status, 11, 30, height - 28, muted, width - 680);
        if (engine->ready) {
            constexpr Color warning_color{235 / 255.f, 195 / 255.f, 138 / 255.f, 1};
            constexpr Color overload_color{1.f, .35f, .32f, 1.f};
            const auto now = std::chrono::steady_clock::now();
            const float elapsed = std::chrono::duration<float>(now - meter_updated).count();
            for (std::size_t channel = 0; channel < meter_db.size(); ++channel) {
                const float amplitude = engine->output_peaks[channel].exchange(0.f, std::memory_order_relaxed);
                const float db = amplitude > 0 ? 20.f * std::log10(amplitude) : -std::numeric_limits<float>::infinity();
                meter_db[channel] = std::max({-60.f, db, meter_db[channel] - 24.f * elapsed});
                if (amplitude > 0 && db >= held_peak_db[channel]) {
                    held_peak_db[channel] = db;
                    peak_hold_until[channel] = now + std::chrono::milliseconds(1500);
                } else if (now > peak_hold_until[channel]) {
                    const float release = std::chrono::duration<float>(now - std::max(meter_updated, peak_hold_until[channel])).count();
                    held_peak_db[channel] = std::max(db, held_peak_db[channel] - 12.f * release);
                    if (held_peak_db[channel] < -60.f) held_peak_db[channel] = -std::numeric_limits<float>::infinity();
                }
            }
            meter_updated = now;
            draw.text(std::format(L"{} Hz  ·  {} samples", engine->sample_rate, engine->period), 10, width - 634, height - 27, muted, 160);
            const Rect gain_area{width - 464, height - 40, 90, 36};
            const bool gain_hover = !help && popup == Popup::none && gain_area.contains(pointer_x, pointer_y);
            if (gesture == Gesture::gain || gain_hover) draw.rect(gain_area, active_surface, 4);
            draw.text(L"MASTER", 9, width - 456, height - 36, muted, 76);
            const float gain_db = engine->master_db.load(std::memory_order_relaxed);
            draw.text(gain_db == minimum_master_db ? L"−∞ dB" : std::format(L"{:.1f} dB", gain_db == 0 ? 0.f : gain_db), 12, width - 456, height - 23, gesture == Gesture::gain || gain_hover ? accent : foreground, 76);
            const float meter_x = width - 331, meter_width = 156;
            for (std::size_t channel = 0; channel < meter_db.size(); ++channel) {
                const float y = height - 32 + static_cast<float>(channel) * 8;
                const float filled = meter_width * std::clamp((meter_db[channel] + 60.f) / 60.f, 0.f, 1.f);
                draw.text(channel == 0 ? L"L" : L"R", 8, meter_x - 15, y - 4, muted, 10);
                draw.rect({meter_x, y, meter_width, 4}, rule_color, 1);
                if (filled > 0) draw.rect({meter_x, y, std::min(filled, meter_width * .9f), 4}, accent, 1);
                if (filled > meter_width * .9f) draw.rect({meter_x + meter_width * .9f, y, filled - meter_width * .9f, 4}, meter_db[channel] > 0 ? overload_color : warning_color, 1);
                if (held_peak_db[channel] >= -60.f) {
                    const float held_x = meter_x + (meter_width - 1) * std::clamp((held_peak_db[channel] + 60.f) / 60.f, 0.f, 1.f);
                    draw.rect({held_x, y - 1, 1, 6}, held_peak_db[channel] > 0 ? overload_color : foreground);
                }
            }
            for (const int db : {-60, -36, -12, 0}) {
                const float x = meter_x + meter_width * (db + 60.f) / 60.f;
                draw.text(std::to_wstring(db), 8, x - 12, height - 15, muted, 24, true);
            }
            const float peak_db = std::ranges::max(held_peak_db);
            draw.text(L"PEAK dBFS", 8, width - 149, height - 36, muted, 70);
            draw.text(peak_db == -std::numeric_limits<float>::infinity() ? L"−∞" : std::format(L"{:.1f}", peak_db), 12, width - 149, height - 23, peak_db > 0 ? overload_color : foreground, 70);
            const bool overloaded = engine->overloaded.load(std::memory_order_relaxed);
            const Rect overload_area{width - 74, height - 38, 46, 32};
            if (overloaded) draw.rect(overload_area, {.2f, .04f, .04f, 1.f}, 4);
            else if (!help && popup == Popup::none && overload_area.contains(pointer_x, pointer_y)) draw.rect(overload_area, hover_surface, 4);
            draw.text(L"OVER", 10, width - 74, height - 27, overloaded ? overload_color : muted, 46, true);
        }
        if (popup != Popup::none) {
            draw.rect(popup_area, rule_color, 7);
            draw.rect({popup_area.x + 1, popup_area.y + 1, popup_area.width - 2, popup_area.height - 2}, surface_color, 6);
            if (popup == Popup::key) {
                for (int i = 0; i < 12; ++i) {
                    const float x = 39.f + (i % 6) * 47, y = 110.f + (i / 6) * 42;
                    if (i == sequence.tonic) draw.rect({x, y, 44, 36}, active_surface, 4);
                    draw.text(names[i], 14, x, y + 8, i == sequence.tonic ? accent : foreground, 44, true);
                }
                draw.text(L"Major", 13, 48, 209, sequence.minor ? muted : accent, 110, true);
                draw.text(L"Minor", 13, 185, 209, sequence.minor ? accent : muted, 110, true);
            } else if (popup == Popup::snap || popup == Popup::meter) {
                for (int i = 0; i < 7; ++i) {
                    const auto [beats, beat_unit] = meter_presets[i];
                    const bool active = popup == Popup::snap ? snap == steps[i] : sequence.beats == beats && sequence.beat_unit == beat_unit;
                    draw.text(popup == Popup::snap ? std::wstring(step_names[i]) : std::format(L"{}/{}", beats, beat_unit), 13, popup_area.x + 14, popup_area.y + 9 + i * 30.f, active ? accent : foreground, popup_area.width - 28);
                }
            }
        }
        if (help) {
            const float left = width / 2 - 310, top = height / 2 - 297.5f;
            draw.rect({left, top, 620, 595}, rule_color, 10);
            draw.rect({left + 1, top + 1, 618, 593}, surface_color, 9);
            draw.text(L"Editing gestures", 21, left + 26, top + 20, foreground, 550);
            const std::array<std::pair<const wchar_t*, const wchar_t*>, 23> lines{{
                {L"Blank drag / Shift + drag", L"Select / add to selection"},
                {L"Ctrl + click / drag", L"Write note / set its length"},
                {L"Note body / right edge", L"Move / resize selected notes"},
                {L"Double-click blank", L"New note"},
                {L"1 / 2 (also assigns selection)", L"Override inherited treble / bass voice"},
                {L"Right-click note", L"Delete that note"},
                {L"Wheel / Shift + wheel", L"Time / pitch scroll"},
                {L"Ctrl / Ctrl + Shift + wheel", L"Time / row height zoom"},
                {L"Middle click / drag", L"Seek / pan"},
                {L"Middle-click pitch labels", L"Return to start"},
                {L"Ctrl + 0 / Ctrl + Shift + 0", L"Fit all / fit selection"},
                {L"Ruler / F", L"Seek / resume centered follow"},
                {L"P", L"Automatic sustain on / off"},
                {L"Space / Enter / Home", L"Play-pause / stop-return / start"},
                {L"R", L"Start from locator / paused position"},
                {L"Arrows / Shift + arrows", L"Grid, degree / bar, octave"},
                {L"Hold Alt during drag / wheel", L"Copy while held / note velocity"},
                {L"Master drag / wheel", L"Output gain (−∞ to +6 dB)"},
                {L"Master double-click / OVER click", L"Reset gain / clear output overload"},
                {L"Ctrl + C / X / V / D", L"Copy / cut / paste / duplicate"},
                {L"Ctrl + Z / Ctrl + Shift + Z", L"Undo / redo"},
                {L"Ctrl + N / Ctrl + E", L"New sequence / export MIDI"},
                {L"F2 / Ctrl + W / Esc", L"Piano / quit / cancel"}
            }};
            for (std::size_t i = 0; i < lines.size(); ++i) {
                draw.text(lines[i].first, 12, left + 26, top + 61 + static_cast<float>(i) * 23, accent, 270);
                draw.text(lines[i].second, 12, left + 306, top + 61 + static_cast<float>(i) * 23, foreground, 290);
            }
        }
        draw.draw();
    }

    void Window::inherit_selection() {
        if (selection.empty()) return;
        const auto& note = *std::ranges::find(document.sequence.notes, *selection.begin(), &Note::id);
        if (selection.size() == 1) last_length = note.length;
        if (std::ranges::none_of(document.sequence.notes, [&](const auto& candidate) { return selection.contains(candidate.id) && candidate.voice != note.voice; })) {
            if (input_voice != note.voice) toast_until = {};
            input_voice = note.voice;
        }
    }
    void Window::changed() {
        inherit_selection();
        engine->publish(document.sequence);
    }
    std::pair<int, int> Window::degree_range() const {
        int lowest{}, highest = highest_pitch / 12 * 7;
        while (document.pitch(lowest) < lowest_pitch) ++lowest;
        while (document.pitch(highest) > highest_pitch) --highest;
        return {lowest, highest};
    }
    void Window::constrain_pitch_view() {
        const auto [lowest, highest] = degree_range();
        const double lowest_top = std::min<double>(highest, lowest - 1 + grid.height / document.row_height);
        document.top_degree = std::clamp(document.top_degree, lowest_top, static_cast<double>(highest));
    }
    void Window::locate(double beat) {
        playback_start = std::max(0.0, std::round(beat / snap) * snap);
        engine->seek = playback_start;
        follow = true;
        follow_after = std::chrono::steady_clock::now() + std::chrono::milliseconds(650);
    }
    void Window::finish_gesture(bool commit) {
        if (gesture == Gesture::none) return;
        const auto operation = std::exchange(gesture, Gesture::none);
        KillTimer(handle, 3);
        if (operation == Gesture::gain) {
            if (!commit) engine->master_db.store(original_gain, std::memory_order_relaxed);
            if (GetCapture() == handle) ReleaseCapture();
            cursor();
            return;
        }
        const bool editing = operation == Gesture::move || operation == Gesture::copy || operation == Gesture::resize_end || operation == Gesture::create || operation == Gesture::velocity;
        if (editing && document.sequence != before) {
            if (commit) {
                document.undo.push_back(std::move(before));
                document.redo.clear();
                changed();
            } else {
                document.sequence = std::move(before);
                engine->publish(document.sequence);
            }
        }
        if (!commit && operation != Gesture::pan && operation != Gesture::seek && operation != Gesture::audition) selection = before_selection;
        if (!commit && operation == Gesture::pan) {
            document.first_beat = original_first;
            document.top_degree = original_top;
        }
        if (commit && operation == Gesture::pan && !moved) locate(anchor_beat);
        if (commit && operation == Gesture::seek) follow_after = std::chrono::steady_clock::now() + std::chrono::milliseconds(650);
        if (commit && dragged) {
            const auto& note = *std::ranges::find(document.sequence.notes, dragged, &Note::id);
            last_length = note.length;
            if (input_voice != note.voice) toast_until = {};
            input_voice = note.voice;
        } else if (commit && operation == Gesture::marquee) inherit_selection();
        engine->preview_held = false;
        engine->preview = -2;
        audition_pitch = -1;
        dragged = 0;
        original_notes.clear();
        if (GetCapture() == handle) ReleaseCapture();
        cursor();
    }
    void Window::finish_tempo_input(bool commit) {
        if (!input) return;
        std::wstring value(GetWindowTextLengthW(input) + 1, L'\0');
        GetWindowTextW(input, value.data(), static_cast<int>(value.size()));
        value.pop_back();
        const bool focused = GetFocus() == input && GetActiveWindow() == handle;
        const auto editor = std::exchange(input, nullptr);
        SetWindowLongPtrW(editor, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(input_procedure));
        DestroyWindow(editor);
        DeleteObject(std::exchange(input_font, nullptr));
        if (focused) SetFocus(handle);
        if (!commit || value.empty() || value == input_original) return;
        const auto previous = document.sequence;
        try {
            const double number = std::stod(value);
            document.sequence.bpm = static_cast<int>(std::clamp(number, 20.0, 400.0));
        } catch (const std::invalid_argument&) {
            toast = L"Enter a number";
            toast_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        } catch (const std::out_of_range&) {
            toast = L"Number is too large";
            toast_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }
        if (document.sequence != previous) {
            document.undo.push_back(previous);
            document.redo.clear();
            changed();
        }
    }
    void Window::pointer(UINT message, float x, float y, WPARAM flags) {
        pointer_x = x;
        pointer_y = y;
        auto& sequence = document.sequence;
        const bool shift = (flags & MK_SHIFT) != 0, control = (flags & MK_CONTROL) != 0;
        const auto [lowest, highest] = degree_range();
        const double raw_beat = document.first_beat + (x - grid.x) / document.pixels_per_beat;
        const double beat = std::max(0.0, raw_beat);
        const double snapped = std::round(beat / snap) * snap;
        const double row = document.top_degree - (y - grid.y) / document.row_height;
        const int degree = std::clamp(static_cast<int>(std::ceil(row)), lowest, highest);
        const auto [target, id] = hit(x, y);
        hover = target;
        hovered = id;
        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK || message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK) {
            finish_tempo_input(true);
            if (gesture == Gesture::velocity) finish_gesture(true);
            if (gesture != Gesture::none) return;
            SetFocus(handle);
            if (help) {
                help = false;
                return;
            }
            if (popup != Popup::none) {
                if (message == WM_LBUTTONDOWN && popup_area.contains(x, y)) {
                    if (popup == Popup::key) {
                        int tonic = sequence.tonic;
                        bool minor = sequence.minor;
                        if (Rect{39, 110, 282, 84}.contains(x, y)) tonic = static_cast<int>((x - 39) / 47) + static_cast<int>((y - 110) / 42) * 6;
                        else if (y >= 198) {
                            minor = x >= 178;
                            popup = Popup::none;
                        }
                        if (tonic != sequence.tonic || minor != sequence.minor) {
                            if (document.change_key(tonic, minor)) {
                                constrain_pitch_view();
                                changed();
                            } else {
                                toast = L"Key change exceeds the piano range (A0-C8)";
                                toast_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                            }
                        }
                    } else if (popup == Popup::snap || popup == Popup::meter) {
                        const int index = std::clamp(static_cast<int>((y - popup_area.y - 4) / 30), 0, 6);
                        const auto [beats, beat_unit] = meter_presets[index];
                        if (popup == Popup::snap) {
                            constexpr std::array steps{1.0, .5, .25, .125, 2.0 / 3, 1.0 / 3, 1.0 / 6};
                            snap = steps[index];
                        } else if (sequence.beats != beats || sequence.beat_unit != beat_unit) {
                            document.checkpoint();
                            sequence.beats = beats;
                            sequence.beat_unit = beat_unit;
                            changed();
                        }
                        popup = Popup::none;
                    }
                    return;
                }
                popup = Popup::none;
                return;
            }
            if (message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK) {
                if (!id) return;
                document.checkpoint();
                std::erase_if(sequence.notes, [&](const auto& note) { return note.id == id; });
                selection.erase(id);
                changed();
                cursor();
                return;
            }
            if (message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK) {
                if (Rect{0, grid.y, grid.x, grid.height}.contains(x, y)) {
                    locate(0);
                    document.first_beat = 0;
                    return;
                }
                if (!grid.contains(x, y) && !pedal_lane.contains(x, y) && !(x >= grid.x && x < grid.x + grid.width && y >= 100 && y < grid.y)) return;
                gesture = Gesture::pan;
                anchor_x = x;
                anchor_y = y;
                anchor_beat = raw_beat;
                original_first = document.first_beat;
                original_top = document.top_degree;
                pan_vertical = grid.contains(x, y);
                moved = false;
                dragged = 0;
                SetCapture(handle);
                cursor();
                return;
            }
            if (y < 55 && x > width - 160) {
                if (engine->ready) engine->show_plugin();
                return;
            }
            if (Rect{28, 65, 160, 35}.contains(x, y)) {
                popup = Popup::key;
                popup_area = {28, 99, 300, 146};
                return;
            }
            if (Rect{200, 65, 100, 35}.contains(x, y)) {
                const Rect area{208, 70, 55, 26};
                const float scale = renderer->scale;
                input_original = std::to_wstring(sequence.bpm);
                input = CreateWindowExW(0, L"EDIT", input_original.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER, static_cast<int>(area.x * scale), static_cast<int>(area.y * scale), static_cast<int>(area.width * scale), static_cast<int>(area.height * scale), handle, nullptr, GetModuleHandleW(nullptr), nullptr);
                input_font = CreateFontW(-static_cast<int>(16 * scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                SendMessageW(input, WM_SETFONT, reinterpret_cast<WPARAM>(input_font), TRUE);
                SendMessageW(input, EM_SETLIMITTEXT, 32, 0);
                SetWindowLongPtrW(input, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
                input_procedure = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(input, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(edit_procedure)));
                SetFocus(input);
                SendMessageW(input, EM_SETSEL, 0, -1);
                return;
            }
            if (Rect{320, 65, 70, 35}.contains(x, y)) {
                popup = Popup::meter;
                popup_area = {320, 99, 90, 218};
                return;
            }
            if (Rect{width - 110, 65, 84, 35}.contains(x, y)) {
                popup = Popup::snap;
                popup_area = {width - 144, 99, 116, 218};
                return;
            }
            if (target == Gesture::gain) {
                if (message == WM_LBUTTONDBLCLK) engine->master_db.store(0.f, std::memory_order_relaxed);
                else {
                    gesture = Gesture::gain;
                    anchor_y = y;
                    original_gain = engine->master_db.load(std::memory_order_relaxed);
                    SetCapture(handle);
                }
                cursor();
                return;
            }
            if (engine->ready && Rect{width - 74, height - 38, 46, 32}.contains(x, y)) {
                engine->overloaded.store(false, std::memory_order_relaxed);
                return;
            }
            if (target == Gesture::none) return;
            before = sequence;
            before_selection = selection;
            anchor_x = x;
            anchor_y = y;
            anchor_beat = raw_beat;
            anchor_row = row;
            anchor_degree = degree;
            moved = false;
            additive = shift;
            dragged = id;
            if (target == Gesture::seek) {
                gesture = Gesture::seek;
                locate(beat);
            } else if (target == Gesture::audition) {
                gesture = Gesture::audition;
                if (!engine->playing) {
                    audition_pitch = document.pitch(degree);
                    engine->preview_held = true;
                    engine->preview = audition_pitch;
                }
            } else if (grid.contains(x, y)) {
                if (control || (message == WM_LBUTTONDBLCLK && !id)) {
                    if (std::ceil(row) < lowest || std::ceil(row) > highest) return;
                    gesture = Gesture::create;
                    selection.clear();
                    sequence.notes.push_back({sequence.next_id++, snapped, std::max(snap, std::round(last_length / snap) * snap), document.pitch(degree), 96, input_voice});
                    dragged = sequence.notes.back().id;
                    selection.insert(dragged);
                    original_notes = {sequence.notes.back()};
                    engine->publish(sequence);
                } else if (!id) {
                    gesture = Gesture::marquee;
                    if (!shift) selection.clear();
                } else {
                    if (shift && target != Gesture::copy) {
                        if (selection.contains(id)) {
                            selection.erase(id);
                            inherit_selection();
                            dragged = 0;
                            return;
                        }
                        selection.insert(id);
                    } else if (!selection.contains(id)) selection = {id};
                    gesture = target;
                    original_notes.clear();
                    for (const auto& note : sequence.notes) if (selection.contains(note.id)) original_notes.push_back(note);
                    const auto& note = *std::ranges::find(sequence.notes, id, &Note::id);
                    last_length = note.length;
                    if (input_voice != note.voice) toast_until = {};
                    input_voice = note.voice;
                }
                if (id || gesture == Gesture::create) {
                    if (!engine->playing) {
                        const auto& note = *std::ranges::find(sequence.notes, dragged, &Note::id);
                        audition_pitch = note.pitch;
                        engine->preview_velocity = note.velocity;
                        engine->preview = note.pitch;
                    }
                }
            }
            SetCapture(handle);
            cursor();
            return;
        }
        if (gesture == Gesture::gain) {
            if (message == WM_MOUSEMOVE || message == WM_LBUTTONUP) engine->master_db.store(std::clamp(std::round((original_gain + (anchor_y - y) * .1f) * 10.f) / 10.f, minimum_master_db, maximum_master_db), std::memory_order_relaxed);
            if (message == WM_LBUTTONUP) finish_gesture(true);
            return;
        }
        if (message == WM_LBUTTONUP && gesture != Gesture::move && gesture != Gesture::copy) {
            if (gesture != Gesture::pan) finish_gesture(true);
            return;
        }
        if (message == WM_MBUTTONUP && gesture != Gesture::pan) return;
        if (message != WM_MOUSEMOVE && message != WM_MBUTTONUP && message != WM_LBUTTONUP) return;
        if (gesture == Gesture::none || gesture == Gesture::velocity) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, handle, 0};
            TrackMouseEvent(&tracking);
            cursor();
            return;
        }
        const bool copied = gesture == Gesture::copy && moved;
        if (gesture == Gesture::move || gesture == Gesture::copy) gesture = GetKeyState(VK_MENU) < 0 ? Gesture::copy : Gesture::move;
        if (!moved && (std::abs(x - anchor_x) * renderer->scale >= GetSystemMetricsForDpi(SM_CXDRAG, GetDpiForWindow(handle)) || std::abs(y - anchor_y) * renderer->scale >= GetSystemMetricsForDpi(SM_CYDRAG, GetDpiForWindow(handle)))) moved = true;
        if (gesture == Gesture::copy && moved && !copied) {
            const auto source = dragged;
            selection.clear();
            for (const auto& original : original_notes) {
                *std::ranges::find(sequence.notes, original.id, &Note::id) = original;
                auto note = original;
                note.id = sequence.next_id++;
                if (original.id == source) dragged = note.id;
                sequence.notes.push_back(note);
                selection.insert(note.id);
            }
        } else if (gesture == Gesture::move && copied) {
            dragged = original_notes[dragged - before.next_id].id;
            sequence.notes.resize(before.notes.size());
            sequence.next_id = before.next_id;
            selection.clear();
            for (const auto& note : original_notes) selection.insert(note.id);
        }
        if (gesture == Gesture::pan) {
            if (moved) {
                follow = false;
                document.first_beat = std::max(0.0, original_first - (x - anchor_x) / document.pixels_per_beat);
                if (pan_vertical) {
                    document.top_degree = original_top + (y - anchor_y) / document.row_height;
                    constrain_pitch_view();
                }
            }
        } else if (gesture == Gesture::seek) locate(beat);
        else if (gesture == Gesture::audition) {
            if (!engine->playing && grid.y <= y && y < grid.y + grid.height && audition_pitch != document.pitch(degree)) {
                audition_pitch = document.pitch(degree);
                engine->preview = audition_pitch;
            }
        } else if (moved) {
            const double delta = std::round((raw_beat - anchor_beat) / snap) * snap;
            if (gesture == Gesture::marquee) {
                selection = additive ? before_selection : std::unordered_set<std::uint64_t>{};
                const double left = std::min(anchor_beat, raw_beat), right = std::max(anchor_beat, raw_beat);
                const double lower = std::min(anchor_row, row), upper = std::max(anchor_row, row);
                for (const auto& note : sequence.notes) {
                    const int pitch = document.degree(note.pitch);
                    if (note.beat < right && note.beat + note.length > left && pitch > lower && pitch - 1 < upper) selection.insert(note.id);
                }
            } else if (gesture == Gesture::create) {
                auto& note = sequence.notes.back();
                const auto& original = original_notes.front();
                note.beat = std::min(original.beat, snapped);
                note.length = std::max(snap, std::abs(snapped - original.beat));
            } else {
                double time_delta = delta;
                int pitch_delta = degree - anchor_degree;
                for (const auto& note : original_notes) {
                    if (gesture == Gesture::move || gesture == Gesture::copy) time_delta = std::max(time_delta, -std::floor((note.beat + 1e-9) / snap) * snap);
                    pitch_delta = std::clamp(pitch_delta, lowest - document.degree(note.pitch), highest - document.degree(note.pitch));
                }
                for (std::size_t i = 0; i < original_notes.size(); ++i) {
                    const auto& original = original_notes[i];
                    const auto target_id = gesture == Gesture::copy ? before.next_id + i : original.id;
                    auto& note = *std::ranges::find(sequence.notes, target_id, &Note::id);
                    if (gesture == Gesture::move || gesture == Gesture::copy) {
                        note.beat = original.beat + time_delta;
                        note.pitch = document.pitch(document.degree(original.pitch) + pitch_delta);
                    } else if (gesture == Gesture::resize_end) {
                        const double earliest = std::ceil((original.beat + snap - 1e-9) / snap) * snap;
                        const double end = std::round((original.beat + original.length + raw_beat - anchor_beat) / snap) * snap;
                        note.length = std::max(earliest, end) - original.beat;
                    }
                }
                const auto& note = *std::ranges::find(sequence.notes, dragged, &Note::id);
                if (!engine->playing && note.pitch != audition_pitch) {
                    audition_pitch = note.pitch;
                    engine->preview_velocity = note.velocity;
                    engine->preview = note.pitch;
                }
            }
            if (gesture != Gesture::marquee) engine->publish(sequence);
        }
        if (message == WM_MBUTTONUP || message == WM_LBUTTONUP) finish_gesture(true);
        else cursor();
    }

    void Window::wheel(float x, float y, double steps, bool horizontal) {
        if (input || help || popup != Popup::none) return;
        if (!horizontal && (gesture == Gesture::none || gesture == Gesture::velocity) && hit(x, y).first == Gesture::gain) {
            if (gesture == Gesture::velocity) finish_gesture(true);
            const float db = engine->master_db.load(std::memory_order_relaxed) + static_cast<float>(steps) * .5f;
            engine->master_db.store(std::clamp(std::round(db * 10.f) / 10.f, minimum_master_db, maximum_master_db), std::memory_order_relaxed);
            return;
        }
        if (!grid.contains(x, y) && !pedal_lane.contains(x, y) && !(x >= grid.x && x < grid.x + grid.width && y >= 100 && y < grid.y)) return;
        if (gesture != Gesture::none && gesture != Gesture::velocity) return;
        auto& sequence = document.sequence;
        const bool control = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
        if (alt && !horizontal) {
            const auto [target, id] = hit(x, y);
            if (!id) return;
            if (gesture == Gesture::velocity && id != dragged) finish_gesture(true);
            if (gesture == Gesture::none) {
                before = sequence;
                before_selection = selection;
                gesture = Gesture::velocity;
                dragged = id;
                original_notes.clear();
                for (const auto& note : sequence.notes) if (selection.contains(id) ? selection.contains(note.id) : note.id == id) original_notes.push_back(note);
            }
            int delta = static_cast<int>(std::round(steps * 4));
            for (const auto& original : original_notes) {
                const auto& note = *std::ranges::find(sequence.notes, original.id, &Note::id);
                delta = std::clamp(delta, 1 - note.velocity, 127 - note.velocity);
            }
            for (const auto& original : original_notes) std::ranges::find(sequence.notes, original.id, &Note::id)->velocity += delta;
            engine->publish(sequence);
            SetTimer(handle, 3, 300, nullptr);
            return;
        }
        finish_gesture(true);
        if (control && shift && !horizontal) {
            const double row = document.top_degree - (y - grid.y) / document.row_height;
            document.row_height = std::clamp(document.row_height * std::pow(1.15, steps), 18.0, 96.0);
            document.top_degree = row + (y - grid.y) / document.row_height;
            constrain_pitch_view();
        } else if (control && !horizontal) {
            const double beat = document.first_beat + (x - grid.x) / document.pixels_per_beat;
            document.pixels_per_beat = std::clamp(document.pixels_per_beat * std::pow(1.15, steps), 12.0, 720.0);
            document.first_beat = std::max(0.0, beat - (x - grid.x) / document.pixels_per_beat);
            follow_after = std::chrono::steady_clock::now() + std::chrono::milliseconds(650);
        } else if (shift && !horizontal) {
            document.top_degree += steps * 2;
            constrain_pitch_view();
            follow = false;
        } else {
            document.first_beat = std::max(0.0, document.first_beat + (horizontal ? steps : -steps) * 100 / document.pixels_per_beat);
            follow = false;
        }
        pointer_x = x;
        pointer_y = y;
        std::tie(hover, hovered) = hit(x, y);
        cursor();
    }
    void Window::command(int action) {
        auto& sequence = document.sequence;
        popup = Popup::none;
        if (action != paste_notes && selection.empty()) return;
        if (action == paste_notes && clipboard.empty()) return;
        if (action == paste_notes && std::ranges::any_of(clipboard_degrees, [&](int degree) { return document.pitch(degree) < lowest_pitch || document.pitch(degree) > highest_pitch; })) {
            toast = L"Paste exceeds the piano range (A0-C8)";
            toast_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            return;
        }
        if (action == copy_notes || action == cut_notes) {
            clipboard.clear();
            clipboard_degrees.clear();
            double first = std::numeric_limits<double>::max();
            for (const auto& note : sequence.notes) if (selection.contains(note.id)) first = std::min(first, note.beat);
            for (auto note : sequence.notes)
                if (selection.contains(note.id)) {
                    note.beat -= first;
                    clipboard.push_back(note);
                    clipboard_degrees.push_back(document.degree(note.pitch));
                }
            if (action == copy_notes) return;
        }
        document.checkpoint();
        if (action == cut_notes || action == delete_notes) {
            std::erase_if(sequence.notes, [&](const auto& note) { return selection.contains(note.id); });
            selection.clear();
        } else if (action == paste_notes) {
            selection.clear();
            const double pending = engine->seek;
            const double start = std::round((pending >= 0 ? pending : engine->position.load()) / snap) * snap;
            for (std::size_t i = 0; i < clipboard.size(); ++i) {
                auto note = clipboard[i];
                note.id = sequence.next_id++;
                note.beat += start;
                note.pitch = document.pitch(clipboard_degrees[i]);
                selection.insert(note.id);
                sequence.notes.push_back(note);
            }
        } else if (action == duplicate_notes) {
            double first = std::numeric_limits<double>::max(), last{};
            std::vector<Note> notes;
            for (const auto& note : sequence.notes)
                if (selection.contains(note.id)) {
                    notes.push_back(note);
                    first = std::min(first, note.beat);
                    last = std::max(last, note.beat + note.length);
                }
            selection.clear();
            for (auto note : notes) {
                note.id = sequence.next_id++;
                note.beat += std::ceil((last - first - 1e-9) / snap) * snap;
                selection.insert(note.id);
                sequence.notes.push_back(note);
            }
        }
        changed();
    }
    void Window::key(WPARAM key) {
        const bool control = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        auto& sequence = document.sequence;
        if (key == VK_ESCAPE) {
            if (gesture != Gesture::none) finish_gesture(false);
            else if (input) finish_tempo_input(false);
            else if (popup != Popup::none || help) {
                popup = Popup::none;
                help = false;
            } else selection.clear();
            return;
        }
        if (input) return;
        if (gesture == Gesture::velocity) finish_gesture(true);
        if (gesture != Gesture::none) {
            cursor();
            return;
        }
        if (key == VK_F1) {
            popup = Popup::none;
            help = !help;
            return;
        }
        if (help) return;
        if (key == VK_F2) {
            popup = Popup::none;
            if (engine->ready) engine->show_plugin();
            return;
        }
        if (key == VK_F11) {
            ShowWindow(handle, IsZoomed(handle) ? SW_RESTORE : SW_MAXIMIZE);
            return;
        }
        if (key == VK_SPACE && engine->ready) {
            if (!engine->playing) {
                if (play_from_locator) engine->seek = playback_start;
                follow = true;
                follow_after = {};
            }
            engine->preview = -2;
            engine->preview_held = false;
            engine->playing = !engine->playing;
            return;
        }
        if (key == VK_RETURN) {
            engine->playing = false;
            engine->seek = playback_start;
            return;
        }
        if (key == VK_HOME) {
            locate(0);
            document.first_beat = 0;
            return;
        }
        if (key == 'F' && !control) {
            follow = true;
            follow_after = {};
            return;
        }
        if (key == 'P' && !control) {
            engine->automatic_sustain = !engine->automatic_sustain;
            return;
        }
        if (key == 'R' && !control && !shift) {
            play_from_locator = !play_from_locator;
            toast = play_from_locator ? L"Playback: start from locator" : L"Playback: resume from paused position";
            toast_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            return;
        }
        if (!control && !shift && (key == '1' || key == '2' || key == VK_NUMPAD1 || key == VK_NUMPAD2)) {
            popup = Popup::none;
            input_voice = key == '1' || key == VK_NUMPAD1 ? Voice::treble : Voice::bass;
            if (std::ranges::any_of(sequence.notes, [&](const auto& note) { return selection.contains(note.id) && note.voice != input_voice; })) {
                document.checkpoint();
                for (auto& note : sequence.notes) if (selection.contains(note.id)) note.voice = input_voice;
                changed();
            }
            toast = input_voice == Voice::treble ? L"Treble voice" : L"Bass voice";
            toast_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            return;
        }
        if (control && key == '0') {
            double first = std::numeric_limits<double>::max(), last{};
            int low = 127, high{};
            for (const auto& note : sequence.notes)
                if (!shift || selection.contains(note.id)) {
                    first = std::min(first, note.beat);
                    last = std::max(last, note.beat + note.length);
                    low = std::min(low, document.degree(note.pitch));
                    high = std::max(high, document.degree(note.pitch));
                }
            if (last == 0) return;
            const double margin = std::max(snap, (last - first) * .04);
            document.first_beat = std::max(0.0, first - margin);
            document.pixels_per_beat = std::min(grid.width / (last + margin - document.first_beat), 720.0);
            document.row_height = std::min<double>(grid.height / (high - low + 3), 96.0);
            document.top_degree = high + 1.0;
            constrain_pitch_view();
            follow = false;
            return;
        }
        if (control && key == 'N') {
            popup = Popup::none;
            document = {};
            document.pixels_per_beat = grid.width / (4.0 * document.sequence.beats * 4 / document.sequence.beat_unit);
            selection.clear();
            hover = Gesture::none;
            hovered = 0;
            last_length = 1;
            input_voice = Voice::treble;
            engine->playing = false;
            engine->preview_held = false;
            engine->preview = -2;
            engine->seek = 0;
            playback_start = 0;
            follow = true;
            follow_after = {};
            changed();
            return;
        }
        if (control && key == 'E') {
            export_midi();
            return;
        }
        if (control && key == 'A') {
            for (const auto& note : sequence.notes) selection.insert(note.id);
            inherit_selection();
            return;
        }
        if (control && key == 'Z') {
            popup = Popup::none;
            if ((shift ? document.redo : document.undo).empty()) return;
            document.history(shift);
            std::erase_if(selection, [&](auto id) { return std::ranges::find(sequence.notes, id, &Note::id) == sequence.notes.end(); });
            changed();
            return;
        }
        if (control && key == 'C') command(copy_notes);
        else if (control && key == 'X') command(cut_notes);
        else if (control && key == 'V') command(paste_notes);
        else if (control && key == 'D') command(duplicate_notes);
        else if (key == VK_DELETE || key == VK_BACK) command(delete_notes);
        else if (key == VK_UP || key == VK_DOWN || key == VK_LEFT || key == VK_RIGHT) {
            if (selection.empty()) return;
            const auto [lowest, highest] = degree_range();
            int pitch_delta = (key == VK_UP ? 1 : key == VK_DOWN ? -1 : 0) * (shift ? 7 : 1);
            const double step = shift ? sequence.beats * 4.0 / sequence.beat_unit : snap;
            double time_delta = (key == VK_RIGHT ? 1 : key == VK_LEFT ? -1 : 0) * step;
            for (const auto& note : sequence.notes)
                if (selection.contains(note.id)) {
                    pitch_delta = std::clamp(pitch_delta, lowest - document.degree(note.pitch), highest - document.degree(note.pitch));
                    time_delta = std::max(time_delta, -std::floor((note.beat + 1e-9) / step) * step);
                }
            if (pitch_delta == 0 && time_delta == 0) return;
            document.checkpoint();
            for (auto& note : sequence.notes)
                if (selection.contains(note.id)) {
                    note.pitch = document.pitch(document.degree(note.pitch) + pitch_delta);
                    note.beat += time_delta;
                }
            changed();
        }
        cursor();
    }

    void Window::export_midi() {
        Microsoft::WRL::ComPtr<IFileSaveDialog> dialog;
        CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        const COMDLG_FILTERSPEC type{L"MIDI file", L"*.mid"};
        dialog->SetFileTypes(1, &type);
        dialog->SetDefaultExtension(L"mid");
        dialog->SetFileName(L"Sketch.mid");
        if (dialog->Show(handle) != S_OK) return;
        Microsoft::WRL::ComPtr<IShellItem> item;
        dialog->GetResult(&item);
        PWSTR path{};
        item->GetDisplayName(SIGDN_FILESYSPATH, &path);
        document.midi(path, engine->automatic_sustain);
        CoTaskMemFree(path);
        toast      = L"MIDI exported";
        toast_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    }
    LRESULT CALLBACK Window::edit_procedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
        auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_CONTEXTMENU || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP || message == WM_RBUTTONDBLCLK) return 0;
        if (message == WM_KEYDOWN && (w_param == VK_RETURN || w_param == VK_ESCAPE)) {
            self->finish_tempo_input(w_param == VK_RETURN);
            self->paint();
            return 0;
        }
        if (message == WM_CHAR && (w_param == VK_RETURN || w_param == VK_ESCAPE || w_param == VK_TAB)) return 0;
        if (message == WM_KILLFOCUS) PostMessageW(self->handle, WM_APP + 5, reinterpret_cast<WPARAM>(window), 0);
        return CallWindowProcW(self->input_procedure, window, message, w_param, l_param);
    }
    LRESULT CALLBACK Window::procedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
        auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(l_param)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, w_param, l_param);
        try {
            if (message == WM_CONTEXTMENU || message == WM_RBUTTONUP || message == WM_NCRBUTTONDOWN || message == WM_NCRBUTTONUP || message == WM_NCRBUTTONDBLCLK) return 0;
            if (message == WM_NCCALCSIZE && w_param) return 0;
            if (message == WM_NCHITTEST) {
                POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
                ScreenToClient(window, &point);
                RECT client{};
                GetClientRect(window, &client);
                const float scale = self->renderer ? self->renderer->scale : GetDpiForWindow(window) / 96.f;
                if (!IsZoomed(window)) {
                    const bool left = point.x < 7 * scale, right = point.x > client.right - 7 * scale, top = point.y < 7 * scale, bottom = point.y > client.bottom - 7 * scale;
                    if (top) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
                    if (bottom) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
                    if (left) return HTLEFT;
                    if (right) return HTRIGHT;
                }
                if (point.y < 55 * scale && point.x < client.right - 170 * scale) return HTCAPTION;
                return HTCLIENT;
            }
            if (message == WM_GETMINMAXINFO) {
                auto* info = reinterpret_cast<MINMAXINFO*>(l_param);
                const float scale = GetDpiForWindow(window) / 96.f;
                info->ptMinTrackSize = {static_cast<LONG>(960 * scale), static_cast<LONG>(600 * scale)};
                return 0;
            }
            if (message == WM_ERASEBKGND) return 1;
            if (message == WM_CTLCOLOREDIT) {
                const auto context = reinterpret_cast<HDC>(w_param);
                SetTextColor(context, RGB(156, 220, 255));
                SetBkColor(context, RGB(0, 0, 0));
                return reinterpret_cast<LRESULT>(GetStockObject(BLACK_BRUSH));
            }
            if (message == WM_PAINT) {
                PAINTSTRUCT paint{};
                BeginPaint(window, &paint);
                EndPaint(window, &paint);
                self->paint();
                return 0;
            }
            if (message == WM_SIZE && self->renderer) {
                self->finish_tempo_input(true);
                self->finish_gesture(false);
                self->popup = Popup::none;
                self->width = LOWORD(l_param) / self->renderer->scale;
                self->height = HIWORD(l_param) / self->renderer->scale;
                self->grid = {132, 132, self->width - 164, self->height - 218};
                self->pedal_lane = {self->grid.x, self->grid.y + self->grid.height, self->grid.width, 32};
                if (w_param != SIZE_MINIMIZED) self->renderer->resize();
                return 0;
            }
            if (message == WM_DPICHANGED && self->renderer) {
                self->finish_tempo_input(true);
                self->finish_gesture(false);
                self->popup = Popup::none;
                self->renderer->scale = HIWORD(w_param) / 96.f;
                self->renderer->reset_fonts();
                const auto* rect = reinterpret_cast<RECT*>(l_param);
                SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER);
                self->cursor();
                return 0;
            }
            if (message == WM_TIMER) {
                if (w_param == 1) {
                    if (self->engine->ready) self->engine->idle();
                    const auto gesture = self->gesture;
                    const bool note_drag = gesture == Gesture::move || gesture == Gesture::copy || gesture == Gesture::create || gesture == Gesture::resize_end || gesture == Gesture::marquee;
                    const bool time_drag = note_drag || gesture == Gesture::seek;
                    if (self->moved && time_drag && GetCapture() == window) {
                        auto& document = self->document;
                        const auto& grid = self->grid;
                        const float x = self->pointer_x, y = self->pointer_y;
                        const double horizontal = x < grid.x + 24 ? std::max(-40.f, x - grid.x - 24) : x > grid.x + grid.width - 24 ? std::min(40.f, x - grid.x - grid.width + 24) : 0;
                        const double vertical = !note_drag ? 0 : y < grid.y + 24 ? std::max(-40.f, y - grid.y - 24) : y > grid.y + grid.height - 24 ? std::min(40.f, y - grid.y - grid.height + 24) : 0;
                        if (horizontal || vertical) {
                            document.first_beat = std::max(0.0, document.first_beat + horizontal * .2 / document.pixels_per_beat);
                            document.top_degree -= vertical * .2 / document.row_height;
                            self->constrain_pitch_view();
                            const WPARAM flags = (GetKeyState(VK_SHIFT) < 0 ? MK_SHIFT : 0) | (GetKeyState(VK_CONTROL) < 0 ? MK_CONTROL : 0);
                            self->pointer(WM_MOUSEMOVE, x, y, flags);
                        }
                    }
                    if (self->engine->playing && self->follow && self->gesture == Gesture::none && !self->input && !self->help && self->popup == Popup::none && std::chrono::steady_clock::now() >= self->follow_after) {
                        self->document.first_beat = std::max(0.0, self->engine->position.load() - self->grid.width / (2 * self->document.pixels_per_beat));
                        std::tie(self->hover, self->hovered) = self->hit(self->pointer_x, self->pointer_y);
                    }
                    self->paint();
                } else if (w_param == 3) self->finish_gesture(true);
                return 0;
            }
            if (message == WM_APP + 1) {
                self->engine->initialize();
                if (!std::filesystem::exists(self->engine->directory / "piano.component")) self->engine->show_plugin();
                return 0;
            }
            if (message == WM_APP + 2) throw std::runtime_error(self->engine->error);
            if (message == WM_APP + 3) {
                self->engine->save_state();
                return 0;
            }
            if (message == WM_APP + 4) {
                ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
                SetForegroundWindow(window);
                return 0;
            }
            if (message == WM_APP + 5) {
                if (self->input == reinterpret_cast<HWND>(w_param) && GetFocus() != self->input) self->finish_tempo_input(true);
                return 0;
            }
            if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_KEYUP || message == WM_SYSKEYUP) && w_param == VK_MENU) {
                if (self->gesture == Gesture::move || self->gesture == Gesture::copy) {
                    POINT point{};
                    GetCursorPos(&point);
                    ScreenToClient(window, &point);
                    const WPARAM flags = (GetKeyState(VK_SHIFT) < 0 ? MK_SHIFT : 0) | (GetKeyState(VK_CONTROL) < 0 ? MK_CONTROL : 0);
                    self->pointer(WM_MOUSEMOVE, point.x / self->renderer->scale, point.y / self->renderer->scale, flags);
                }
                self->cursor();
                self->paint();
                return 0;
            }
            if (message == WM_KEYDOWN || (message == WM_SYSKEYDOWN && w_param == VK_ESCAPE && self->gesture != Gesture::none)) {
                if (!(l_param & (1LL << 30)) || w_param == VK_LEFT || w_param == VK_RIGHT || w_param == VK_UP || w_param == VK_DOWN || w_param == VK_DELETE || w_param == VK_BACK) self->key(w_param);
                self->cursor();
                self->paint();
                return 0;
            }
            if (message == WM_KEYUP || message == WM_SYSKEYUP || message == WM_SYSKEYDOWN) {
                self->cursor();
                if (message == WM_KEYUP) return 0;
            }
            if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
                POINT cursor{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
                ScreenToClient(window, &cursor);
                self->wheel(cursor.x / self->renderer->scale, cursor.y / self->renderer->scale, GET_WHEEL_DELTA_WPARAM(w_param) / 120.0, message == WM_MOUSEHWHEEL);
                return 0;
            }
            if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK || message == WM_LBUTTONUP || message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK || message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK || message == WM_MBUTTONUP || message == WM_MOUSEMOVE) {
                if (self->renderer) self->pointer(message, GET_X_LPARAM(l_param) / self->renderer->scale, GET_Y_LPARAM(l_param) / self->renderer->scale, w_param);
                return 0;
            }
            if (message == WM_SETCURSOR && LOWORD(l_param) == HTCLIENT && self->renderer) {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(window, &point);
                self->pointer_x = point.x / self->renderer->scale;
                self->pointer_y = point.y / self->renderer->scale;
                std::tie(self->hover, self->hovered) = self->hit(self->pointer_x, self->pointer_y);
                self->cursor();
                return TRUE;
            }
            if (message == WM_MOUSELEAVE && GetCapture() != window) {
                self->hover = Gesture::none;
                self->hovered = 0;
                self->pointer_x = self->pointer_y = -1;
                return 0;
            }
            if (message == WM_CAPTURECHANGED && reinterpret_cast<HWND>(l_param) != window) self->finish_gesture(false);
            if (message == WM_CANCELMODE || (message == WM_ACTIVATE && LOWORD(w_param) == WA_INACTIVE)) {
                self->finish_tempo_input(false);
                self->finish_gesture(false);
                self->popup = Popup::none;
                if (self->engine) {
                    self->engine->preview_held = false;
                    self->engine->preview = -2;
                }
            }
            if (message == WM_CLOSE) {
                self->finish_tempo_input(true);
                self->finish_gesture(false);
                self->engine->playing = false;
                self->engine->preview_held = false;
                self->engine->preview = -2;
                self->engine->save_state();
                PostQuitMessage(0);
                return 0;
            }
        } catch (const std::exception& error) {
            self->fatal_error = error.what();
            PostQuitMessage(1);
            return 0;
        }
        return DefWindowProcW(window, message, w_param, l_param);
    }
    std::pair<Window::Gesture, std::uint64_t> Window::hit(float x, float y) const {
        if (help || popup != Popup::none) return {Gesture::none, 0};
        if (engine->ready && Rect{width - 464, height - 40, 90, 36}.contains(x, y)) return {Gesture::gain, 0};
        if (pedal_lane.contains(x, y)) return {Gesture::seek, 0};
        const float end_x = grid.x + grid.width, end_y = grid.y + grid.height;
        if (x >= grid.x && x < end_x && y >= 100 && y < grid.y) return {Gesture::seek, 0};
        if (x >= 28 && x < grid.x && y >= grid.y && y < end_y) {
            const int degree = static_cast<int>(std::ceil(document.top_degree - (y - grid.y) / document.row_height));
            const auto [lowest, highest] = degree_range();
            return {degree >= lowest && degree <= highest ? Gesture::audition : Gesture::none, 0};
        }
        if (!grid.contains(x, y)) return {Gesture::none, 0};
        for (const auto& note : document.sequence.notes | std::views::reverse) {
            const double top = grid.y + (document.top_degree - document.degree(note.pitch)) * document.row_height + 3;
            const double left = grid.x + (note.beat - document.first_beat) * document.pixels_per_beat;
            const double right = std::max(left + 4, grid.x + (note.beat + note.length - document.first_beat) * document.pixels_per_beat - 2);
            if (x < std::max<double>(grid.x, left) || x >= std::min<double>(end_x, right) || y < std::max<double>(grid.y, top) || y >= std::min<double>(end_y, top + document.row_height - 6)) continue;
            if (GetKeyState(VK_MENU) < 0 && GetKeyState(VK_CONTROL) >= 0) return {Gesture::copy, note.id};
            const double edge = std::min<double>(resize_edge_width, (right - left) * resize_edge_fraction);
            if (right <= end_x && right - x < edge) return {Gesture::resize_end, note.id};
            return {Gesture::move, note.id};
        }
        return {Gesture::marquee, 0};
    }
    void Window::cursor() {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(handle, &point);
        pointer_x = point.x / renderer->scale;
        pointer_y = point.y / renderer->scale;
        if (gesture == Gesture::none || gesture == Gesture::velocity) std::tie(hover, hovered) = hit(pointer_x, pointer_y);
        const auto target = gesture == Gesture::none || gesture == Gesture::velocity ? hover : gesture;
        const auto [lowest, highest] = degree_range();
        const int degree = static_cast<int>(std::ceil(document.top_degree - (pointer_y - grid.y) / document.row_height));
        const bool writing = gesture == Gesture::create || ((gesture == Gesture::none || gesture == Gesture::velocity) && GetKeyState(VK_CONTROL) < 0 && !help && !input && popup == Popup::none && grid.contains(pointer_x, pointer_y) && degree >= lowest && degree <= highest);
        if (writing || target == Gesture::copy) {
            auto& icon = writing ? pencil_cursor : copy_cursor;
            auto& icon_dpi = writing ? pencil_dpi : copy_dpi;
            const UINT dpi = GetDpiForWindow(handle);
            if (icon_dpi != dpi) {
                constexpr std::array<std::string_view, 16> pencil_pixels{
                    "................",
                    "...........##...",
                    "..........#oo#..",
                    ".........#oooo#.",
                    "........######..",
                    ".......#oooo#...",
                    "......#oooo#....",
                    ".....#oooo#.....",
                    "....#oooo#......",
                    "...#oooo#.......",
                    "..#oooo#........",
                    ".#oooo#.........",
                    ".#ooo#..........",
                    ".#oo#...........",
                    ".###............",
                    "................"
                };
                constexpr std::array<std::string_view, 16> copy_pixels{
                    "#...............",
                    "##..............",
                    "#o#.............",
                    "#oo#............",
                    "#ooo#...........",
                    "#oooo#..........",
                    "#ooooo#.........",
                    "#oooooo#........",
                    "#ooooooo#.......",
                    "#oooo####.......",
                    "#oo#oo#.........",
                    "#o#.#oo#...###..",
                    "##..#oo#..##o##.",
                    ".....#o#.##ooo##",
                    ".....###..##o##.",
                    "...........###.."
                };
                const auto& pixels = writing ? pencil_pixels : copy_pixels;
                const int cursor_width = GetSystemMetricsForDpi(SM_CXCURSOR, dpi), cursor_height = GetSystemMetricsForDpi(SM_CYCURSOR, dpi);
                const int stride = ((cursor_width + 15) / 16) * 2, plane_size = stride * cursor_height;
                std::vector<std::uint8_t> mask(plane_size * 2);
                std::fill_n(mask.begin(), plane_size, std::uint8_t{0xff});
                for (int y = 0; y < cursor_height; ++y)
                    for (int x = 0; x < cursor_width; ++x) {
                        const char pixel = pixels[y * 16 / cursor_height][x * 16 / cursor_width];
                        if (pixel == '.') continue;
                        const int offset = y * stride + x / 8;
                        const auto bit = static_cast<std::uint8_t>(0x80u >> (x % 8));
                        mask[offset] &= static_cast<std::uint8_t>(~bit);
                        if (pixel == 'o') mask[plane_size + offset] |= bit;
                    }
                const HBITMAP bitmap = CreateBitmap(cursor_width, cursor_height * 2, 1, 1, mask.data());
                ICONINFO info{.fIcon = FALSE, .xHotspot = writing ? static_cast<DWORD>(cursor_width / 16) : 0, .yHotspot = writing ? static_cast<DWORD>(cursor_height * 14 / 16) : 0, .hbmMask = bitmap, .hbmColor = nullptr};
                const HCURSOR next = CreateIconIndirect(&info);
                DeleteObject(bitmap);
                if (!next) throw std::runtime_error("Cannot create the editing cursor.");
                SetCursor(next);
                if (icon) DestroyCursor(icon);
                icon = next;
                icon_dpi = dpi;
            }
            SetCursor(icon);
            return;
        }
        LPCWSTR shape = IDC_ARROW;
        if (target == Gesture::move) shape = IDC_SIZEALL;
        if (target == Gesture::marquee || target == Gesture::seek) shape = IDC_CROSS;
        if (target == Gesture::resize_end) shape = IDC_SIZEWE;
        if (target == Gesture::gain) shape = IDC_SIZENS;
        if (target == Gesture::pan || target == Gesture::audition) shape = IDC_HAND;
        if (gesture == Gesture::none && !help && popup == Popup::none && engine->ready && Rect{width - 74, height - 38, 46, 32}.contains(pointer_x, pointer_y)) shape = IDC_HAND;
        if (gesture == Gesture::none && popup == Popup::none && Rect{200, 65, 100, 35}.contains(pointer_x, pointer_y)) shape = IDC_IBEAM;
        SetCursor(LoadCursorW(nullptr, shape));
    }
}

