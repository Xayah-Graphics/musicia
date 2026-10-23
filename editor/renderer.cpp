module;

#include <dwrite_2.h>
#include <windows.h>
#include <wrl/client.h>

module editor.renderer;

import std;
import vulkan;
import musicia.audio;
import editor.waveform;

namespace editor {
    struct Push {
        std::array<float, 4> rect{};
        std::array<float, 4> view{};
        std::array<std::uint32_t, 4> data{};
        std::array<std::uint32_t, 4> track{};
        std::array<float, 4> screen{};
        std::array<float, 4> color{};
        float playhead{};
    };

    Buffer::Buffer(const vk::raii::PhysicalDevice& physical, const vk::raii::Device& device, vk::DeviceSize size, vk::BufferUsageFlags usage) {
        handle                  = vk::raii::Buffer(device, {.size = size, .usage = usage, .sharingMode = vk::SharingMode::eExclusive});
        const auto requirements = handle.getMemoryRequirements();
        const auto properties   = physical.getMemoryProperties();
        const auto flags        = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        std::uint32_t type      = 0;
        for (; type < properties.memoryTypeCount; ++type)
            if ((requirements.memoryTypeBits & (1u << type)) && (properties.memoryTypes[type].propertyFlags & flags) == flags) break;
        if (type == properties.memoryTypeCount) throw std::runtime_error("Required coherent GPU memory is unavailable.");
        memory = vk::raii::DeviceMemory(device, {.allocationSize = requirements.size, .memoryTypeIndex = type});
        handle.bindMemory(*memory, 0);
        mapped = memory.mapMemory(0, size);
    }

    Renderer::Renderer(HWND window)
        : window(window),
          atlasPixels(2048 * 2048) {
        const vk::ApplicationInfo app{.pApplicationName = "Musicia", .applicationVersion = 1, .pEngineName = "Musicia", .apiVersion = vk::ApiVersion14};
        const std::array extensions{"VK_KHR_surface", "VK_KHR_win32_surface"};
        instance      = vk::raii::Instance(context, {.pApplicationInfo = &app, .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()), .ppEnabledExtensionNames = extensions.data()});
        surface       = vk::raii::SurfaceKHR(instance, vk::Win32SurfaceCreateInfoKHR{.hinstance = GetModuleHandleW(nullptr), .hwnd = window});
        auto devices  = instance.enumeratePhysicalDevices();
        auto selected = std::ranges::find_if(devices, [](const auto& candidate) { return candidate.getProperties().vendorID == 0x10de; });
        if (selected == devices.end()) throw std::runtime_error("The configured NVIDIA Vulkan device is unavailable.");
        physical            = std::move(*selected);
        const auto families = physical.getQueueFamilyProperties();
        for (; family < families.size(); ++family)
            if ((families[family].queueFlags & vk::QueueFlagBits::eGraphics) && physical.getSurfaceSupportKHR(family, *surface)) break;
        if (family == families.size()) throw std::runtime_error("A graphics and presentation queue is required.");
        constexpr float priority = 1;
        const vk::DeviceQueueCreateInfo queueInfo{.queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
        const vk::PhysicalDeviceVulkan13Features features{.synchronization2 = true, .dynamicRendering = true};
        constexpr const char* deviceExtension = "VK_KHR_swapchain";
        device                                = vk::raii::Device(physical, {.pNext = &features, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queueInfo, .enabledExtensionCount = 1, .ppEnabledExtensionNames = &deviceExtension});
        queue                                 = vk::raii::Queue(device, family, 0);

        atlas                       = vk::raii::Image(device, {.imageType = vk::ImageType::e2D, .format = vk::Format::eR8Unorm, .extent = {2048, 2048, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal, .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled, .sharingMode = vk::SharingMode::eExclusive});
        const auto requirements     = atlas.getMemoryRequirements();
        const auto memoryProperties = physical.getMemoryProperties();
        std::uint32_t memoryType    = 0;
        for (; memoryType < memoryProperties.memoryTypeCount; ++memoryType)
            if ((requirements.memoryTypeBits & (1u << memoryType)) && (memoryProperties.memoryTypes[memoryType].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) break;
        atlasMemory = vk::raii::DeviceMemory(device, {.allocationSize = requirements.size, .memoryTypeIndex = memoryType});
        atlas.bindMemory(*atlasMemory, 0);
        atlasView    = vk::raii::ImageView(device, {.image = *atlas, .viewType = vk::ImageViewType::e2D, .format = vk::Format::eR8Unorm, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
        sampler      = vk::raii::Sampler(device, {.magFilter = vk::Filter::eLinear, .minFilter = vk::Filter::eLinear, .mipmapMode = vk::SamplerMipmapMode::eNearest, .addressModeU = vk::SamplerAddressMode::eClampToEdge, .addressModeV = vk::SamplerAddressMode::eClampToEdge, .addressModeW = vk::SamplerAddressMode::eClampToEdge});
        atlasStaging = Buffer(physical, device, atlasPixels.size(), vk::BufferUsageFlagBits::eTransferSrc);
        vertexBuffer = Buffer(physical, device, 4 * 1024 * 1024, vk::BufferUsageFlagBits::eVertexBuffer);
        waveBuffer   = Buffer(physical, device, 16, vk::BufferUsageFlagBits::eStorageBuffer);

        const std::array bindings{vk::DescriptorSetLayoutBinding{.binding = 0, .descriptorType = vk::DescriptorType::eSampledImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment}, vk::DescriptorSetLayoutBinding{.binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eVertex}, vk::DescriptorSetLayoutBinding{.binding = 2, .descriptorType = vk::DescriptorType::eSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment}};
        descriptorLayout = vk::raii::DescriptorSetLayout(device, {.bindingCount = static_cast<std::uint32_t>(bindings.size()), .pBindings = bindings.data()});
        const std::array sizes{vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 1}, vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 1}, vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1}};
        descriptorPool = vk::raii::DescriptorPool(device, {.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, .maxSets = 1, .poolSizeCount = static_cast<std::uint32_t>(sizes.size()), .pPoolSizes = sizes.data()});
        descriptor     = std::move(vk::raii::DescriptorSets(device, {.descriptorPool = *descriptorPool, .descriptorSetCount = 1, .pSetLayouts = &*descriptorLayout}).front());
        const vk::DescriptorImageInfo imageInfo{.imageView = *atlasView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorImageInfo samplerInfo{.sampler = *sampler};
        const vk::DescriptorBufferInfo bufferInfo{.buffer = *waveBuffer.handle, .range = vk::WholeSize};
        const std::array writes{vk::WriteDescriptorSet{.dstSet = *descriptor, .dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &imageInfo}, vk::WriteDescriptorSet{.dstSet = *descriptor, .dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &bufferInfo}, vk::WriteDescriptorSet{.dstSet = *descriptor, .dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampler, .pImageInfo = &samplerInfo}};
        device.updateDescriptorSets(writes, {});
        const vk::PushConstantRange pushRange{.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, .size = sizeof(Push)};
        pipelineLayout = vk::raii::PipelineLayout(device, {.setLayoutCount = 1, .pSetLayouts = &*descriptorLayout, .pushConstantRangeCount = 1, .pPushConstantRanges = &pushRange});

        std::wstring executable(32768, L'\0');
        executable.resize(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())));
        const auto shaderDirectory = std::filesystem::path(executable).parent_path() / "shaders";
        const auto createPipeline  = [&](const std::string& name, bool waveform) {
            std::array<vk::raii::ShaderModule, 2> modules{vk::raii::ShaderModule{nullptr}, vk::raii::ShaderModule{nullptr}};
            std::array<vk::PipelineShaderStageCreateInfo, 2> stages;
            for (std::size_t i = 0; i < 2; ++i) {
                std::ifstream stream(shaderDirectory / (name + (i == 0 ? ".vert.spv" : ".frag.spv")), std::ios::binary | std::ios::ate);
                stream.exceptions(std::ios::failbit | std::ios::badbit);
                std::vector<std::uint32_t> code(static_cast<std::size_t>(stream.tellg()) / 4);
                stream.seekg(0);
                stream.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(code.size() * 4));
                modules[i] = vk::raii::ShaderModule(device, {.codeSize = code.size() * 4, .pCode = code.data()});
                stages[i]  = {.stage = i == 0 ? vk::ShaderStageFlagBits::eVertex : vk::ShaderStageFlagBits::eFragment, .module = *modules[i], .pName = "main"};
            }
            const vk::VertexInputBindingDescription binding{.binding = 0, .stride = sizeof(Vertex), .inputRate = vk::VertexInputRate::eVertex};
            const std::array attributes{vk::VertexInputAttributeDescription{0, 0, vk::Format::eR32G32Sfloat, 0}, vk::VertexInputAttributeDescription{1, 0, vk::Format::eR32G32Sfloat, 8}, vk::VertexInputAttributeDescription{2, 0, vk::Format::eR32G32B32A32Sfloat, 16}, vk::VertexInputAttributeDescription{3, 0, vk::Format::eR32G32B32A32Sfloat, 32}};
            const vk::PipelineVertexInputStateCreateInfo input{.vertexBindingDescriptionCount = waveform ? 0u : 1u, .pVertexBindingDescriptions = &binding, .vertexAttributeDescriptionCount = waveform ? 0u : static_cast<std::uint32_t>(attributes.size()), .pVertexAttributeDescriptions = attributes.data()};
            const vk::PipelineInputAssemblyStateCreateInfo assembly{.topology = vk::PrimitiveTopology::eTriangleList};
            const vk::PipelineViewportStateCreateInfo viewport{.viewportCount = 1, .scissorCount = 1};
            const vk::PipelineRasterizationStateCreateInfo raster{.polygonMode = vk::PolygonMode::eFill, .cullMode = vk::CullModeFlagBits::eNone, .frontFace = vk::FrontFace::eClockwise, .lineWidth = 1};
            const vk::PipelineMultisampleStateCreateInfo multisample{.rasterizationSamples = vk::SampleCountFlagBits::e1};
            const vk::PipelineColorBlendAttachmentState blend{.blendEnable = true, .srcColorBlendFactor = vk::BlendFactor::eOne, .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha, .colorBlendOp = vk::BlendOp::eAdd, .srcAlphaBlendFactor = vk::BlendFactor::eOne, .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha, .alphaBlendOp = vk::BlendOp::eAdd, .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
            const vk::PipelineColorBlendStateCreateInfo blending{.attachmentCount = 1, .pAttachments = &blend};
            const std::array dynamics{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
            const vk::PipelineDynamicStateCreateInfo dynamic{.dynamicStateCount = static_cast<std::uint32_t>(dynamics.size()), .pDynamicStates = dynamics.data()};
            constexpr vk::Format format = vk::Format::eB8G8R8A8Srgb;
            const vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount = 1, .pColorAttachmentFormats = &format};
            return vk::raii::Pipeline(device, nullptr, vk::GraphicsPipelineCreateInfo{.pNext = &rendering, .stageCount = 2, .pStages = stages.data(), .pVertexInputState = &input, .pInputAssemblyState = &assembly, .pViewportState = &viewport, .pRasterizationState = &raster, .pMultisampleState = &multisample, .pColorBlendState = &blending, .pDynamicState = &dynamic, .layout = *pipelineLayout});
        };
        uiPipeline        = createPipeline("ui", false);
        wavePipeline      = createPipeline("wave", true);
        commandPool       = vk::raii::CommandPool(device, {.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer, .queueFamilyIndex = family});
        command           = std::move(vk::raii::CommandBuffers(device, {.commandPool = *commandPool, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1}).front());
        acquired          = vk::raii::Semaphore(device, vk::SemaphoreCreateInfo{});
        finished          = vk::raii::Fence(device, {.flags = vk::FenceCreateFlagBits::eSignaled});
        const auto result = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), reinterpret_cast<IUnknown**>(fonts.GetAddressOf()));
        if (FAILED(result)) throw std::runtime_error("Unable to initialize DirectWrite.");
        resize();
    }

    Renderer::~Renderer() {
        if (*device) device.waitIdle();
    }

    void Renderer::resize() {
        device.waitIdle();
        const auto capabilities = physical.getSurfaceCapabilitiesKHR(*surface);
        extent                  = capabilities.currentExtent;
        if (!extent.width || !extent.height) return;
        views.clear();
        presented.clear();
        images.clear();
        swapchain.clear();
        auto count = std::max(capabilities.minImageCount, 3u);
        if (capabilities.maxImageCount) count = std::min(count, capabilities.maxImageCount);
        swapchain = vk::raii::SwapchainKHR(device, {.surface = *surface, .minImageCount = count, .imageFormat = vk::Format::eB8G8R8A8Srgb, .imageColorSpace = vk::ColorSpaceKHR::eSrgbNonlinear, .imageExtent = extent, .imageArrayLayers = 1, .imageUsage = vk::ImageUsageFlagBits::eColorAttachment, .imageSharingMode = vk::SharingMode::eExclusive, .preTransform = capabilities.currentTransform, .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque, .presentMode = vk::PresentModeKHR::eFifo, .clipped = true});
        images    = swapchain.getImages();
        for (const auto image : images) {
            views.emplace_back(device, vk::ImageViewCreateInfo{.image = image, .viewType = vk::ImageViewType::e2D, .format = vk::Format::eB8G8R8A8Srgb, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
            presented.emplace_back(device, vk::SemaphoreCreateInfo{});
        }
    }

    void Renderer::upload(const musicia::AudioClip& clip) {
        const auto peaks = waveform(clip);
        device.waitIdle();
        channels = clip.channels;
        frames   = static_cast<std::uint32_t>(clip.frames);
        peakOffsets.clear();
        peakBlocks.clear();
        std::size_t floats = 0;
        for (const auto& level : peaks) {
            peakOffsets.push_back(static_cast<std::uint32_t>(floats));
            peakBlocks.push_back(static_cast<std::uint32_t>(level.blockFrames));
            floats += level.values.size() * sizeof(Peak) / sizeof(float);
        }
        rawOffset = static_cast<std::uint32_t>(floats);
        floats += clip.samples.size();
        waveBuffer        = Buffer(physical, device, floats * sizeof(float), vk::BufferUsageFlagBits::eStorageBuffer);
        auto* destination = static_cast<float*>(waveBuffer.mapped);
        for (std::size_t i = 0; i < peaks.size(); ++i) std::memcpy(destination + peakOffsets[i], peaks[i].values.data(), peaks[i].values.size() * sizeof(Peak));
        std::memcpy(destination + rawOffset, clip.samples.data(), clip.samples.size() * sizeof(float));
        const vk::DescriptorBufferInfo info{.buffer = *waveBuffer.handle, .range = vk::WholeSize};
        device.updateDescriptorSets(vk::WriteDescriptorSet{.dstSet = *descriptor, .dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &info}, {});
        resetFonts();
    }

    void Renderer::resetFonts() {
        glyphs.clear();
        std::ranges::fill(atlasPixels, std::uint8_t{0});
        atlasX     = 1;
        atlasY     = 1;
        atlasRow   = 0;
        atlasDirty = true;
    }

    void Renderer::rect(Rect area, Color color, float radius) {
        area.x *= scale;
        area.y *= scale;
        area.width *= scale;
        area.height *= scale;
        for (const auto& corner : std::array{std::array{0.f, 0.f}, std::array{1.f, 0.f}, std::array{1.f, 1.f}, std::array{0.f, 0.f}, std::array{1.f, 1.f}, std::array{0.f, 1.f}}) vertices.push_back({area.x + area.width * corner[0], area.y + area.height * corner[1], area.width * corner[0], area.height * corner[1], color, radius > 0 ? 2.f : 0.f, radius * scale, area.width, area.height});
    }

    void Renderer::text(std::wstring_view value, float size, float x, float y, Color color, float width, bool centered, bool fit) {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
        auto result = fonts->CreateTextFormat(L"Segoe UI Variable", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * scale, L"en-US", &format);
        if (FAILED(result)) throw std::runtime_error("Unable to create text format.");
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (centered) format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        if (!fit) {
            Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
            fonts->CreateEllipsisTrimmingSign(format.Get(), &ellipsis);
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            format->SetTrimming(&trimming, ellipsis.Get());
        }
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        result = fonts->CreateTextLayout(value.data(), static_cast<UINT32>(value.size()), format.Get(), width * scale, size * scale * 2, &layout);
        if (FAILED(result)) throw std::runtime_error("Unable to lay out text.");
        if (fit) {
            DWRITE_TEXT_METRICS metrics{};
            layout->GetMetrics(&metrics);
            if (metrics.widthIncludingTrailingWhitespace > width * scale) return;
        }
        textColor = color;
        result    = layout->Draw(nullptr, this, x * scale, y * scale);
        if (FAILED(result)) throw std::runtime_error("Unable to rasterize text.");
    }

    void Renderer::draw() {
        if (!extent.width || !extent.height) return;
        (void) device.waitForFences(*finished, true, std::numeric_limits<std::uint64_t>::max());
        std::uint32_t index{};
        vk::Result acquireResult;
        try {
            std::tie(acquireResult, index) = swapchain.acquireNextImage(std::numeric_limits<std::uint64_t>::max(), *acquired, nullptr);
        } catch (const vk::OutOfDateKHRError&) {
            resize();
            return;
        }
        std::memcpy(vertexBuffer.mapped, vertices.data(), vertices.size() * sizeof(Vertex));
        command.reset();
        command.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        if (atlasDirty) {
            std::memcpy(atlasStaging.mapped, atlasPixels.data(), atlasPixels.size());
            vk::ImageMemoryBarrier2 barrier{.srcStageMask = atlasInitialized ? vk::PipelineStageFlagBits2::eFragmentShader : vk::PipelineStageFlagBits2::eNone, .srcAccessMask = atlasInitialized ? vk::AccessFlagBits2::eShaderSampledRead : vk::AccessFlagBits2::eNone, .dstStageMask = vk::PipelineStageFlagBits2::eTransfer, .dstAccessMask = vk::AccessFlagBits2::eTransferWrite, .oldLayout = atlasInitialized ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eTransferDstOptimal, .srcQueueFamilyIndex = vk::QueueFamilyIgnored, .dstQueueFamilyIndex = vk::QueueFamilyIgnored, .image = *atlas, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
            command.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
            command.copyBufferToImage(*atlasStaging.handle, *atlas, vk::ImageLayout::eTransferDstOptimal, vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1}, .imageExtent = {2048, 2048, 1}});
            barrier.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer;
            barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
            barrier.dstStageMask  = vk::PipelineStageFlagBits2::eFragmentShader;
            barrier.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
            barrier.oldLayout     = vk::ImageLayout::eTransferDstOptimal;
            barrier.newLayout     = vk::ImageLayout::eShaderReadOnlyOptimal;
            command.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
            atlasInitialized = true;
            atlasDirty       = false;
        }
        vk::ImageMemoryBarrier2 presentBarrier{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput, .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput, .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite, .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eColorAttachmentOptimal, .srcQueueFamilyIndex = vk::QueueFamilyIgnored, .dstQueueFamilyIndex = vk::QueueFamilyIgnored, .image = images[index], .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        command.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &presentBarrier});
        const auto linear = [](float value) { return value <= .04045f ? value / 12.92f : std::pow((value + .055f) / 1.055f, 2.4f); };
        const vk::RenderingAttachmentInfo attachment{.imageView = *views[index], .imageLayout = vk::ImageLayout::eColorAttachmentOptimal, .loadOp = vk::AttachmentLoadOp::eClear, .storeOp = vk::AttachmentStoreOp::eStore, .clearValue = vk::ClearValue(vk::ClearColorValue(std::array{linear(background.r), linear(background.g), linear(background.b), background.a}))};
        command.beginRendering({.renderArea = {{0, 0}, extent}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &attachment});
        command.setViewport(0, vk::Viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1});
        command.setScissor(0, vk::Rect2D{{0, 0}, extent});
        command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipelineLayout, 0, *descriptor, {});
        command.bindVertexBuffers(0, *vertexBuffer.handle, vk::DeviceSize{0});
        Push push{.screen = {static_cast<float>(extent.width), static_cast<float>(extent.height), 0, scale}};
        command.pushConstants<Push>(*pipelineLayout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, push);
        command.bindPipeline(vk::PipelineBindPoint::eGraphics, *uiPipeline);
        command.draw(waveLayer, 1, 0, 0);
        if (showWave) {
            command.bindPipeline(vk::PipelineBindPoint::eGraphics, *wavePipeline);
            // Both views read the same peak pyramid and PCM buffer.
            for (const bool overview : {false, true}) {
                const auto area = overview ? overviewRect : waveRect;
                const double first = overview ? 0 : waveFirst;
                const double count = overview ? frames : waveCount;
                const bool detail = !overview && count <= area.width / 3.5f;
                const auto columns = static_cast<std::uint32_t>(detail ? area.width * scale : area.width / (overview ? 2.f : 3.5f));
                push.view = {static_cast<float>(count / columns), static_cast<float>(first - std::floor(first)), area.width * scale / columns, detail ? 1.f : 0.f};
                push.data = {static_cast<std::uint32_t>(first), 0, 0, rawOffset};
                push.track = {channels, 0, frames, 0};
                push.screen[2] = overview ? 1.f : 0.f;
                push.playhead = static_cast<float>((area.x + (wavePosition - first) / count * area.width) * scale);
                for (std::size_t level = 0; level < peakBlocks.size() && peakBlocks[level] <= push.view[0] / 4; ++level) {
                    push.data[1] = peakOffsets[level];
                    push.data[2] = peakBlocks[level];
                    push.track[3] = (frames + peakBlocks[level] - 1) / peakBlocks[level];
                }
                const auto color = overview ? muted : waveformColor;
                push.color = {color.r, color.g, color.b, color.a};
                const auto tracks = overview ? 1u : channels;
                for (std::uint32_t channel = 0; channel < tracks; ++channel) {
                    const auto channelHeight = area.height / tracks;
                    push.rect = {area.x * scale, (area.y + channelHeight * channel) * scale, area.width * scale, channelHeight * scale};
                    push.track[1] = channel;
                    command.pushConstants<Push>(*pipelineLayout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, push);
                    command.draw(6, columns, 0, 0);
                }
            }
        }
        command.bindPipeline(vk::PipelineBindPoint::eGraphics, *uiPipeline);
        command.draw(static_cast<std::uint32_t>(vertices.size()) - waveLayer, 1, waveLayer, 0);
        command.endRendering();
        presentBarrier.srcStageMask  = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        presentBarrier.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
        presentBarrier.dstStageMask  = vk::PipelineStageFlagBits2::eNone;
        presentBarrier.dstAccessMask = vk::AccessFlagBits2::eNone;
        presentBarrier.oldLayout     = vk::ImageLayout::eColorAttachmentOptimal;
        presentBarrier.newLayout     = vk::ImageLayout::ePresentSrcKHR;
        command.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &presentBarrier});
        command.end();
        const vk::SemaphoreSubmitInfo wait{.semaphore = *acquired, .stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput};
        const vk::SemaphoreSubmitInfo signal{.semaphore = *presented[index], .stageMask = vk::PipelineStageFlagBits2::eAllCommands};
        const vk::CommandBufferSubmitInfo buffer{.commandBuffer = *command};
        device.resetFences(*finished);
        queue.submit2(vk::SubmitInfo2{.waitSemaphoreInfoCount = 1, .pWaitSemaphoreInfos = &wait, .commandBufferInfoCount = 1, .pCommandBufferInfos = &buffer, .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal}, *finished);
        try {
            const auto result = queue.presentKHR({.waitSemaphoreCount = 1, .pWaitSemaphores = &*presented[index], .swapchainCount = 1, .pSwapchains = &*swapchain, .pImageIndices = &index});
            if (result == vk::Result::eSuboptimalKHR || acquireResult == vk::Result::eSuboptimalKHR) resize();
        } catch (const vk::OutOfDateKHRError&) {
            resize();
        }
    }

    HRESULT Renderer::QueryInterface(REFIID iid, void** object) {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWritePixelSnapping) || iid == __uuidof(IDWriteTextRenderer)) {
            *object = static_cast<IDWriteTextRenderer*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG Renderer::AddRef() {
        return 1;
    }

    ULONG Renderer::Release() {
        return 1;
    }

    HRESULT Renderer::IsPixelSnappingDisabled(void*, BOOL* disabled) {
        *disabled = FALSE;
        return S_OK;
    }

    HRESULT Renderer::GetCurrentTransform(void*, DWRITE_MATRIX* transform) {
        *transform = {1, 0, 0, 1, 0, 0};
        return S_OK;
    }

    HRESULT Renderer::GetPixelsPerDip(void*, FLOAT* pixels) {
        *pixels = 1;
        return S_OK;
    }

    HRESULT Renderer::DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE measuring, const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) {
        try {
            for (UINT32 i = 0; i < run->glyphCount; ++i) {
                const auto key = std::tuple{run->fontFace, run->glyphIndices[i], run->fontEmSize};
                auto found     = glyphs.find(key);
                if (found == glyphs.end()) {
                    const FLOAT advance = 0;
                    const DWRITE_GLYPH_RUN single{run->fontFace, run->fontEmSize, 1, &run->glyphIndices[i], &advance, nullptr, run->isSideways, 0};
                    Microsoft::WRL::ComPtr<IDWriteGlyphRunAnalysis> analysis;
                    auto result = fonts->CreateGlyphRunAnalysis(&single, nullptr, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, measuring, DWRITE_GRID_FIT_MODE_ENABLED, DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0, 0, &analysis);
                    if (FAILED(result)) return result;
                    RECT bounds{};
                    result = analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &bounds);
                    if (FAILED(result)) return result;
                    Glyph glyph{.face = run->fontFace, .left = bounds.left, .top = bounds.top, .width = bounds.right - bounds.left, .height = bounds.bottom - bounds.top};
                    if (atlasX + glyph.width + 1 >= 2048) {
                        atlasX = 1;
                        atlasY += atlasRow + 2;
                        atlasRow = 0;
                    }
                    glyph.x = atlasX;
                    glyph.y = atlasY;
                    if (glyph.width && glyph.height) {
                        std::vector<BYTE> pixels(glyph.width * glyph.height);
                        result = analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &bounds, pixels.data(), static_cast<UINT32>(pixels.size()));
                        if (FAILED(result)) return result;
                        for (int row = 0; row < glyph.height; ++row) std::memcpy(atlasPixels.data() + (atlasY + row) * 2048 + atlasX, pixels.data() + row * glyph.width, glyph.width);
                        atlasX += glyph.width + 2;
                        atlasRow   = std::max(atlasRow, glyph.height);
                        atlasDirty = true;
                    }
                    found = glyphs.emplace(key, std::move(glyph)).first;
                }
                const auto& glyph     = found->second;
                const float direction = run->bidiLevel & 1 ? -1.f : 1.f;
                const auto offset     = run->glyphOffsets ? run->glyphOffsets[i] : DWRITE_GLYPH_OFFSET{};
                const float gx        = std::round(x + direction * offset.advanceOffset - (direction < 0 ? run->glyphAdvances[i] : 0)) + glyph.left;
                const float gy        = std::round(y - offset.ascenderOffset) + glyph.top;
                if (glyph.width && glyph.height) {
                    for (const auto& corner : std::array{std::array{0.f, 0.f}, std::array{1.f, 0.f}, std::array{1.f, 1.f}, std::array{0.f, 0.f}, std::array{1.f, 1.f}, std::array{0.f, 1.f}}) vertices.push_back({gx + glyph.width * corner[0], gy + glyph.height * corner[1], (glyph.x + glyph.width * corner[0]) / 2048.f, (glyph.y + glyph.height * corner[1]) / 2048.f, textColor, 1});
                }
                x += direction * run->glyphAdvances[i];
            }
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }

    HRESULT Renderer::DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) {
        return E_NOTIMPL;
    }

    HRESULT Renderer::DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) {
        return E_NOTIMPL;
    }

    HRESULT Renderer::DrawInlineObject(void* drawingContext, FLOAT x, FLOAT y, IDWriteInlineObject* object, BOOL sideways, BOOL rtl, IUnknown* effect) {
        return object->Draw(drawingContext, this, x, y, sideways, rtl, effect);
    }
}
