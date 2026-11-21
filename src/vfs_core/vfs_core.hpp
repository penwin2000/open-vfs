#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <open_vfs/status.hpp>

namespace open_vfs::vfs_core {

constexpr std::size_t kSectorAlignment = 16;

enum class ContainerKind : uint8_t {
    Unknown = 0,
    Rpf9 = 1,
    Rds5 = 2,
    Xvd = 3,
    AlignmentChunk = 4,
};

struct MountedArchiveInfo {
    std::string virtual_root;
    std::filesystem::path host_path;
    ContainerKind kind = ContainerKind::Unknown;
    uint64_t file_size = 0;
};

struct MountPoint {
    std::string virtual_prefix;
    std::filesystem::path host_root;
    std::vector<MountedArchiveInfo> discovered_archives;
};

struct MountRegistry {
    std::vector<MountPoint> mounts;
};

struct VirtualStreamHandle {
    std::size_t mount_index = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    bool valid = false;
};

// Little-endian safe binary readers
std::optional<uint16_t> read_u16_le(std::span<const uint8_t> data, std::size_t offset) noexcept;
std::optional<uint32_t> read_u32_le(std::span<const uint8_t> data, std::size_t offset) noexcept;
std::optional<uint64_t> read_u64_le(std::span<const uint8_t> data, std::size_t offset) noexcept;

// 16-byte alignment utilities
bool is_aligned_16(uint64_t value) noexcept;
uint64_t align_up_16(uint64_t value) noexcept;

// Virtual path resolution
std::string normalize_virtual_path(std::string_view raw_path);
std::string join_virtual_path(std::string_view base_path, std::string_view relative_path);

// Virtual filesystem mount manager
Status mount_directory(MountRegistry& registry,
                       std::string_view virtual_prefix,
                       const std::filesystem::path& host_dir);

std::optional<std::size_t> resolve_mount(const MountRegistry& registry,
                                         std::string_view virtual_path);

// Pending VFS streaming & enumeration hooks
Status open_virtual_stream(const MountRegistry& registry,
                           std::string_view virtual_path,
                           VirtualStreamHandle& out_handle);

Status read_aligned_chunk(const VirtualStreamHandle& handle,
                          uint64_t aligned_offset,
                          std::span<uint8_t> out_buffer,
                          std::size_t& out_bytes_read);

Status enumerate_mounted_entries(const MountRegistry& registry,
                                 std::string_view virtual_prefix,
                                 std::vector<std::string>& out_paths);

}  // namespace open_vfs::vfs_core
