# Every runtime binding uses Vulkan for GPU delivery. A regular dependency lets
# the Windows loader release it after the runtime's native owners have retired.
add_library(simxr_vulkan_loader INTERFACE)
target_link_libraries(simxr_vulkan_loader INTERFACE Vulkan::Vulkan)
