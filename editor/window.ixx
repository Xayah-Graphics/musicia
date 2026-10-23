module;

#include <oleidl.h>
#include <windows.h>

export module editor.window;

import std;
import musicia.audio;
import editor.renderer;
import editor.timeline;
import editor.playback;
import musicia.analysis;
import musicia.sheetsage2;

export namespace editor {
    struct Window final : IDropTarget {
        HWND handle{};
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
        bool loading{}, loaded{}, expanded{}, drop_active{}, accepts_drop{}, restore_playback{};
        float width{448}, height{274};
        bool follow{true}, hovering{}, drag_moved{};
        float hover_x{}, hover_y{}, drag_x{}, drag_y{}, drag_height{}, wave_height{144};
        double drag_frame{}, drag_first{}, drag_count{};
        enum struct Drag { none, seek, pan, height, overview, view, left, right };
        Drag drag{Drag::none};
        Rect content, waveform, overview, wave_divider, melody;
        std::array<Rect, 3> lanes;
        std::array<Rect, 2> melody_tabs;
        int melody_track{};
        struct ChordSegment {
            double start{}, end{};
            std::wstring name, numeral;
        };
        std::vector<ChordSegment> chord_segments;
        struct MelodyPassage {
            double start{}, end{};
            std::array<float, 2> low{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
            std::array<float, 2> high{-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
            std::array<std::wstring, 14> labels;
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
        void refresh();
        void render();
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
