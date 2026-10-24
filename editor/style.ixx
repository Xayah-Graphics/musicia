export module editor.style;

import std;

export namespace ui {
    struct Color {
        float r{}, g{}, b{}, a{1};
    };

    inline constexpr Color background{0, 0, 0, 1};
    inline constexpr Color foreground{247 / 255.f, 247 / 255.f, 247 / 255.f, 1};
    inline constexpr Color muted{166 / 255.f, 166 / 255.f, 166 / 255.f, 1};
    inline constexpr Color waveform_color{241 / 255.f, 241 / 255.f, 241 / 255.f, 1};
    inline constexpr std::array note_colors{Color{227 / 255.f, 157 / 255.f, 175 / 255.f, 1}, Color{124 / 255.f, 188 / 255.f, 225 / 255.f, 1}};
    inline constexpr std::array note_hover_colors{Color{231 / 255.f, 170 / 255.f, 185 / 255.f, 1}, Color{141 / 255.f, 197 / 255.f, 229 / 255.f, 1}};
    inline constexpr std::array note_active_colors{Color{237 / 255.f, 192 / 255.f, 204 / 255.f, 1}, Color{171 / 255.f, 212 / 255.f, 236 / 255.f, 1}};
    inline constexpr std::array note_border_colors{Color{238 / 255.f, 196 / 255.f, 207 / 255.f, 1}, Color{176 / 255.f, 215 / 255.f, 237 / 255.f, 1}};
    inline constexpr Color note_text_color{16 / 255.f, 19 / 255.f, 18 / 255.f, 1};
    inline constexpr Color note_active_border{1, 1, 1, 1};
    inline constexpr std::array chord_colors{
        Color{114 / 255.f, 159 / 255.f, 240 / 255.f, 1},
        Color{205 / 255.f, 187 / 255.f, 118 / 255.f, 1},
        Color{112 / 255.f, 190 / 255.f, 199 / 255.f, 1},
        Color{130 / 255.f, 214 / 255.f, 114 / 255.f, 1},
        Color{240 / 255.f, 128 / 255.f, 90 / 255.f, 1},
        Color{101 / 255.f, 185 / 255.f, 158 / 255.f, 1},
        Color{222 / 255.f, 115 / 255.f, 151 / 255.f, 1},
        Color{189 / 255.f, 139 / 255.f, 229 / 255.f, 1}
    };
    inline constexpr Color accent{156 / 255.f, 220 / 255.f, 1, 1};
    inline constexpr Color surface_color{14 / 255.f, 14 / 255.f, 14 / 255.f, 1};
    inline constexpr Color active_surface{24 / 255.f, 24 / 255.f, 24 / 255.f, 1};
    inline constexpr Color hover_surface{40 / 255.f, 40 / 255.f, 40 / 255.f, 1};
    inline constexpr Color rule_color{43 / 255.f, 43 / 255.f, 43 / 255.f, 1};
    inline constexpr Color tonic_row_color{9 / 255.f, 18 / 255.f, 22 / 255.f, 1};
    inline constexpr Color pitch_grid{14 / 255.f, 26 / 255.f, 31 / 255.f, 1};
    inline constexpr Color bar_grid{69 / 255.f, 110 / 255.f, 124 / 255.f, 1};
    inline constexpr Color beat_grid{38 / 255.f, 62 / 255.f, 71 / 255.f, 1};
    inline constexpr Color eighth_grid{67 / 255.f, 103 / 255.f, 116 / 255.f, 1};
    inline constexpr Color sixteenth_grid{45 / 255.f, 72 / 255.f, 81 / 255.f, 1};
    inline constexpr Color thirty_second_grid{30 / 255.f, 48 / 255.f, 54 / 255.f, 1};
    inline constexpr Color warning_color{235 / 255.f, 195 / 255.f, 138 / 255.f, 1};
    inline constexpr Color overload_color{1.f, .35f, .32f, 1.f};
    inline constexpr Color overload_surface{.2f, .04f, .04f, 1.f};

    inline constexpr auto font_family = L"Segoe UI Variable";
    inline constexpr float value_size{17}, secondary_size{15}, body_size{13}, detail_size{12}, label_size{11}, unit_size{10}, micro_size{9}, caption_size{8};
    inline constexpr float outer_margin{28}, label_x{30}, timeline_x{132}, right_margin{32};
    inline constexpr float header_height{55}, footer_height{42}, toolbar_y{71}, ruler_y{112};
    inline constexpr float key_x{30}, tempo_x{210}, tempo_unit_x{266}, meter_x{327}, clock_right{210};
    inline constexpr float control_radius{4}, popup_radius{7}, note_radius{3};
    inline constexpr float playhead_width{4}, bar_width{2}, beat_width{1}, dot_width{2};
    inline constexpr float note_border_width{1.5f}, note_active_border_width{2.5f};
    inline constexpr float output_info_right{634}, master_right{464}, output_meter_right{331}, output_meter_width{156}, peak_right{149}, overload_right{74};
    inline constexpr float meter_floor{-60}, meter_release{24}, peak_release{12};
    inline constexpr std::chrono::milliseconds peak_hold{1500}, follow_delay{600}, follow_duration{700};
    inline constexpr std::chrono::milliseconds focus_duration{360}, focus_fade_duration{200};
    inline constexpr std::array<float, 2> focus_grid_levels{.10f, .25f}, focus_note_levels{.20f, .40f}, focus_annotation_levels{.15f, .30f};
    inline constexpr float focus_label_reduction{.45f}, focus_underline_width{4};

    inline constexpr std::chrono::milliseconds chord_color_duration{240};
    inline constexpr float chord_color_fraction{.35f}, chord_grid_mix{1.f}, chord_annotation_mix{1.f}, chord_wave_mix{.94f};

    Color harmonic_color(Color base, const std::array<float, 4>& tint, float amount) {
        if (tint[3] == 0) return base;
        std::array channels{base.r, base.g, base.b};
        for (std::size_t channel = 0; channel < channels.size(); ++channel) {
            const float source = channels[channel] <= .04045f ? channels[channel] / 12.92f : std::pow((channels[channel] + .055f) / 1.055f, 2.4f);
            const float value = std::lerp(source, tint[channel] / tint[3], amount * tint[3]);
            channels[channel] = value <= .0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.f / 2.4f) - .055f;
        }
        return {channels[0], channels[1], channels[2], base.a};
    }

    std::wstring time_label(double seconds, bool precise) {
        const auto hundredths = static_cast<std::uint64_t>(std::max(0.0, seconds) * 100);
        if (precise) return std::format(L"{:02}:{:02}.{:02}", hundredths / 6000, hundredths / 100 % 60, hundredths % 100);
        return std::format(L"{:02}:{:02}", hundredths / 6000, hundredths / 100 % 60);
    }
}
