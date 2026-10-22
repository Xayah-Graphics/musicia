module;

#include <oleidl.h>
#include <windows.h>

export module editor.window;

import std;
import music.audio;
import editor.renderer;
import editor.timeline;
import editor.playback;
import music.analysis;
import music.sheetsage2;

export namespace editor {
    struct Window final : IDropTarget {
        HWND handle{};
        std::string fatalError;

        Window();
        ~Window();
        Window(const Window&)            = delete;
        Window& operator=(const Window&) = delete;
        void load(std::filesystem::path path);

    private:
        Playback audio;
        Timeline timeline;
        std::optional<Renderer> renderer;
        std::future<music::AudioClip> loadingClip;
        std::optional<music::SheetSage2> model;
        std::future<music::Analysis> analyzing;
        std::stop_source analysisStop;
        std::shared_ptr<const std::vector<float>> analysisSamples;
        std::optional<music::Analysis> analysis;
        music::AnalysisProgress progress;
        std::wstring analysisError;
        bool analysisPending{};
        std::filesystem::path loadingPath;
        std::filesystem::path pendingPath;
        std::wstring error;
        bool loading{}, loaded{}, expanded{}, dropActive{}, acceptsDrop{}, restorePlayback{};
        float width{448}, height{274};
        bool follow{true}, hovering{}, dragMoved{};
        float hoverX{}, hoverY{}, dragX{};
        double dragFrame{}, dragFirst{}, dragCount{};
        enum struct Drag { none, seek, pan, overview, view, left, right };
        Drag drag{Drag::none};
        Rect content, waveform, overview;
        std::array<Rect, 3> lanes;

        void layout();
        void refresh();
        void render();
        void moveDrag(float x);
        void finishDrag(bool commit = false);
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
