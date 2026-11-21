#include "vfs_core.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <utility>
#include <open_vfs/macros.hpp>

namespace open_vfs::vfs_core {

namespace {

constexpr uint32_t kRpf9Magic = 0x52504639u;
constexpr uint32_t kRpf9MagicAsciiLe = 0x39465052u;
constexpr uint32_t kRds5Magic = 0x52445335u;
constexpr uint32_t kRds5MagicAsciiLe = 0x35534452u;
constexpr uint32_t kXvdMagic = 0x1DD3014Eu;
constexpr std::size_t kXvdSignedHeaderOffset = 0x200u;
constexpr std::size_t kMagicProbeSize = 0x210u;

constexpr std::array<uint8_t, 8> kXvdSignature = {'m', 's', 'f', 't', '-', 'x', 'v', 'd'};
constexpr std::array<uint8_t, 7> kPriPrefix = {'m', 'r', 'm', '_', 'p', 'r', 'i'};
constexpr std::array<uint8_t, 8> kPriVnxtSignature = {'m', 'r', 'm', '_', 'v', 'n', 'x', 't'};

bool matches_bytes_at(std::span<const uint8_t> data,
                      std::size_t offset,
                      std::span<const uint8_t> needle) noexcept {
    if (offset > data.size() || data.size() - offset < needle.size()) {
        return false;
    }
    return std::equal(needle.begin(),
                      needle.end(),
                      data.begin() + static_cast<std::ptrdiff_t>(offset));
}

ContainerKind classify_magic_header(std::span<const uint8_t> header) noexcept {
    const auto magic_le = read_u32_le(header, 0);
    if (!magic_le.has_value()) {
        return ContainerKind::Unknown;
    }

    if (*magic_le == kRpf9Magic || *magic_le == kRpf9MagicAsciiLe) {
        return ContainerKind::Rpf9;
    }
    if (*magic_le == kRds5Magic || *magic_le == kRds5MagicAsciiLe) {
        return ContainerKind::Rds5;
    }

    if (header.size() >= 8u) {
        if (matches_bytes_at(header, 0, kPriPrefix)) {
            const uint8_t ver = header[7];
            if (ver == '0' || ver == '1' || ver == '2' || ver == '3' || ver == 'f') {
                return ContainerKind::Pri;
            }
        }
        if (matches_bytes_at(header, 0, kPriVnxtSignature)) {
            return ContainerKind::Pri;
        }
        if (matches_bytes_at(header, 0, kXvdSignature)) {
            return ContainerKind::Xvd;
        }
    }

    if (*magic_le == kXvdMagic && matches_bytes_at(header, 4, kXvdSignature)) {
        return ContainerKind::Xvd;
    }
    if (matches_bytes_at(header, kXvdSignedHeaderOffset, kXvdSignature)) {
        return ContainerKind::Xvd;
    }

    return ContainerKind::Unknown;
}

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

// True when normalized `path` equals normalized `ancestor` or lies beneath it.
bool is_same_or_descendant(std::string_view path, std::string_view ancestor) noexcept {
    if (ancestor == "/") {
        return true;
    }
    if (path.size() < ancestor.size() || path.compare(0, ancestor.size(), ancestor) != 0) {
        return false;
    }
    return path.size() == ancestor.size() || path[ancestor.size()] == '/';
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
Status verify_data_layout(const std::filesystem::path& host_dir,
                          DataLayoutFingerprint* out_observed) {
    if (out_observed != nullptr) {
        *out_observed = DataLayoutFingerprint{};
    }

    std::error_code ec;
    if (host_dir.empty() || !std::filesystem::exists(host_dir, ec) ||
        !std::filesystem::is_directory(host_dir, ec)) {
        return Status::NotFound;
    }

    DataLayoutFingerprint observed{};
    std::array<uint8_t, kMagicProbeSize> probe_buf{};

    std::filesystem::recursive_directory_iterator it(
        host_dir, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        return Status::ErrUnsupportedDataLayout;
    }

    const std::filesystem::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto& entry = *it;
        std::error_code entry_ec;

        if (entry.is_directory(entry_ec) && !entry_ec) {
            const auto name_path = entry.path().filename();
            const auto& native_name = name_path.native();
            if (!native_name.empty() &&
                native_name.front() == static_cast<std::filesystem::path::value_type>('.')) {
                it.disable_recursion_pending();
            }
            continue;
        }

        if (!entry.is_regular_file(entry_ec) || entry_ec) {
            continue;
        }

        const auto file_size = entry.file_size(entry_ec);
        if (entry_ec || file_size < 4u) {
            continue;
        }

        std::ifstream file(entry.path(), std::ios::binary);
        if (!file.is_open()) {
            continue;
        }

        file.read(reinterpret_cast<char*>(probe_buf.data()),
                  static_cast<std::streamsize>(probe_buf.size()));
        if (file.bad()) {
            continue;
        }

        const std::streamsize raw_read = file.gcount();
        if (raw_read < 4) {
            continue;
        }

        const std::span<const uint8_t> header(probe_buf.data(),
                                              static_cast<std::size_t>(raw_read));
        switch (classify_magic_header(header)) {
            case ContainerKind::Rpf9:
                ++observed.rpf9_count;
                break;
            case ContainerKind::Rds5:
                ++observed.rds5_count;
                break;
            case ContainerKind::Xvd:
                ++observed.xvd_count;
                break;
            case ContainerKind::Pri:
                ++observed.pri_count;
                break;
            case ContainerKind::AlignmentChunk:
            case ContainerKind::Unknown:
                break;
        }
    }

    if (out_observed != nullptr) {
        *out_observed = observed;
    }

    if (observed != kExpectedDataLayout) {
        return Status::ErrUnsupportedDataLayout;
    }
    return Status::Ok;
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

OPEN_VFS_IMPL
Status open_virtual_stream(const MountRegistry& registry,
                           std::string_view virtual_path,
                           VirtualStreamHandle& out_handle) {
    out_handle = VirtualStreamHandle{};

    const std::string target = normalize_virtual_path(virtual_path);
    if (target == "/") {
        return Status::NotFound;
    }

    // Among mounts whose prefix covers the path, the longest prefix wins so a
    // nested mount shadows its parent. Archives listed under a mount that does
    // not cover the requested path are never opened through it.
    bool found = false;
    std::size_t best_prefix_length = 0;
    VirtualStreamHandle best{};

    for (std::size_t mount_idx = 0; mount_idx < registry.mounts.size(); ++mount_idx) {
        const auto& mount = registry.mounts[mount_idx];
        const std::string mount_prefix = normalize_virtual_path(mount.virtual_prefix);
        if (!is_same_or_descendant(target, mount_prefix)) {
            continue;
        }
        if (found && mount_prefix.size() <= best_prefix_length) {
            continue;
        }

        for (std::size_t archive_idx = 0; archive_idx < mount.discovered_archives.size();
             ++archive_idx) {
            const auto& archive = mount.discovered_archives[archive_idx];
            if (normalize_virtual_path(archive.virtual_root) != target) {
                continue;
            }
            found = true;
            best_prefix_length = mount_prefix.size();
            best.mount_index = mount_idx;
            best.archive_index = archive_idx;
            best.offset = 0;
            best.size = archive.file_size;
            best.host_path = archive.host_path;
            best.valid = true;
            break;
        }
    }

    if (!found) {
        return Status::NotFound;
    }
    out_handle = std::move(best);
    return Status::Ok;
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

OPEN_VFS_IMPL
Status enumerate_mounted_entries(const MountRegistry& registry,
                                 std::string_view virtual_prefix,
                                 std::vector<std::string>& out_paths) {
    out_paths.clear();

    const std::string prefix = normalize_virtual_path(virtual_prefix);
    bool prefix_covered = false;

    for (const auto& mount : registry.mounts) {
        const std::string mount_prefix = normalize_virtual_path(mount.virtual_prefix);
        // The requested prefix is visible when it lies inside this mount or is an
        // ancestor of it (e.g. listing "/" exposes every mount below the root).
        if (!is_same_or_descendant(prefix, mount_prefix) &&
            !is_same_or_descendant(mount_prefix, prefix)) {
            continue;
        }
        prefix_covered = true;

        for (const auto& archive : mount.discovered_archives) {
            std::string entry_path = normalize_virtual_path(archive.virtual_root);
            if (entry_path != prefix && is_same_or_descendant(entry_path, prefix)) {
                out_paths.push_back(std::move(entry_path));
            }
        }
    }

    if (!prefix_covered) {
        return Status::NotFound;
    }

    // Deterministic, duplicate-free listing even when overlapping mounts expose
    // the same virtual path.
    std::sort(out_paths.begin(), out_paths.end());
    out_paths.erase(std::unique(out_paths.begin(), out_paths.end()), out_paths.end());
    return Status::Ok;
}

}  // namespace open_vfs::vfs_core
