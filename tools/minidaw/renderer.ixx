module;

#include <dwrite_2.h>
#include <windows.h>
#include <wrl/client.h>

export module minidaw.renderer;

import std;
import vulkan;

export namespace minidaw {
    struct Color {
        float r{}, g{}, b{}, a{1};
    };

    inline constexpr Color background{0, 0, 0, 1};
    inline constexpr Color foreground{247 / 255.f, 247 / 255.f, 247 / 255.f, 1};
    inline constexpr Color muted{166 / 255.f, 166 / 255.f, 166 / 255.f, 1};
    inline constexpr Color waveform_color{241 / 255.f, 241 / 255.f, 241 / 255.f, 1};
    inline constexpr Color treble_note_color{227 / 255.f, 157 / 255.f, 175 / 255.f, 1};
    inline constexpr Color treble_note_hover_color{231 / 255.f, 170 / 255.f, 185 / 255.f, 1};
    inline constexpr Color treble_note_selected_color{237 / 255.f, 192 / 255.f, 204 / 255.f, 1};
    inline constexpr Color treble_note_border_color{238 / 255.f, 196 / 255.f, 207 / 255.f, 1};
    inline constexpr Color bass_note_color{124 / 255.f, 188 / 255.f, 225 / 255.f, 1};
    inline constexpr Color bass_note_hover_color{141 / 255.f, 197 / 255.f, 229 / 255.f, 1};
    inline constexpr Color bass_note_selected_color{171 / 255.f, 212 / 255.f, 236 / 255.f, 1};
    inline constexpr Color bass_note_border_color{176 / 255.f, 215 / 255.f, 237 / 255.f, 1};
    inline constexpr Color note_text_color{16 / 255.f, 19 / 255.f, 18 / 255.f, 1};
    inline constexpr Color note_selected_border_color{1, 1, 1, 1};
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

    struct Rect {
        float x{}, y{}, width{}, height{};

        bool contains(float px, float py) const {
            return px >= x && px < x + width && py >= y && py < y + height;
        }
    };

    struct Vertex {
        float x{}, y{}, u{}, v{};
        Color color;
        float mode{}, radius{}, width{}, height{};
    };

    struct Buffer {
        vk::raii::DeviceMemory memory{nullptr};
        vk::raii::Buffer handle{nullptr};
        void* mapped{};

        Buffer() = default;
        Buffer(const vk::raii::PhysicalDevice& physical, const vk::raii::Device& device, vk::DeviceSize size, vk::BufferUsageFlags usage);
    };

    struct Glyph {
        Microsoft::WRL::ComPtr<IDWriteFontFace> face;
        int left{}, top{}, width{}, height{}, x{}, y{};
    };

    struct Renderer final : IDWriteTextRenderer {
        float scale{1};
        std::vector<Vertex> vertices;

        explicit Renderer(HWND window);
        ~Renderer();
        Renderer(const Renderer&)            = delete;
        Renderer& operator=(const Renderer&) = delete;
        void resize();
        void reset_fonts();
        void rect(Rect area, Color color, float radius = 0);
        float text(std::wstring_view value, float size, float x, float y, Color color, float width = 2000, bool centered = false, bool fit = false);
        void draw();

    private:
        HWND window{};
        vk::raii::Context context;
        vk::raii::Instance instance{nullptr};
        vk::raii::SurfaceKHR surface{nullptr};
        vk::raii::PhysicalDevice physical{nullptr};
        vk::raii::Device device{nullptr};
        vk::raii::Queue queue{nullptr};
        std::uint32_t family{};
        vk::raii::DeviceMemory atlas_memory{nullptr};
        vk::raii::Image atlas{nullptr};
        vk::raii::ImageView atlas_view{nullptr};
        vk::raii::Sampler sampler{nullptr};
        Buffer atlas_staging;
        Buffer vertex_buffer;
        std::size_t vertex_capacity{4 * 1024 * 1024};
        vk::raii::DescriptorSetLayout descriptor_layout{nullptr};
        vk::raii::DescriptorPool descriptor_pool{nullptr};
        vk::raii::DescriptorSet descriptor{nullptr};
        vk::raii::PipelineLayout pipeline_layout{nullptr};
        vk::raii::Pipeline ui_pipeline{nullptr};
        vk::raii::SwapchainKHR swapchain{nullptr};
        std::vector<vk::Image> images;
        std::vector<vk::raii::ImageView> views;
        std::vector<vk::raii::Semaphore> presented;
        vk::Extent2D extent;
        vk::raii::CommandPool command_pool{nullptr};
        vk::raii::CommandBuffer command{nullptr};
        vk::raii::Semaphore acquired{nullptr};
        vk::raii::Fence finished{nullptr};
        bool atlas_initialized{}, atlas_dirty{true};
        std::vector<std::uint8_t> atlas_pixels;
        int atlas_x{1}, atlas_y{1}, atlas_row{};
        Microsoft::WRL::ComPtr<IDWriteFactory2> fonts;
        std::map<std::tuple<IDWriteFontFace*, std::uint16_t, float>, Glyph> glyphs;
        Color text_color;

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
        ULONG STDMETHODCALLTYPE AddRef() override;
        ULONG STDMETHODCALLTYPE Release() override;
        HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override;
        HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) override;
        HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels) override;
        HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE measuring, const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override;
        HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override;
        HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override;
        HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject* object, BOOL sideways, BOOL rtl, IUnknown* effect) override;
    };
}
