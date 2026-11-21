#include "rpf9.hpp"

#include <limits>
#include <open_vfs/macros.hpp>
#include "../vfs_core/vfs_core.hpp"

namespace open_vfs::rpf9 {

OPEN_VFS_IMPL
bool validate_rpf9_magic(uint32_t raw_magic) noexcept {
    return raw_magic == kRpf9Magic;
}

OPEN_VFS_IMPL
Status parse_rpf9_header(std::span<const uint8_t> data, Rpf9Header& out_header) noexcept {
    if (data.size() < kRpf9HeaderSize) {
        return Status::OutOfBounds;
    }

    const auto magic = vfs_core::read_u32_le(data, 0x00);
    const auto metadata_end_offset = vfs_core::read_u32_le(data, 0x04);
    const auto region_b_offset = vfs_core::read_u32_le(data, 0x08);
    const auto tail_table_size = vfs_core::read_u32_le(data, 0x0C);
    const auto archive_seed = vfs_core::read_u64_le(data, 0x10);
    const auto format_constant = vfs_core::read_u32_le(data, 0x18);
    const auto region_a_offset = vfs_core::read_u32_le(data, 0x1C);
    const auto archive_flags = vfs_core::read_u16_le(data, 0x20);
    const auto region_b_mode = vfs_core::read_u16_le(data, 0x22);
    const auto entry_count = vfs_core::read_u32_le(data, 0x24);
    const auto key_profile_tag = vfs_core::read_u32_le(data, 0x28);
    const auto alignment = vfs_core::read_u32_le(data, 0x30);

    if (!magic || !metadata_end_offset || !region_b_offset || !tail_table_size ||
        !archive_seed || !format_constant || !region_a_offset || !archive_flags ||
        !region_b_mode || !entry_count || !key_profile_tag || !alignment) {
        return Status::OutOfBounds;
    }

    if (!validate_rpf9_magic(*magic)) {
        return Status::InvalidMagic;
    }

    if (!vfs_core::is_aligned_16(*metadata_end_offset) ||
        !vfs_core::is_aligned_16(*region_b_offset) ||
        !vfs_core::is_aligned_16(*tail_table_size) ||
        *alignment != kRpf9HeaderAlignment) {
        return Status::AlignmentError;
    }

    if (*format_constant != kRpf9FormatConstant) {
        return Status::UnsupportedFormat;
    }

    if (*region_a_offset != kRpf9RegionAOffset) {
        return Status::CorruptData;
    }

    if (*region_b_mode != kRpf9RegionBModeCompact &&
        *region_b_mode != kRpf9RegionBModeExtended) {
        return Status::UnsupportedFormat;
    }

    const uint8_t index_width = data[0x2C];
    const uint8_t byte_2d = data[0x2D];
    const uint8_t byte_2e = data[0x2E];
    const uint8_t byte_2f = data[0x2F];

    if (index_width != expected_rpf9_index_width(*entry_count)) {
        return Status::CorruptData;
    }

    if (byte_2d != kRpf9HeaderByte2D ||
        byte_2e != kRpf9HeaderByte2E ||
        byte_2f != kRpf9HeaderByte2F) {
        return Status::UnsupportedFormat;
    }

    for (std::size_t i = 0; i < kRpf9ReservedPaddingSize; ++i) {
        if (data[kRpf9ReservedPaddingOffset + i] != 0u) {
            return Status::CorruptData;
        }
    }

    out_header.magic = *magic;
    out_header.metadata_end_offset = *metadata_end_offset;
    out_header.region_b_offset = *region_b_offset;
    out_header.tail_table_size = *tail_table_size;
    out_header.archive_seed = *archive_seed;
    out_header.format_constant = *format_constant;
    out_header.region_a_offset = *region_a_offset;
    out_header.archive_flags = *archive_flags;
    out_header.region_b_mode = *region_b_mode;
    out_header.entry_count = *entry_count;
    out_header.key_profile_tag = *key_profile_tag;
    out_header.index_width = index_width;
    out_header.header_byte_2d = byte_2d;
    out_header.header_byte_2e = byte_2e;
    out_header.header_byte_2f = byte_2f;
    out_header.alignment = *alignment;
    return Status::Ok;
}

OPEN_VFS_IMPL
Status validate_rpf9_regions(const Rpf9Header& header,
                             uint64_t file_size,
                             Rpf9RegionLayout& out_layout) noexcept {
    out_layout = Rpf9RegionLayout{};

    if (!validate_rpf9_magic(header.magic)) {
        return Status::InvalidMagic;
    }

    if (!vfs_core::is_aligned_16(header.metadata_end_offset) ||
        !vfs_core::is_aligned_16(header.region_b_offset) ||
        !vfs_core::is_aligned_16(header.tail_table_size) ||
        header.alignment != kRpf9HeaderAlignment) {
        return Status::AlignmentError;
    }

    if (header.format_constant != kRpf9FormatConstant) {
        return Status::UnsupportedFormat;
    }

    if (header.region_a_offset != kRpf9RegionAOffset) {
        return Status::CorruptData;
    }

    if (header.region_b_mode != kRpf9RegionBModeCompact &&
        header.region_b_mode != kRpf9RegionBModeExtended) {
        return Status::UnsupportedFormat;
    }

    if (header.index_width != expected_rpf9_index_width(header.entry_count)) {
        return Status::CorruptData;
    }

    const uint64_t region_a_start = static_cast<uint64_t>(header.region_a_offset);
    const uint64_t region_b_start = static_cast<uint64_t>(header.region_b_offset);
    const uint64_t metadata_end = static_cast<uint64_t>(header.metadata_end_offset);
    const uint64_t tail_size = static_cast<uint64_t>(header.tail_table_size);

    if (region_b_start < region_a_start || metadata_end < region_b_start) {
        return Status::CorruptData;
    }

    if (tail_size > file_size) {
        return Status::OutOfBounds;
    }

    const uint64_t payload_end = file_size - tail_size;
    if (metadata_end > payload_end) {
        return Status::OutOfBounds;
    }

    const uint64_t region_b_size = metadata_end - region_b_start;
    if (header.region_b_mode == kRpf9RegionBModeCompact) {
        const uint64_t expected_b_size =
            expected_rpf9_region_b_size(header.entry_count, header.index_width);
        if (region_b_size != expected_b_size) {
            return Status::CorruptData;
        }
    }

    out_layout.crypto_block = Rpf9ByteRange{
        static_cast<uint64_t>(kRpf9CryptoBlockOffset),
        static_cast<uint64_t>(kRpf9CryptoBlockSize),
    };
    out_layout.region_a = Rpf9ByteRange{
        region_a_start,
        region_b_start - region_a_start,
    };
    out_layout.region_b = Rpf9ByteRange{
        region_b_start,
        region_b_size,
    };
    out_layout.payload = Rpf9ByteRange{
        metadata_end,
        payload_end - metadata_end,
    };
    out_layout.region_c = Rpf9ByteRange{
        payload_end,
        tail_size,
    };
    return Status::Ok;
}

OPEN_VFS_IMPL
Status scan_rpf9_plaintext_payloads(std::span<const uint8_t> archive_bytes,
                                    const Rpf9Header& header,
                                    std::vector<Rpf9PlaintextPayloadHit>& out_hits) {
    out_hits.clear();

    Rpf9RegionLayout regions{};
    const Status region_status =
        validate_rpf9_regions(header, static_cast<uint64_t>(archive_bytes.size()), regions);
    if (region_status != Status::Ok) {
        return region_status;
    }

    const std::size_t payload_start = static_cast<std::size_t>(regions.payload.offset);
    const std::size_t payload_end = static_cast<std::size_t>(regions.payload.end_offset());
    constexpr std::size_t kPageAlign = static_cast<std::size_t>(kRpf9PageAlignment);
    constexpr std::size_t kStreamAlign = static_cast<std::size_t>(kRpf9StreamAlignment);

    for (std::size_t off = payload_start; off < payload_end;) {
        const std::size_t remaining = payload_end - off;
        bool matched = false;

        if ((off & (kPageAlign - 1u)) == 0u && remaining >= 4u) {
            const auto magic = vfs_core::read_u32_le(archive_bytes, off);
            if (magic.has_value()) {
                if (*magic == kRpf9Magic) {
                    out_hits.push_back(Rpf9PlaintextPayloadHit{
                        static_cast<uint64_t>(off),
                        Rpf9PlaintextKind::NestedRpf9,
                    });
                    matched = true;
                } else if (*magic == kSgd5Magic) {
                    out_hits.push_back(Rpf9PlaintextPayloadHit{
                        static_cast<uint64_t>(off),
                        Rpf9PlaintextKind::Sgd5,
                    });
                    matched = true;
                } else if (*magic == kAdatMagic) {
                    out_hits.push_back(Rpf9PlaintextPayloadHit{
                        static_cast<uint64_t>(off),
                        Rpf9PlaintextKind::Adat,
                    });
                    matched = true;
                }
            }
        }

        if (!matched && remaining >= 2u &&
            archive_bytes[off] == kZlibBestCompressionCmf &&
            archive_bytes[off + 1u] == kZlibBestCompressionFlg) {
            out_hits.push_back(Rpf9PlaintextPayloadHit{
                static_cast<uint64_t>(off),
                Rpf9PlaintextKind::Zlib78Da,
            });
        }

        if (remaining <= kStreamAlign) {
            break;
        }
        off += kStreamAlign;
    }

    return Status::Ok;
}

OPEN_VFS_IMPL
Status parse_rpf9_toc_entry(std::span<const uint8_t> entry_bytes, Rpf9TocEntry& out_entry) noexcept {
    if (entry_bytes.size() < kRpf9TocEntrySize) {
        return Status::OutOfBounds;
    }

    const auto name_offset = vfs_core::read_u32_le(entry_bytes, 0);
    const auto flags = vfs_core::read_u32_le(entry_bytes, 4);
    const auto offset_or_index = vfs_core::read_u32_le(entry_bytes, 8);
    const auto size_or_count = vfs_core::read_u32_le(entry_bytes, 12);

    if (!name_offset || !flags || !offset_or_index || !size_or_count) {
        return Status::OutOfBounds;
    }

    out_entry.name_offset = *name_offset;
    out_entry.flags = *flags;
    out_entry.offset_or_index = *offset_or_index;
    out_entry.size_or_count = *size_or_count;
    out_entry.is_directory = ((*flags) & 0x80000000u) != 0u;
    return Status::Ok;
}

OPEN_VFS_IMPL
Status parse_rpf9_toc_table(std::span<const uint8_t> archive_bytes,
                            const Rpf9Header& header,
                            std::vector<Rpf9TocEntry>& out_entries) {
    out_entries.clear();

    if (!validate_rpf9_magic(header.magic)) {
        return Status::InvalidMagic;
    }
    if (!vfs_core::is_aligned_16(header.region_a_offset)) {
        return Status::AlignmentError;
    }

    const uint64_t archive_size = static_cast<uint64_t>(archive_bytes.size());
    const uint64_t table_offset = static_cast<uint64_t>(header.region_a_offset);
    if (table_offset > archive_size) {
        return Status::OutOfBounds;
    }

    const uint64_t entry_count = static_cast<uint64_t>(header.entry_count);
    constexpr uint64_t kEntrySize = static_cast<uint64_t>(kRpf9TocEntrySize);
    if (entry_count > (std::numeric_limits<uint64_t>::max() / kEntrySize)) {
        return Status::OutOfBounds;
    }

    const uint64_t table_size = entry_count * kEntrySize;
    if (table_size > archive_size - table_offset) {
        return Status::OutOfBounds;
    }

    out_entries.reserve(static_cast<std::size_t>(entry_count));
    std::size_t cursor = static_cast<std::size_t>(table_offset);
    for (uint32_t i = 0; i < header.entry_count; ++i) {
        Rpf9TocEntry entry{};
        const Status status = parse_rpf9_toc_entry(
            archive_bytes.subspan(cursor, kRpf9TocEntrySize), entry);
        if (status != Status::Ok) {
            out_entries.clear();
            return status;
        }
        out_entries.push_back(entry);
        cursor += kRpf9TocEntrySize;
    }

    return Status::Ok;
}

OPEN_VFS_STUB
Status resolve_rpf9_name_table(std::span<const uint8_t> names_heap,
                               const std::vector<Rpf9TocEntry>& entries,
                               std::vector<std::string>& out_names) {
    (void)names_heap;
    (void)entries;
    out_names.clear();
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status locate_rpf9_entry_by_path(const std::vector<Rpf9TocEntry>& entries,
                                 const std::vector<std::string>& names,
                                 std::string_view virtual_path,
                                 std::size_t& out_index) {
    (void)entries;
    (void)names;
    (void)virtual_path;
    out_index = 0;
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status parse_rpf9_block_header(std::span<const uint8_t> block_bytes,
                               Rpf9BlockHeader& out_block) noexcept {
    (void)block_bytes;
    out_block = Rpf9BlockHeader{};
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status decompress_rpf9_block(const Rpf9BlockHeader& block_header,
                             std::span<const uint8_t> compressed_payload,
                             std::vector<uint8_t>& out_plain) {
    (void)block_header;
    (void)compressed_payload;
    out_plain.clear();
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status decrypt_rpf9_payload(uint32_t encryption_flags,
                            std::span<uint8_t> payload_in_out) {
    (void)encryption_flags;
    (void)payload_in_out;
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status extract_rpf9_entry_bytes(std::span<const uint8_t> archive_bytes,
                                const Rpf9Header& header,
                                const Rpf9TocEntry& entry,
                                std::vector<uint8_t>& out_payload) {
    (void)archive_bytes;
    (void)header;
    (void)entry;
    out_payload.clear();
    return Status::NotImplemented;
}

}  // namespace open_vfs::rpf9
