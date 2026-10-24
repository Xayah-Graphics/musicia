module;

#include <oleidl.h>
#include <windows.h>

export module editor.window;

import std;
export import editor.timing;
import musicia.audio;
import editor.renderer;
import editor.timeline;
import editor.playback;
import musicia.analysis;
import musicia.sheetsage2;

export namespace editor {
    struct Window final : IDropTarget {
        HWND handle{};
        ui::FrameClock frames;
        std::string fatal_error;

        Window();
        ~Window();
        Window(const Window&)            = delete;
        Window& operator=(const Window&) = delete;
        void load(std::filesystem::path path);

    private:
        Playback audio;
        Timeline timeline;
        std::optional<Renderer> renderer;
        std::future<musicia::AudioClip> loading_clip;
        std::optional<musicia::SheetSage2> model;
        std::future<musicia::Analysis> analyzing;
        std::stop_source analysis_stop;
        std::shared_ptr<const std::vector<float>> analysis_samples;
        std::optional<musicia::Analysis> analysis;
        musicia::AnalysisProgress progress;
        std::wstring analysis_error;
        bool analysis_pending{};
        std::filesystem::path loading_path;
        std::filesystem::path pending_path;
        std::wstring error;
        bool loading{}, loaded{}, expanded{}, drop_active{}, accepts_drop{};
        float width{448}, height{274};
        bool initial_view{true}, hovering{}, drag_moved{};
        std::optional<std::chrono::steady_clock::time_point> follow_resume_at;
        double follow_from{};
        std::vector<float> bar_focus, focus_from;
        std::vector<std::array<float, 4>> bar_tints;
        std::array<float, 4> bar_tint_from{};
        std::array<float, 4> chord_tint{}, chord_tint_from{}, chord_tint_to{};
        std::optional<int> chord_color_target;
        std::chrono::steady_clock::time_point chord_color_started{};
        std::chrono::duration<double> chord_color_span{ui::chord_color_duration};
        std::optional<std::size_t> focus_target;
        float focus_strength{}, focus_strength_from{}, focus_strength_target{};
        bool focus_animating{};
        std::chrono::steady_clock::time_point focus_started{}, focus_fade_started{};
        float hover_x{}, hover_y{}, drag_x{}, drag_y{}, drag_height{}, wave_height{112};
        double drag_frame{}, drag_first{}, drag_count{};
        float original_gain{};
        std::array<float, 2> meter_db{ui::meter_floor, ui::meter_floor};
        std::array<float, 2> held_peak_db{-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
        std::array<std::chrono::steady_clock::time_point, 2> peak_hold_until{};
        std::chrono::steady_clock::time_point meter_updated{std::chrono::steady_clock::now()};
        enum struct Drag { none, pan, pan_overview, height, view, left, right, gain };
        Drag drag{Drag::none};
        Rect content, waveform, overview, wave_divider, melody;
        std::array<Rect, 3> lanes;
        std::array<Rect, 2> melody_tabs;
        int melody_track{};
        struct ChordSegment {
            double start{}, end{};
            std::wstring name, numeral;
            int color_index{-1};
        };
        std::vector<ChordSegment> chord_segments;
        struct MelodyPassage {
            double start{}, end{};
            std::array<float, 2> low{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
            std::array<float, 2> high{-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
            std::array<int, 14> pitch_offsets{};
            std::array<std::wstring, 14> labels, solfege_labels;
        };
        std::vector<MelodyPassage> melody_passages;
        struct MelodyNote {
            std::size_t source{}, passage{};
            double start{}, end{};
            float row{};
            bool in_scale{};
            std::wstring name, label;
        };
        std::vector<MelodyNote> melody_notes;

        void layout();
        void rebuild_analysis_view();
        Rect note_rect(const MelodyNote& segment) const;
        const MelodyNote* note_at(float x, float y) const;
        void locate(double frame);
        void refresh();
        void render();
        void cursor();
        void move_drag(float x, float y);
        void finish_drag(bool commit = false);
        LRESULT message(UINT message, WPARAM wparam, LPARAM lparam);
        static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
        ULONG STDMETHODCALLTYPE AddRef() override;
        ULONG STDMETHODCALLTYPE Release() override;
        HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* object, DWORD keys, POINTL point, DWORD* effect) override;
        HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL point, DWORD* effect) override;
        HRESULT STDMETHODCALLTYPE DragLeave() override;
        HRESULT STDMETHODCALLTYPE Drop(IDataObject* object, DWORD keys, POINTL point, DWORD* effect) override;
    };
}
