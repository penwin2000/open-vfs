#include "test_harness.hpp"
#include "render/render.hpp"

#include <array>
#include <vector>

int main() {
    using namespace open_vfs;
    using namespace open_vfs::render;

    // 1. Swapchain configuration validation
    SwapchainConfig cfg{};
    OVF_EXPECT(validate_swapchain_config(cfg));

    SwapchainConfig bad_buffers = cfg;
    bad_buffers.buffer_count = 1;
    OVF_EXPECT(!validate_swapchain_config(bad_buffers));

    SwapchainConfig bad_fmt = cfg;
    bad_fmt.format = PixelFormat::Bc7Unorm;
    OVF_EXPECT(!validate_swapchain_config(bad_fmt));

    // 2. Headless window lifecycle & message pump
    WindowHandle win{};
    OVF_EXPECT(create_win32_window("open-vfs-test", 1280, 720, true, win) == Status::Ok);
    OVF_EXPECT(win.headless);
    OVF_EXPECT(pump_win32_messages(win));
    win.should_close = true;
    OVF_EXPECT(!pump_win32_messages(win));

    // 3. Pending DX12 & asset decoding stubs
    Dx12Context ctx{};
    OVF_EXPECT(init_dx12_device(ctx) == Status::NotImplemented);
    OVF_EXPECT(create_dx12_swapchain(ctx, win, cfg) == Status::NotImplemented);
    OVF_EXPECT(create_dx12_swapchain(ctx, win, bad_buffers) == Status::InvalidArgument);

    std::array<uint8_t, 16> synthetic_bc{};
    std::vector<uint8_t> rgba;
    TextureDesc tex{4, 4, 1, PixelFormat::Bc7Unorm};
    OVF_EXPECT(decode_bc_texture(tex, synthetic_bc, rgba) == Status::NotImplemented);

    MeshGeometry mesh{};
    OVF_EXPECT(parse_mesh_geometry(synthetic_bc, mesh) == Status::NotImplemented);

    return open_vfs::test::g_failures == 0 ? 0 : 1;
}
