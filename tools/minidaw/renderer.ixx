module;

#include <dwrite_2.h>
#include <windows.h>
#include <wrl/client.h>

export module minidaw.renderer;

import std;
export import editor.style;
import vulkan;

export namespace minidaw {
    struct Rect {
        float x{}, y{}, width{}, height{};

        bool contains(float px, float py) const {
            return px >= x && px < x + width && py >= y && py < y + height;
        }
    };

    struct Vertex {
        float x{}, y{}, u{}, v{};
        ui::Color color;
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
        void rect(Rect area, ui::Color color, float radius = 0);
        float text(std::wstring_view value, float size, float x, float y, ui::Color color, float width = 2000, bool centered = false, bool fit = false);
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
        Microsoft::WRL::ComPtr<IDWriteTypography> typography;
        std::map<std::tuple<IDWriteFontFace*, std::uint16_t, float>, Glyph> glyphs;
        ui::Color text_color;

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
