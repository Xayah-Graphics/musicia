find_package(Vulkan 1.4 REQUIRED)

add_library(vulkan_module STATIC)
target_sources(vulkan_module PUBLIC FILE_SET CXX_MODULES BASE_DIRS "${Vulkan_INCLUDE_DIRS}" FILES "${Vulkan_INCLUDE_DIRS}/vulkan/vulkan.cppm")
target_link_libraries(vulkan_module PUBLIC Vulkan::Vulkan)
target_compile_definitions(vulkan_module PUBLIC VK_USE_PLATFORM_WIN32_KHR VULKAN_HPP_NO_STRUCT_CONSTRUCTORS NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(vulkan_module PROPERTIES CXX_MODULE_STD ON)
