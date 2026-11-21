#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <open_vfs/status.hpp>
#include "audio/audio.hpp"
#include "input/input.hpp"
#include "rds5/rds5.hpp"
#include "render/render.hpp"
#include "rpf9/rpf9.hpp"
#include "vfs_core/vfs_core.hpp"
#include "world_vm/world_vm.hpp"
#include "xvd/xvd.hpp"

namespace {

struct CliOptions {
    std::filesystem::path data_dir;
    bool headless = false;
    bool show_help = false;
    bool show_version = false;
};

void print_usage() {
    std::cout << "open-vfs - Clean-room C++20 / DirectX 12 standalone desktop runtime\n\n"
              << "Usage:\n"
              << "  open-vfs.exe --data-dir <path> [--headless]\n"
              << "  open-vfs.exe --version\n"
              << "  open-vfs.exe --help\n\n"
              << "Options:\n"
              << "  --data-dir <path>  Local directory containing read-only archive containers\n"
              << "  --headless         Mount, probe containers, run one simulation step, and exit\n"
              << "  --version          Print runtime version and subsystem status\n"
              << "  --help             Show this help message\n";
}

bool parse_args(int argc, char** argv, CliOptions& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            out.show_help = true;
            return true;
        }
        if (arg == "--version" || arg == "-v") {
            out.show_version = true;
            return true;
        }
        if (arg == "--headless") {
            out.headless = true;
            continue;
        }
        if (arg == "--data-dir") {
            if (i + 1 >= argc) {
                std::cerr << "[error] --data-dir requires a directory path.\n";
                return false;
            }
            out.data_dir = argv[++i];
            continue;
        }
        std::cerr << "[error] Unknown argument: " << arg << "\n";
        return false;
    }
    return true;
}

void probe_archive_headers(const open_vfs::vfs_core::MountPoint& mount) {
    std::size_t valid_rpf9 = 0;
    std::size_t valid_rds5 = 0;
    std::size_t valid_xvd = 0;

    std::vector<uint8_t> header_buf(64, 0);
    for (const auto& archive : mount.discovered_archives) {
        std::ifstream file(archive.host_path, std::ios::binary);
        if (!file) {
            continue;
        }
        file.read(reinterpret_cast<char*>(header_buf.data()),
                  static_cast<std::streamsize>(header_buf.size()));
        const auto bytes_read = static_cast<std::size_t>(file.gcount());
        const std::span<const uint8_t> prefix(header_buf.data(), bytes_read);

        if (archive.kind == open_vfs::vfs_core::ContainerKind::Rpf9) {
            open_vfs::rpf9::Rpf9Header hdr{};
            if (open_vfs::rpf9::parse_rpf9_header(prefix, hdr) == open_vfs::Status::Ok) {
                ++valid_rpf9;
            }
        } else if (archive.kind == open_vfs::vfs_core::ContainerKind::Rds5) {
            open_vfs::rds5::Rds5Header hdr{};
            if (open_vfs::rds5::parse_rds5_header(prefix, hdr) == open_vfs::Status::Ok) {
                ++valid_rds5;
            }
        } else if (archive.kind == open_vfs::vfs_core::ContainerKind::Xvd) {
            open_vfs::xvd::XvdHeader hdr{};
            if (open_vfs::xvd::parse_xvd_header(prefix, hdr) == open_vfs::Status::Ok) {
                ++valid_xvd;
            }
        }
    }

    std::cout << "[vfs] Discovered containers: " << mount.discovered_archives.size()
              << " (valid headers -> RPF9: " << valid_rpf9
              << ", RDS5: " << valid_rds5
              << ", XVD: " << valid_xvd << ")\n";
}

}  // namespace

int main(int argc, char** argv) {
    CliOptions options{};
    if (!parse_args(argc, argv, options)) {
        print_usage();
        return 1;
    }
    if (options.show_help) {
        print_usage();
        return 0;
    }
    if (options.show_version) {
        std::cout << "open-vfs 0.1.0 (C++20 / DirectX 12 Windows 11 clean-room runtime)\n";
        return 0;
    }
    if (options.data_dir.empty()) {
        std::cerr << "[error] Missing required --data-dir <path> argument.\n\n";
        print_usage();
        return 1;
    }

    // 1. Mount local read-only archive directory into virtual filesystem
    open_vfs::vfs_core::MountRegistry vfs;
    const auto mount_status = open_vfs::vfs_core::mount_directory(vfs, "/data", options.data_dir);
    if (mount_status != open_vfs::Status::Ok) {
        std::cerr << "[vfs] Failed to mount --data-dir ("
                  << open_vfs::status_to_string(mount_status) << ").\n";
        return 2;
    }
    probe_archive_headers(vfs.mounts.front());

    // 2. Initialize native Win32 window and DirectX 12 context
    open_vfs::render::SwapchainConfig swapchain_cfg{};
    if (!open_vfs::render::validate_swapchain_config(swapchain_cfg)) {
        std::cerr << "[render] Invalid default swapchain configuration.\n";
        return 3;
    }

    open_vfs::render::WindowHandle window{};
    const auto win_status = open_vfs::render::create_win32_window(
        "open-vfs (DirectX 12)",
        swapchain_cfg.width,
        swapchain_cfg.height,
        options.headless,
        window);
    if (win_status != open_vfs::Status::Ok) {
        std::cerr << "[render] Failed to initialize window ("
                  << open_vfs::status_to_string(win_status) << ").\n";
        return 4;
    }

    open_vfs::render::Dx12Context dx12{};
    const auto dx12_status = open_vfs::render::init_dx12_device(dx12);
    if (dx12_status == open_vfs::Status::NotImplemented) {
        std::cout << "[render] DX12 device bootstrap pending (stub active; continuing runtime loop).\n";
    }

    // 3. Initialize audio format, input snapshot, and world VM state
    open_vfs::audio::AudioFormat audio_fmt{};
    open_vfs::audio::AudioDeviceContext audio_ctx{};
    if (open_vfs::audio::validate_audio_format(audio_fmt)) {
        (void)open_vfs::audio::init_wasapi_device(audio_fmt, audio_ctx);
    }

    open_vfs::world_vm::EntityRegistry entities{};
    const auto root_entity = open_vfs::world_vm::create_entity(entities);
    open_vfs::world_vm::SimulationClock sim_clock{};

    // 4. Main desktop loop
    auto prev_time = std::chrono::steady_clock::now();
    while (open_vfs::render::pump_win32_messages(window)) {
        const auto now = std::chrono::steady_clock::now();
        const double dt = options.headless
                              ? sim_clock.fixed_dt
                              : std::chrono::duration<double>(now - prev_time).count();
        prev_time = now;

        open_vfs::input::InputState kb_state{};
        open_vfs::input::InputState pad_state{};
        (void)open_vfs::input::poll_keyboard_state(kb_state);
        (void)open_vfs::input::poll_xinput_gamepad(0, pad_state);
        const auto frame_input = open_vfs::input::merge_input_states(kb_state, pad_state);
        (void)frame_input;

        const uint32_t ticks = open_vfs::world_vm::step_fixed_timestep(sim_clock, dt);
        for (uint32_t t = 0; t < ticks; ++t) {
            (void)open_vfs::world_vm::step_rigid_body_physics(entities, sim_clock.fixed_dt);
        }

        if (dx12.initialized) {
            (void)open_vfs::render::begin_dx12_frame(dx12);
            (void)open_vfs::render::end_dx12_frame(dx12);
        }

        if (options.headless) {
            break;
        }
    }

    (void)open_vfs::world_vm::destroy_entity(entities, root_entity);
    std::cout << "[runtime] Clean shutdown after " << sim_clock.total_ticks << " simulation tick(s).\n";
    return 0;
}
