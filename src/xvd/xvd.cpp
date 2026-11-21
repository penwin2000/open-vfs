#include "xvd.hpp"

#include <algorithm>
#include <open_vfs/macros.hpp>
#include "../vfs_core/vfs_core.hpp"

namespace open_vfs::xvd {

OPEN_VFS_IMPL
bool validate_xvd_magic(std::span<const uint8_t> header_prefix) noexcept {
    if (header_prefix.size() < 12) {
        return false;
    }
    const bool has_sig = std::equal(kXvdSignature.begin(), kXvdSignature.end(), header_prefix.begin());
    const auto magic = vfs_core::read_u32_le(header_prefix, 8);
    if (has_sig && magic.has_value() && *magic == kXvdMagic) {
        return true;
    }
    const auto lead_magic = vfs_core::read_u32_le(header_prefix, 0);
    const bool trailing_sig = std::equal(kXvdSignature.begin(),
                                         kXvdSignature.end(),
                                         header_prefix.begin() + 4);
    return lead_magic.has_value() && *lead_magic == kXvdMagic && trailing_sig;
}

OPEN_VFS_IMPL
Status parse_xvd_header(std::span<const uint8_t> data, XvdHeader& out_header) noexcept {
    if (data.size() < kXvdHeaderSize) {
        return Status::OutOfBounds;
    }
    if (!validate_xvd_magic(data.subspan(0, 12))) {
        return Status::InvalidMagic;
    }

    const auto version = vfs_core::read_u32_le(data, 12);
    const auto page_size = vfs_core::read_u32_le(data, 16);
    const auto partition_count = vfs_core::read_u32_le(data, 20);
    const auto drive_size = vfs_core::read_u64_le(data, 24);
    const auto data_offset = vfs_core::read_u64_le(data, 32);
    const auto hash_tree_offset = vfs_core::read_u64_le(data, 40);
    const auto userdata_offset = vfs_core::read_u64_le(data, 48);

    if (!version || !page_size || !partition_count || !drive_size || !data_offset ||
        !hash_tree_offset || !userdata_offset) {
        return Status::OutOfBounds;
    }
    if (*page_size == 0 || !vfs_core::is_aligned_16(*page_size) || !vfs_core::is_aligned_16(*data_offset)) {
        return Status::AlignmentError;
    }

    out_header.magic = kXvdMagic;
    out_header.format_version = *version;
    out_header.page_size = *page_size;
    out_header.partition_count = *partition_count;
    out_header.drive_size = *drive_size;
    out_header.data_offset = *data_offset;
    out_header.hash_tree_offset = *hash_tree_offset;
    out_header.userdata_offset = *userdata_offset;
    return Status::Ok;
}

OPEN_VFS_IMPL
Status parse_xvd_partition_entry(std::span<const uint8_t> data,
                                 XvdPartitionEntry& out_entry) noexcept {
    if (data.size() < kXvdPartitionEntrySize) {
        return Status::OutOfBounds;
    }

    const auto partition_id = vfs_core::read_u32_le(data, 0);
    const auto flags = vfs_core::read_u32_le(data, 4);
    const auto start_lba = vfs_core::read_u64_le(data, 8);
    const auto sector_count = vfs_core::read_u64_le(data, 16);
    const auto byte_length = vfs_core::read_u64_le(data, 24);

    if (!partition_id || !flags || !start_lba || !sector_count || !byte_length) {
        return Status::OutOfBounds;
    }

    out_entry.partition_id = *partition_id;
    out_entry.flags = *flags;
    out_entry.start_lba = *start_lba;
    out_entry.sector_count = *sector_count;
    out_entry.byte_length = *byte_length;
    return Status::Ok;
}

OPEN_VFS_STUB
Status enumerate_xvd_partitions(std::span<const uint8_t> volume_bytes,
                                const XvdHeader& header,
                                std::vector<XvdPartitionEntry>& out_partitions) {
    (void)volume_bytes;
    (void)header;
    out_partitions.clear();
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status verify_xvd_hash_tree(std::span<const uint8_t> volume_bytes,
                            const XvdHeader& header,
                            bool& out_valid) {
    (void)volume_bytes;
    (void)header;
    out_valid = false;
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status map_xvd_logical_page(const XvdHeader& header,
                            uint64_t logical_page_index,
                            uint64_t& out_physical_offset) noexcept {
    (void)header;
    (void)logical_page_index;
    out_physical_offset = 0;
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status read_xvd_page(std::span<const uint8_t> volume_bytes,
                     const XvdHeader& header,
                     uint64_t logical_page_index,
                     std::span<uint8_t> out_page_buffer) {
    (void)volume_bytes;
    (void)header;
    (void)logical_page_index;
    (void)out_page_buffer;
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status mount_xvd_partition(const XvdHeader& header,
                           const XvdPartitionEntry& partition,
                           std::string& out_mount_label) {
    (void)header;
    (void)partition;
    out_mount_label.clear();
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status parse_xvd_userdata_segment(std::span<const uint8_t> userdata_bytes,
                                  XvdUserDataHeader& out_userdata) noexcept {
    (void)userdata_bytes;
    out_userdata = XvdUserDataHeader{};
    return Status::NotImplemented;
}

}  // namespace open_vfs::xvd
