#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <open_vfs/status.hpp>

namespace open_vfs::rpf9 {

constexpr uint32_t kRpf9Magic = 0x52504639u;         // "RPF9"
constexpr uint32_t kRpf9MagicAsciiLe = 0x39465052u;  // {'R','P','F','9'} read as LE u32
constexpr std::size_t kRpf9HeaderSize = 32;
constexpr std::size_t kRpf9TocEntrySize = 16;

struct Rpf9Header {
    uint32_t magic = 0;
    uint32_t entry_count = 0;
    uint32_t names_length = 0;
    uint32_t encryption_flags = 0;
    uint64_t toc_offset = 0;
    uint64_t names_offset = 0;
};

struct Rpf9TocEntry {
    uint32_t name_offset = 0;
    uint32_t flags = 0;
    uint32_t offset_or_index = 0;
    uint32_t size_or_count = 0;
    bool is_directory = false;
};

struct Rpf9BlockHeader {
    uint32_t compressed_size = 0;
    uint32_t uncompressed_size = 0;
    uint32_t codec_id = 0;
    uint32_t block_flags = 0;
};

bool validate_rpf9_magic(uint32_t raw_magic) noexcept;

Status parse_rpf9_header(std::span<const uint8_t> data, Rpf9Header& out_header) noexcept;

Status parse_rpf9_toc_entry(std::span<const uint8_t> entry_bytes, Rpf9TocEntry& out_entry) noexcept;

Status parse_rpf9_toc_table(std::span<const uint8_t> archive_bytes,
                            const Rpf9Header& header,
                            std::vector<Rpf9TocEntry>& out_entries);

Status resolve_rpf9_name_table(std::span<const uint8_t> names_heap,
                               const std::vector<Rpf9TocEntry>& entries,
                               std::vector<std::string>& out_names);

Status locate_rpf9_entry_by_path(const std::vector<Rpf9TocEntry>& entries,
                                 const std::vector<std::string>& names,
                                 std::string_view virtual_path,
                                 std::size_t& out_index);

Status parse_rpf9_block_header(std::span<const uint8_t> block_bytes,
                               Rpf9BlockHeader& out_block) noexcept;

Status decompress_rpf9_block(const Rpf9BlockHeader& block_header,
                             std::span<const uint8_t> compressed_payload,
                             std::vector<uint8_t>& out_plain);

Status decrypt_rpf9_payload(uint32_t encryption_flags,
                            std::span<uint8_t> payload_in_out);

Status extract_rpf9_entry_bytes(std::span<const uint8_t> archive_bytes,
                                const Rpf9Header& header,
                                const Rpf9TocEntry& entry,
                                std::vector<uint8_t>& out_payload);

}  // namespace open_vfs::rpf9
