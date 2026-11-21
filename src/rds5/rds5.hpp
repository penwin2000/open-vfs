#pragma once

#include <algorithm>
#include <array>
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
constexpr uint8_t kRds5CodecBc1 = 0x1u;                  // Hardware BCPack BC1
constexpr uint8_t kRds5CodecBc2 = 0x2u;                  // Hardware BCPack BC2
constexpr uint8_t kRds5CodecBc3 = 0x3u;                  // Hardware BCPack BC3
constexpr uint8_t kRds5CodecBc4 = 0x4u;                  // Hardware BCPack BC4
constexpr uint8_t kRds5CodecBc5 = 0x5u;                  // Hardware BCPack BC5
constexpr uint8_t kRds5CodecBc6 = 0x6u;                  // Hardware BCPack BC6
constexpr uint8_t kRds5CodecBc7 = 0x7u;                  // Hardware BCPack BC7
constexpr uint8_t kRds5CodecZlib = 0x8u;                 // RFC 1950 zlib stream

constexpr uint32_t kEtpfMagic = 0x46505445u;             // "ETPF" (bytes 45 54 50 46 LE)
constexpr uint32_t kEtpfVersion = 0x0000027Au;
constexpr std::size_t kEtpfHeaderSize = 16;              // [+0x00, +0x10)
constexpr std::size_t kEtpfDescriptorSize = 16;          // Primary 16-byte GPU texture descriptor
constexpr std::size_t kEtpfKeySize = 8;                  // Strictly ascending u64 resource key
constexpr std::size_t kEtpfSecondaryEntrySize = 8;       // Opaque 8-byte secondary entry
constexpr std::size_t kEtpfTotalBytesPerEntry =
    kEtpfDescriptorSize + kEtpfKeySize + kEtpfSecondaryEntrySize;  // 32 bytes/entry
constexpr std::size_t kEtpfAuxPoolAlignment = 16;        // aux_pool_size (S_aux) alignment
constexpr std::size_t kEtpfChunkSizeCodeCount = 4;       // Four 5-bit codes in q23 bits [21..40]
constexpr uint8_t kEtpfMaxChunkSizeCode = 31u;           // 5-bit code range [0..31]
constexpr uint32_t kEtpfMaxVirtPage = 0x1FFFFFu;         // 21-bit virt_page field
constexpr uint16_t kEtpfMaxDimension = 16384u;           // (0x3FFF) + 1
constexpr uint8_t kEtpfMaxDepthOrSlices = 64u;           // (0x3F) + 1

enum class EtpfTextureDimension : uint8_t {
    Texture2D = 1,
    TextureCube = 2,
    Texture3D = 3,
    Texture2DArray = 5,
};

[[nodiscard]] constexpr bool is_valid_etpf_dimension(uint8_t raw_flags) noexcept {
    return raw_flags == static_cast<uint8_t>(EtpfTextureDimension::Texture2D) ||
           raw_flags == static_cast<uint8_t>(EtpfTextureDimension::TextureCube) ||
           raw_flags == static_cast<uint8_t>(EtpfTextureDimension::Texture3D) ||
           raw_flags == static_cast<uint8_t>(EtpfTextureDimension::Texture2DArray);
}

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
    uint8_t codec_id = 0;           // (raw >> 26) & 0xFULL (0x0 raw, 0x1..0x7 BCPack, 0x8 zlib)
};

struct Rds5ChunkInfo {
    uint64_t disk_offset = 0;        // 256-byte aligned start offset in container
    uint64_t disk_span = 0;          // descriptors[i + 1].disk_offset - descriptors[i].disk_offset
    uint64_t stored_size = 0;        // disk_span - trailing_padding
    uint64_t virtual_offset = 0;     // 4 KiB aligned start offset in virtual stream
    uint64_t uncompressed_size = 0;  // descriptors[i + 1].virtual_offset - descriptors[i].virtual_offset
    uint16_t trailing_padding = 0;   // Trailing alignment padding (0..4,095 bytes)
    uint8_t codec_id = 0;            // 0x0 raw, 0x1..0x7 hardware BCPack BC1..BC7, 0x8 RFC 1950 zlib
};

struct EtpfHeader {
    uint32_t magic = 0;          // 0x46505445 ("ETPF")
    uint32_t version = 0;        // 0x0000027A
    uint32_t entry_count = 0;    // Number of primary 16-byte GPU texture descriptors (N)
    uint32_t aux_pool_size = 0;  // 16-byte aligned byte length of trailing aux pool (S_aux)

    [[nodiscard]] constexpr uint64_t descriptors_offset() const noexcept {
        return static_cast<uint64_t>(kEtpfHeaderSize);
    }

    [[nodiscard]] constexpr uint64_t keys_offset() const noexcept {
        return static_cast<uint64_t>(kEtpfHeaderSize) +
               (static_cast<uint64_t>(entry_count) * static_cast<uint64_t>(kEtpfDescriptorSize));
    }

    [[nodiscard]] constexpr uint64_t secondary_offset() const noexcept {
        return keys_offset() +
               (static_cast<uint64_t>(entry_count) * static_cast<uint64_t>(kEtpfKeySize));
    }

    [[nodiscard]] constexpr uint64_t aux_pool_offset() const noexcept {
        return secondary_offset() +
               (static_cast<uint64_t>(entry_count) * static_cast<uint64_t>(kEtpfSecondaryEntrySize));
    }

    [[nodiscard]] constexpr uint64_t total_span() const noexcept {
        return aux_pool_offset() + static_cast<uint64_t>(aux_pool_size);
    }
};

struct EtpfTextureDescriptor {
    uint32_t raw_w0 = 0;
    uint32_t raw_w1 = 0;
    uint64_t raw_q23 = 0;

    // Decoded from w0 (+0x00)
    uint16_t width = 0;               // (w0 & 0x3FFF) + 1  (1..16384)
    uint16_t height = 0;              // ((w0 >> 14) & 0x3FFF) + 1  (1..16384)
    uint8_t tail_mip_count = 0;       // (w0 >> 28) & 0xF

    // Decoded from w1 (+0x04)
    uint8_t streamed_mip_count = 0;   // w1 & 0x7
    uint8_t dxgi_format = 0;          // (w1 >> 3) & 0x7F
    uint8_t depth_or_slices = 0;      // ((w1 >> 10) & 0x3F) + 1  (1..64)
    uint8_t dimension_flags = 0;      // (w1 >> 21) & 0x7 (1=2D, 2=Cube, 3=3D, 5=2DArray)
    uint8_t packed_tier_count = 0;    // (w1 >> 24) & 0x7
    bool is_cube_tile_flag = false;   // (w1 >> 27) & 1
    bool aux_flag_10 = false;         // (w1 >> 28) & 1

    // Decoded from q23 (+0x08)
    uint32_t virt_page = 0;           // q23 & 0x1FFFFF (4 KiB virtual page index)
    std::array<uint8_t, kEtpfChunkSizeCodeCount> chunk_size_codes{};  // 4 x 5-bit codes [21..40]
    uint8_t sub64k_chunk_count = 0;   // (q23 >> 51) & 0x3
    uint8_t tile0_free_pages = 0;     // (q23 >> 53) & 0xF

    [[nodiscard]] constexpr EtpfTextureDimension dimension() const noexcept {
        return static_cast<EtpfTextureDimension>(dimension_flags);
    }

    [[nodiscard]] constexpr uint64_t virtual_offset_bytes() const noexcept {
        return static_cast<uint64_t>(virt_page) * kRds5VirtualPageSize;
    }
};

struct EtpfPackedDescriptorWords {
    uint32_t w0 = 0;
    uint32_t w1 = 0;
    uint64_t q23 = 0;
};

struct EtpfTable {
    EtpfHeader header{};
    std::vector<EtpfTextureDescriptor> descriptors;  // [+0x10, +0x10 + 16*N), monotonic by virt_page
    std::vector<uint64_t> resource_keys;             // [+0x10 + 16*N, +0x10 + 24*N), strictly ascending
    std::vector<uint64_t> secondary_entries;         // [+0x10 + 24*N, +0x10 + 32*N), opaque 8-byte/entry
    std::vector<uint8_t> aux_pool;                   // [+0x10 + 32*N, +0x10 + 32*N + S_aux), 16-byte aligned

    [[nodiscard]] std::optional<std::size_t> find_key_index(uint64_t resource_key) const noexcept {
        const auto it = std::lower_bound(resource_keys.begin(), resource_keys.end(), resource_key);
        if (it == resource_keys.end() || *it != resource_key) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(it - resource_keys.begin());
    }
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

[[nodiscard]] constexpr std::optional<EtpfPackedDescriptorWords> pack_etpf_texture_descriptor(
    const EtpfTextureDescriptor& desc) noexcept {
    if (desc.width == 0u || desc.width > kEtpfMaxDimension ||
        desc.height == 0u || desc.height > kEtpfMaxDimension ||
        desc.tail_mip_count > 0x0Fu) {
        return std::nullopt;
    }
    if (desc.streamed_mip_count > 0x07u ||
        desc.dxgi_format > 0x7Fu ||
        desc.depth_or_slices == 0u || desc.depth_or_slices > kEtpfMaxDepthOrSlices ||
        !is_valid_etpf_dimension(desc.dimension_flags) ||
        desc.packed_tier_count > 0x07u) {
        return std::nullopt;
    }
    if (desc.virt_page > kEtpfMaxVirtPage ||
        desc.sub64k_chunk_count > 0x03u ||
        desc.tile0_free_pages > 0x0Fu) {
        return std::nullopt;
    }
    for (uint8_t code : desc.chunk_size_codes) {
        if (code > kEtpfMaxChunkSizeCode) {
            return std::nullopt;
        }
    }

    const uint32_t w0 =
        (static_cast<uint32_t>(desc.width - 1u) & 0x3FFFu) |
        ((static_cast<uint32_t>(desc.height - 1u) & 0x3FFFu) << 14u) |
        ((static_cast<uint32_t>(desc.tail_mip_count) & 0x0Fu) << 28u);

    const uint32_t w1 =
        (static_cast<uint32_t>(desc.streamed_mip_count) & 0x07u) |
        ((static_cast<uint32_t>(desc.dxgi_format) & 0x7Fu) << 3u) |
        ((static_cast<uint32_t>(desc.depth_or_slices - 1u) & 0x3Fu) << 10u) |
        ((static_cast<uint32_t>(desc.dimension_flags) & 0x07u) << 21u) |
        ((static_cast<uint32_t>(desc.packed_tier_count) & 0x07u) << 24u) |
        (desc.is_cube_tile_flag ? (uint32_t{1} << 27u) : 0u) |
        (desc.aux_flag_10 ? (uint32_t{1} << 28u) : 0u);

    const uint64_t q23 =
        (static_cast<uint64_t>(desc.virt_page) & 0x1FFFFFull) |
        ((static_cast<uint64_t>(desc.chunk_size_codes[0]) & 0x1Full) << 21u) |
        ((static_cast<uint64_t>(desc.chunk_size_codes[1]) & 0x1Full) << 26u) |
        ((static_cast<uint64_t>(desc.chunk_size_codes[2]) & 0x1Full) << 31u) |
        ((static_cast<uint64_t>(desc.chunk_size_codes[3]) & 0x1Full) << 36u) |
        ((static_cast<uint64_t>(desc.sub64k_chunk_count) & 0x03ull) << 51u) |
        ((static_cast<uint64_t>(desc.tile0_free_pages) & 0x0Full) << 53u);

    return EtpfPackedDescriptorWords{w0, w1, q23};
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

// Lookup the verified 5-bit ETPF chunk_size_codes table (codes 0..31), returning
// the 4 KiB page count (bytes = pages * 4096):
//   - code 0 returns 0 (absent / empty slot)
//   - verified codes return their 4 KiB page count
//   - unobserved codes (9, 17..20, 25, 29..31) and codes > 31 return std::nullopt
// Note: RDS5 chunk descriptors remain authoritative for exact per-chunk uncompressed_size.
[[nodiscard]] std::optional<uint32_t> lookup_etpf_chunk_size_pages(uint8_t code) noexcept;

[[nodiscard]] inline std::optional<uint64_t> lookup_etpf_chunk_size_bytes(uint8_t code) noexcept {
    const auto pages = lookup_etpf_chunk_size_pages(code);
    if (!pages.has_value()) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(*pages) * kRds5VirtualPageSize;
}

Status parse_etpf_texture_descriptor(std::span<const uint8_t> data,
                                     EtpfTextureDescriptor& out_descriptor) noexcept;

Status parse_etpf_table(std::span<const uint8_t> chunk0_data,
                        EtpfTable& out_table);

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
