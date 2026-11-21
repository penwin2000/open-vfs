#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <open_vfs/status.hpp>

namespace open_vfs::rds5 {

constexpr uint32_t kRds5Magic = 0x35534452u;             // "RDS5" (bytes 52 44 53 35 LE)
constexpr std::size_t kRds5HeaderSize = 16;              // Fixed 16-byte header [0x00..0x10)
constexpr std::size_t kRds5DescriptorSize = 8;           // 8-byte LE uint64_t bitfield descriptor
constexpr uint32_t kRds5MinDescriptorCount = 2;          // At least Chunk 0 + EOF sentinel
constexpr uint8_t kRds5MaxChunkShift = 0x16u;            // log2(4 MiB) = 22
constexpr uint64_t kRds5MaxChunkSize = uint64_t{1} << kRds5MaxChunkShift;
constexpr uint64_t kRds5DiskUnitBytes = 256ull;          // 2^8 bytes (bits [0..25] << 8)
constexpr uint64_t kRds5VirtualPageSize = 4096ull;       // 2^12 bytes (bits [30..51] << 12)
constexpr uint64_t kRds5TableAlignment = 0x100ull;       // Chunk 0 starts at ALIGN_UP(table_end, 0x100)
constexpr uint64_t kRds5MaxTotalUncompressedSize = 0x00FFFFFFFFFFFFFFull;  // 56-bit header field
constexpr uint64_t kRds5MaxDiskOffset = 0x03FFFFFFull << 8u;
constexpr uint64_t kRds5MaxVirtualOffset = 0x3FFFFFull << 12u;
constexpr uint16_t kRds5MaxTrailingPadding = 0x0FFFu;    // 12-bit field (0..4,095 bytes)

constexpr uint8_t kRds5CodecRaw = 0x0u;                  // Uncompressed chunk
constexpr uint8_t kRds5CodecZlib = 0x8u;                 // RFC 1950 zlib stream

constexpr uint32_t kEtpfMagic = 0x46505445u;             // "ETPF" (bytes 45 54 50 46 LE)
constexpr uint32_t kEtpfVersion = 0x0000027Au;
constexpr std::size_t kEtpfHeaderSize = 16;
constexpr std::size_t kEtpfDescriptorSize = 16;

struct Rds5Header {
    uint32_t magic = 0;
    uint32_t descriptor_count = 0;         // chunk_count + 1 (includes EOF sentinel)
    uint64_t total_uncompressed_size = 0;  // 56-bit LE at +0x08..+0x0E (4 KiB-aligned)
    uint8_t max_chunk_shift = 0;           // +0x0F (0x16 => 4 MiB max uncompressed chunk)

    [[nodiscard]] constexpr uint32_t chunk_count() const noexcept {
        return descriptor_count > 0u ? (descriptor_count - 1u) : 0u;
    }

    [[nodiscard]] constexpr uint64_t max_chunk_size() const noexcept {
        return max_chunk_shift < 64u ? (uint64_t{1} << max_chunk_shift) : 0ull;
    }
};

struct Rds5ChunkDescriptor {
    uint64_t raw = 0;               // Raw 64-bit LE descriptor value
    uint64_t disk_offset = 0;       // (raw & 0x03FFFFFFULL) << 8 (256-byte units)
    uint64_t virtual_offset = 0;    // ((raw >> 30) & 0x3FFFFFULL) << 12 (4 KiB page units)
    uint16_t trailing_padding = 0;  // (raw >> 52) & 0xFFFULL (0..4,095 bytes)
    uint8_t codec_id = 0;           // (raw >> 26) & 0xFULL (0x0 raw, 0x8 zlib, ...)
};

struct Rds5ChunkInfo {
    uint64_t disk_offset = 0;        // 256-byte aligned start offset in container
    uint64_t disk_span = 0;          // descriptors[i + 1].disk_offset - descriptors[i].disk_offset
    uint64_t stored_size = 0;        // disk_span - trailing_padding
    uint64_t virtual_offset = 0;     // 4 KiB aligned start offset in virtual stream
    uint64_t uncompressed_size = 0;  // descriptors[i + 1].virtual_offset - descriptors[i].virtual_offset
    uint16_t trailing_padding = 0;   // Trailing alignment padding (0..4,095 bytes)
    uint8_t codec_id = 0;            // 0x0 raw, 0x8 RFC 1950 zlib, 0x1/0x3..0x7 stream codecs
};

struct EtpfHeader {
    uint32_t magic = 0;                   // 0x46505445 ("ETPF")
    uint32_t version = 0;                 // 0x0000027A
    uint32_t entry_count = 0;             // Number of 16-byte descriptors at +0x10
    uint32_t secondary_table_offset = 0;  // Byte offset within Chunk 0 to secondary sub-table
};

struct EtpfResourceDescriptor {
    uint64_t resource_id = 0;
    uint32_t virtual_offset = 0;
    uint32_t size = 0;
};

[[nodiscard]] constexpr std::optional<uint64_t> pack_rds5_descriptor(
    uint64_t disk_offset,
    uint8_t codec_id,
    uint64_t virtual_offset,
    uint16_t trailing_padding) noexcept {
    if ((disk_offset & (kRds5DiskUnitBytes - 1ull)) != 0ull ||
        disk_offset > kRds5MaxDiskOffset) {
        return std::nullopt;
    }
    if (codec_id > 0x0Fu) {
        return std::nullopt;
    }
    if ((virtual_offset & (kRds5VirtualPageSize - 1ull)) != 0ull ||
        virtual_offset > kRds5MaxVirtualOffset) {
        return std::nullopt;
    }
    if (trailing_padding > kRds5MaxTrailingPadding) {
        return std::nullopt;
    }

    const uint64_t disk_units = disk_offset >> 8u;
    const uint64_t virt_pages = virtual_offset >> 12u;
    return (disk_units & 0x03FFFFFFull) |
           ((static_cast<uint64_t>(codec_id) & 0xFull) << 26u) |
           ((virt_pages & 0x3FFFFFull) << 30u) |
           ((static_cast<uint64_t>(trailing_padding) & 0xFFFull) << 52u);
}

bool validate_rds5_magic(uint32_t raw_magic) noexcept;

Status parse_rds5_header(std::span<const uint8_t> data, Rds5Header& out_header) noexcept;

Status parse_rds5_descriptor(std::span<const uint8_t> data,
                             Rds5ChunkDescriptor& out_descriptor) noexcept;

Status parse_rds5_descriptor_table(std::span<const uint8_t> container_bytes,
                                   const Rds5Header& header,
                                   std::vector<Rds5ChunkDescriptor>& out_descriptors,
                                   bool validate_full_container = true);

Status parse_rds5_chunk_index(std::span<const uint8_t> container_bytes,
                              const Rds5Header& header,
                              std::vector<Rds5ChunkInfo>& out_chunks,
                              bool validate_full_container = true);

Status parse_etpf_header(std::span<const uint8_t> chunk0_data,
                         EtpfHeader& out_header) noexcept;

Status read_rds5_stream_chunk(std::span<const uint8_t> container_bytes,
                              const Rds5ChunkInfo& chunk,
                              std::vector<uint8_t>& out_raw_chunk);

Status decompress_rds5_chunk(const Rds5ChunkInfo& chunk,
                             std::span<const uint8_t> raw_chunk,
                             std::vector<uint8_t>& out_plain);

Status resolve_rds5_dependencies(const std::vector<EtpfResourceDescriptor>& descriptors,
                                 uint64_t root_resource_id,
                                 std::vector<std::size_t>& out_dependency_indices);

Status assemble_rds5_resource(const EtpfResourceDescriptor& descriptor,
                              std::span<const uint8_t> virtual_pages,
                              std::span<const uint8_t> physical_pages,
                              std::vector<uint8_t>& out_image);

}  // namespace open_vfs::rds5
