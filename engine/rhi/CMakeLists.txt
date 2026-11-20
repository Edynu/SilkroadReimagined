add_library(sr_rhi STATIC
    src/Rhi.cpp
    src/vulkan/VulkanDevice.cpp
    src/vulkan/VmaImplementation.cpp
)
add_library(sr::rhi ALIAS sr_rhi)

target_include_directories(sr_rhi PUBLIC include PRIVATE src)
target_include_directories(sr_rhi SYSTEM PRIVATE ${vulkanmemoryallocator_SOURCE_DIR}/include)
target_link_libraries(sr_rhi PRIVATE Vulkan::Vulkan sr_compile_options)
target_compile_definitions(sr_rhi PRIVATE
    VMA_STATIC_VULKAN_FUNCTIONS=1
    VMA_DYNAMIC_VULKAN_FUNCTIONS=0
    $<$<PLATFORM_ID:Windows>:VK_USE_PLATFORM_WIN32_KHR>)
