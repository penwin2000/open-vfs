#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <open_vfs/status.hpp>

namespace open_vfs::xvd {

constexpr uint32_t kXvdMagic = 0x1DD3014Eu;
constexpr std::array<uint8_t, 8> kXvdSignature = {'m', 's', 'f', 't', '-', 'x', 'v', 'd'};
constexpr std::size_t kXvdHeaderSize = 64;
constexpr std::size_t kXvdPartitionEntrySize = 32;
constexpr uint32_t kXvdPageSize = 4096;

struct XvdHeader {
    uint32_t magic = 0;
    uint32_t format_version = 0;
    uint32_t page_size = kXvdPageSize;
    uint32_t partition_count = 0;
    uint64_t drive_size = 0;
    uint64_t data_offset = 0;
    uint64_t hash_tree_offset = 0;
    uint64_t userdata_offset = 0;
};

struct XvdPartitionEntry {
    uint32_t partition_id = 0;
    uint32_t flags = 0;
    uint64_t start_lba = 0;
    uint64_t sector_count = 0;
    uint64_t byte_length = 0;
};

struct XvdUserDataHeader {
    uint32_t version = 0;
    uint32_t payload_length = 0;
    uint64_t data_offset = 0;
};

bool validate_xvd_magic(std::span<const uint8_t> header_prefix) noexcept;

Status parse_xvd_header(std::span<const uint8_t> data, XvdHeader& out_header) noexcept;

Status parse_xvd_partition_entry(std::span<const uint8_t> data,
                                 XvdPartitionEntry& out_entry) noexcept;

Status enumerate_xvd_partitions(std::span<const uint8_t> volume_bytes,
                                const XvdHeader& header,
                                std::vector<XvdPartitionEntry>& out_partitions);

Status verify_xvd_hash_tree(std::span<const uint8_t> volume_bytes,
                            const XvdHeader& header,
                            bool& out_valid);

Status map_xvd_logical_page(const XvdHeader& header,
                            uint64_t logical_page_index,
                            uint64_t& out_physical_offset) noexcept;

Status read_xvd_page(std::span<const uint8_t> volume_bytes,
                     const XvdHeader& header,
                     uint64_t logical_page_index,
                     std::span<uint8_t> out_page_buffer);

Status mount_xvd_partition(const XvdHeader& header,
                           const XvdPartitionEntry& partition,
                           std::string& out_mount_label);

Status parse_xvd_userdata_segment(std::span<const uint8_t> userdata_bytes,
                                  XvdUserDataHeader& out_userdata) noexcept;

}  // namespace open_vfs::xvd
