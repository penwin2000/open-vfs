#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <open_vfs/status.hpp>

namespace open_vfs::rpf9 {

constexpr uint32_t kRpf9Magic = 0x52504639u;             // Bytes 39 46 50 52 read as LE u32
constexpr std::size_t kRpf9HeaderSize = 64;              // 64-byte plaintext header [0x00..0x40)
constexpr std::size_t kRpf9CryptoBlockOffset = 0x40;     // Start of 272-byte crypto block
constexpr std::size_t kRpf9CryptoBlockSize = 272;        // [0x40..0x150)
constexpr uint32_t kRpf9FormatConstant = 0x00047A00u;    // +0x18 verified format constant
constexpr uint32_t kRpf9RegionAOffset = 0x00000150u;     // +0x1C start of Region A (64 + 272)
constexpr uint16_t kRpf9RegionBModeCompact = 1u;         // +0x22 mode 1: ALIGN_UP(count * (8 + width), 16)
constexpr uint16_t kRpf9RegionBModeExtended = 2u;        // +0x22 mode 2: extended Region B sizing
constexpr uint8_t kRpf9HeaderByte2D = 0x0Fu;             // +0x2D verified constant byte
constexpr uint8_t kRpf9HeaderByte2E = 0x09u;             // +0x2E verified constant byte
constexpr uint8_t kRpf9HeaderByte2F = 0x00u;             // +0x2F verified constant byte
constexpr uint32_t kRpf9HeaderAlignment = 0x10u;         // +0x30 verified 16-byte alignment field
constexpr std::size_t kRpf9ReservedPaddingOffset = 0x34; // +0x34..+0x3F (12 zero bytes)
constexpr std::size_t kRpf9ReservedPaddingSize = 12;
constexpr std::size_t kRpf9TocEntrySize = 16;

// Plaintext payload scanner alignment strides and signatures
constexpr uint64_t kRpf9PageAlignment = 0x1000ull;       // 4 KiB alignment for nested RPF9/SGD5/ADAT
constexpr uint64_t kRpf9StreamAlignment = 0x10ull;       // 16-byte alignment for 78 DA zlib streams
constexpr uint32_t kSgd5Magic = 0x35444753u;             // Bytes 53 47 44 35 ("SGD5") read as LE u32
constexpr uint32_t kAdatMagic = 0x54414441u;             // Bytes 41 44 41 54 ("ADAT") read as LE u32
constexpr uint8_t kZlibBestCompressionCmf = 0x78u;       // RFC 1950 CMF (deflate, 32 KiB window)
constexpr uint8_t kZlibBestCompressionFlg = 0xDAu;       // RFC 1950 FLG (best compression)

struct Rpf9Header {
    uint32_t magic = 0;                // +0x00: 0x52504639 (bytes 39 46 50 52 LE)
    uint32_t metadata_end_offset = 0;  // +0x04: 0x10-aligned end of Region B / start of Payload
    uint32_t region_b_offset = 0;      // +0x08: 0x10-aligned start of Region B
    uint32_t tail_table_size = 0;      // +0x0C: 0x10-aligned size of EOF Region C
    uint64_t archive_seed = 0;         // +0x10: 64-bit archive seed
    uint32_t format_constant = 0;      // +0x18: 0x00047A00
    uint32_t region_a_offset = 0;      // +0x1C: 0x00000150 (immediately after crypto block)
    uint16_t archive_flags = 0;        // +0x20: archive flags
    uint16_t region_b_mode = 0;        // +0x22: 1 (compact formula) or 2 (extended layout)
    uint32_t entry_count = 0;          // +0x24: number of archive entries
    uint32_t key_profile_tag = 0;      // +0x28: key-profile selector tag
    uint8_t index_width = 0;           // +0x2C: 1 when entry_count <= 255, 2 when >= 256
    uint8_t header_byte_2d = 0;        // +0x2D: 0x0F
    uint8_t header_byte_2e = 0;        // +0x2E: 0x09
    uint8_t header_byte_2f = 0;        // +0x2F: 0x00
    uint32_t alignment = 0;            // +0x30: 0x10 (16-byte alignment)
};

struct Rpf9ByteRange {
    uint64_t offset = 0;
    uint64_t size = 0;

    [[nodiscard]] constexpr uint64_t end_offset() const noexcept {
        return offset + size;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return size == 0ull;
    }

    constexpr bool operator==(const Rpf9ByteRange&) const noexcept = default;
};

struct Rpf9RegionLayout {
    Rpf9ByteRange crypto_block{};  // [0x40, 0x150) — 272-byte crypto block
    Rpf9ByteRange region_a{};      // [0x150, region_b_offset)
    Rpf9ByteRange region_b{};      // [region_b_offset, metadata_end_offset)
    Rpf9ByteRange payload{};       // [metadata_end_offset, file_size - tail_table_size)
    Rpf9ByteRange region_c{};      // [file_size - tail_table_size, file_size)

    constexpr bool operator==(const Rpf9RegionLayout&) const noexcept = default;
};

enum class Rpf9RegionKind : uint8_t {
    RegionA = 0,
    RegionB = 1,
    RegionC = 2,
    Payload = 3,
};

// Minimal abstract decryptor hook so encrypted Region A/B/C tables and encrypted
// payloads can be plugged in cleanly without blocking plaintext extraction.
class Rpf9Decryptor {
public:
    virtual ~Rpf9Decryptor() = default;

    [[nodiscard]] virtual Status decrypt_region(const Rpf9Header& header,
                                                std::span<const uint8_t> crypto_block,
                                                Rpf9RegionKind region,
                                                std::span<uint8_t> buffer_in_out) const = 0;
};

enum class Rpf9PlaintextKind : uint8_t {
    NestedRpf9 = 0,  // 0x1000-aligned nested RPF9 (bytes 39 46 50 52)
    Sgd5 = 1,        // 0x1000-aligned SGD5 stream (bytes 53 47 44 35)
    Adat = 2,        // 0x1000-aligned ADAT stream (bytes 41 44 41 54)
    Zlib78Da = 3,    // 0x10-aligned RFC 1950 zlib stream (bytes 78 DA)
};

struct Rpf9PlaintextPayloadHit {
    uint64_t offset = 0;  // Archive-relative byte offset within Payload Region
    Rpf9PlaintextKind kind = Rpf9PlaintextKind::NestedRpf9;

    constexpr bool operator==(const Rpf9PlaintextPayloadHit&) const noexcept = default;
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

[[nodiscard]] constexpr uint8_t expected_rpf9_index_width(uint32_t entry_count) noexcept {
    return entry_count <= 255u ? 1u : 2u;
}

[[nodiscard]] constexpr uint64_t expected_rpf9_region_b_size(uint32_t entry_count,
                                                             uint8_t index_width) noexcept {
    const uint64_t raw = static_cast<uint64_t>(entry_count) *
                         (8ull + static_cast<uint64_t>(index_width));
    return (raw + 15ull) & ~15ull;
}

bool validate_rpf9_magic(uint32_t raw_magic) noexcept;

Status parse_rpf9_header(std::span<const uint8_t> data, Rpf9Header& out_header) noexcept;

Status validate_rpf9_regions(const Rpf9Header& header,
                             uint64_t file_size,
                             Rpf9RegionLayout& out_layout) noexcept;

Status scan_rpf9_plaintext_payloads(std::span<const uint8_t> archive_bytes,
                                    const Rpf9Header& header,
                                    std::vector<Rpf9PlaintextPayloadHit>& out_hits);

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
