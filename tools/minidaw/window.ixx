module;
#include <windows.h>
export module minidaw.window;
import std;
export import editor.timing;
import minidaw.renderer;
import minidaw.document;
import minidaw.engine;

export namespace minidaw {
    struct Window {
        HWND handle{};
        ui::FrameClock frames;
        std::unique_ptr<Renderer> renderer;
        Document document;
        std::unique_ptr<Engine> engine;
        std::string fatal_error;
        explicit Window();
        ~Window();
        void paint();
        void inherit_selection();
        void changed();
        std::pair<int, int> degree_range() const;
        void constrain_pitch_view();
        void locate(double beat);
        void finish_gesture(bool commit);
        void finish_tempo_input(bool commit);
        void pointer(UINT message, float x, float y, WPARAM flags);
        void wheel(float x, float y, double steps, bool horizontal);
        void command(int action);
        void key(WPARAM key);
        void export_midi();
        static LRESULT CALLBACK edit_procedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param);
        static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param);

    private:
        enum struct Gesture { none, marquee, move, copy, resize_end, create, pan, seek, audition, velocity, gain };
        enum struct Popup { none, key, meter, snap };
        enum Action { copy_notes = 1, cut_notes, paste_notes, duplicate_notes, delete_notes };
        static constexpr float resize_edge_width{24}, resize_edge_fraction{.4f};
        static constexpr std::array<std::pair<int, int>, 7> meter_presets{{{2, 4}, {3, 4}, {4, 4}, {3, 8}, {6, 8}, {9, 8}, {12, 8}}};
        std::pair<Gesture, std::uint64_t> hit(float x, float y) const;
        void cursor();
        float width{1600}, height{960};
        Rect grid, pedal_lane;
        double snap{.25}, last_length{1}, anchor_beat{}, anchor_row{}, original_first{}, original_top{}, playback_start{};
        int anchor_degree{}, audition_pitch{-1};
        float anchor_x{}, anchor_y{}, pointer_x{}, pointer_y{};
        float original_gain{};
        std::array<float, 2> meter_db{ui::meter_floor, ui::meter_floor};
        std::array<float, 2> held_peak_db{-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
        std::array<std::chrono::steady_clock::time_point, 2> peak_hold_until{};
        std::chrono::steady_clock::time_point meter_updated{std::chrono::steady_clock::now()};
        Gesture gesture{}, hover{};
        Popup popup{};
        Voice input_voice{Voice::treble};
        Rect popup_area;
        std::uint64_t dragged{}, hovered{};
        Sequence before;
        std::vector<Note> original_notes;
        std::vector<Note> clipboard;
        std::vector<int> clipboard_degrees;
        std::unordered_set<std::uint64_t> selection, before_selection;
        bool help{}, moved{}, additive{}, pan_vertical{}, follow{true}, play_from_locator{true};
        HWND input{};
        WNDPROC input_procedure{};
        HFONT input_font{};
        HCURSOR pencil_cursor{}, copy_cursor{};
        UINT pencil_dpi{}, copy_dpi{};
        std::wstring input_original, toast;
        std::chrono::steady_clock::time_point toast_until{};
        std::optional<std::chrono::steady_clock::time_point> follow_resume_at;
        double follow_from{};
    };
}
