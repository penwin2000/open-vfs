#include "vfs_core.hpp"

#include <algorithm>
#include <cctype>
#include <open_vfs/macros.hpp>

namespace open_vfs::vfs_core {

namespace {

ContainerKind classify_extension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (ext == ".rpf") {
        return ContainerKind::Rpf9;
    }
    if (ext == ".ttpf" || ext == ".rds") {
        return ContainerKind::Rds5;
    }
    if (ext == ".xvd") {
        return ContainerKind::Xvd;
    }
    if (ext == ".alignmentchunk") {
        return ContainerKind::AlignmentChunk;
    }
    return ContainerKind::Unknown;
}

}  // namespace

OPEN_VFS_IMPL
std::optional<uint16_t> read_u16_le(std::span<const uint8_t> data, std::size_t offset) noexcept {
    if (offset > data.size() || data.size() - offset < 2) {
        return std::nullopt;
    }
    return static_cast<uint16_t>(static_cast<uint16_t>(data[offset]) |
                                 (static_cast<uint16_t>(data[offset + 1]) << 8u));
}

OPEN_VFS_IMPL
std::optional<uint32_t> read_u32_le(std::span<const uint8_t> data, std::size_t offset) noexcept {
    if (offset > data.size() || data.size() - offset < 4) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(static_cast<uint32_t>(data[offset]) |
                                 (static_cast<uint32_t>(data[offset + 1]) << 8u) |
                                 (static_cast<uint32_t>(data[offset + 2]) << 16u) |
                                 (static_cast<uint32_t>(data[offset + 3]) << 24u));
}

OPEN_VFS_IMPL
std::optional<uint64_t> read_u64_le(std::span<const uint8_t> data, std::size_t offset) noexcept {
    if (offset > data.size() || data.size() - offset < 8) {
        return std::nullopt;
    }
    const auto lo = read_u32_le(data, offset);
    const auto hi = read_u32_le(data, offset + 4);
    if (!lo.has_value() || !hi.has_value()) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(*lo) | (static_cast<uint64_t>(*hi) << 32u);
}

OPEN_VFS_IMPL
bool is_aligned_16(uint64_t value) noexcept {
    return (value & (kSectorAlignment - 1u)) == 0u;
}

OPEN_VFS_IMPL
uint64_t align_up_16(uint64_t value) noexcept {
    constexpr uint64_t kMask = kSectorAlignment - 1u;
    if (value > (UINT64_MAX - kMask)) {
        return value;
    }
    return (value + kMask) & ~kMask;
}

OPEN_VFS_IMPL
std::string normalize_virtual_path(std::string_view raw_path) {
    std::vector<std::string> segments;
    std::size_t i = 0;
    while (i < raw_path.size()) {
        while (i < raw_path.size() && (raw_path[i] == '/' || raw_path[i] == '\\')) {
            ++i;
        }
        std::size_t start = i;
        while (i < raw_path.size() && raw_path[i] != '/' && raw_path[i] != '\\') {
            ++i;
        }
        if (i > start) {
            std::string_view seg = raw_path.substr(start, i - start);
            if (seg == ".") {
                continue;
            }
            if (seg == "..") {
                if (!segments.empty()) {
                    segments.pop_back();
                }
                continue;
            }
            segments.emplace_back(seg);
        }
    }

    if (segments.empty()) {
        return "/";
    }

    std::string result;
    for (const auto& seg : segments) {
        result.push_back('/');
        result.append(seg);
    }
    return result;
}

OPEN_VFS_IMPL
std::string join_virtual_path(std::string_view base_path, std::string_view relative_path) {
    if (!relative_path.empty() && (relative_path.front() == '/' || relative_path.front() == '\\')) {
        return normalize_virtual_path(relative_path);
    }
    std::string combined;
    combined.reserve(base_path.size() + relative_path.size() + 1);
    combined.append(base_path);
    combined.push_back('/');
    combined.append(relative_path);
    return normalize_virtual_path(combined);
}

OPEN_VFS_IMPL
Status mount_directory(MountRegistry& registry,
                       std::string_view virtual_prefix,
                       const std::filesystem::path& host_dir) {
    std::error_code ec;
    if (host_dir.empty() || !std::filesystem::exists(host_dir, ec) ||
        !std::filesystem::is_directory(host_dir, ec)) {
        return Status::NotFound;
    }

    MountPoint mp;
    mp.virtual_prefix = normalize_virtual_path(virtual_prefix);
    mp.host_root = std::filesystem::weakly_canonical(host_dir, ec);
    if (ec) {
        mp.host_root = host_dir;
    }

    for (const auto& entry : std::filesystem::directory_iterator(host_dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const ContainerKind kind = classify_extension(entry.path());
        if (kind == ContainerKind::Unknown) {
            continue;
        }
        MountedArchiveInfo info;
        info.virtual_root = join_virtual_path(mp.virtual_prefix, entry.path().filename().string());
        info.host_path = entry.path();
        info.kind = kind;
        info.file_size = entry.file_size(ec);
        mp.discovered_archives.push_back(std::move(info));
    }

    registry.mounts.push_back(std::move(mp));
    return Status::Ok;
}

OPEN_VFS_IMPL
std::optional<std::size_t> resolve_mount(const MountRegistry& registry,
                                         std::string_view virtual_path) {
    const std::string normalized = normalize_virtual_path(virtual_path);
    std::optional<std::size_t> best_match;
    std::size_t best_length = 0;

    for (std::size_t idx = 0; idx < registry.mounts.size(); ++idx) {
        const auto& prefix = registry.mounts[idx].virtual_prefix;
        if (prefix == "/") {
            if (!best_match.has_value() || best_length < 1) {
                best_match = idx;
                best_length = 1;
            }
            continue;
        }
        if (normalized == prefix ||
            (normalized.size() > prefix.size() &&
             normalized.compare(0, prefix.size(), prefix) == 0 &&
             normalized[prefix.size()] == '/')) {
            if (prefix.size() > best_length) {
                best_match = idx;
                best_length = prefix.size();
            }
        }
    }
    return best_match;
}

OPEN_VFS_STUB
Status open_virtual_stream(const MountRegistry& registry,
                           std::string_view virtual_path,
                           VirtualStreamHandle& out_handle) {
    (void)registry;
    (void)virtual_path;
    out_handle = VirtualStreamHandle{};
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status read_aligned_chunk(const VirtualStreamHandle& handle,
                          uint64_t aligned_offset,
                          std::span<uint8_t> out_buffer,
                          std::size_t& out_bytes_read) {
    (void)handle;
    (void)out_buffer;
    out_bytes_read = 0;
    if (!is_aligned_16(aligned_offset)) {
        return Status::AlignmentError;
    }
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status enumerate_mounted_entries(const MountRegistry& registry,
                                 std::string_view virtual_prefix,
                                 std::vector<std::string>& out_paths) {
    (void)registry;
    (void)virtual_prefix;
    out_paths.clear();
    return Status::NotImplemented;
}

}  // namespace open_vfs::vfs_core
