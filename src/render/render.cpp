#include "render.hpp"

#include <open_vfs/macros.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

namespace open_vfs::render {

namespace {

#ifdef _WIN32
constexpr wchar_t kWindowClassName[] = L"OpenVfsDesktopWindowClass";

LRESULT CALLBACK open_vfs_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_CLOSE) {
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
#endif

}  // namespace

OPEN_VFS_IMPL
bool validate_swapchain_config(const SwapchainConfig& config) noexcept {
    if (config.width == 0 || config.height == 0 || config.width > 16384 || config.height > 16384) {
        return false;
    }
    if (config.buffer_count < 2 || config.buffer_count > 3) {
        return false;
    }
    return config.format == PixelFormat::Rgba8Unorm || config.format == PixelFormat::Bgra8Unorm;
}

OPEN_VFS_IMPL
Status create_win32_window(std::string_view title,
                           uint32_t width,
                           uint32_t height,
                           bool headless,
                           WindowHandle& out_window) {
    if (width == 0 || height == 0) {
        return Status::InvalidArgument;
    }

    out_window = WindowHandle{};
    out_window.width = width;
    out_window.height = height;
    out_window.headless = headless;

    if (headless) {
        return Status::Ok;
    }

#ifdef _WIN32
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = open_vfs_wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = kWindowClassName;

    if (!GetClassInfoExW(instance, kWindowClassName, &wc)) {
        if (RegisterClassExW(&wc) == 0) {
            return Status::DeviceUnavailable;
        }
    }

    std::wstring wide_title(title.begin(), title.end());
    if (wide_title.empty()) {
        wide_title = L"open-vfs";
    }

    RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(
        0,
        kWindowClassName,
        wide_title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (hwnd == nullptr) {
        return Status::DeviceUnavailable;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    out_window.native_hwnd = hwnd;
    return Status::Ok;
#else
    (void)title;
    return Status::DeviceUnavailable;
#endif
}

OPEN_VFS_IMPL
bool pump_win32_messages(WindowHandle& window) noexcept {
    if (window.headless) {
        return !window.should_close;
    }
#ifdef _WIN32
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            window.should_close = true;
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !window.should_close;
#else
    return !window.should_close;
#endif
}

OPEN_VFS_STUB
Status init_dx12_device(Dx12Context& ctx) {
    ctx.initialized = false;
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status create_dx12_swapchain(Dx12Context& ctx,
                             const WindowHandle& window,
                             const SwapchainConfig& config) {
    (void)ctx;
    (void)window;
    if (!validate_swapchain_config(config)) {
        return Status::InvalidArgument;
    }
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status begin_dx12_frame(Dx12Context& ctx) {
    (void)ctx;
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status end_dx12_frame(Dx12Context& ctx) {
    (void)ctx;
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status decode_bc_texture(const TextureDesc& desc,
                         std::span<const uint8_t> compressed_bytes,
                         std::vector<uint8_t>& out_rgba8) {
    (void)desc;
    (void)compressed_bytes;
    out_rgba8.clear();
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status upload_texture_resource(Dx12Context& ctx,
                               const TextureDesc& desc,
                               std::span<const uint8_t> pixel_bytes,
                               uint64_t& out_resource_handle) {
    (void)ctx;
    (void)desc;
    (void)pixel_bytes;
    out_resource_handle = 0;
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status parse_mesh_geometry(std::span<const uint8_t> mesh_blob, MeshGeometry& out_mesh) {
    (void)mesh_blob;
    out_mesh = MeshGeometry{};
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status compile_hlsl_pipeline(Dx12Context& ctx,
                             std::string_view shader_source,
                             uint64_t& out_pso_handle) {
    (void)ctx;
    (void)shader_source;
    out_pso_handle = 0;
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status submit_mesh_draw_call(Dx12Context& ctx,
                             uint64_t pso_handle,
                             const MeshGeometry& mesh) {
    (void)ctx;
    (void)pso_handle;
    (void)mesh;
    return Status::NotImplemented;
}

}  // namespace open_vfs::render
