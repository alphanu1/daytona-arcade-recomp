#include <kos.h>

#include "host_limits.h"
#include "host_pvr.h"
#include "pc_endjinn_frame.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

extern "C" void enj_host_input_request_quit(void);
extern "C" void enj_host_input_shutdown(void);

namespace {

vid_mode_t g_vid_mode{640, 480};
float g_bg_color[3] = {0.0f, 0.0f, 0.0f};
SDL_Window *g_window = nullptr;
VkInstance g_instance = VK_NULL_HANDLE;
VkSurfaceKHR g_surface = VK_NULL_HANDLE;
VkPhysicalDevice g_physical_device = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
VkQueue g_graphics_queue = VK_NULL_HANDLE;
uint32_t g_graphics_queue_family = 0u;
VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
VkFormat g_swapchain_format = VK_FORMAT_B8G8R8A8_UNORM;
VkExtent2D g_extent{1280u, 960u};
VkRect2D g_content_rect{{0, 0}, {1280u, 960u}};
std::vector<VkImage> g_swapchain_images;
std::vector<VkImageView> g_swapchain_views;
/* The replay-raster gate may run from a macOS GUI launcher whose drawable is
 * not presentable to the process.  This explicit opt-in target preserves the
 * Vulkan/PVR render path but never calls acquire/present. */
bool g_offscreen_capture = false;
bool g_offscreen_color_initialized = false;
VkImage g_offscreen_color_image = VK_NULL_HANDLE;
VkDeviceMemory g_offscreen_color_memory = VK_NULL_HANDLE;
VkImageView g_offscreen_color_view = VK_NULL_HANDLE;
VkImage g_depth_image = VK_NULL_HANDLE;
VkDeviceMemory g_depth_memory = VK_NULL_HANDLE;
VkImageView g_depth_view = VK_NULL_HANDLE;
VkFormat g_depth_format = VK_FORMAT_UNDEFINED;
VkRenderPass g_render_pass = VK_NULL_HANDLE;
std::vector<VkFramebuffer> g_framebuffers;
VkPipelineLayout g_pipeline_layout = VK_NULL_HANDLE;
VkPipeline g_opaque_pipeline = VK_NULL_HANDLE;
VkPipeline g_opaque_no_depth_write_pipeline = VK_NULL_HANDLE;
VkPipeline g_opaque_no_depth_test_pipeline = VK_NULL_HANDLE;
VkPipeline g_punch_through_pipeline = VK_NULL_HANDLE;
VkPipeline g_punch_through_no_depth_write_pipeline = VK_NULL_HANDLE;
VkPipeline g_translucent_pipeline = VK_NULL_HANDLE;
VkPipeline g_modifier_translucent_depth_pipeline = VK_NULL_HANDLE;
VkPipeline g_modifier_xor_pipeline = VK_NULL_HANDLE;
VkPipeline g_modifier_or_pipeline = VK_NULL_HANDLE;
VkPipeline g_modifier_include_pipeline = VK_NULL_HANDLE;
VkPipeline g_modifier_exclude_pipeline = VK_NULL_HANDLE;
VkPipeline g_modifier_clear_current_pipeline = VK_NULL_HANDLE;
VkPipeline g_modifier_opaque_receiver_pipeline = VK_NULL_HANDLE;
VkPipeline g_modifier_legacy_include_pipeline = VK_NULL_HANDLE;
VkPipeline g_modifier_legacy_exclude_pipeline = VK_NULL_HANDLE;
VkPipeline g_modifier_translucent_receiver_pipeline = VK_NULL_HANDLE;
VkDescriptorSetLayout g_texture_set_layout = VK_NULL_HANDLE;
VkDescriptorPool g_descriptor_pool = VK_NULL_HANDLE;
VkCommandPool g_command_pool = VK_NULL_HANDLE;
std::vector<VkCommandBuffer> g_command_buffers;
VkSemaphore g_image_available = VK_NULL_HANDLE;
VkSemaphore g_render_finished = VK_NULL_HANDLE;
VkFence g_in_flight = VK_NULL_HANDLE;
VkBuffer g_vertex_buffer = VK_NULL_HANDLE;
VkDeviceMemory g_vertex_memory = VK_NULL_HANDLE;
size_t g_vertex_capacity = 0u;
VkBuffer g_translucent_modifier_buffer = VK_NULL_HANDLE;
VkDeviceMemory g_translucent_modifier_memory = VK_NULL_HANDLE;
uint64_t g_presented_frames = 0u;
bool g_translucent_autosort = true;
bool g_fsaa_enabled = false;
bool g_fullscreen_toggle_requested = false;
int g_last_window_width = 1280;
int g_last_window_height = 960;
bool g_swapchain_readback_supported = false;
bool g_screenshot_environment_checked = false;
bool g_screenshot_pending = false;
std::string g_screenshot_path;
uint64_t g_screenshot_frame = 0u;
/* ENJ_SCREENSHOT_EVERY=N (testing): after the first capture, one every N
 * presented frames, each to ENJ_SCREENSHOT_PATH with "-FRAME.ppm" appended. */
uint64_t screenshot_every()
{
    static const uint64_t every = [] {
        const char *v = std::getenv("ENJ_SCREENSHOT_EVERY");
        return v != nullptr ? std::strtoull(v, nullptr, 10) : 0ull;
    }();
    return every;
}

std::string g_screenshot_base;


/* shaders/pvr_state.glsl's TextureState. */
struct TexturePush {
    uint32_t indexed;
    uint32_t palette_base;
    uint32_t filter_mode;
    uint32_t flags;
    uint32_t modifier_count;
    uint32_t modifier_area;
    uint32_t tex_env;
    float color_scale;
    uint32_t mip_bias;
    float alpha_ref;
    uint32_t reserved0;
    uint32_t reserved1;
};
constexpr uint32_t kFlagTextured = 1u, kFlagUseAlpha = 2u, kFlagIgnoreTexAlpha = 4u, kFlagPunch = 8u,
                   kFlagClampU = 16u, kFlagClampV = 32u, kFlagFlipU = 64u, kFlagFlipV = 128u;

/* Pipelines for ordinary polygons, one per PVR depth/blend state. */
VkShaderModule g_vert_module = VK_NULL_HANDLE;
VkShaderModule g_pvr_frag_module = VK_NULL_HANDLE;
std::map<uint32_t, VkPipeline> g_state_pipelines;

using pc_endjinn::DrawBatch;
using pc_endjinn::FrameDrawData;
using pc_endjinn::GpuModifierTriangle;
using pc_endjinn::PcVertex;
using enj_host_pvr::QueuedPrimitive;

struct GpuImage {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkSampler sampler{VK_NULL_HANDLE};
    uint32_t width{};
    uint32_t height{};
    uint32_t mip_count{};
    VkFormat format{VK_FORMAT_UNDEFINED};
};

struct GpuTexture {
    pvr_ptr_t source{};
    uint32_t source_format{};
    uint32_t width{};
    uint32_t height{};
    pvr_filter_mode_t filter{PVR_FILTER_NEAREST};
    uint64_t revision{};
    bool indexed{};
    uint32_t palette_base{};
    GpuImage image;
    VkDescriptorSet descriptor{VK_NULL_HANDLE};
};

GpuImage g_white_texture;
GpuImage g_default_index_texture;
GpuImage g_palette_texture;
std::vector<GpuTexture> g_texture_cache;
VkDescriptorSet g_default_texture_descriptor = VK_NULL_HANDLE;
uint64_t g_uploaded_palette_revision = 0u;

bool create_draw_resources();

bool offscreen_capture_requested()
{
    const char *const value = std::getenv("ENJ_OFFSCREEN_CAPTURE");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

bool native_composition_raster_requested()
{
    const char *const value = std::getenv("ENJ_NATIVE_COMPOSITION_RASTER");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

bool SDLCALL pc_endjinn_event_watch(void *, SDL_Event *event)
{
    if (event != nullptr &&
        (event->type == SDL_EVENT_QUIT || event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
         (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_ESCAPE))) {
        enj_host_input_request_quit();
    }
    if (event != nullptr && event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat) {
        const bool alt_enter = event->key.key == SDLK_RETURN &&
            (event->key.mod & SDL_KMOD_ALT) != 0;
        if (event->key.key == SDLK_F11 || alt_enter) {
            g_fullscreen_toggle_requested = true;
        }
    }
    return true;
}

void update_content_rect()
{
    if (g_offscreen_capture && native_composition_raster_requested()) {
        g_content_rect = {{0, 0}, g_extent};
        return;
    }
    const uint64_t width = g_extent.width;
    const uint64_t height = g_extent.height;
    uint32_t content_width = g_extent.width;
    uint32_t content_height = g_extent.height;
#if defined(ENJ_INTEGER_PRESENT_WIDTH) != defined(ENJ_INTEGER_PRESENT_HEIGHT)
#error "define both ENJ_INTEGER_PRESENT_WIDTH and ENJ_INTEGER_PRESENT_HEIGHT"
#elif defined(ENJ_INTEGER_PRESENT_WIDTH)
    static_assert(ENJ_INTEGER_PRESENT_WIDTH > 0,
                  "integer presentation width must be positive");
    static_assert(ENJ_INTEGER_PRESENT_HEIGHT > 0,
                  "integer presentation height must be positive");
    const uint32_t integer_scale = std::min(
        g_extent.width / static_cast<uint32_t>(ENJ_INTEGER_PRESENT_WIDTH),
        g_extent.height / static_cast<uint32_t>(ENJ_INTEGER_PRESENT_HEIGHT));
    if (integer_scale != 0u) {
        content_width = static_cast<uint32_t>(ENJ_INTEGER_PRESENT_WIDTH) *
            integer_scale;
        content_height = static_cast<uint32_t>(ENJ_INTEGER_PRESENT_HEIGHT) *
            integer_scale;
    } else if (width * ENJ_INTEGER_PRESENT_HEIGHT >
               height * ENJ_INTEGER_PRESENT_WIDTH) {
        content_width = static_cast<uint32_t>(
            height * ENJ_INTEGER_PRESENT_WIDTH / ENJ_INTEGER_PRESENT_HEIGHT);
    } else {
        content_height = static_cast<uint32_t>(
            width * ENJ_INTEGER_PRESENT_HEIGHT / ENJ_INTEGER_PRESENT_WIDTH);
    }
#else
    if (width * 3u > height * 4u) {
        content_width = static_cast<uint32_t>(height * 4u / 3u);
    } else if (width * 3u < height * 4u) {
        content_height = static_cast<uint32_t>(width * 3u / 4u);
    }
#endif
    g_content_rect.offset.x = static_cast<int32_t>((g_extent.width - content_width) / 2u);
    g_content_rect.offset.y = static_cast<int32_t>((g_extent.height - content_height) / 2u);
    g_content_rect.extent = {content_width, content_height};
}

void update_window_mode()
{
    if (g_window == nullptr) {
        return;
    }
    if (g_fullscreen_toggle_requested) {
        const SDL_WindowFlags flags = SDL_GetWindowFlags(g_window);
        const bool fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0u;
        (void)SDL_SetWindowFullscreen(g_window, !fullscreen);
        g_fullscreen_toggle_requested = false;
        return;
    }
    if ((SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0u) {
        return;
    }

    int width = 0;
    int height = 0;
    SDL_GetWindowSize(g_window, &width, &height);
    if (width <= 0 || height <= 0) {
        return;
    }
    if (std::abs(width * 3 - height * 4) > 4) {
        const int width_delta = std::abs(width - g_last_window_width);
        const int height_delta = std::abs(height - g_last_window_height);
        if (width_delta >= height_delta) {
            height = std::max(1, width * 3 / 4);
        } else {
            width = std::max(1, height * 4 / 3);
        }
        SDL_SetWindowSize(g_window, width, height);
    }
    g_last_window_width = width;
    g_last_window_height = height;
}

void set_window_title(const char *title)
{
    if (g_window != nullptr && title != nullptr) {
        SDL_SetWindowTitle(g_window, title);
    }
}

std::vector<char> read_file(const char *path)
{
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
        return {};
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return {};
    }
    std::vector<char> bytes(static_cast<size_t>(size));
    file.seekg(0);
    file.read(bytes.data(), size);
    return bytes;
}

/* SPIR-V built by CMake (glslc) and embedded: pc_shaders.inc defines
 * kEmbeddedShaders[] = {{"flat.vert.spv", bytes, size}, ...}. */
struct EmbeddedShader {
    const char *name;
    const unsigned char *bytes;
    size_t size;
};
#include "pc_shaders.inc"

std::vector<char> read_shader_file(const char *name)
{
    for (const EmbeddedShader &shader : kEmbeddedShaders) {
        if (std::strcmp(shader.name, name) == 0) {
            return std::vector<char>(reinterpret_cast<const char *>(shader.bytes),
                                     reinterpret_cast<const char *>(shader.bytes) + shader.size);
        }
    }
    const char *roots[] = {
        "",
        "build/pc-endjinn/",
    };
    for (const char *root : roots) {
        char path[512];
        std::snprintf(path, sizeof(path), "%s%s", root, name);
        std::vector<char> bytes = read_file(path);
        if (!bytes.empty()) {
            return bytes;
        }
    }
    return {};
}

float clamp01(float value)
{
    return std::min(std::max(value, 0.0f), 1.0f);
}

uint32_t find_memory_type(uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(g_physical_device, &props);
    for (uint32_t i = 0u; i < props.memoryTypeCount; i++) {
        if ((bits & (1u << i)) != 0u &&
            (props.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return 0u;
}

VkShaderModule create_shader_module(const std::vector<char> &bytes)
{
    if (bytes.empty()) {
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytes.size();
    info.pCode = reinterpret_cast<const uint32_t *>(bytes.data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_device, &info, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

void destroy_gpu_image(GpuImage &texture)
{
    if (texture.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(g_device, texture.sampler, nullptr);
    }
    if (texture.view != VK_NULL_HANDLE) {
        vkDestroyImageView(g_device, texture.view, nullptr);
    }
    if (texture.image != VK_NULL_HANDLE) {
        vkDestroyImage(g_device, texture.image, nullptr);
    }
    if (texture.memory != VK_NULL_HANDLE) {
        vkFreeMemory(g_device, texture.memory, nullptr);
    }
    texture = {};
}

void destroy_texture_resources()
{
    for (GpuTexture &texture : g_texture_cache) {
        destroy_gpu_image(texture.image);
    }
    g_texture_cache.clear();
    g_default_texture_descriptor = VK_NULL_HANDLE;
    destroy_gpu_image(g_white_texture);
    destroy_gpu_image(g_default_index_texture);
    destroy_gpu_image(g_palette_texture);
    g_uploaded_palette_revision = 0u;
    if (g_descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(g_device, g_descriptor_pool, nullptr);
        g_descriptor_pool = VK_NULL_HANDLE;
    }
}

bool create_staging_buffer(VkDeviceSize size, VkBuffer &buffer,
                           VkDeviceMemory &memory)
{
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_device, &info, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_device, buffer, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = find_memory_type(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_device, &alloc, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(g_device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }
    return vkBindBufferMemory(g_device, buffer, memory, 0u) == VK_SUCCESS;
}

bool create_readback_buffer(VkDeviceSize size, VkBuffer &buffer,
                            VkDeviceMemory &memory)
{
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_device, &info, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_device, buffer, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = find_memory_type(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_device, &alloc, nullptr, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(g_device, buffer, memory, 0u) != VK_SUCCESS) {
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(g_device, memory, nullptr);
            memory = VK_NULL_HANDLE;
        }
        vkDestroyBuffer(g_device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool create_translucent_modifier_buffer()
{
    const VkDeviceSize size = static_cast<VkDeviceSize>(
        enj_host_limits::translucent_modifier_triangles) *
        sizeof(GpuModifierTriangle);
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_device, &info, nullptr,
                       &g_translucent_modifier_buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_device, g_translucent_modifier_buffer, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = find_memory_type(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_device, &alloc, nullptr,
                         &g_translucent_modifier_memory) != VK_SUCCESS ||
        vkBindBufferMemory(g_device, g_translucent_modifier_buffer,
                           g_translucent_modifier_memory, 0u) != VK_SUCCESS) {
        if (g_translucent_modifier_memory != VK_NULL_HANDLE) {
            vkFreeMemory(g_device, g_translucent_modifier_memory, nullptr);
            g_translucent_modifier_memory = VK_NULL_HANDLE;
        }
        vkDestroyBuffer(g_device, g_translucent_modifier_buffer, nullptr);
        g_translucent_modifier_buffer = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void load_screenshot_request_from_environment()
{
    if (g_screenshot_environment_checked) {
        return;
    }
    g_screenshot_environment_checked = true;
    const char *const candidate = std::getenv("ENJ_SCREENSHOT_PATH");
    if (candidate == nullptr || *candidate == '\0') {
        return;
    }
    g_screenshot_path = candidate;
    g_screenshot_pending = true;
    const char *const frame_text = std::getenv("ENJ_SCREENSHOT_FRAME");
    if (frame_text != nullptr && *frame_text != '\0') {
        char *end = nullptr;
        const unsigned long long parsed = std::strtoull(frame_text, &end, 10);
        if (end != frame_text && *end == '\0') {
            g_screenshot_frame = static_cast<uint64_t>(parsed);
        }
    }
}

const char *screenshot_path()
{
    load_screenshot_request_from_environment();
    if (!g_screenshot_pending || g_presented_frames < g_screenshot_frame) {
        return nullptr;
    }
    if (screenshot_every() != 0u) {
        if (g_screenshot_base.empty()) {
            g_screenshot_base = g_screenshot_path;
        }
        char suffix[32];
        std::snprintf(suffix, sizeof suffix, "-%06llu.ppm", static_cast<unsigned long long>(g_screenshot_frame));
        g_screenshot_path = g_screenshot_base + suffix;
    }
    return g_screenshot_path.c_str();
}

void finish_screenshot_request()
{
    if (screenshot_every() != 0u) {
        g_screenshot_frame += screenshot_every();
        return;
    }
    g_screenshot_pending = false;
    g_screenshot_path.clear();
}

bool write_screenshot_ppm(const char *path, VkDeviceMemory memory,
                          VkDeviceSize size)
{
    if (g_swapchain_format != VK_FORMAT_B8G8R8A8_UNORM || path == nullptr) {
        return false;
    }
    void *mapped = nullptr;
    if (vkMapMemory(g_device, memory, 0u, size, 0u, &mapped) != VK_SUCCESS) {
        return false;
    }
    FILE *const output = std::fopen(path, "wb");
    if (output == nullptr) {
        vkUnmapMemory(g_device, memory);
        return false;
    }
    std::fprintf(output, "P6\n%u %u\n255\n", g_extent.width, g_extent.height);
    const uint8_t *const pixels = static_cast<const uint8_t *>(mapped);
    for (uint32_t y = 0u; y < g_extent.height; ++y) {
        for (uint32_t x = 0u; x < g_extent.width; ++x) {
            const uint8_t *const bgra = pixels + (y * g_extent.width + x) * 4u;
            const uint8_t rgb[3] = {bgra[2], bgra[1], bgra[0]};
            std::fwrite(rgb, sizeof(rgb), 1u, output);
        }
    }
    const bool ok = std::fclose(output) == 0;
    vkUnmapMemory(g_device, memory);
    return ok;
}

bool upload_gpu_image(GpuImage &texture, VkFormat format,
                      const std::vector<enj_host_pvr::DecodedMip> &mips,
                      pvr_filter_mode_t filter)
{
    if (mips.empty() || mips[0].pixels.empty()) {
        return false;
    }
    size_t byte_count = 0u;
    for (const auto &mip : mips) {
        byte_count += mip.pixels.size();
    }
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    if (!create_staging_buffer(byte_count, staging, staging_memory)) {
        return false;
    }
    void *mapped = nullptr;
    if (vkMapMemory(g_device, staging_memory, 0u, byte_count, 0u, &mapped) != VK_SUCCESS) {
        vkDestroyBuffer(g_device, staging, nullptr);
        vkFreeMemory(g_device, staging_memory, nullptr);
        return false;
    }
    size_t offset = 0u;
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(mips.size());
    for (size_t level = 0u; level < mips.size(); level++) {
        const auto &mip = mips[level];
        std::memcpy(static_cast<uint8_t *>(mapped) + offset,
                    mip.pixels.data(), mip.pixels.size());
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = static_cast<uint32_t>(level);
        region.imageSubresource.layerCount = 1u;
        region.imageExtent = {mip.width, mip.height, 1u};
        regions.push_back(region);
        offset += mip.pixels.size();
    }
    vkUnmapMemory(g_device, staging_memory);

    const bool existing = texture.image != VK_NULL_HANDLE;
    if (!existing) {
        VkImageCreateInfo image{};
        image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.extent = {mips[0].width, mips[0].height, 1u};
        image.mipLevels = static_cast<uint32_t>(mips.size());
        image.arrayLayers = 1u;
        image.format = format;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(g_device, &image, nullptr, &texture.image) != VK_SUCCESS) {
            vkDestroyBuffer(g_device, staging, nullptr);
            vkFreeMemory(g_device, staging_memory, nullptr);
            return false;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(g_device, texture.image, &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = find_memory_type(
            req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(g_device, &alloc, nullptr, &texture.memory) != VK_SUCCESS ||
            vkBindImageMemory(g_device, texture.image, texture.memory, 0u) != VK_SUCCESS) {
            vkDestroyBuffer(g_device, staging, nullptr);
            vkFreeMemory(g_device, staging_memory, nullptr);
            destroy_gpu_image(texture);
            return false;
        }
        texture.width = mips[0].width;
        texture.height = mips[0].height;
        texture.mip_count = static_cast<uint32_t>(mips.size());
        texture.format = format;
    }

    VkCommandBufferAllocateInfo command_alloc{};
    command_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_alloc.commandPool = g_command_pool;
    command_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_alloc.commandBufferCount = 1u;
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(g_device, &command_alloc, &command) != VK_SUCCESS) {
        vkDestroyBuffer(g_device, staging, nullptr);
        vkFreeMemory(g_device, staging_memory, nullptr);
        return false;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(command, &begin);
    VkImageMemoryBarrier to_transfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_transfer.oldLayout = existing ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                     : VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcAccessMask = existing ? VK_ACCESS_SHADER_READ_BIT : 0u;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_transfer.image = texture.image;
    to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_transfer.subresourceRange.levelCount = texture.mip_count;
    to_transfer.subresourceRange.layerCount = 1u;
    vkCmdPipelineBarrier(
        command,
        existing ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &to_transfer);
    vkCmdCopyBufferToImage(command, staging, texture.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()), regions.data());
    VkImageMemoryBarrier to_shader = to_transfer;
    to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0u, 0u, nullptr,
                         0u, nullptr, 1u, &to_shader);
    vkEndCommandBuffer(command);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &command;
    const bool submitted =
        vkQueueSubmit(g_graphics_queue, 1u, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
    if (submitted) {
        vkQueueWaitIdle(g_graphics_queue);
    }
    vkFreeCommandBuffers(g_device, g_command_pool, 1u, &command);
    vkDestroyBuffer(g_device, staging, nullptr);
    vkFreeMemory(g_device, staging_memory, nullptr);
    if (!submitted) {
        return false;
    }

    if (!existing) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = texture.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = texture.mip_count;
        view.subresourceRange.layerCount = 1u;
        if (vkCreateImageView(g_device, &view, nullptr, &texture.view) != VK_SUCCESS) {
            destroy_gpu_image(texture);
            return false;
        }
        const bool nearest = filter == PVR_FILTER_NEAREST;
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        sampler.minFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        sampler.mipmapMode = filter == PVR_FILTER_TRILINEAR1 ||
                              filter == PVR_FILTER_TRILINEAR2
            ? VK_SAMPLER_MIPMAP_MODE_LINEAR
            : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.maxLod = static_cast<float>(texture.mip_count);
        if (vkCreateSampler(g_device, &sampler, nullptr, &texture.sampler) != VK_SUCCESS) {
            destroy_gpu_image(texture);
            return false;
        }
    }
    return true;
}

enj_host_pvr::DecodedMip solid_mip(uint32_t rgba)
{
    enj_host_pvr::DecodedMip mip{1u, 1u, std::vector<uint8_t>(4u)};
    std::memcpy(mip.pixels.data(), &rgba, 4u);
    return mip;
}

void write_texture_descriptor(const GpuTexture &texture)
{
    const GpuImage &color = texture.indexed ? g_white_texture : texture.image;
    const GpuImage &index = texture.indexed ? texture.image : g_default_index_texture;
    VkDescriptorImageInfo images[3] = {
        {color.sampler, color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {index.sampler, index.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {g_palette_texture.sampler, g_palette_texture.view,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[4]{};
    for (uint32_t i = 0u; i < 3u; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = texture.descriptor;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1u;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    VkDescriptorBufferInfo modifier_buffer{
        g_translucent_modifier_buffer, 0u, VK_WHOLE_SIZE};
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = texture.descriptor;
    writes[3].dstBinding = 3u;
    writes[3].descriptorCount = 1u;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo = &modifier_buffer;
    vkUpdateDescriptorSets(g_device, 4u, writes, 0u, nullptr);
}

bool allocate_texture_descriptor(GpuTexture &texture)
{
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = g_descriptor_pool;
    alloc.descriptorSetCount = 1u;
    alloc.pSetLayouts = &g_texture_set_layout;
    if (vkAllocateDescriptorSets(g_device, &alloc, &texture.descriptor) != VK_SUCCESS) {
        return false;
    }
    write_texture_descriptor(texture);
    return true;
}

bool create_builtin_textures()
{
    const std::vector<enj_host_pvr::DecodedMip> white = {solid_mip(0xffffffffu)};
    enj_host_pvr::DecodedMip index{1u, 1u, std::vector<uint8_t>(1u, 0u)};
    const auto palette = enj_host_pvr::palette_rgba();
    enj_host_pvr::DecodedMip palette_mip{
        1024u, 1u, std::vector<uint8_t>(palette.size() * sizeof(uint32_t))};
    std::memcpy(palette_mip.pixels.data(), palette.data(), palette_mip.pixels.size());
    g_uploaded_palette_revision = enj_host_pvr::palette_revision();
    const bool uploaded =
           upload_gpu_image(g_white_texture, VK_FORMAT_R8G8B8A8_UNORM, white,
                            PVR_FILTER_NEAREST) &&
           upload_gpu_image(g_default_index_texture, VK_FORMAT_R8_UINT, {index},
                            PVR_FILTER_NEAREST) &&
           upload_gpu_image(g_palette_texture, VK_FORMAT_R8G8B8A8_UNORM,
                            {palette_mip}, PVR_FILTER_NEAREST);
    if (!uploaded) {
        return false;
    }
    GpuTexture untextured{};
    untextured.image = g_white_texture;
    if (!allocate_texture_descriptor(untextured)) {
        return false;
    }
    g_default_texture_descriptor = untextured.descriptor;
    return true;
}

bool update_palette_texture()
{
    const uint64_t revision = enj_host_pvr::palette_revision();
    if (revision == g_uploaded_palette_revision) {
        return true;
    }
    const auto palette = enj_host_pvr::palette_rgba();
    enj_host_pvr::DecodedMip mip{
        1024u, 1u, std::vector<uint8_t>(palette.size() * sizeof(uint32_t))};
    std::memcpy(mip.pixels.data(), palette.data(), mip.pixels.size());
    if (!upload_gpu_image(g_palette_texture, VK_FORMAT_R8G8B8A8_UNORM, {mip},
                          PVR_FILTER_NEAREST)) {
        return false;
    }
    g_uploaded_palette_revision = revision;
    return true;
}

uint32_t texture_storage_format(uint32_t format)
{
    const uint32_t pixel_format = (format >> 27u) & 7u;
    if (pixel_format == PVR_PIXEL_MODE_PAL_4BPP) {
        return format & ~PVR_TXRFMT_4BPP_PAL(0x3fu);
    }
    if (pixel_format == PVR_PIXEL_MODE_PAL_8BPP) {
        return format & ~PVR_TXRFMT_8BPP_PAL(0x03u);
    }
    return format;
}

GpuTexture *gpu_texture_for(const DrawBatch &batch)
{
    if (!batch.textured) {
        return nullptr;
    }
    const uint32_t storage_format = texture_storage_format(batch.texture_format);
    auto found = std::find_if(
        g_texture_cache.begin(), g_texture_cache.end(),
        [&](const GpuTexture &texture) {
            return texture.source == batch.texture &&
                   texture.source_format == storage_format &&
                   texture.width == batch.texture_width &&
                   texture.height == batch.texture_height &&
                   texture.filter == batch.texture_filter;
        });
    const uint64_t revision = enj_host_pvr::texture_revision(batch.texture);
    if (found != g_texture_cache.end() && found->revision == revision) {
        return &*found;
    }

    QueuedPrimitive primitive{};
    primitive.textured = true;
    primitive.texture = batch.texture;
    primitive.texture_format = batch.texture_format;
    primitive.texture_width = batch.texture_width;
    primitive.texture_height = batch.texture_height;
    primitive.texture_filter = batch.texture_filter;
    enj_host_pvr::DecodedTexture decoded;
    if (!enj_host_pvr::decode_texture(primitive, decoded)) {
        return nullptr;
    }
    if (found == g_texture_cache.end()) {
        g_texture_cache.push_back({});
        found = g_texture_cache.end() - 1;
        found->source = batch.texture;
        found->source_format = storage_format;
        found->width = batch.texture_width;
        found->height = batch.texture_height;
        found->filter = batch.texture_filter;
    } else {
        destroy_gpu_image(found->image);
    }
    found->indexed = decoded.indexed;
    found->palette_base = decoded.palette_base;
    found->revision = revision;
    const VkFormat format = decoded.indexed ? VK_FORMAT_R8_UINT
                                            : VK_FORMAT_R8G8B8A8_UNORM;
    const pvr_filter_mode_t upload_filter =
        decoded.indexed ? PVR_FILTER_NEAREST : batch.texture_filter;
    if (!upload_gpu_image(found->image, format, decoded.mips, upload_filter)) {
        return nullptr;
    }
    if (found->descriptor == VK_NULL_HANDLE) {
        if (!allocate_texture_descriptor(*found)) {
            return nullptr;
        }
    } else {
        write_texture_descriptor(*found);
    }
    return &*found;
}

void destroy_frame_resources()
{
    destroy_texture_resources();
    if (g_translucent_modifier_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_device, g_translucent_modifier_buffer, nullptr);
        g_translucent_modifier_buffer = VK_NULL_HANDLE;
    }
    if (g_translucent_modifier_memory != VK_NULL_HANDLE) {
        vkFreeMemory(g_device, g_translucent_modifier_memory, nullptr);
        g_translucent_modifier_memory = VK_NULL_HANDLE;
    }
    if (g_vertex_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_device, g_vertex_buffer, nullptr);
        g_vertex_buffer = VK_NULL_HANDLE;
    }
    if (g_vertex_memory != VK_NULL_HANDLE) {
        vkFreeMemory(g_device, g_vertex_memory, nullptr);
        g_vertex_memory = VK_NULL_HANDLE;
    }
    g_vertex_capacity = 0u;
    if (g_in_flight != VK_NULL_HANDLE) {
        vkDestroyFence(g_device, g_in_flight, nullptr);
        g_in_flight = VK_NULL_HANDLE;
    }
    if (g_render_finished != VK_NULL_HANDLE) {
        vkDestroySemaphore(g_device, g_render_finished, nullptr);
        g_render_finished = VK_NULL_HANDLE;
    }
    if (g_image_available != VK_NULL_HANDLE) {
        vkDestroySemaphore(g_device, g_image_available, nullptr);
        g_image_available = VK_NULL_HANDLE;
    }
    if (!g_command_buffers.empty()) {
        vkFreeCommandBuffers(
            g_device,
            g_command_pool,
            static_cast<uint32_t>(g_command_buffers.size()),
            g_command_buffers.data());
        g_command_buffers.clear();
    }
    if (g_command_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(g_device, g_command_pool, nullptr);
        g_command_pool = VK_NULL_HANDLE;
    }
    if (g_opaque_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(g_device, g_opaque_pipeline, nullptr);
        g_opaque_pipeline = VK_NULL_HANDLE;
    }
    if (g_opaque_no_depth_write_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(g_device, g_opaque_no_depth_write_pipeline, nullptr);
        g_opaque_no_depth_write_pipeline = VK_NULL_HANDLE;
    }
    if (g_opaque_no_depth_test_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(g_device, g_opaque_no_depth_test_pipeline, nullptr);
        g_opaque_no_depth_test_pipeline = VK_NULL_HANDLE;
    }
    if (g_punch_through_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(g_device, g_punch_through_pipeline, nullptr);
        g_punch_through_pipeline = VK_NULL_HANDLE;
    }
    if (g_punch_through_no_depth_write_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(g_device, g_punch_through_no_depth_write_pipeline,
                          nullptr);
        g_punch_through_no_depth_write_pipeline = VK_NULL_HANDLE;
    }
    if (g_translucent_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(g_device, g_translucent_pipeline, nullptr);
        g_translucent_pipeline = VK_NULL_HANDLE;
    }
    const auto destroy_pipeline = [](VkPipeline &pipeline) {
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(g_device, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    };
    destroy_pipeline(g_modifier_xor_pipeline);
    destroy_pipeline(g_modifier_or_pipeline);
    destroy_pipeline(g_modifier_include_pipeline);
    destroy_pipeline(g_modifier_exclude_pipeline);
    destroy_pipeline(g_modifier_clear_current_pipeline);
    destroy_pipeline(g_modifier_opaque_receiver_pipeline);
    destroy_pipeline(g_modifier_translucent_depth_pipeline);
    destroy_pipeline(g_modifier_legacy_include_pipeline);
    destroy_pipeline(g_modifier_legacy_exclude_pipeline);
    destroy_pipeline(g_modifier_translucent_receiver_pipeline);
    for (auto &[key, pipeline] : g_state_pipelines) {
        destroy_pipeline(pipeline);
    }
    g_state_pipelines.clear();
    if (g_pvr_frag_module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(g_device, g_pvr_frag_module, nullptr);
        g_pvr_frag_module = VK_NULL_HANDLE;
    }
    if (g_vert_module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(g_device, g_vert_module, nullptr);
        g_vert_module = VK_NULL_HANDLE;
    }
    if (g_pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(g_device, g_pipeline_layout, nullptr);
        g_pipeline_layout = VK_NULL_HANDLE;
    }
    if (g_texture_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(g_device, g_texture_set_layout, nullptr);
        g_texture_set_layout = VK_NULL_HANDLE;
    }
    for (VkFramebuffer framebuffer : g_framebuffers) {
        vkDestroyFramebuffer(g_device, framebuffer, nullptr);
    }
    g_framebuffers.clear();
    if (g_render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(g_device, g_render_pass, nullptr);
        g_render_pass = VK_NULL_HANDLE;
    }
    if (g_depth_view != VK_NULL_HANDLE) {
        vkDestroyImageView(g_device, g_depth_view, nullptr);
        g_depth_view = VK_NULL_HANDLE;
    }
    if (g_depth_image != VK_NULL_HANDLE) {
        vkDestroyImage(g_device, g_depth_image, nullptr);
        g_depth_image = VK_NULL_HANDLE;
    }
    if (g_depth_memory != VK_NULL_HANDLE) {
        vkFreeMemory(g_device, g_depth_memory, nullptr);
        g_depth_memory = VK_NULL_HANDLE;
    }
    if (g_offscreen_color_view != VK_NULL_HANDLE) {
        vkDestroyImageView(g_device, g_offscreen_color_view, nullptr);
        g_offscreen_color_view = VK_NULL_HANDLE;
    }
    if (g_offscreen_color_image != VK_NULL_HANDLE) {
        vkDestroyImage(g_device, g_offscreen_color_image, nullptr);
        g_offscreen_color_image = VK_NULL_HANDLE;
    }
    if (g_offscreen_color_memory != VK_NULL_HANDLE) {
        vkFreeMemory(g_device, g_offscreen_color_memory, nullptr);
        g_offscreen_color_memory = VK_NULL_HANDLE;
    }
    g_offscreen_color_initialized = false;
    for (VkImageView view : g_swapchain_views) {
        vkDestroyImageView(g_device, view, nullptr);
    }
    g_swapchain_views.clear();
    if (g_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
        g_swapchain = VK_NULL_HANDLE;
    }
    g_swapchain_images.clear();
}

bool create_swapchain()
{
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_physical_device, g_surface, &caps) !=
        VK_SUCCESS) {
        return false;
    }
    uint32_t format_count = 0u;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_physical_device, g_surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_physical_device, g_surface, &format_count, formats.data());
    VkSurfaceFormatKHR chosen = formats.empty()
        ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
        : formats[0];
    for (const VkSurfaceFormatKHR &format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM) {
            chosen = format;
            break;
        }
    }
    g_swapchain_format = chosen.format;
    if (caps.currentExtent.width != UINT32_MAX) {
        g_extent = caps.currentExtent;
    } else {
        int drawable_w = 0;
        int drawable_h = 0;
        SDL_GetWindowSizeInPixels(g_window, &drawable_w, &drawable_h);
        g_extent.width = std::min(
            std::max(static_cast<uint32_t>(std::max(drawable_w, 1)), caps.minImageExtent.width),
            caps.maxImageExtent.width);
        g_extent.height = std::min(
            std::max(static_cast<uint32_t>(std::max(drawable_h, 1)), caps.minImageExtent.height),
            caps.maxImageExtent.height);
    }
    uint32_t image_count = caps.minImageCount + 1u;
    if (caps.maxImageCount > 0u && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }
    g_swapchain_readback_supported =
        (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0u;
    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = g_surface;
    info.minImageCount = image_count;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = g_extent;
    info.imageArrayLayers = 1u;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (g_swapchain_readback_supported) {
        info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    /* The frame clock paces to 59.94 Hz itself (enj_platform_pc_endjinn.cpp):
     * mailbox keeps a faster display from adding its own waits. */
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    {
        uint32_t mode_count = 0u;
        vkGetPhysicalDeviceSurfacePresentModesKHR(g_physical_device, g_surface, &mode_count, nullptr);
        std::vector<VkPresentModeKHR> modes(mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(g_physical_device, g_surface, &mode_count, modes.data());
        if (std::getenv("DAYTONA_VSYNC") == nullptr &&
            std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) {
            info.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        }
    }
    info.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(g_device, &info, nullptr, &g_swapchain) != VK_SUCCESS) {
        return false;
    }
    vkGetSwapchainImagesKHR(g_device, g_swapchain, &image_count, nullptr);
    g_swapchain_images.resize(image_count);
    vkGetSwapchainImagesKHR(g_device, g_swapchain, &image_count, g_swapchain_images.data());
    g_swapchain_views.resize(g_swapchain_images.size());
    for (size_t i = 0; i < g_swapchain_images.size(); i++) {
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = g_swapchain_images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = g_swapchain_format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1u;
        view.subresourceRange.layerCount = 1u;
        if (vkCreateImageView(g_device, &view, nullptr, &g_swapchain_views[i]) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool create_offscreen_color_resources()
{
    /* Use a large default target unless a test requests the configured
     * application aperture directly. */
    g_swapchain_format = VK_FORMAT_B8G8R8A8_UNORM;
    if (native_composition_raster_requested()) {
#if defined(ENJ_INTEGER_PRESENT_WIDTH) && defined(ENJ_INTEGER_PRESENT_HEIGHT)
        g_extent = VkExtent2D{ENJ_INTEGER_PRESENT_WIDTH,
                              ENJ_INTEGER_PRESENT_HEIGHT};
#else
        g_extent = VkExtent2D{640u, 480u};
#endif
    } else {
        g_extent = VkExtent2D{1280u, 960u};
    }
    g_swapchain_readback_supported = true;
    update_content_rect();
    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.extent = {g_extent.width, g_extent.height, 1u};
    image.mipLevels = 1u;
    image.arrayLayers = 1u;
    image.format = g_swapchain_format;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(g_device, &image, nullptr, &g_offscreen_color_image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g_device, g_offscreen_color_image, &req);
    VkMemoryAllocateInfo memory{};
    memory.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memory.allocationSize = req.size;
    memory.memoryTypeIndex = find_memory_type(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_device, &memory, nullptr, &g_offscreen_color_memory) != VK_SUCCESS ||
        vkBindImageMemory(g_device, g_offscreen_color_image, g_offscreen_color_memory, 0u) !=
            VK_SUCCESS) {
        return false;
    }
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = g_offscreen_color_image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = g_swapchain_format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1u;
    view.subresourceRange.layerCount = 1u;
    if (vkCreateImageView(g_device, &view, nullptr, &g_offscreen_color_view) != VK_SUCCESS) {
        return false;
    }
    std::fprintf(stderr,
                 "pc-enDjinn: offscreen Vulkan capture target=%ux%u native=%u\n",
                 g_extent.width, g_extent.height,
                 native_composition_raster_requested() ? 1u : 0u);
    return true;
}

bool recreate_draw_resources()
{
    if (g_device == VK_NULL_HANDLE) {
        return false;
    }
    vkDeviceWaitIdle(g_device);
    destroy_frame_resources();
    return create_draw_resources();
}

bool create_depth_resources()
{
    constexpr VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT};
    for (VkFormat candidate : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(g_physical_device, candidate, &properties);
        if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            g_depth_format = candidate;
            break;
        }
    }
    if (g_depth_format == VK_FORMAT_UNDEFINED) {
        return false;
    }
    if (g_depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT) {
        std::fprintf(stderr, "daytona: no 32-bit float depth buffer; distant geometry may z-fight\n");
    }
    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.extent = {g_extent.width, g_extent.height, 1u};
    image.mipLevels = 1u;
    image.arrayLayers = 1u;
    image.format = g_depth_format;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(g_device, &image, nullptr, &g_depth_image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g_device, g_depth_image, &req);
    VkMemoryAllocateInfo mem{};
    mem.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mem.allocationSize = req.size;
    mem.memoryTypeIndex = find_memory_type(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_device, &mem, nullptr, &g_depth_memory) != VK_SUCCESS) {
        return false;
    }
    if (vkBindImageMemory(g_device, g_depth_image, g_depth_memory, 0u) != VK_SUCCESS) {
        return false;
    }

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = g_depth_image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = g_depth_format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    view.subresourceRange.levelCount = 1u;
    view.subresourceRange.layerCount = 1u;
    return vkCreateImageView(g_device, &view, nullptr, &g_depth_view) == VK_SUCCESS;
}

bool create_render_pipeline()
{
    VkAttachmentDescription color{};
    color.format = g_swapchain_format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.initialLayout = g_offscreen_capture
        ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentDescription depth{};
    depth.format = g_depth_format;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color_ref{};
    color_ref.attachment = 0u;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depth_ref{};
    depth_ref.attachment = 1u;
    depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1u;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0u;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    const VkAttachmentDescription attachments[2] = {color, depth};
    VkRenderPassCreateInfo render_pass{};
    render_pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass.attachmentCount = 2u;
    render_pass.pAttachments = attachments;
    render_pass.subpassCount = 1u;
    render_pass.pSubpasses = &subpass;
    render_pass.dependencyCount = 1u;
    render_pass.pDependencies = &dep;
    if (vkCreateRenderPass(g_device, &render_pass, nullptr, &g_render_pass) != VK_SUCCESS) {
        return false;
    }
    const size_t framebuffer_count = g_offscreen_capture ? 1u : g_swapchain_views.size();
    g_framebuffers.resize(framebuffer_count);
    for (size_t i = 0; i < framebuffer_count; i++) {
        VkImageView attachments_for_fb[2] = {
            g_offscreen_capture ? g_offscreen_color_view : g_swapchain_views[i], g_depth_view};
        VkFramebufferCreateInfo fb{};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = g_render_pass;
        fb.attachmentCount = 2u;
        fb.pAttachments = attachments_for_fb;
        fb.width = g_extent.width;
        fb.height = g_extent.height;
        fb.layers = 1u;
        if (vkCreateFramebuffer(g_device, &fb, nullptr, &g_framebuffers[i]) != VK_SUCCESS) {
            return false;
        }
    }

    std::vector<char> vert = read_shader_file("flat.vert.spv");
    std::vector<char> frag = read_shader_file("pvr.frag.spv");
    std::vector<char> modifier_frag = read_shader_file("flat_modifier.frag.spv");
    std::vector<char> punch_frag = read_shader_file("pvr.frag.spv");
    VkShaderModule vert_module = create_shader_module(vert);
    VkShaderModule frag_module = create_shader_module(frag);
    VkShaderModule modifier_frag_module = create_shader_module(modifier_frag);
    VkShaderModule punch_frag_module = create_shader_module(punch_frag);
    if (vert_module == VK_NULL_HANDLE || frag_module == VK_NULL_HANDLE ||
        modifier_frag_module == VK_NULL_HANDLE ||
        punch_frag_module == VK_NULL_HANDLE) {
        std::fprintf(stderr, "pc-enDjinn: missing pc-endjinn flat shaders\n");
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_module;
    stages[1].pName = "main";
    VkVertexInputBindingDescription binding{};
    binding.binding = 0u;
    binding.stride = sizeof(PcVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0] = {0u, 0u, VK_FORMAT_R32G32B32_SFLOAT, offsetof(PcVertex, position)};
    attrs[1] = {1u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(PcVertex, color)};
    attrs[2] = {2u, 0u, VK_FORMAT_R32G32_SFLOAT, offsetof(PcVertex, uv)};
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1u;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 3u;
    vertex_input.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    update_content_rect();
    VkViewport viewport{
        static_cast<float>(g_content_rect.offset.x),
        static_cast<float>(g_content_rect.offset.y),
        static_cast<float>(g_content_rect.extent.width),
        static_cast<float>(g_content_rect.extent.height),
        0.0f,
        1.0f};
    VkRect2D scissor = g_content_rect;
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1u;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1u;
    viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    // ponytail: 2D PVR packets mix cull directions; add per-context pipelines for 3D parity.
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth_state{};
    depth_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_state.depthTestEnable = VK_TRUE;
    depth_state.depthWriteEnable = VK_TRUE;
    depth_state.depthCompareOp = VK_COMPARE_OP_GREATER;
    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1u;
    blend.pAttachments = &blend_att;
    VkDescriptorSetLayoutBinding texture_bindings[4]{};
    for (uint32_t i = 0u; i < 3u; i++) {
        texture_bindings[i].binding = i;
        texture_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texture_bindings[i].descriptorCount = 1u;
        texture_bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    texture_bindings[3].binding = 3u;
    texture_bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    texture_bindings[3].descriptorCount = 1u;
    texture_bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set_layout{};
    set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout.bindingCount = 4u;
    set_layout.pBindings = texture_bindings;
    if (vkCreateDescriptorSetLayout(g_device, &set_layout, nullptr,
                                    &g_texture_set_layout) != VK_SUCCESS) {
        return false;
    }
    VkDescriptorPoolSize pool_sizes[2]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[0].descriptorCount = 3072u;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[1].descriptorCount = 1024u;
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 1024u;
    pool.poolSizeCount = 2u;
    pool.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(g_device, &pool, nullptr, &g_descriptor_pool) != VK_SUCCESS) {
        return false;
    }
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(TexturePush);
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1u;
    layout.pSetLayouts = &g_texture_set_layout;
    layout.pushConstantRangeCount = 1u;
    layout.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(g_device, &layout, nullptr, &g_pipeline_layout) != VK_SUCCESS) {
        return false;
    }
    VkGraphicsPipelineCreateInfo pipe{};
    pipe.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipe.stageCount = 2u;
    pipe.pStages = stages;
    pipe.pVertexInputState = &vertex_input;
    pipe.pInputAssemblyState = &assembly;
    pipe.pViewportState = &viewport_state;
    pipe.pRasterizationState = &raster;
    pipe.pMultisampleState = &ms;
    pipe.pDepthStencilState = &depth_state;
    pipe.pColorBlendState = &blend;
    pipe.layout = g_pipeline_layout;
    pipe.renderPass = g_render_pass;
    bool ok = vkCreateGraphicsPipelines(
        g_device, VK_NULL_HANDLE, 1u, &pipe, nullptr, &g_opaque_pipeline) == VK_SUCCESS;

    depth_state.depthWriteEnable = VK_FALSE;
    ok = ok && vkCreateGraphicsPipelines(
        g_device, VK_NULL_HANDLE, 1u, &pipe, nullptr,
        &g_opaque_no_depth_write_pipeline) == VK_SUCCESS;
    depth_state.depthWriteEnable = VK_TRUE;

    stages[1].module = punch_frag_module;
    ok = ok && vkCreateGraphicsPipelines(
        g_device, VK_NULL_HANDLE, 1u, &pipe, nullptr, &g_punch_through_pipeline) == VK_SUCCESS;

    depth_state.depthWriteEnable = VK_FALSE;
    ok = ok && vkCreateGraphicsPipelines(
        g_device, VK_NULL_HANDLE, 1u, &pipe, nullptr,
        &g_punch_through_no_depth_write_pipeline) == VK_SUCCESS;

    stages[1].module = frag_module;
    /* PVR_DEPTHCMP_ALWAYS bypasses the existing depth buffer. */
    depth_state.depthTestEnable = VK_FALSE;
    depth_state.depthWriteEnable = VK_FALSE;
    ok = ok && vkCreateGraphicsPipelines(
        g_device, VK_NULL_HANDLE, 1u, &pipe, nullptr,
        &g_opaque_no_depth_test_pipeline) == VK_SUCCESS;

    depth_state.depthTestEnable = VK_TRUE;
    blend_att.blendEnable = VK_TRUE;
    blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_att.colorBlendOp = VK_BLEND_OP_ADD;
    blend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_att.alphaBlendOp = VK_BLEND_OP_ADD;
    ok = ok && vkCreateGraphicsPipelines(
        g_device, VK_NULL_HANDLE, 1u, &pipe, nullptr, &g_translucent_pipeline) == VK_SUCCESS;

    /* Transparent modifier receivers evaluate the submitted volume events
     * at each fragment's own PVR depth, then discard the unselected area. */
    stages[1].module = modifier_frag_module;
    ok = ok && vkCreateGraphicsPipelines(
        g_device, VK_NULL_HANDLE, 1u, &pipe, nullptr,
        &g_modifier_translucent_depth_pipeline) == VK_SUCCESS;
    stages[1].module = frag_module;

    /* Opaque modifier volumes use two stencil bits, matching the PVR's
     * per-pixel state machine: bit 1 is the current volume result and bit 0
     * is the accumulated area-1 result. */
    depth_state.depthTestEnable = VK_TRUE;
    depth_state.depthWriteEnable = VK_FALSE;
    depth_state.depthCompareOp = VK_COMPARE_OP_GREATER;
    depth_state.stencilTestEnable = VK_TRUE;
    depth_state.front.failOp = VK_STENCIL_OP_KEEP;
    depth_state.front.passOp = VK_STENCIL_OP_INVERT;
    depth_state.front.depthFailOp = VK_STENCIL_OP_KEEP;
    depth_state.front.compareOp = VK_COMPARE_OP_ALWAYS;
    depth_state.front.compareMask = 0x02u;
    depth_state.front.writeMask = 0x02u;
    depth_state.front.reference = 0u;
    depth_state.back = depth_state.front;
    blend_att.colorWriteMask = 0u;
    blend_att.blendEnable = VK_FALSE;
    ok = ok && vkCreateGraphicsPipelines(
        g_device, VK_NULL_HANDLE, 1u, &pipe, nullptr,
        &g_modifier_xor_pipeline) == VK_SUCCESS;

    depth_state.front.passOp = VK_STENCIL_OP_REPLACE;
    depth_state.front.reference = 0x02u;
    depth_state.back = depth_state.front;
    ok = ok && vkCreateGraphicsPipelines(
        g_device, VK_NULL_HANDLE, 1u, &pipe, nullptr,
        &g_modifier_or_pipeline) == VK_SUCCESS;

    /* Inclusion computes summary |= current. The reference value selects
     * current (bit 1) and writes one to summary (bit 0). */
    depth_state.depthTestEnable = VK_FALSE;
    depth_state.front.compareOp = VK_COMPARE_OP_EQUAL;
    depth_state.front.compareMask = 0x02u;
    depth_state.front.writeMask = 0x01u;
    depth_state.front.reference = 0x03u;
    depth_state.front.passOp = VK_STENCIL_OP_REPLACE;
    depth_state.back = depth_state.front;
    ok = ok && vkCreateGraphicsPipelines(
        g_device, VK_NULL_HANDLE, 1u, &pipe, nullptr,
        &g_modifier_include_pipeline) == VK_SUCCESS;

    /* Exclusion computes summary &= !current. */
    depth_state.front.reference = 0x02u;
    depth_state.front.passOp = VK_STENCIL_OP_ZERO;
    depth_state.back = depth_state.front;
    ok = ok && vkCreateGraphicsPipelines(
        g_device, VK_NULL_HANDLE, 1u, &pipe, nullptr,
        &g_modifier_exclude_pipeline) == VK_SUCCESS;

    depth_state.front.compareOp = VK_COMPARE_OP_ALWAYS;
    depth_state.front.compareMask = 0x02u;
    depth_state.front.writeMask = 0x02u;
    depth_state.front.reference = 0u;
    depth_state.front.passOp = VK_STENCIL_OP_ZERO;
    depth_state.back = depth_state.front;
    ok = ok && vkCreateGraphicsPipelines(
        g_device, VK_NULL_HANDLE, 1u, &pipe, nullptr,
        &g_modifier_clear_current_pipeline) == VK_SUCCESS;

    /* Re-shade only the visible area-1 fragments of opaque receivers. */
    depth_state.depthTestEnable = VK_TRUE;
    depth_state.depthCompareOp = VK_COMPARE_OP_EQUAL;
    depth_state.front.compareOp = VK_COMPARE_OP_EQUAL;
    depth_state.front.compareMask = 0x01u;
    depth_state.front.writeMask = 0u;
    depth_state.front.reference = 0x01u;
    depth_state.front.passOp = VK_STENCIL_OP_KEEP;
    depth_state.back = depth_state.front;
    blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend_att.blendEnable = VK_FALSE;
    ok = ok && vkCreateGraphicsPipelines(
        g_device, VK_NULL_HANDLE, 1u, &pipe, nullptr,
        &g_modifier_opaque_receiver_pipeline) == VK_SUCCESS;

    /* Keep the old screen-space mask on a separate bit for translucent
     * receivers, whose per-layer classification needs a future event path. */
    depth_state.depthTestEnable = VK_FALSE;
    depth_state.front.compareOp = VK_COMPARE_OP_ALWAYS;
    depth_state.front.compareMask = 0x04u;
    depth_state.front.writeMask = 0x04u;
    depth_state.front.reference = 0x04u;
    depth_state.front.passOp = VK_STENCIL_OP_REPLACE;
    depth_state.back = depth_state.front;
    blend_att.colorWriteMask = 0u;
    ok = ok && vkCreateGraphicsPipelines(
        g_device, VK_NULL_HANDLE, 1u, &pipe, nullptr,
        &g_modifier_legacy_include_pipeline) == VK_SUCCESS;

    depth_state.front.passOp = VK_STENCIL_OP_ZERO;
    depth_state.back = depth_state.front;
    ok = ok && vkCreateGraphicsPipelines(
        g_device, VK_NULL_HANDLE, 1u, &pipe, nullptr,
        &g_modifier_legacy_exclude_pipeline) == VK_SUCCESS;

    depth_state.depthTestEnable = VK_TRUE;
    depth_state.depthCompareOp = VK_COMPARE_OP_GREATER;
    depth_state.front.compareOp = VK_COMPARE_OP_EQUAL;
    depth_state.front.compareMask = 0x04u;
    depth_state.front.writeMask = 0u;
    depth_state.front.reference = 0x04u;
    depth_state.front.passOp = VK_STENCIL_OP_KEEP;
    depth_state.back = depth_state.front;
    blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend_att.blendEnable = VK_TRUE;
    ok = ok && vkCreateGraphicsPipelines(
        g_device, VK_NULL_HANDLE, 1u, &pipe, nullptr,
        &g_modifier_translucent_receiver_pipeline) == VK_SUCCESS;

    vkDestroyShaderModule(g_device, punch_frag_module, nullptr);
    vkDestroyShaderModule(g_device, modifier_frag_module, nullptr);
    /* Kept for the state pipelines, built on first use (state_pipeline). */
    g_pvr_frag_module = frag_module;
    g_vert_module = vert_module;
    return ok;
}

bool create_draw_resources()
{
    g_offscreen_capture = offscreen_capture_requested();
    if ((!g_offscreen_capture && !create_swapchain()) ||
        (g_offscreen_capture && !create_offscreen_color_resources()) ||
        !create_depth_resources() || !create_render_pipeline() ||
        !create_translucent_modifier_buffer()) {
        return false;
    }
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.queueFamilyIndex = g_graphics_queue_family;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(g_device, &pool, nullptr, &g_command_pool) != VK_SUCCESS) {
        return false;
    }
    g_command_buffers.resize(g_framebuffers.size());
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = g_command_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = static_cast<uint32_t>(g_command_buffers.size());
    if (vkAllocateCommandBuffers(g_device, &alloc, g_command_buffers.data()) != VK_SUCCESS) {
        return false;
    }
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    const bool sync_created =
        vkCreateSemaphore(g_device, &sem, nullptr, &g_image_available) == VK_SUCCESS &&
        vkCreateSemaphore(g_device, &sem, nullptr, &g_render_finished) == VK_SUCCESS &&
        vkCreateFence(g_device, &fence, nullptr, &g_in_flight) == VK_SUCCESS;
    return sync_created && create_builtin_textures();
}

bool ensure_vertex_buffer(size_t vertex_count)
{
    const size_t bytes = vertex_count * sizeof(PcVertex);
    if (bytes == 0u) {
        return true;
    }
    if (g_vertex_buffer != VK_NULL_HANDLE && g_vertex_capacity >= bytes) {
        return true;
    }
    if (g_vertex_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_device, g_vertex_buffer, nullptr);
        vkFreeMemory(g_device, g_vertex_memory, nullptr);
    }
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = bytes;
    buffer.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_device, &buffer, nullptr, &g_vertex_buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_device, g_vertex_buffer, &req);
    VkMemoryAllocateInfo mem{};
    mem.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mem.allocationSize = req.size;
    mem.memoryTypeIndex = find_memory_type(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_device, &mem, nullptr, &g_vertex_memory) != VK_SUCCESS) {
        return false;
    }
    vkBindBufferMemory(g_device, g_vertex_buffer, g_vertex_memory, 0u);
    g_vertex_capacity = bytes;
    return true;
}

bool upload_translucent_modifiers(
    const std::vector<GpuModifierTriangle> &modifiers)
{
    if (modifiers.size() >
        enj_host_limits::translucent_modifier_triangles) {
        std::fprintf(stderr,
                     "pc-enDjinn: translucent modifier triangle overflow "
                     "(%zu > %zu); frame rejected\n",
                     modifiers.size(),
                     enj_host_limits::translucent_modifier_triangles);
        return false;
    }
    if (modifiers.empty()) {
        return true;
    }
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(modifiers.size()) *
        sizeof(GpuModifierTriangle);
    void *mapped = nullptr;
    if (vkMapMemory(g_device, g_translucent_modifier_memory, 0u, bytes, 0u,
                    &mapped) != VK_SUCCESS) {
        return false;
    }
    std::memcpy(mapped, modifiers.data(), static_cast<size_t>(bytes));
    vkUnmapMemory(g_device, g_translucent_modifier_memory);
    return true;
}

VkBlendFactor pvr_blend_factor(uint32_t mode, bool source)
{
    switch (mode & 7u) {
        case 0u: return VK_BLEND_FACTOR_ZERO;
        case 1u: return VK_BLEND_FACTOR_ONE;
        case 2u: return source ? VK_BLEND_FACTOR_DST_COLOR : VK_BLEND_FACTOR_SRC_COLOR;
        case 3u: return source ? VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 4u: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 5u: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 6u: return VK_BLEND_FACTOR_DST_ALPHA;
        default: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
}

/* An ordinary polygon's pipeline: the ISP depth function and write, and on
 * the translucent list the TSP's source and destination blend factors. */
VkPipeline state_pipeline(const DrawBatch &batch)
{
    const bool blended = batch.list == PVR_LIST_TR_POLY;
    const uint32_t key = (batch.depth_func & 7u) | (batch.depth_write ? 8u : 0u) |
        (blended ? 16u : 0u) | (blended ? ((batch.blend_src & 7u) << 5) | ((batch.blend_dst & 7u) << 8) : 0u);
    const auto found = g_state_pipelines.find(key);
    if (found != g_state_pipelines.end()) {
        return found->second;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = g_vert_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = g_pvr_frag_module;
    stages[1].pName = "main";
    VkVertexInputBindingDescription binding{0u, sizeof(PcVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[3] = {
        {0u, 0u, VK_FORMAT_R32G32B32_SFLOAT, offsetof(PcVertex, position)},
        {1u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(PcVertex, color)},
        {2u, 0u, VK_FORMAT_R32G32_SFLOAT, offsetof(PcVertex, uv)}};
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1u;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 3u;
    vertex_input.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{static_cast<float>(g_content_rect.offset.x), static_cast<float>(g_content_rect.offset.y),
                        static_cast<float>(g_content_rect.extent.width),
                        static_cast<float>(g_content_rect.extent.height), 0.0f, 1.0f};
    VkRect2D scissor = g_content_rect;
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1u;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1u;
    viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE; /* culling is decided per triangle (pc_endjinn_frame.cpp) */
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    /* PVR depth is 1/w: larger is nearer, and so is the stored value. */
    static constexpr VkCompareOp kCompare[8] = {
        VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL,
        VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    VkPipelineDepthStencilStateCreateInfo depth_state{};
    depth_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_state.depthTestEnable = VK_TRUE;
    depth_state.depthWriteEnable = batch.depth_write ? VK_TRUE : VK_FALSE;
    depth_state.depthCompareOp = kCompare[batch.depth_func & 7u];
    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (blended) {
        blend_att.blendEnable = VK_TRUE;
        blend_att.srcColorBlendFactor = pvr_blend_factor(batch.blend_src, true);
        blend_att.dstColorBlendFactor = pvr_blend_factor(batch.blend_dst, false);
        blend_att.colorBlendOp = VK_BLEND_OP_ADD;
        blend_att.srcAlphaBlendFactor = blend_att.srcColorBlendFactor;
        blend_att.dstAlphaBlendFactor = blend_att.dstColorBlendFactor;
        blend_att.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1u;
    blend.pAttachments = &blend_att;
    VkGraphicsPipelineCreateInfo pipe{};
    pipe.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipe.stageCount = 2u;
    pipe.pStages = stages;
    pipe.pVertexInputState = &vertex_input;
    pipe.pInputAssemblyState = &assembly;
    pipe.pViewportState = &viewport_state;
    pipe.pRasterizationState = &raster;
    pipe.pMultisampleState = &ms;
    pipe.pDepthStencilState = &depth_state;
    pipe.pColorBlendState = &blend;
    pipe.layout = g_pipeline_layout;
    pipe.renderPass = g_render_pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1u, &pipe, nullptr, &pipeline) != VK_SUCCESS) {
        std::fprintf(stderr, "daytona: pipeline for state %08x failed\n", key);
        pipeline = g_opaque_pipeline;
    }
    g_state_pipelines[key] = pipeline;
    return pipeline;
}

bool draw_frame(const FrameDrawData &frame)
{
    if (g_device == VK_NULL_HANDLE || (!g_offscreen_capture && g_swapchain == VK_NULL_HANDLE) ||
        g_opaque_pipeline == VK_NULL_HANDLE ||
        g_opaque_no_depth_write_pipeline == VK_NULL_HANDLE ||
        g_opaque_no_depth_test_pipeline == VK_NULL_HANDLE ||
        g_punch_through_pipeline == VK_NULL_HANDLE ||
        g_punch_through_no_depth_write_pipeline == VK_NULL_HANDLE ||
        g_translucent_pipeline == VK_NULL_HANDLE ||
        g_modifier_translucent_depth_pipeline == VK_NULL_HANDLE ||
        g_modifier_xor_pipeline == VK_NULL_HANDLE ||
        g_modifier_or_pipeline == VK_NULL_HANDLE ||
        g_modifier_include_pipeline == VK_NULL_HANDLE ||
        g_modifier_exclude_pipeline == VK_NULL_HANDLE ||
        g_modifier_clear_current_pipeline == VK_NULL_HANDLE ||
        g_modifier_opaque_receiver_pipeline == VK_NULL_HANDLE ||
        g_modifier_legacy_include_pipeline == VK_NULL_HANDLE ||
        g_modifier_legacy_exclude_pipeline == VK_NULL_HANDLE ||
        g_modifier_translucent_receiver_pipeline == VK_NULL_HANDLE ||
        g_command_buffers.empty()) {
        set_window_title("pc-enDjinn - draw unavailable");
        return false;
    }
    /* Palette and texture refreshes can replace images used by the preceding
     * frame, so wait before mutating either cache.  Keep the fence signalled
     * until every fallible preparation step has completed. */
    if (vkWaitForFences(g_device, 1u, &g_in_flight, VK_TRUE, UINT64_MAX) !=
        VK_SUCCESS) {
        set_window_title("pc-enDjinn - frame fence wait failed");
        return false;
    }
    if (!update_palette_texture()) {
        set_window_title("pc-enDjinn - palette upload failed");
        return false;
    }
    for (const DrawBatch &batch : frame.batches) {
        if (batch.textured && gpu_texture_for(batch) == nullptr) {
            std::fprintf(stderr, "pc-enDjinn: texture decode/upload failed\n");
        }
    }
    uint32_t image_index = 0u;
    if (!g_offscreen_capture) {
        VkResult acquire = vkAcquireNextImageKHR(
            g_device, g_swapchain, UINT64_MAX, g_image_available, VK_NULL_HANDLE, &image_index);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            return recreate_draw_resources();
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            set_window_title("pc-enDjinn - vkAcquireNextImageKHR failed");
            return false;
        }
    }

    if (!ensure_vertex_buffer(frame.vertices.size())) {
        set_window_title("pc-enDjinn - vertex buffer failed");
        return false;
    }
    if (!frame.vertices.empty()) {
        void *mapped = nullptr;
        const VkDeviceSize bytes =
            static_cast<VkDeviceSize>(frame.vertices.size() * sizeof(PcVertex));
        if (vkMapMemory(g_device, g_vertex_memory, 0u, bytes, 0u, &mapped) != VK_SUCCESS) {
            set_window_title("pc-enDjinn - vertex map failed");
            return false;
        }
        std::memcpy(mapped, frame.vertices.data(), static_cast<size_t>(bytes));
        vkUnmapMemory(g_device, g_vertex_memory);
    }
    if (!upload_translucent_modifiers(frame.translucent_modifiers)) {
        set_window_title("pc-enDjinn - translucent modifier upload failed");
        return false;
    }

    const char *const requested_screenshot = screenshot_path();
    const bool capture_screenshot = requested_screenshot != nullptr &&
        (g_offscreen_capture || g_swapchain_readback_supported);
    VkBuffer screenshot_buffer = VK_NULL_HANDLE;
    VkDeviceMemory screenshot_memory = VK_NULL_HANDLE;
    const VkDeviceSize screenshot_bytes =
        static_cast<VkDeviceSize>(g_extent.width) * g_extent.height * 4u;
    if (capture_screenshot &&
        !create_readback_buffer(screenshot_bytes, screenshot_buffer,
                                screenshot_memory)) {
        std::fprintf(stderr, "pc-enDjinn: screenshot readback allocation failed\n");
        return false;
    }

    VkCommandBuffer cmd = g_command_buffers[image_index];
    vkResetCommandBuffer(cmd, 0u);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
        if (screenshot_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(g_device, screenshot_buffer, nullptr);
            vkFreeMemory(g_device, screenshot_memory, nullptr);
        }
        set_window_title("pc-enDjinn - command begin failed");
        return false;
    }
    if (g_offscreen_capture && !g_offscreen_color_initialized) {
        VkImageMemoryBarrier initialize{};
        initialize.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        initialize.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        initialize.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        initialize.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        initialize.image = g_offscreen_color_image;
        initialize.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        initialize.subresourceRange.levelCount = 1u;
        initialize.subresourceRange.layerCount = 1u;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0u,
                             0u, nullptr, 0u, nullptr, 1u, &initialize);
        g_offscreen_color_initialized = true;
    }
    VkClearValue clears[2]{};
    clears[0].color.float32[0] = 0.0f;
    clears[0].color.float32[1] = 0.0f;
    clears[0].color.float32[2] = 0.0f;
    clears[0].color.float32[3] = 1.0f;
    clears[1].depthStencil = {0.0f, 0u};
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = g_render_pass;
    pass.framebuffer = g_framebuffers[image_index];
    pass.renderArea.extent = g_extent;
    pass.clearValueCount = 2u;
    pass.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment content_clear{};
    content_clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    content_clear.colorAttachment = 0u;
    content_clear.clearValue.color.float32[0] = g_bg_color[0];
    content_clear.clearValue.color.float32[1] = g_bg_color[1];
    content_clear.clearValue.color.float32[2] = g_bg_color[2];
    content_clear.clearValue.color.float32[3] = 1.0f;
    VkClearRect content_clear_rect{};
    content_clear_rect.rect = g_content_rect;
    content_clear_rect.layerCount = 1u;
    vkCmdClearAttachments(cmd, 1u, &content_clear, 1u, &content_clear_rect);
    if (!frame.vertices.empty()) {
        VkDeviceSize offset = 0u;
        vkCmdBindVertexBuffers(cmd, 0u, 1u, &g_vertex_buffer, &offset);
        uint32_t opaque_volume_first_vertex = UINT32_MAX;
        for (const DrawBatch &batch : frame.batches) {
            const bool opaque_modifier_volume =
                batch.modifier_volume && batch.list == PVR_LIST_OP_MOD;
            if (opaque_modifier_volume &&
                opaque_volume_first_vertex == UINT32_MAX) {
                opaque_volume_first_vertex = batch.first_vertex;
            }
            VkPipeline pipeline = g_opaque_pipeline;
            if (batch.modifier_volume) {
                if (opaque_modifier_volume) {
                    /* A non-final modifier instruction denotes the PVR's
                     * open/planar OR mode. Closed volumes, including their
                     * final polygon, use parity/XOR. */
                    pipeline = !batch.modifier_volume_last &&
                            batch.modifier_mode != PVR_MODIFIER_OTHER_POLY
                        ? g_modifier_or_pipeline
                        : g_modifier_xor_pipeline;
                } else {
                    pipeline = batch.modifier_mode ==
                            PVR_MODIFIER_EXCLUDE_LAST_POLY
                        ? g_modifier_legacy_exclude_pipeline
                        : g_modifier_legacy_include_pipeline;
                }
            } else if (batch.modifier && batch.list == PVR_LIST_OP_POLY) {
                pipeline = g_modifier_opaque_receiver_pipeline;
            } else if (batch.modifier_receiver &&
                       batch.list == PVR_LIST_TR_POLY) {
                pipeline = g_modifier_translucent_depth_pipeline;
            } else {
                pipeline = state_pipeline(batch);
            }
            GpuTexture *texture = gpu_texture_for(batch);
            const VkDescriptorSet descriptor = texture == nullptr
                ? g_default_texture_descriptor
                : texture->descriptor;
            TexturePush push{};
            push.indexed = texture != nullptr && texture->indexed ? 1u : 0u;
            push.palette_base = batch.palette_base;
            push.filter_mode = static_cast<uint32_t>(batch.texture_filter);
            push.flags = (texture != nullptr ? kFlagTextured : 0u) |
                (batch.use_alpha ? kFlagUseAlpha : 0u) |
                (batch.ignore_tex_alpha ? kFlagIgnoreTexAlpha : 0u) |
                (batch.alpha_cutout || batch.list == PVR_LIST_PT_POLY ? kFlagPunch : 0u) |
                ((batch.uv_clamp & 2u) != 0u ? kFlagClampU : 0u) |
                ((batch.uv_clamp & 1u) != 0u ? kFlagClampV : 0u) |
                ((batch.uv_flip & 2u) != 0u ? kFlagFlipU : 0u) |
                ((batch.uv_flip & 1u) != 0u ? kFlagFlipV : 0u);
            push.tex_env = batch.tex_env;
            push.mip_bias = batch.mip_bias;
            /* Area 1 of a cheap-shadow receiver: FPU_SHAD_SCALE darkens it. */
            push.color_scale = batch.modifier && batch.list == PVR_LIST_OP_POLY
                ? enj_host_pvr::shadow_scale() : 1.0f;
            push.alpha_ref = static_cast<float>(pc_pvr_regs[0x11c / 4] & 0xffu) / 255.0f;
            push.modifier_count = static_cast<uint32_t>(
                frame.translucent_modifiers.size());
            push.modifier_area = batch.modifier_receiver &&
                    batch.list == PVR_LIST_TR_POLY
                ? (batch.modifier ? 2u : 1u)
                : 0u;
            const auto draw = [&](VkPipeline selected, uint32_t first,
                                  uint32_t count) {
                if (count == 0u) {
                    return;
                }
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  selected);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        g_pipeline_layout, 0u, 1u,
                                        &descriptor, 0u, nullptr);
                vkCmdPushConstants(cmd, g_pipeline_layout,
                                   VK_SHADER_STAGE_FRAGMENT_BIT, 0u,
                                   sizeof(push), &push);
                vkCmdDraw(cmd, count, 1u, first, 0u);
            };
            draw(pipeline, batch.first_vertex, batch.vertex_count);

            if (opaque_modifier_volume && batch.modifier_volume_last) {
                const uint32_t volume_end =
                    batch.first_vertex + batch.vertex_count;
                const uint32_t volume_vertex_count =
                    opaque_volume_first_vertex == UINT32_MAX ||
                            volume_end < opaque_volume_first_vertex
                        ? 0u
                        : volume_end - opaque_volume_first_vertex;
                draw(batch.modifier_mode == PVR_MODIFIER_EXCLUDE_LAST_POLY
                         ? g_modifier_exclude_pipeline
                         : g_modifier_include_pipeline,
                     opaque_volume_first_vertex, volume_vertex_count);
                draw(g_modifier_clear_current_pipeline,
                     opaque_volume_first_vertex, volume_vertex_count);
                opaque_volume_first_vertex = UINT32_MAX;
            }
        }
    }
    vkCmdEndRenderPass(cmd);
    if (g_offscreen_capture) {
        if (capture_screenshot) {
            VkImageMemoryBarrier to_transfer{};
            to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_transfer.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            to_transfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            to_transfer.image = g_offscreen_color_image;
            to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            to_transfer.subresourceRange.levelCount = 1u;
            to_transfer.subresourceRange.layerCount = 1u;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0u,
                                 0u, nullptr, 0u, nullptr, 1u, &to_transfer);
            VkBufferImageCopy copy{};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1u;
            copy.imageExtent = {g_extent.width, g_extent.height, 1u};
            vkCmdCopyImageToBuffer(cmd, g_offscreen_color_image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   screenshot_buffer, 1u, &copy);
            VkImageMemoryBarrier restore{};
            restore.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            restore.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            restore.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            restore.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            restore.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            restore.image = g_offscreen_color_image;
            restore.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            restore.subresourceRange.levelCount = 1u;
            restore.subresourceRange.layerCount = 1u;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0u,
                                 0u, nullptr, 0u, nullptr, 1u, &restore);
        }
    } else {
        VkImageMemoryBarrier after_render{};
        after_render.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        after_render.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        after_render.newLayout = capture_screenshot
            ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
            : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        after_render.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        after_render.dstAccessMask = capture_screenshot
            ? VK_ACCESS_TRANSFER_READ_BIT : 0u;
        after_render.image = g_swapchain_images[image_index];
        after_render.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        after_render.subresourceRange.levelCount = 1u;
        after_render.subresourceRange.layerCount = 1u;
        vkCmdPipelineBarrier(
            cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            capture_screenshot ? VK_PIPELINE_STAGE_TRANSFER_BIT
                               : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0u, 0u, nullptr, 0u, nullptr, 1u, &after_render);
        if (capture_screenshot) {
            VkBufferImageCopy copy{};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1u;
            copy.imageExtent = {g_extent.width, g_extent.height, 1u};
            vkCmdCopyImageToBuffer(cmd, g_swapchain_images[image_index],
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   screenshot_buffer, 1u, &copy);
            VkImageMemoryBarrier to_present{};
            to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            to_present.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            to_present.image = g_swapchain_images[image_index];
            to_present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            to_present.subresourceRange.levelCount = 1u;
            to_present.subresourceRange.layerCount = 1u;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0u,
                                 0u, nullptr, 0u, nullptr, 1u, &to_present);
        }
    }
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        if (screenshot_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(g_device, screenshot_buffer, nullptr);
            vkFreeMemory(g_device, screenshot_memory, nullptr);
        }
        set_window_title("pc-enDjinn - command end failed");
        return false;
    }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = g_offscreen_capture ? 0u : 1u;
    submit.pWaitSemaphores = g_offscreen_capture ? nullptr : &g_image_available;
    submit.pWaitDstStageMask = g_offscreen_capture ? nullptr : &wait_stage;
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = g_offscreen_capture ? 0u : 1u;
    submit.pSignalSemaphores = g_offscreen_capture ? nullptr : &g_render_finished;
    if (vkResetFences(g_device, 1u, &g_in_flight) != VK_SUCCESS) {
        if (screenshot_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(g_device, screenshot_buffer, nullptr);
            vkFreeMemory(g_device, screenshot_memory, nullptr);
        }
        set_window_title("pc-enDjinn - frame fence reset failed");
        return false;
    }
    if (vkQueueSubmit(g_graphics_queue, 1u, &submit, g_in_flight) != VK_SUCCESS) {
        if (screenshot_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(g_device, screenshot_buffer, nullptr);
            vkFreeMemory(g_device, screenshot_memory, nullptr);
        }
        set_window_title("pc-enDjinn - queue submit failed");
        return false;
    }

    if (capture_screenshot) {
        vkWaitForFences(g_device, 1u, &g_in_flight, VK_TRUE, UINT64_MAX);
        const bool written = write_screenshot_ppm(
            requested_screenshot, screenshot_memory, screenshot_bytes);
        vkDestroyBuffer(g_device, screenshot_buffer, nullptr);
        vkFreeMemory(g_device, screenshot_memory, nullptr);
        if (!written) {
            std::fprintf(stderr, "pc-enDjinn: screenshot write failed\n");
        } else {
            std::fprintf(stderr, "pc-enDjinn: screenshot=%s\n", requested_screenshot);
            finish_screenshot_request();
        }
    } else if (requested_screenshot != nullptr) {
        std::fprintf(stderr, "pc-enDjinn: swapchain readback unsupported\n");
        finish_screenshot_request();
    }

    if (g_offscreen_capture) {
        return true;
    }
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1u;
    present.pWaitSemaphores = &g_render_finished;
    present.swapchainCount = 1u;
    present.pSwapchains = &g_swapchain;
    present.pImageIndices = &image_index;
    const VkResult presented = vkQueuePresentKHR(g_graphics_queue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        (void)recreate_draw_resources();
    }
    return true;
}

bool has_device_extension(VkPhysicalDevice device, const char *name)
{
    uint32_t count = 0u;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()) !=
        VK_SUCCESS) {
        return false;
    }
    for (const VkExtensionProperties &extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool has_instance_extension(const char *name)
{
    uint32_t count = 0u;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) !=
        VK_SUCCESS) {
        return false;
    }
    for (const VkExtensionProperties &extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool choose_graphics_device()
{
    uint32_t device_count = 0u;
    if (vkEnumeratePhysicalDevices(g_instance, &device_count, nullptr) != VK_SUCCESS ||
        device_count == 0u) {
        return false;
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    if (vkEnumeratePhysicalDevices(g_instance, &device_count, devices.data()) != VK_SUCCESS) {
        return false;
    }
    for (VkPhysicalDevice device : devices) {
        uint32_t queue_count = 0u;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queue_count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_count, queues.data());
        for (uint32_t i = 0u; i < queue_count; i++) {
            VkBool32 present_supported = VK_FALSE;
            (void)vkGetPhysicalDeviceSurfaceSupportKHR(device, i, g_surface, &present_supported);
            if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u && present_supported) {
                g_physical_device = device;
                g_graphics_queue_family = i;
                return true;
            }
        }
    }
    return false;
}

bool create_vulkan_device()
{
    if (!choose_graphics_device()) {
        return false;
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = g_graphics_queue_family;
    queue_info.queueCount = 1u;
    queue_info.pQueuePriorities = &priority;

    std::vector<const char *> extensions;
    extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
    if (has_device_extension(g_physical_device, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
    }
#endif

    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1u;
    info.pQueueCreateInfos = &queue_info;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    if (vkCreateDevice(g_physical_device, &info, nullptr, &g_device) != VK_SUCCESS) {
        return false;
    }
    vkGetDeviceQueue(g_device, g_graphics_queue_family, 0u, &g_graphics_queue);
    return true;
}

bool create_vulkan_context()
{
    if (g_instance != VK_NULL_HANDLE) {
        return true;
    }
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "pc-enDjinn: SDL_InitSubSystem failed: %s\n", SDL_GetError());
        return false;
    }
    /* A Vulkan surface still needs an SDL window on macOS, even when the
     * packet stream is rendered into our opt-in readback image.  Keep that
     * implementation detail out of the user's desktop for replay capture:
     * ENJ_OFFSCREEN_CAPTURE never acquires/presents a swapchain drawable.
     * Normal interactive launches retain SDL's visible-window default. */
    const SDL_WindowFlags window_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
        SDL_WINDOW_HIGH_PIXEL_DENSITY |
        (offscreen_capture_requested() ? SDL_WINDOW_HIDDEN : 0u);
    g_window = SDL_CreateWindow(
        "Daytona USA",
        g_last_window_width,
        g_last_window_height,
        window_flags);
    if (g_window == nullptr) {
        std::fprintf(stderr, "pc-enDjinn: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    /* A Terminal-launched Vulkan acceptance run may otherwise remain behind
     * its launcher window on macOS, leaving CAMetalLayer::nextDrawable()
     * indefinitely blocked.  Keep normal window focus unchanged; the replay
     * raster gate opts in explicitly and still renders through the ordinary
     * shimmed PVR swapchain path. */
    const char *const raise_window = std::getenv("ENJ_RAISE_WINDOW");
    if (!offscreen_capture_requested() && raise_window != nullptr &&
        std::strcmp(raise_window, "1") == 0) {
        SDL_ShowWindow(g_window);
        SDL_RaiseWindow(g_window);
    }
    SDL_AddEventWatch(pc_endjinn_event_watch, nullptr);
    set_window_title("pc-enDjinn - creating Vulkan instance");

    Uint32 sdl_extension_count = 0u;
    const char *const *sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_extension_count);
    if (sdl_extensions == nullptr) {
        std::fprintf(stderr, "pc-enDjinn: SDL_Vulkan_GetInstanceExtensions failed: %s\n",
            SDL_GetError());
        return false;
    }
    std::vector<const char *> extensions(sdl_extensions, sdl_extensions + sdl_extension_count);

    VkInstanceCreateFlags flags = 0u;
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
    if (has_instance_extension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Daytona USA";
    app.pEngineName = "enDjinn";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.flags = flags;
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instance_info.ppEnabledExtensionNames = extensions.data();
    const VkResult instance_result = vkCreateInstance(&instance_info, nullptr, &g_instance);
    if (instance_result != VK_SUCCESS) {
        std::fprintf(stderr, "pc-enDjinn: vkCreateInstance failed: %d\n",
            static_cast<int>(instance_result));
        char title[128];
        std::snprintf(
            title,
            sizeof(title),
            "pc-enDjinn - vkCreateInstance failed %d",
            static_cast<int>(instance_result));
        set_window_title(title);
        return false;
    }
    if (!SDL_Vulkan_CreateSurface(g_window, g_instance, nullptr, &g_surface)) {
        std::fprintf(stderr, "pc-enDjinn: SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        set_window_title("pc-enDjinn - SDL_Vulkan_CreateSurface failed");
        return false;
    }
    if (!create_vulkan_device()) {
        std::fprintf(stderr, "pc-enDjinn: Vulkan device creation failed\n");
        set_window_title("pc-enDjinn - Vulkan device creation failed");
        return false;
    }
    if (!create_draw_resources()) {
        std::fprintf(stderr, "pc-enDjinn: Vulkan draw resource creation failed\n");
        set_window_title("pc-enDjinn - Vulkan draw resource creation failed");
        return false;
    }
    set_window_title("pc-enDjinn - Vulkan ready");
    return true;
}

}  // namespace

namespace pc_endjinn {

vid_mode_t *video_mode() { return &g_vid_mode; }

uint64_t timer_ns_gettime64(void)
{
    const uint64_t counter = SDL_GetPerformanceCounter();
    const uint64_t frequency = SDL_GetPerformanceFrequency();
    if (frequency == 0u) {
        return 0u;
    }
    return (counter / frequency) * 1000000000ull +
           ((counter % frequency) * 1000000000ull) / frequency;
}

void vid_border_color(uint8_t r, uint8_t g, uint8_t b)
{
    (void)r;
    (void)g;
    (void)b;
}

void vid_set_mode(vid_display_mode_generic_t display_mode, vid_pixel_mode_t pixel_mode)
{
    (void)display_mode;
    (void)pixel_mode;
    g_vid_mode.width = 640;
    g_vid_mode.height = 480;
}

void pvr_init(const pvr_init_params_t *params)
{
    g_translucent_autosort = params == nullptr || params->autosort_disabled == 0u;
    g_fsaa_enabled = params != nullptr && params->fsaa_enabled != 0;
    if (!create_vulkan_context()) {
        std::fprintf(stderr, "pc-enDjinn: pvr_init failed\n");
    }
}

void pvr_shutdown(void)
{
    if (g_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_device);
        destroy_frame_resources();
        vkDestroyDevice(g_device, nullptr);
        g_device = VK_NULL_HANDLE;
    }
    g_graphics_queue = VK_NULL_HANDLE;
    g_physical_device = VK_NULL_HANDLE;
    if (g_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(g_instance, g_surface, nullptr);
        g_surface = VK_NULL_HANDLE;
    }
    if (g_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(g_instance, nullptr);
        g_instance = VK_NULL_HANDLE;
    }
    if (g_window != nullptr) {
        SDL_RemoveEventWatch(pc_endjinn_event_watch, nullptr);
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
    }
    enj_host_input_shutdown();
}

void pvr_set_bg_color(float r, float g, float b)
{
    g_bg_color[0] = clamp01(r);
    g_bg_color[1] = clamp01(g);
    g_bg_color[2] = clamp01(b);
}

void pvr_wait_ready(void) {}
void pvr_scene_begin(void)
{
    enj_host_pvr::scene_begin();
}

void pvr_scene_finish(void)
{
    update_window_mode();
    if (g_device == VK_NULL_HANDLE) {
        return;
    }
    const FrameDrawData frame = pc_endjinn::build_frame_draw_data(
        g_vid_mode, g_fsaa_enabled, g_translucent_autosort);
    const char *const sort_diagnostics =
        std::getenv("ENJ_TRANSLUCENT_SORT_DIAGNOSTICS");
    if (sort_diagnostics != nullptr && std::strcmp(sort_diagnostics, "1") == 0 &&
        (g_presented_frames % 60u) == 0u) {
        const enj_host_translucent_sort::Diagnostics &diagnostics =
            frame.translucent_sort;
        std::fprintf(
            stderr,
            "pc-enDjinn: translucent-sort frame=%llu primitives=%zu "
            "modifier-triangles=%zu candidates=%zu dependencies=%zu "
            "unordered=%zu cycle-breaks=%zu average-fallback=%s\n",
            static_cast<unsigned long long>(g_presented_frames),
            diagnostics.primitive_count,
            frame.translucent_modifiers.size(),
            diagnostics.candidate_pairs,
            diagnostics.dependency_edges,
            diagnostics.unordered_pairs,
            diagnostics.cycle_breaks,
            diagnostics.average_depth_fallback ? "yes" : "no");
    }
    (void)draw_frame(frame);
    g_presented_frames++;
    if (g_window != nullptr && (g_presented_frames <= 3u || (g_presented_frames % 30u) == 0u)) {
        char title[128];
        std::snprintf(
            title,
            sizeof(title),
            "pc-enDjinn - pvr primitives %zu - vk verts %zu",
            enj_host_pvr::primitives().size(),
            frame.vertices.size());
        SDL_SetWindowTitle(g_window, title);
    }
}

uint64_t pvr_presented_frame_count(void)
{
    return g_presented_frames;
}

bool capture_next_frame(const char *path)
{
    if (path == nullptr || *path == '\0') {
        return false;
    }
    load_screenshot_request_from_environment();
    if (g_screenshot_pending) {
        return false;
    }
    g_screenshot_path = path;
    g_screenshot_frame = g_presented_frames;
    g_screenshot_pending = true;
    return true;
}

void pvr_list_begin(pvr_list_t list) { enj_host_pvr::list_begin(list); }
void pvr_list_finish(void) {}
void pvr_wait_render_done(void) {}

void pvr_fog_table_color(float a, float r, float g, float b)
{
    (void)a;
    (void)r;
    (void)g;
    (void)b;
}

void pvr_fog_table_linear(float start, float end)
{
    (void)start;
    (void)end;
}

void *pvr_dr_target(void)
{
    return enj_host_pvr::dr_target();
}

void pvr_dr_commit(void *ptr)
{
    enj_host_pvr::dr_commit(ptr);
}

}  // namespace pc_endjinn
