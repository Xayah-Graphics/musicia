module;

#include <dwrite_2.h>
#include <windows.h>
#include <wrl/client.h>

export module editor.renderer;

import std;
import vulkan;
import music.audio;
import editor.waveform;

export namespace editor {
    struct Color {
        float r{}, g{}, b{}, a{1};
    };

    inline constexpr Color background{0, 0, 0, 1};
    inline constexpr Color foreground{246 / 255.f, 247 / 255.f, 249 / 255.f, 1};
    inline constexpr Color muted{160 / 255.f, 166 / 255.f, 175 / 255.f, 1};
    inline constexpr Color waveformColor{236 / 255.f, 241 / 255.f, 245 / 255.f, 1};
    inline constexpr Color noteColor{211 / 255.f, 223 / 255.f, 231 / 255.f, 1};
    inline constexpr Color accent{156 / 255.f, 220 / 255.f, 1, 1};
    inline constexpr Color surfaceColor{11 / 255.f, 14 / 255.f, 17 / 255.f, 1};
    inline constexpr Color activeSurface{17 / 255.f, 24 / 255.f, 28 / 255.f, 1};
    inline constexpr Color hoverSurface{28 / 255.f, 40 / 255.f, 46 / 255.f, 1};
    inline constexpr Color ruleColor{37 / 255.f, 43 / 255.f, 48 / 255.f, 1};
    inline constexpr Color barGrid{52 / 255.f, 53 / 255.f, 54 / 255.f, 1};
    inline constexpr Color beatGrid{32 / 255.f, 32 / 255.f, 32 / 255.f, 1};
    inline constexpr Color eighthGrid{18 / 255.f, 18 / 255.f, 18 / 255.f, 1};
    inline constexpr Color sixteenthGrid{10 / 255.f, 10 / 255.f, 10 / 255.f, 1};

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
        std::uint32_t waveLayer{};
        Rect waveRect, overviewRect;
        double waveFirst{}, waveCount{1}, wavePosition{};
        bool showWave{};

        explicit Renderer(HWND window);
        ~Renderer();
        Renderer(const Renderer&)            = delete;
        Renderer& operator=(const Renderer&) = delete;
        void resize();
        void upload(const music::AudioClip& clip);
        void resetFonts();
        void rect(Rect area, Color color, float radius = 0);
        void text(std::wstring_view value, float size, float x, float y, Color color, float width = 2000, bool centered = false, bool fit = false);
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
        vk::raii::DeviceMemory atlasMemory{nullptr};
        vk::raii::Image atlas{nullptr};
        vk::raii::ImageView atlasView{nullptr};
        vk::raii::Sampler sampler{nullptr};
        Buffer atlasStaging;
        Buffer vertexBuffer;
        Buffer waveBuffer;
        vk::raii::DescriptorSetLayout descriptorLayout{nullptr};
        vk::raii::DescriptorPool descriptorPool{nullptr};
        vk::raii::DescriptorSet descriptor{nullptr};
        vk::raii::PipelineLayout pipelineLayout{nullptr};
        vk::raii::Pipeline uiPipeline{nullptr};
        vk::raii::Pipeline wavePipeline{nullptr};
        vk::raii::SwapchainKHR swapchain{nullptr};
        std::vector<vk::Image> images;
        std::vector<vk::raii::ImageView> views;
        std::vector<vk::raii::Semaphore> presented;
        vk::Extent2D extent;
        vk::raii::CommandPool commandPool{nullptr};
        vk::raii::CommandBuffer command{nullptr};
        vk::raii::Semaphore acquired{nullptr};
        vk::raii::Fence finished{nullptr};
        std::vector<std::uint32_t> peakOffsets;
        std::vector<std::uint32_t> peakBlocks;
        std::uint32_t rawOffset{}, channels{}, frames{};
        bool atlasInitialized{}, atlasDirty{true};
        std::vector<std::uint8_t> atlasPixels;
        int atlasX{1}, atlasY{1}, atlasRow{};
        Microsoft::WRL::ComPtr<IDWriteFactory2> fonts;
        std::map<std::tuple<IDWriteFontFace*, std::uint16_t, float>, Glyph> glyphs;
        Color textColor;

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
