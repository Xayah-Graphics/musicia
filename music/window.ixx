module;

#include <oleidl.h>
#include <windows.h>

export module music.window;

import std;
import music.audio;
import music.renderer;

export namespace music {
    struct Window final : IDropTarget {
        HWND handle{};
        std::string fatalError;

        Window();
        ~Window();
        Window(const Window&)            = delete;
        Window& operator=(const Window&) = delete;
        void load(std::filesystem::path path);

    private:
        Audio audio;
        Timeline timeline;
        std::optional<Renderer> renderer;
        std::future<AudioClip> loadingClip;
        std::filesystem::path loadingPath;
        std::filesystem::path pendingPath;
        std::wstring error;
        bool loading{}, loaded{}, expanded{}, dropActive{}, acceptsDrop{}, restorePlayback{};
        float width{448}, height{274};
        double dragFrame{}, dragViewOffset{};
        enum struct Drag { none, seek, view };
        Drag drag{Drag::none};
        Rect waveform, overview;

        void layout();
        void refresh();
        void render();
        void moveDrag(float x);
        void finishDrag();
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
