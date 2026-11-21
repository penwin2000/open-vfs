#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <open_vfs/status.hpp>

namespace open_vfs::render {

enum class PixelFormat : uint32_t {
    Unknown = 0,
    Rgba8Unorm = 1,
    Bgra8Unorm = 2,
    Bc1Unorm = 3,
    Bc3Unorm = 4,
    Bc7Unorm = 5,
};

struct SwapchainConfig {
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t buffer_count = 2;
    PixelFormat format = PixelFormat::Rgba8Unorm;
    bool vsync = true;
};

struct WindowHandle {
    void* native_hwnd = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    bool headless = false;
    bool should_close = false;
};

struct Dx12Context {
    void* device = nullptr;
    void* command_queue = nullptr;
    void* swapchain = nullptr;
    uint64_t frame_index = 0;
    bool initialized = false;
};

struct TextureDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_levels = 1;
    PixelFormat format = PixelFormat::Unknown;
};

struct MeshSubmesh {
    uint32_t index_offset = 0;
    uint32_t index_count = 0;
    uint32_t material_slot = 0;
};

struct MeshGeometry {
    uint32_t vertex_count = 0;
    uint32_t vertex_stride = 0;
    std::vector<uint8_t> vertex_bytes;
    std::vector<uint32_t> indices;
    std::vector<MeshSubmesh> submeshes;
};

bool validate_swapchain_config(const SwapchainConfig& config) noexcept;

Status create_win32_window(std::string_view title,
                           uint32_t width,
                           uint32_t height,
                           bool headless,
                           WindowHandle& out_window);

bool pump_win32_messages(WindowHandle& window) noexcept;

Status init_dx12_device(Dx12Context& ctx);

Status create_dx12_swapchain(Dx12Context& ctx,
                             const WindowHandle& window,
                             const SwapchainConfig& config);

Status begin_dx12_frame(Dx12Context& ctx);

Status end_dx12_frame(Dx12Context& ctx);

Status decode_bc_texture(const TextureDesc& desc,
                         std::span<const uint8_t> compressed_bytes,
                         std::vector<uint8_t>& out_rgba8);

Status upload_texture_resource(Dx12Context& ctx,
                               const TextureDesc& desc,
                               std::span<const uint8_t> pixel_bytes,
                               uint64_t& out_resource_handle);

Status parse_mesh_geometry(std::span<const uint8_t> mesh_blob, MeshGeometry& out_mesh);

Status compile_hlsl_pipeline(Dx12Context& ctx,
                             std::string_view shader_source,
                             uint64_t& out_pso_handle);

Status submit_mesh_draw_call(Dx12Context& ctx,
                             uint64_t pso_handle,
                             const MeshGeometry& mesh);

}  // namespace open_vfs::render
